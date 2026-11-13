#!/usr/bin/env python3
"""
night_soak_charts.py -- 夜跑 soak 数据的内存 / 温度图表 (SVG -> PNG, 不用 matplotlib)

用法:
    python3 scripts/night_soak_charts.py <metrics.csv> <输出目录>

输出:
    <输出目录>/night_soak_heap_flat.png    活堆平线 + RSS + 100MB 泄漏红线
    <输出目录>/night_soak_temp_curve.png   NPU/SoC/BigCPU/LittleCPU 温度曲线 + 85C 降频线

实现:
    板子上没有 matplotlib, 只有 python3 + cv2 + numpy。
    参考 rk3588_zero_copy/scripts/hw_load_chart.py 的做法 —— 直接拼 SVG 字符串,
    再用 chromium headless 把 SVG(包在 HTML 里)截图成 PNG。

数据约定:
    * CSV 每 30 s 一行; pid 列为空的行 = 程序已退出后看护进程继续采的空行,
      必须过滤, 否则图上会出现一段假的平台。
    * heap_kb / rss_kb 为 0 表示该次采样没读到有效值 (进程切换瞬间), 不参与统计/画线。
    * rss_kb < MIN_PLAUSIBLE_RSS_KB 的样本属于启动期的壳/包装进程, 不是被测程序本身。
    * 帧号回退 / 采样时间出现大缺口 = 程序重启。曲线在该处断开, 不做跨重启的直线
      连接, 否则会画出一条并不存在的斜坡。
    * 温度传感器读数是 0.9 C 量化的, 逐点连线会变成锯齿; 曲线用 7.5 min 滚动峰值
      (peak-hold) 绘制, 它保留每个传感器的真实最高温度。
    * 图中文字一律用英文 + 数字 (板子上没有中文字体, 会出现豆腐块)。
"""
from __future__ import annotations

import csv
import html
import math
import os
import shutil
import subprocess
import sys
import tempfile
from datetime import datetime

# ---------------------------------------------------------------- 样式常量
# 风格与 rk3588_zero_copy/scripts/hw_load_chart.py 保持一致 (深色 rktop 风格面板)
PAGE_BG = "#0b0e13"
PANEL_BG = "#12151c"
PANEL_ST = "#2b313d"
GRID = "#242a35"
TICK_FG = "#7d8590"
TEXT_FG = "#9aa4b2"
TITLE_FG = "#c9d1d9"
HEAD_FG = "#e6edf3"
FONT = "DejaVu Sans, Arial, Helvetica, sans-serif"
MONO = "DejaVu Sans Mono, Menlo, Consolas, monospace"

HEAP_C = "#4cc9f0"  # 活堆
RSS_C = "#f2a541"  # 进程总占用
LEAK_C = "#ff5c5c"  # 100MB 泄漏红线 / 85C 降频线
TREND_C = "#8d99ae"
RESTART_C = "#7a8494"

TEMP_SERIES = [
    ("NPU", "npu_temp_c", "#38bdf8"),
    ("SoC", "soc_temp_c", "#fbbf24"),
    ("Big CPU", "bigcpu_temp_c", "#a78bfa"),
    ("Little CPU", "little_temp_c", "#34d399"),
]

LEAK_LINE_KB = 100 * 1024  # 100MB 泄漏红线
THROTTLE_C = 85.0  # 典型降频阈值
MIN_PLAUSIBLE_RSS_KB = 8000  # 低于此值的 RSS 属于启动期壳进程, 不是被测程序
TEMP_WINDOW = 15  # 温度 peak-hold 窗口 (15 * 30 s = 7.5 min)
RESTART_GAP_S = 120  # 采样间隔超过这个值视为程序重启


# ---------------------------------------------------------------- 数据读取
def _num(v):
    if v is None:
        return None
    s = str(v).strip()
    if not s or s in ("-", "nan", "NaN", "None"):
        return None
    try:
        return float(s)
    except ValueError:
        return None


def load_rows(path):
    """读 CSV -> 每行一个 dict (数值字段已转 float, 无法解析的为 None)"""
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            t = _num(r.get("time"))
            if t is None:
                continue
            rows.append(
                {
                    "time": t,
                    "frame": _num(r.get("frame")),
                    "rss_kb": _num(r.get("rss_kb")),
                    "heap_kb": _num(r.get("heap_kb")),
                    "npu_temp_c": _num(r.get("npu_temp_c")),
                    "soc_temp_c": _num(r.get("soc_temp_c")),
                    "bigcpu_temp_c": _num(r.get("bigcpu_temp_c")),
                    "little_temp_c": _num(r.get("little_temp_c")),
                    "cpu_freq_khz": _num(r.get("cpu_freq_khz")),
                    "npu_pct": _num(r.get("npu_pct")),
                    "restarts": _num(r.get("restarts")),
                    "pid": (r.get("pid") or "").strip(),
                }
            )
    return rows


def split_alive(rows):
    """pid 非空 = 程序存活期间的采样; pid 为空 = 程序已退出后的空行 (必须过滤)"""
    alive = [r for r in rows if r["pid"] != ""]
    empty = [r for r in rows if r["pid"] == ""]
    return alive, empty


def rolling_max(vals, w=TEMP_WINDOW):
    """居中窗口滚动峰值 (peak-hold), 保留全局最大值"""
    if not vals:
        return []
    h = w // 2
    n = len(vals)
    out = []
    for i in range(n):
        out.append(max(vals[max(0, i - h):min(n, i + h + 1)]))
    return out


def rolling_mean(vals, w=TEMP_WINDOW):
    """居中窗口滚动平均, 用于画出平滑的趋势线"""
    if not vals:
        return []
    h = w // 2
    n = len(vals)
    out = []
    for i in range(n):
        win = vals[max(0, i - h):min(n, i + h + 1)]
        out.append(sum(win) / len(win))
    return out


def nice_step(span, target=6):
    if span <= 0:
        return 1.0
    raw = span / float(target)
    mag = 10 ** math.floor(math.log10(raw))
    for m in (1, 2, 2.5, 5, 10):
        if raw <= m * mag:
            return m * mag
    return 10 * mag


def nice_range(lo, hi, target=6, pad_frac=0.12):
    """给 [lo,hi] 一个好看的坐标量程 + 刻度"""
    if hi <= lo:
        hi = lo + 1.0
    span = hi - lo
    lo -= span * pad_frac
    hi += span * pad_frac
    step = nice_step(hi - lo, target)
    return math.floor(lo / step) * step, math.ceil(hi / step) * step, step


def ticks(lo, hi, step):
    """生成 [lo,hi] 内的刻度列表"""
    out = []
    v = math.ceil(lo / step - 1e-9) * step
    while v <= hi + step * 1e-6:
        out.append(round(v, 6))
        v += step
    return out


def fmt_int(v):
    return f"{int(round(v)):,}"


def fmt_kb(v):
    return f"{int(round(v)):,} KB"


def text_w(s, size):
    """粗略估算文字宽度 (等宽近似)"""
    return len(str(s)) * size * 0.58


# ---------------------------------------------------------------- SVG 基础件
class Svg:
    def __init__(self, w, h):
        self.w = w
        self.h = h
        self.parts = []

    def add(self, s):
        self.parts.append(s)

    def rect(self, x, y, w, h, fill, stroke=None, sw=1.0, rx=0, extra=""):
        st = f' stroke="{stroke}" stroke-width="{sw}"' if stroke else ""
        rxf = f' rx="{rx}"' if rx else ""
        self.add(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" '
                 f'fill="{fill}"{st}{rxf}{extra}/>')

    def line(self, x1, y1, x2, y2, stroke, sw=1.0, dash=None, cap=None):
        d = f' stroke-dasharray="{dash}"' if dash else ""
        c = f' stroke-linecap="{cap}"' if cap else ""
        self.add(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" '
                 f'stroke="{stroke}" stroke-width="{sw}"{d}{c}/>')

    def text(self, x, y, s, fill=TEXT_FG, size=11, anchor="start", weight=None,
             family=MONO, extra=""):
        wt = f' font-weight="{weight}"' if weight else ""
        self.add(f'<text x="{x:.1f}" y="{y:.1f}" fill="{fill}" text-anchor="{anchor}" '
                 f'font-family="{family}" font-size="{size}"{wt}{extra}>{html.escape(str(s))}</text>')

    def tag(self, x, y, s, color, size=11, anchor="start", weight="bold"):
        """带底色的标注, 避免压在曲线上看不清"""
        w = text_w(s, size)
        if anchor == "end":
            bx = x - w
        elif anchor == "middle":
            bx = x - w / 2.0
        else:
            bx = x
        self.rect(bx - 4, y - size - 1, w + 8, size + 6, "#12151c", extra=' opacity="0.85"')
        self.text(x, y, s, color, size, anchor, weight, FONT)

    def polyline(self, pts, color, sw=2.0, dash=None, opacity=None):
        if len(pts) < 2:
            return
        d = f' stroke-dasharray="{dash}"' if dash else ""
        o = f' stroke-opacity="{opacity}"' if opacity else ""
        p = " ".join(f"{x:.1f},{y:.1f}" for x, y in pts)
        self.add(f'<polyline points="{p}" fill="none" stroke="{color}" stroke-width="{sw}" '
                 f'stroke-linejoin="round" stroke-linecap="round"{d}{o}/>')

    def polygon(self, pts, fill, opacity=1.0):
        if len(pts) < 3:
            return
        p = " ".join(f"{x:.1f},{y:.1f}" for x, y in pts)
        self.add(f'<polygon points="{p}" fill="{fill}" opacity="{opacity}"/>')

    def circle(self, x, y, r, fill, stroke=None, sw=1.5):
        st = f' stroke="{stroke}" stroke-width="{sw}"' if stroke else ""
        self.add(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{r:.1f}" fill="{fill}"{st}/>')

    def render(self):
        return (
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" height="{self.h}" '
            f'viewBox="0 0 {self.w} {self.h}">'
            f'<rect width="{self.w}" height="{self.h}" fill="{PAGE_BG}"/>'
            + "\n".join(self.parts)
            + "\n</svg>\n"
        )


class Axes:
    """一个绘图区: 画面板/网格/刻度, 并给出 数值->像素 的映射"""

    def __init__(self, svg, x0, y0, w, h, xlim, ylim, xticks, yticks,
                 xfmt=str, yfmt=str, left=78, right=26, top=44, bottom=64):
        self.svg = svg
        self.x0, self.y0, self.w, self.h = x0, y0, w, h
        self.px0 = x0 + left
        self.py0 = y0 + top
        self.pw = w - left - right
        self.ph = h - top - bottom
        self.xmin, self.xmax = xlim
        self.ymin, self.ymax = ylim
        self.xticks, self.yticks = xticks, yticks
        self.xfmt, self.yfmt = xfmt, yfmt

    def sx(self, v):
        if self.xmax == self.xmin:
            return self.px0
        return self.px0 + self.pw * (v - self.xmin) / (self.xmax - self.xmin)

    def sy(self, v):
        if self.ymax == self.ymin:
            return self.py0 + self.ph
        return self.py0 + self.ph * (1.0 - (v - self.ymin) / (self.ymax - self.ymin))

    def draw(self, xlabel=None, clock_fn=None):
        s = self.svg
        s.rect(self.x0, self.y0, self.w, self.h, PANEL_BG, PANEL_ST)
        for tv in self.xticks:
            gx = self.sx(tv)
            s.line(gx, self.py0, gx, self.py0 + self.ph, GRID, 1)
        for tv in self.yticks:
            gy = self.sy(tv)
            s.line(self.px0, gy, self.px0 + self.pw, gy, GRID, 1)
        for tv in self.yticks:
            gy = self.sy(tv)
            s.text(self.px0 - 9, gy + 4, self.yfmt(tv), TICK_FG, 11, "end", family=MONO)
        s.line(self.px0, self.py0 + self.ph, self.px0 + self.pw, self.py0 + self.ph, PANEL_ST, 1.5)
        s.line(self.px0, self.py0, self.px0, self.py0 + self.ph, PANEL_ST, 1.5)
        for tv in self.xticks:
            gx = self.sx(tv)
            s.line(gx, self.py0 + self.ph, gx, self.py0 + self.ph + 5, TICK_FG, 1)
            s.text(gx, self.py0 + self.ph + 19, self.xfmt(tv), TICK_FG, 11, "middle", family=MONO)
        if clock_fn is not None:
            for tv in self.xticks:
                gx = self.sx(tv)
                s.text(gx, self.py0 + self.ph + 35, clock_fn(tv), "#5c6572", 10, "middle", family=MONO)
        if xlabel:
            s.text(self.px0 + self.pw / 2.0, self.py0 + self.ph + (58 if clock_fn else 40),
                   xlabel, TEXT_FG, 12, "middle", family=FONT)
        return self

    def y_title(self, txt):
        cx = self.x0 + 24
        cy = self.py0 + self.ph / 2.0
        self.svg.text(cx, cy, txt, TICK_FG, 12, "middle", None, FONT,
                      extra=f' transform="rotate(-90 {cx:.1f} {cy:.1f})"')

    def heading(self, txt, right=None):
        """面板标题放在绘图区上方的留白条里, 避免压住曲线"""
        self.svg.text(self.px0 + 12, self.y0 + 22, txt, TITLE_FG, 13, "start", "bold", FONT)
        if right:
            self.svg.text(self.px0 + self.pw - 4, self.y0 + 22, right, LEAK_C, 13,
                          "end", "bold", FONT)

    def legend_v(self, entries, x, y, dy=22, size=11):
        """entries: [(label, color, dashed_bool)] 竖排"""
        s = self.svg
        for i, (label, color, dashed) in enumerate(entries):
            ly = y + i * dy
            if dashed:
                s.line(x, ly - 4, x + 20, ly - 4, color, 2.4, dash="7,5")
            else:
                s.line(x, ly - 4, x + 20, ly - 4, color, 3.0)
            s.text(x + 27, ly, label, TEXT_FG, size, "start", family=FONT)
        return y + len(entries) * dy

    def legend_h(self, entries, x, y, gap=26, size=11):
        """entries: [(label, color, dashed_bool)] 横排"""
        s = self.svg
        cx = x
        for label, color, dashed in entries:
            if dashed:
                s.line(cx, y - 4, cx + 20, y - 4, color, 2.4, dash="7,5")
            else:
                s.line(cx, y - 4, cx + 20, y - 4, color, 3.0)
            s.text(cx + 27, y, label, TEXT_FG, size, "start", family=FONT)
            cx += 27 + int(text_w(label, size)) + gap
        return cx


def split_segments(pairs, breaks):
    """pairs: [(x,y)...]; breaks: 需要断开的下标集合 -> 返回若干段"""
    segs = []
    cur = []
    for i, p in enumerate(pairs):
        if i in breaks and cur:
            segs.append(cur)
            cur = []
        cur.append(p)
    if cur:
        segs.append(cur)
    return segs


# ---------------------------------------------------------------- 统计
def compute_stats(rows):
    alive, empty = split_alive(rows)
    if not alive:
        raise SystemExit("no live samples (pid column is empty everywhere)")

    t0 = alive[0]["time"]
    t1 = alive[-1]["time"]
    dur = t1 - t0

    # 重启点 (alive 下标): 帧号回退, 或采样时间出现大缺口
    restart_alive = set()
    for i in range(1, len(alive)):
        p, c = alive[i - 1], alive[i]
        if c["frame"] is not None and p["frame"] is not None and c["frame"] < p["frame"]:
            restart_alive.add(i)
        elif c["time"] - p["time"] > RESTART_GAP_S:
            restart_alive.add(i)

    def series(col, keep):
        """返回 [(t_rel, value, alive_idx), ...] 以及该序列内部需要断开的下标"""
        items = []
        for i, r in enumerate(alive):
            v = r[col]
            if v is None or not keep(v):
                continue
            items.append((r["time"] - t0, v, i))
        breaks = {k for k, it in enumerate(items) if it[2] in restart_alive}
        return items, breaks

    heap_items, heap_breaks = series("heap_kb", lambda v: v > 0)
    rss_items, rss_breaks = series("rss_kb", lambda v: v >= MIN_PLAUSIBLE_RSS_KB)

    heap = [(t, v) for t, v, _ in heap_items]
    rss = [(t, v) for t, v, _ in rss_items]
    hv = [v for _, v in heap]
    rv = [v for _, v in rss]

    frames = [r["frame"] for r in alive if r["frame"] is not None and r["frame"] >= 0]

    def maxv(key):
        return max((r[key] for r in alive if r[key] is not None), default=None)

    def minv(key):
        return min((r[key] for r in alive if r[key] is not None), default=None)

    temps = {name: maxv(col) for name, col, _ in TEMP_SERIES}
    temp_min = {name: minv(col) for name, col, _ in TEMP_SERIES}
    cpu_freqs = [r["cpu_freq_khz"] for r in alive if r["cpu_freq_khz"]]
    npu_pcts = [r["npu_pct"] for r in alive if r["npu_pct"] is not None]
    restarts = [r["restarts"] for r in alive if r["restarts"] is not None]

    net = hv[-1] - hv[0]
    hours = dur / 3600.0
    slope = net / hours if hours else 0.0

    # 温度 peak-hold 序列 (与存活段下标对齐, 便于画图)
    tm = {}
    for name, col, _ in TEMP_SERIES:
        raw = [r[col] for r in alive]
        clean = [v if v is not None else 0.0 for v in raw]
        tm[name] = (raw, rolling_max(clean))

    # 每个传感器的真实最高温所在位置
    temp_peaks = []
    for name, col, color in TEMP_SERIES:
        bi, bv = None, None
        for i, r in enumerate(alive):
            v = r[col]
            if v is not None and (bv is None or v > bv):
                bi, bv = i, v
        temp_peaks.append((name, color, bi, bv))

    return {
        "rows_total": len(rows),
        "alive": alive,
        "empty": empty,
        "t0": t0,
        "t1": t1,
        "dur": dur,
        "hours": hours,
        "heap": heap,
        "heap_breaks": heap_breaks,
        "rss": rss,
        "rss_breaks": rss_breaks,
        "heap_start": hv[0],
        "heap_end": hv[-1],
        "heap_max": max(hv),
        "heap_min": min(hv),
        "heap_net": net,
        "heap_slope_h": slope,
        "heap_peak_at": max(heap, key=lambda p: p[1])[0],
        "rss_start": rv[0],
        "rss_end": rv[-1],
        "rss_max": max(rv),
        "rss_min": min(rv),
        "rss_pct_of_limit": 100.0 * max(rv) / LEAK_LINE_KB,
        "frame_first": frames[0] if frames else None,
        "frame_last": frames[-1] if frames else None,
        "temps": temps,
        "temps_min": temp_min,
        "temp_series": tm,
        "temp_peaks": temp_peaks,
        "cpu_freq_min": min(cpu_freqs) if cpu_freqs else None,
        "cpu_freq_max": max(cpu_freqs) if cpu_freqs else None,
        "npu_pct_min": min(npu_pcts) if npu_pcts else None,
        "npu_pct_max": max(npu_pcts) if npu_pcts else None,
        "restarts": max(restarts) if restarts else None,
        "heap_ge_leak": any(v >= LEAK_LINE_KB for v in hv),
        "rss_ge_leak": any(v >= LEAK_LINE_KB for v in rv),
        "n_dropped_rss": sum(1 for r in alive
                              if r["rss_kb"] and 0 < r["rss_kb"] < MIN_PLAUSIBLE_RSS_KB),
        "n_restarts": len(restart_alive),
    }


# ---------------------------------------------------------------- 图 1: 活堆平线
def build_heap_svg(st, W=1800, H=1120):
    s = Svg(W, H)
    s.rect(0, 0, W, H, PAGE_BG)

    s.text(60, 42,
           f"Night Soak Live Heap: Flat Over {st['hours']:.2f} h / "
           f"{fmt_int(st['frame_last'])} Frames (net +{fmt_int(st['heap_net'])} KB, "
           f"+{st['heap_slope_h']:.0f} KB/h)", HEAD_FG, 24, "start", "bold", FONT)
    s.text(60, 68,
           f"RK3588 zero-copy NPU pipeline  |  "
           f"{datetime.fromtimestamp(st['t0']).strftime('%Y-%m-%d %H:%M:%S')} -> "
           f"{datetime.fromtimestamp(st['t1']).strftime('%H:%M:%S')}  |  "
           f"{st['dur']:,} s = {st['hours']:.2f} h  |  30 s sampling  |  "
           f"{len(st['alive'])} live samples (pid non-empty)  |  "
           f"post-exit watchdog rows filtered out",
           TICK_FG, 13, "start", None, FONT)

    hours_max = st["hours"]
    xt = [float(i) for i in range(0, int(hours_max) + 1)]
    xfmt = lambda v: f"{v:.0f}"

    def clock_fn(v):
        return datetime.fromtimestamp(st["t0"] + v * 3600.0).strftime("%H:%M")

    # ================= 上半部分: 活堆明细 (y 轴放大) =================
    hv = [v for _, v in st["heap"]]
    ymin, ymax, step = nice_range(min(hv), max(hv), target=7, pad_frac=0.18)
    ax = Axes(s, 60, 92, W - 120, 476, (0.0, hours_max), (ymin, ymax),
              xt, ticks(ymin, ymax, step), xfmt, lambda v: fmt_int(v),
              left=78, right=26, top=44, bottom=64)
    ax.draw(xlabel=None, clock_fn=clock_fn)
    ax.y_title("Live heap (KB)")
    ax.heading("Live heap (KB) - detail view (y axis zoomed onto the live band)")

    # 净增趋势线 (起点 -> 终点)
    sx0, sy0 = ax.sx(st["heap"][0][0] / 3600.0), ax.sy(st["heap"][0][1])
    sx1, sy1 = ax.sx(st["heap"][-1][0] / 3600.0), ax.sy(st["heap"][-1][1])
    s.polyline([(sx0, sy0), (sx1, sy1)], TREND_C, 1.8, dash="8,6")

    # 活堆曲线 (在程序重启处断开)
    for seg in split_segments([(ax.sx(t / 3600.0), ax.sy(v)) for t, v in st["heap"]],
                               st["heap_breaks"]):
        s.polyline(seg, HEAP_C, 2.0)

    # 重启标记
    last_restart_x = None
    for i, (rt, _v) in enumerate(st["heap"]):
        if i in st["heap_breaks"] and 0 < rt / 3600.0 < hours_max:
            gx = ax.sx(rt / 3600.0)
            s.line(gx, ax.py0, gx, ax.py0 + ax.ph, RESTART_C, 1.3, dash="5,6")
            last_restart_x = gx
    if last_restart_x is not None:
        s.tag(last_restart_x + 8, ax.py0 + 18, "app restart", RESTART_C, 11)

    # 起止 / 峰值标记
    hx0, hy0 = ax.sx(st["heap"][0][0] / 3600.0), ax.sy(st["heap"][0][1])
    hx1, hy1 = ax.sx(st["heap"][-1][0] / 3600.0), ax.sy(st["heap"][-1][1])
    px_, py_ = ax.sx(st["heap_peak_at"] / 3600.0), ax.sy(st["heap_max"])
    s.circle(hx0, hy0, 5.5, HEAP_C, PAGE_BG, 1.6)
    s.circle(hx1, hy1, 5.5, HEAP_C, PAGE_BG, 1.6)
    s.circle(px_, py_, 5.5, "#ffffff", LEAK_C, 2.0)

    s.tag(hx0 + 12, hy0 - 13, f"start {fmt_kb(st['heap_start'])}", HEAP_C, 12)
    s.tag(px_ + 11, py_ - 12, f"peak {fmt_kb(st['heap_max'])}", "#ffd7d7", 12)
    s.tag(hx1 - 11, hy1 + 24, f"end {fmt_kb(st['heap_end'])}", HEAP_C, 12, "end")
    s.tag(ax.px0 + ax.pw * 0.40, ax.py0 + ax.ph * 0.72,
          f"start -> end trend:  +{fmt_int(st['heap_net'])} KB  "
          f"(+{st['heap_slope_h']:.0f} KB/h)", TREND_C, 12, "middle")

    # 统计框 (右下角空白区)
    bw, bh = 430, 104
    bx, by = ax.px0 + ax.pw - bw - 16, ax.py0 + ax.ph - bh - 12
    s.rect(bx, by, bw, bh, "#161b24", PANEL_ST, 1.0, rx=4)
    lines = [
        ("Live heap start", fmt_kb(st["heap_start"])),
        ("Live heap end", fmt_kb(st["heap_end"])),
        ("Net growth", f"+{fmt_int(st['heap_net'])} KB"),
        ("Slope", f"+{st['heap_slope_h']:.1f} KB/h"),
        ("Peak / min", f"{fmt_kb(st['heap_max'])} / {fmt_kb(st['heap_min'])}"),
    ]
    for i, (k, v) in enumerate(lines):
        yy = by + 20 + i * 17
        s.text(bx + 13, yy, k, TICK_FG, 11, "start", None, FONT)
        s.text(bx + bw - 13, yy, v, "#d7e0ea", 11, "end", "bold", MONO)

    # 图例
    ax.legend_v([("Live heap (KB)", HEAP_C, False),
                 ("start -> end trend", TREND_C, True),
                 ("app restart", RESTART_C, True)],
                ax.px0 + 24, ax.py0 + ax.ph - 92)

    # ================= 下半部分: RSS + 100MB 红线 =================
    y1 = 110000.0
    ax2 = Axes(s, 60, 592, W - 120, 380, (0.0, hours_max), (0.0, y1),
               xt, ticks(0.0, y1, 20000.0), xfmt, lambda v: fmt_int(v),
               left=78, right=26, top=44, bottom=64)
    ax2.draw(xlabel="Runtime (hours) since program start   |   wall clock below",
             clock_fn=clock_fn)
    ax2.y_title("Memory (KB)")
    ax2.heading("Process RSS (KB) and live heap (KB) on the shared scale, vs the 100 MB leak line",
                right="100 MB leak threshold = 102,400 KB  (never reached)")

    # 100MB 红线 + 危险区
    ly = ax2.sy(LEAK_LINE_KB)
    s.rect(ax2.px0, ax2.py0, ax2.pw, ly - ax2.py0, "#ff5c5c", extra=' opacity="0.09"')
    s.line(ax2.px0, ly, ax2.px0 + ax2.pw, ly, LEAK_C, 2.2, dash="9,6")

    for i, (rt, _v) in enumerate(st["heap"]):
        if i in st["heap_breaks"] and 0 < rt / 3600.0 < hours_max:
            gx = ax2.sx(rt / 3600.0)
            s.line(gx, ax2.py0, gx, ax2.py0 + ax2.ph, RESTART_C, 1.2, dash="5,6")

    # RSS 曲线
    rpts = [(ax2.sx(t / 3600.0), ax2.sy(v)) for t, v in st["rss"]]
    s.polyline(rpts, RSS_C, 2.2)

    # 活堆曲线 (同一坐标, 说明"活堆"只是进程占用的一小部分)
    hpts = [(ax2.sx(t / 3600.0), ax2.sy(v)) for t, v in st["heap"]]
    for seg in split_segments(hpts, st["heap_breaks"]):
        s.polyline(seg, HEAP_C, 1.8)

    # RSS 起止/峰值标记
    rx0, ry0 = ax2.sx(st["rss"][0][0] / 3600.0), ax2.sy(st["rss"][0][1])
    rx1, ry1 = ax2.sx(st["rss"][-1][0] / 3600.0), ax2.sy(st["rss"][-1][1])
    rpeak_t = max(st["rss"], key=lambda p: p[1])[0]
    rpx, rpy = ax2.sx(rpeak_t / 3600.0), ax2.sy(st["rss_max"])
    s.circle(rx0, ry0, 5.0, RSS_C, PAGE_BG, 1.5)
    s.circle(rx1, ry1, 5.0, RSS_C, PAGE_BG, 1.5)
    s.circle(rpx, rpy, 5.0, "#ffffff", LEAK_C, 2.0)
    s.tag(rx0 + 12, ry0 + 22, f"start {fmt_kb(st['rss_start'])}", RSS_C, 11)
    s.tag(rx1 - 11, ry1 + 22, f"end {fmt_kb(st['rss_end'])}", RSS_C, 11, "end")
    s.tag(rpx + 12, rpy + 22, f"RSS peak {fmt_kb(st['rss_max'])}", "#ffd7d7", 11)

    # 活堆说明 (贴着活堆曲线)
    s.tag(ax2.px0 + 20, ax2.sy(st["heap_end"]) - 12,
          f"live heap {fmt_kb(st['heap_end'])} KB = "
          f"{100.0 * st['heap_end'] / LEAK_LINE_KB:.1f}% of the leak line", HEAP_C, 12)

    # headroom 标注 (RSS 峰值 -> 100MB 线)
    bx_ = ax2.sx(hours_max * 0.72)
    s.line(bx_, rpy, bx_, ly, "#ff8f8f", 1.6, dash="4,4")
    s.line(bx_ - 6, rpy, bx_ + 6, rpy, "#ff8f8f", 1.6)
    s.line(bx_ - 6, ly, bx_ + 6, ly, "#ff8f8f", 1.6)
    s.tag(bx_ + 10, rpy + 34,
          f"headroom {fmt_int(LEAK_LINE_KB - st['rss_max'])} KB "
          f"({100.0 * (LEAK_LINE_KB - st['rss_max']) / LEAK_LINE_KB:.1f}%)", "#ff9d9d", 12)

    ax2.legend_v([("Process RSS (KB)", RSS_C, False),
                  ("Live heap (KB)", HEAP_C, False),
                  ("100 MB leak threshold", LEAK_C, True)],
                 ax2.px0 + 20, ax2.py0 + ax2.ph * 0.46)

    # ================= 底部结论 =================
    s.text(60, H - 78,
           f"Live heap: {fmt_kb(st['heap_start'])} -> {fmt_kb(st['heap_end'])}  "
           f"(net +{fmt_int(st['heap_net'])} KB, +{st['heap_slope_h']:.1f} KB/h)   |   "
           f"peak {fmt_kb(st['heap_max'])} = {100.0 * st['heap_max'] / LEAK_LINE_KB:.1f}% "
           f"of the 100 MB line   |   zero crossings of the 100 MB line",
           "#c9d1d9", 13, "start", "bold", FONT)
    s.text(60, H - 52,
           f"Process RSS: {fmt_kb(st['rss_start'])} (start) / "
           f"{fmt_kb(st['rss_max'])} (peak) / {fmt_kb(st['rss_end'])} (end) = "
           f"{st['rss_pct_of_limit']:.1f}% of the 100 MB line   |   "
           f"headroom {fmt_int(LEAK_LINE_KB - st['rss_max'])} KB   |   "
           f"no leak, flat heap over the whole {st['hours']:.2f} h / "
           f"{fmt_int(st['frame_last'])} frame run",
           TICK_FG, 12, "start", None, FONT)
    s.text(60, H - 26,
           "Notes: only rows with a non-empty pid column are plotted (post-exit watchdog rows "
           f"are filtered); the curve is broken at the {st['n_restarts']} program restart(s) "
           "instead of interpolating across them; RSS samples below 8 MB belong to the "
           "launcher shell and are excluded.",
           "#5c6572", 11, "start", None, FONT)
    return s.render()


# ---------------------------------------------------------------- 图 2: 温度曲线
def build_temp_svg(st, W=1800, H=1120):
    s = Svg(W, H)
    s.rect(0, 0, W, H, PAGE_BG)

    tmax = max(st["temps"].values())
    tmin = min(st["temps_min"].values())

    s.text(60, 42,
           f"Night Soak Thermal: All Sensors Peak {tmax:.1f} C, Far Below the "
           f"{THROTTLE_C:.0f} C Throttle Line", HEAD_FG, 24, "start", "bold", FONT)
    s.text(60, 68,
           f"RK3588 zero-copy NPU pipeline  |  {st['hours']:.2f} h / "
           f"{fmt_int(st['frame_last'])} frames  |  NPU / SoC / Big CPU / Little CPU  |  "
           f"30 s sampling  |  {len(st['alive'])} live samples  |  "
           f"no thermal throttling observed",
           TICK_FG, 13, "start", None, FONT)

    hours_max = st["hours"]
    xt = [float(i) for i in range(0, int(hours_max) + 1)]

    def clock_fn(v):
        return datetime.fromtimestamp(st["t0"] + v * 3600.0).strftime("%H:%M")

    # ================= 上半部分: 全量程 (含 85C 降频线) =================
    ymax = 90.0
    ax = Axes(s, 60, 92, W - 120, 430, (0.0, hours_max), (0.0, ymax),
              xt, ticks(0.0, ymax, 10.0), lambda v: f"{v:.0f}", lambda v: f"{v:.0f}",
              left=70, right=26, top=44, bottom=52)
    ax.draw(xlabel=None, clock_fn=clock_fn)
    ax.y_title("Temperature (C)")
    ax.heading(f"Full thermal scale - shaded band = {tmin - 0.5:.1f} - {tmax + 0.5:.1f} C, "
                f"the sensors never leave it",
                right="85 C typical throttle threshold (SoC / CPU DVFS cut)")

    # 85C 降频线 + 危险区
    ty = ax.sy(THROTTLE_C)
    s.rect(ax.px0, ax.py0, ax.pw, ty - ax.py0, "#ff5c5c", extra=' opacity="0.09"')
    s.line(ax.px0, ty, ax.px0 + ax.pw, ty, LEAK_C, 2.2, dash="9,6")

    # 传感器工作区间高亮 (标签写在标题里, 避免压住图例)
    s.rect(ax.px0, ax.sy(tmax + 0.5), ax.pw, ax.sy(tmin - 0.5) - ax.sy(tmax + 0.5),
           "#38bdf8", extra=' opacity="0.12"')
    s.line(ax.px0, ax.sy(tmax + 0.5), ax.px0 + ax.pw, ax.sy(tmax + 0.5), "#3a4757", 1, dash="4,5")
    s.line(ax.px0, ax.sy(tmin - 0.5), ax.px0 + ax.pw, ax.sy(tmin - 0.5), "#3a4757", 1, dash="4,5")

    # 峰值保持曲线
    for name, col, color in TEMP_SERIES:
        _raw, ph = st["temp_series"][name]
        pts = [(ax.sx((r["time"] - st["t0"]) / 3600.0), ax.sy(v))
               for r, v in zip(st["alive"], ph)]
        s.polyline(pts, color, 1.3)

    # headroom 标注
    bx_ = ax.sx(hours_max * 0.95)
    s.line(bx_, ax.sy(tmax), bx_, ty, "#ff8f8f", 1.6, dash="4,4")
    s.line(bx_ - 6, ax.sy(tmax), bx_ + 6, ax.sy(tmax), "#ff8f8f", 1.6)
    s.line(bx_ - 6, ty, bx_ + 6, ty, "#ff8f8f", 1.6)
    s.tag(bx_ - 10, (ax.sy(tmax) + ty) / 2.0 + 4,
          f"{THROTTLE_C - tmax:.1f} C headroom", "#ff9d9d", 13, "end")

    # 图例 + 说明 (放在 45-80 C 的空白带里)
    entries = [(f"{name}  (peak {st['temps'][name]:.1f} C)", color, False)
               for name, _, color in TEMP_SERIES]
    entries.append(("85 C throttle threshold", LEAK_C, True))
    ax.legend_v(entries, ax.px0 + 16, ax.py0 + 50, dy=23, size=12)

    s.text(ax.px0 + ax.pw * 0.40, ax.py0 + 78,
           f"All four sensors stayed between {tmin:.1f} C and {tmax:.1f} C for the entire "
           f"{st['hours']:.2f} h run.", "#c9d1d9", 13, "start", "bold", FONT)
    s.text(ax.px0 + ax.pw * 0.40, ax.py0 + 102,
           f"Peak {tmax:.1f} C leaves {THROTTLE_C - tmax:.1f} C of headroom to the 85 C "
           f"throttle line - no throttling.", "#c9d1d9", 13, "start", "bold", FONT)
    s.text(ax.px0 + ax.pw * 0.40, ax.py0 + 126,
           f"CPU frequency stayed at {fmt_int(st['cpu_freq_min'])}-"
           f"{fmt_int(st['cpu_freq_max'])} kHz, NPU utilisation "
           f"{fmt_int(st['npu_pct_min'])}-{fmt_int(st['npu_pct_max'])}%.",
           TICK_FG, 12, "start", None, FONT)

    # ================= 下半部分: 温度明细 (y 轴放大) =================
    zlo = math.floor((tmin - 0.5) * 2.0) / 2.0
    zhi = math.ceil((tmax + 0.5) * 2.0) / 2.0
    ax2 = Axes(s, 60, 548, W - 120, 340, (0.0, hours_max), (zlo, zhi),
               xt, ticks(zlo, zhi, 1.0), lambda v: f"{v:.0f}",
               lambda v: f"{v:.1f}", left=70, right=26, top=44, bottom=64)
    ax2.draw(xlabel="Runtime (hours) since program start   |   wall clock below",
             clock_fn=clock_fn)
    ax2.y_title("Temperature (C)")
    ax2.heading("Sensor detail (y axis zoomed) - 7.5 min peak-hold of the 30 s samples",
                right="peak:  NPU 37.9 C  |  SoC 39.8 C  |  Big CPU 40.7 C  |  "
                      "Little CPU 40.7 C")

    for name, col, color in TEMP_SERIES:
        _raw, ph = st["temp_series"][name]
        pts = [(ax2.sx((r["time"] - st["t0"]) / 3600.0), ax2.sy(v))
               for r, v in zip(st["alive"], ph)]
        s.polyline(pts, color, 1.8)

    # 每个传感器的最高温: 圆点 + 短标注 (四个峰值都集中在最前面, 用短标签避免互相压住)
    peak_xy = []
    for name, color, bi, bv in st["temp_peaks"]:
        mx = ax2.sx((st["alive"][bi]["time"] - st["t0"]) / 3600.0)
        my = ax2.sy(bv)
        peak_xy.append((name, color, mx, my, bv))
        s.circle(mx, my, 4.5, "#ffffff", color, 2.2)

    # 标注错开摆放, 每个标签带底色, 保证压在线上也看得清
    # (NPU 峰值在 x=0; SoC / Big CPU / Little CPU 的峰值落在同一个 x 上)
    label_spec = [(peak_xy[0], 14, -14)]  # NPU -> 右上
    if len(peak_xy) > 1:
        label_spec.append((peak_xy[1], 16, 4))  # SoC -> 右侧同高
    if len(peak_xy) > 2:
        b_same = (len(peak_xy) > 3 and peak_xy[2][2] == peak_xy[3][2]
                  and peak_xy[2][4] == peak_xy[3][4])
        if b_same:
            label_spec.append((peak_xy[2], 16, 4))  # Big CPU + Little CPU 重合
        else:
            label_spec.append((peak_xy[2], 16, -16))
    if len(peak_xy) > 3 and not (len(peak_xy) > 2
                                  and peak_xy[2][2] == peak_xy[3][2]
                                  and peak_xy[2][4] == peak_xy[3][4]):
        label_spec.append((peak_xy[3], 16, 4))
    for (name, color, mx, my, bv), dx, dy in label_spec:
        s.tag(mx + dx, my + dy, f"{bv:.1f}", color, 12)

    # 图例 (放在 NPU 平线以下的空白带里)
    ax2.legend_h([(f"{name}  (peak {st['temps'][name]:.1f} C)", color, False)
                  for name, _, color in TEMP_SERIES],
                 ax2.px0 + 16, ax2.py0 + ax2.ph - 8, gap=26, size=12)

    # 图例 (底部空白带)
    ax2.legend_h([(f"{name}  (peak {st['temps'][name]:.1f} C)", color, False)
                  for name, _, color in TEMP_SERIES],
                 ax2.px0 + 16, ax2.py0 + ax2.ph - 10, gap=26, size=12)

    # ================= 底部结论 =================
    s.text(60, H - 78,
           "Peak temperatures over "
           f"{st['hours']:.2f} h:  "
           + "   ".join(f"{n} {st['temps'][n]:.1f} C" for n, _, _ in TEMP_SERIES)
           + f"   |   highest {tmax:.1f} C = {THROTTLE_C - tmax:.1f} C below the "
             f"{THROTTLE_C:.0f} C throttle threshold",
           "#c9d1d9", 13, "start", "bold", FONT)
    s.text(60, H - 52,
           f"CPU frequency stayed between {fmt_int(st['cpu_freq_min'])} and "
           f"{fmt_int(st['cpu_freq_max'])} kHz (no DVFS cut)   |   "
           f"NPU utilisation {fmt_int(st['npu_pct_min'])}-{fmt_int(st['npu_pct_max'])}%   |   "
           f"program restarts observed: {fmt_int(st['restarts'])}",
           TICK_FG, 12, "start", None, FONT)
    s.text(60, H - 26,
           "Notes: sensor readings are 0.9 C quantised, so the raw 30 s samples are drawn as a "
           "7.5 min peak-hold (rolling max) curve; the peak-hold preserves each sensor's true "
           "maximum, which is what the annotations report.",
           "#5c6572", 11, "start", None, FONT)
    return s.render()


# ---------------------------------------------------------------- 渲染
def find_chromium():
    for c in ("chromium", "chromium-browser", "google-chrome", "google-chrome-stable"):
        p = shutil.which(c)
        if p:
            return p
    return None


def render_png(svg_text, out_png, W, H, tmpdir):
    chrome = find_chromium()
    if chrome is None:
        raise SystemExit("chromium not found in PATH; cannot render PNG")
    svg_path = os.path.join(tmpdir, "chart.svg")
    html_path = os.path.join(tmpdir, "chart.html")
    with open(svg_path, "w") as f:
        f.write(svg_text)
    html = (
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        f"<style>html,body{{margin:0;padding:0;background:{PAGE_BG};"
        f"width:{W}px;height:{H}px;overflow:hidden}}"
        "svg{display:block}</style></head><body>" + svg_text + "</body></html>"
    )
    with open(html_path, "w") as f:
        f.write(html)
    cmd = [
        chrome, "--headless", "--disable-gpu", "--no-sandbox", "--hide-scrollbars",
        "--force-device-scale-factor=1", "--virtual-time-budget=4000",
        f"--window-size={W},{H}", f"--screenshot={out_png}", html_path,
    ]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if not os.path.exists(out_png) or os.path.getsize(out_png) == 0:
        sys.stderr.write(proc.stderr.decode("utf-8", "replace"))
        raise SystemExit(f"chromium failed to render {out_png}")
    return out_png


# ---------------------------------------------------------------- main
def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    csv_path = sys.argv[1]
    out_dir = sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)

    rows = load_rows(csv_path)
    st = compute_stats(rows)

    W, H = 1800, 1120
    tmpdir = tempfile.mkdtemp(prefix="night_soak_charts_")
    try:
        heap_png = os.path.join(out_dir, "night_soak_heap_flat.png")
        temp_png = os.path.join(out_dir, "night_soak_temp_curve.png")
        render_png(build_heap_svg(st, W, H), heap_png, W, H, tmpdir)
        render_png(build_temp_svg(st, W, H), temp_png, W, H, tmpdir)
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    # ---------------- 复现统计 ----------------
    p = print
    p("=" * 78)
    p("NIGHT SOAK METRICS -- reproduced from %s" % csv_path)
    p("=" * 78)
    p("rows in csv                 : %d" % st["rows_total"])
    p("live samples (pid non-empty): %d" % len(st["alive"]))
    p("filtered empty rows (pid==''): %d  (post-exit watchdog samples, dropped)"
      % len(st["empty"]))
    p("dropped implausible rss     : %d  (launcher shell, rss < %d KB)"
      % (st["n_dropped_rss"], MIN_PLAUSIBLE_RSS_KB))
    p("live segment start           : %s  (unix %d)"
      % (datetime.fromtimestamp(st["t0"]).strftime("%Y-%m-%d %H:%M:%S"), st["t0"]))
    p("live segment end             : %s  (unix %d)"
      % (datetime.fromtimestamp(st["t1"]).strftime("%Y-%m-%d %H:%M:%S"), st["t1"]))
    p("duration                     : %d s = %.2f h" % (st["dur"], st["hours"]))
    p("frames                       : %s -> %s"
      % (fmt_int(st["frame_first"]), fmt_int(st["frame_last"])))
    p("-" * 78)
    p("live heap start / end        : %s / %s"
      % (fmt_int(st["heap_start"]), fmt_int(st["heap_end"])))
    p("live heap peak / min         : %s / %s"
      % (fmt_int(st["heap_max"]), fmt_int(st["heap_min"])))
    p("live heap net growth         : +%s KB  (+%.1f KB/h)"
      % (fmt_int(st["heap_net"]), st["heap_slope_h"]))
    p("RSS start / peak / end       : %s / %s / %s"
      % (fmt_int(st["rss_start"]), fmt_int(st["rss_max"]), fmt_int(st["rss_end"])))
    p("RSS peak vs 100MB line       : %.1f%%  (headroom %s KB)"
      % (st["rss_pct_of_limit"], fmt_int(LEAK_LINE_KB - st["rss_max"])))
    p("crossings of 100MB line      : heap=%s  rss=%s"
      % (st["heap_ge_leak"], st["rss_ge_leak"]))
    p("-" * 78)
    for name, _, _ in TEMP_SERIES:
        p("max temperature %-12s : %.1f C" % (name + ":", st["temps"][name]))
    p("headroom to 85C throttle     : %.1f C"
      % (THROTTLE_C - max(st["temps"].values())))
    p("cpu_freq_khz min / max       : %s / %s"
      % (fmt_int(st["cpu_freq_min"]), fmt_int(st["cpu_freq_max"])))
    p("npu_pct min / max            : %s / %s"
      % (fmt_int(st["npu_pct_min"]), fmt_int(st["npu_pct_max"])))
    p("restarts observed            : %s" % st["restarts"])
    p("-" * 78)
    for path in (heap_png, temp_png):
        p("wrote %s  (%d bytes)" % (path, os.path.getsize(path)))
    p("=" * 78)
    return 0


if __name__ == "__main__":
    sys.exit(main())
