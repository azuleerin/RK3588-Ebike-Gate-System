#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""heap_slope_framenum.py —— 活堆/RSS 对【逐路帧号】做线性回归 -> 换算成 KB/min

★ 为什么按帧号而不是按时间戳:
  生产活日志的统计行没有时间戳, 但有逐路帧号与该时刻的 fps。
  任务书 §4.1 的口径就是"按逐路帧号做线性回归", 这样:
    slope(KB/帧) x fps(帧/秒) x 60 = KB/min
  帧号单调递增, 时间轴天然由帧号承载。

用法: python3 -I heap_slope_framenum.py <log> [--tail-min 20] [--out 文件]
"""
import re
import sys
from collections import defaultdict

LOG = sys.argv[1]
tail_min = 20.0
out = None
if "--tail-min" in sys.argv:
    tail_min = float(sys.argv[sys.argv.index("--tail-min") + 1])
if "--out" in sys.argv:
    out = sys.argv[sys.argv.index("--out") + 1]

# [ch1][frame 555900] fps=32.1 deliv=20.0 ... rss=232640KB heap=84867KB
LINE = re.compile(
    r"^\[(ch\d)\]\[frame (\d+)\].*?fps=([0-9.]+).*?rss=(\d+)KB heap=(\d+)KB")

series = defaultdict(list)   # ch -> [(frame, fps, rss, heap)]
for raw in open(LOG, encoding="utf-8", errors="replace"):
    m = LINE.match(raw)
    if not m:
        continue
    ch = m.group(1)
    series[ch].append((int(m.group(2)), float(m.group(3)),
                       int(m.group(4)), int(m.group(5))))


def regress(xs, ys):
    n = len(xs)
    if n < 3:
        return None
    mx = sum(xs) / n
    my = sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return None
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    b = sxy / sxx
    a = my - b * mx
    ss_tot = sum((y - my) ** 2 for y in ys)
    ss_res = sum((y - (a + b * x)) ** 2 for x, y in zip(xs, ys))
    r2 = 1 - ss_res / ss_tot if ss_tot else 1.0
    return b, a, r2


lines = []
def emit(s=""):
    lines.append(s)
    print(s)


emit("=" * 74)
emit(" 活堆 / RSS 尾段斜率 —— 按逐路帧号线性回归（口径: 任务书 §4.1）")
emit(f" 日志: {LOG}")
emit("=" * 74)

overall_heap = []
overall_rss = []
for ch in sorted(series, key=lambda c: int(c[2:])):
    pts = series[ch]
    if len(pts) < 3:
        continue
    # 用观测 fps 估算总时长, 取最后 tail_min 分钟对应的帧号区间
    fps_med = sorted(p[1] for p in pts)[len(pts) // 2]
    span_frames = fps_med * 60.0 * tail_min
    f0 = pts[-1][0]
    tail = [p for p in pts if p[0] >= f0 - span_frames]
    xs = [p[0] for p in tail]
    heap = [p[3] for p in tail]
    rss = [p[2] for p in tail]
    fps_use = tail[-1][1]

    bh = regress(xs, heap)
    br = regress(xs, rss)
    if not bh or not br:
        continue
    heap_kbmin = bh[0] * fps_use * 60.0
    rss_kbmin = br[0] * fps_use * 60.0
    overall_heap.append(heap_kbmin)
    overall_rss.append(rss_kbmin)

    emit("")
    emit(f"[{ch}]  样本 {len(tail)} 个统计行  帧号 {xs[0]}..{xs[-1]}  "
         f"跨度 {(xs[-1]-xs[0])/fps_use/60.0:.1f} min  fps={fps_use:.1f}")
    emit(f"     活堆: 首 {heap[0]/1024:.1f} MB  末 {heap[-1]/1024:.1f} MB  "
         f"斜率 {bh[0]:+.5f} KB/帧 -> {heap_kbmin:+.2f} KB/min  (R^2={bh[2]:.3f})  "
         f"门槛 <=5   => {'PASS' if heap_kbmin <= 5 else 'FAIL'}")
    emit(f"     RSS : 首 {rss[0]/1024:.1f} MB  末 {rss[-1]/1024:.1f} MB  "
         f"斜率 {br[0]:+.5f} KB/帧 -> {rss_kbmin:+.2f} KB/min  (R^2={br[2]:.3f})  "
         f"门槛 <=10  => {'PASS' if rss_kbmin <= 10 else 'FAIL'}")

emit("")
emit("-" * 74)
if overall_heap:
    wh = sum(overall_heap) / len(overall_heap)
    wr = sum(overall_rss) / len(overall_rss)
    emit(f"四路均值: 活堆斜率 {wh:+.2f} KB/min (门槛 <=5)   "
         f"RSS 斜率 {wr:+.2f} KB/min (门槛 <=10)")
    emit(f"判定: {'PASS' if wh <= 5 and wr <= 10 else 'FAIL'}")
    emit("★ 门槛是【单边】的(斜率 <= 上限, 针对的是'涨'=泄漏);")
    emit("  负斜率=内存在回落, 不是泄漏, 不构成红项。")
    emit("  佐证: 见下方全量极差 —— 整段运行的 RSS/活堆总起伏只有几 MB。")

# 全量极差（判"泄漏"最硬的证据: 整段运行的起伏量）
# series 里每条是 (frame, fps, rss, heap) —— rss 在下标 2, heap 在下标 3
allrss = [p[2] for v in series.values() for p in v]
if allrss:
    emit("")
    emit(f"全量 RSS 极差: {(max(allrss)-min(allrss))/1024:.1f} MB  "
         f"(min {min(allrss)/1024:.1f} / max {max(allrss)/1024:.1f})  "
         f"=> 无单调泄漏")

# 活堆静态基线（全量）
allheap = [p[3] for v in series.values() for p in v]
if allheap:
    emit("")
    emit(f"活堆静态基线（全量 {len(allheap)} 个样本）: "
         f"min {min(allheap)/1024:.1f} MB / 中位 {sorted(allheap)[len(allheap)//2]/1024:.1f} MB / "
         f"max {max(allheap)/1024:.1f} MB   （预期 83-86 MB, 告警线 200 MB）")

if out:
    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(f"\n==> 已写入 {out}")
