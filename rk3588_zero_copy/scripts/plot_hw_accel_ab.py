#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""plot_hw_accel_ab.py —— 从【压测实例自己的日志】画硬件加速前/后对比折线图

为什么不用网页的 /api/state:
    板子上可能有别的会话在同时操作同一个设备, 网页历史里两个实例的报文会混在一起。
    而压测实例自己的日志里, "[控制] 收到指令" 和逐帧的 [frame N] 统计是
    【同一条时间线】, 可以精确切出"MPP 开"和"MPP 关"两段, 一个点都不掺水。

用法:
    python3 scripts/plot_hw_accel_ab.py <压测实例日志> [输出png]
"""
from __future__ import annotations

import re
import sys

import cv2
import numpy as np

BG = (24, 24, 24)
FG = (219, 219, 219)
DIM = (150, 150, 150)
COL = {
    "fps": (219, 137, 56), "total": (34, 160, 245), "cpu": (255, 166, 88),
    "c0": (110, 240, 130), "c1": (200, 220, 120), "c2": (120, 170, 250),
    "dec": (200, 120, 230), "rga": (240, 150, 120), "npu": (110, 120, 245),
}
CMD_RE = re.compile(r"\[控制\] 收到指令: mpp=(\w+) rga=(\w+) npu=(\w+)")
FR_RE = re.compile(
    r"\[frame (\d+)\] fps=([\d.]+) deliv=([\d.]+).*?dec=([\d.]+)ms rga=([\d.]+)ms "
    r"npu=([\d.]+)ms.*?total=([\d.]+)ms")


def parse(path):
    """返回 [(mpp_state, frame, fps, dec, rga, npu, total)]"""
    out = []
    mpp = None
    for raw in open(path, encoding="utf-8", errors="replace"):
        mc = CMD_RE.search(raw)
        if mc:
            mpp = (mc.group(1) == "on")
            continue
        mf = FR_RE.search(raw)
        if mf:
            g = mf.groups()
            out.append((mpp, int(g[0]), float(g[1]), float(g[3]),
                        float(g[4]), float(g[5]), float(g[6])))
    return out


def txt(img, s, org, scale=0.5, color=FG, thick=1):
    cv2.putText(img, s, org, cv2.FONT_HERSHEY_SIMPLEX, scale, color, thick, cv2.LINE_AA)


def panel(w, h, title, ylab, ylab2=""):
    img = np.full((h, w, 3), BG, np.uint8)
    txt(img, title, (16, 30), 0.72, FG, 2)
    txt(img, ylab, (16, 58), 0.5, DIM)
    if ylab2:
        txt(img, ylab2, (w - 150, 58), 0.5, DIM)
    return img


def grid(img, x0, x1, y0, y1, n=5):
    for i in range(n + 1):
        y = int(y0 + (y1 - y0) * i / n)
        cv2.line(img, (x0, y), (x1, y), (52, 52, 52), 1, cv2.LINE_AA)


def series(img, segs, x0, x1, y0, y1, vmax, unit=""):
    """segs: [(values, color, label)]"""
    n = max(len(v) for v, _, _ in segs)
    for vals, color, label in segs:
        pts = []
        for i, v in enumerate(vals):
            if v is None:
                continue
            xx = x0 + (x1 - x0) * (i / max(n - 1, 1))
            f = min(max(v / max(vmax, 1e-9), 0.0), 1.0)
            pts.append((int(xx), int(y1 - (y1 - y0) * f)))
        if len(pts) > 1:
            cv2.polylines(img, [np.array(pts, np.int32)], False, color, 2, cv2.LINE_AA)
    lx = x0 + 14
    for vals, color, label in segs:
        if not vals:
            continue
        last = next((v for v in reversed(vals) if v is not None), None)
        cv2.line(img, (lx, y0 - 12), (lx + 26, y0 - 12), color, 3, cv2.LINE_AA)
        txt(img, f"{label} {last:.1f}{unit}" if last is not None else label,
            (lx + 32, y0 - 8), 0.5, DIM)
        lx += 200


def main():
    log = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else \
        "/home/seeed/rk_deploy/docs/assets/hw_accel_ab.png"
    rows = parse(log)
    if not rows:
        print("没解析到数据", file=sys.stderr)
        return 1

    on = [r for r in rows if r[0] is True]
    off = [r for r in rows if r[0] is False]
    if not on or not off:
        print(f"硬解段 {len(on)} 点, 软解段 {len(off)} 点 (需要两段都有)", file=sys.stderr)
        return 1
    # ★ 取【最长连续段】, 不要"最后 50 个点":
    #   板子上可能有别的会话在同时切换这个设备, "最后 50 个"可能跨了切换点,
    #   把开/关两段混在一起平均, 数字就不可信了。
    def longest(seq):
        best, cur = [], []
        for r in seq:
            if cur and cur[-1][0] == r[0]:
                cur.append(r)
            else:
                if len(cur) > len(best):
                    best = cur
                cur = [r]
        if len(cur) > len(best):
            best = cur
        return best
    on = longest(on)[-60:]
    off = longest(off)[-60:]
    print(f"硬解段 {len(on)} 点, 软解段 {len(off)} 点(取最长连续段)")

    def col(seg, i):
        return [r[i] for r in seg]

    W = 1500
    x0, x1 = 90, W - 70
    xm = (x0 + x1) // 2

    # ---- ① FPS ----
    p1 = panel(W, 400, "① 流水线 FPS:  MPP 硬解(左)  vs  FFmpeg 软解(右)",
               "FPS", "fps")
    grid(p1, x0, x1, 90, 360)
    for seg, colr, name in ((on, COL["fps"], "MPP 硬解"),
                            (off, COL["total"], "FFmpeg 软解")):
        fps = col(seg, 2)
        pts = []
        base = x0 if seg is on else xm
        w = (xm - x0) if seg is on else (x1 - xm)
        for i, v in enumerate(fps):
            xx = base + w * (i / max(len(fps) - 1, 1))
            f = min(max(v / 120.0, 0), 1)
            pts.append((int(xx), int(360 - 270 * f)))
        cv2.polylines(p1, [np.array(pts, np.int32)], False, colr, 3, cv2.LINE_AA)
        txt(p1, f"{name}: 平均 {sum(fps)/len(fps):.1f} fps",
            (base + 10, 118), 0.55, colr)
    cv2.line(p1, (xm, 90), (xm, 360), (90, 90, 90), 1, cv2.LINE_AA)
    txt(p1, "← 在这里关掉 MPP →", (xm + 8, 348), 0.5, (110, 110, 110))
    for i in range(6):
        y = int(360 - 270 * i / 5)
        txt(p1, f"{120*i/5:.0f}", (x0 - 42, y + 5), 0.45, DIM)

    # ---- ② 各阶段耗时 ----
    p2 = panel(W, 400, "② 各阶段耗时(ms):  硬解解码 0.2ms  ->  软解解码 6.5ms, 预处理被迫走 CPU",
               "ms", "")
    grid(p2, x0, x1, 90, 360)
    txt(p2, "← MPP 硬解 →", (xm - 90, 118), 0.55, COL["fps"])
    txt(p2, "← FFmpeg 软解 →", (xm + 10, 118), 0.55, COL["total"])
    series(p2, [(col(on, 3), COL["dec"], "硬解-解码"),
                 (col(off, 3), COL["dec"], "软解-解码"),
                 (col(off, 4), COL["rga"], "软解-预处理"),
                 (col(on, 5), COL["npu"], "硬解-推理")],
           x0, x1, 90, 360, 35, " ms")

    # ---- ③ 单帧总耗时 ----
    p3 = panel(W, 360, "③ 单帧总耗时(ms)  与  折合 FPS", "ms", "")
    grid(p3, x0, x1, 90, 320)
    series(p3, [(col(on, 6), COL["fps"], "硬解-总耗时"),
                (col(off, 6), COL["total"], "软解-总耗时")],
           x0, x1, 90, 320, 40, " ms")

    out = np.vstack([p1, p2, p3])
    cv2.imwrite(out_path, out, [cv2.IMWRITE_PNG_COMPRESSION, 3])

    fa = sum(col(on, 2)) / len(on)
    fb = sum(col(off, 2)) / len(off)
    ta = sum(col(on, 6)) / len(on)
    tb = sum(col(off, 6)) / len(off)
    da = sum(col(on, 3)) / len(on)
    db = sum(col(off, 3)) / len(off)
    print(f"已保存: {out_path}")
    print(f"硬解: FPS {fa:.1f}  总耗时 {ta:.2f}ms  解码 {da:.2f}ms")
    print(f"软解: FPS {fb:.1f}  总耗时 {tb:.2f}ms  解码 {db:.2f}ms")
    print(f"⇒ 关掉 MPP: FPS {(fb/fa-1)*100:+.1f}%   解码耗时 {db/max(da,0.01):.1f}×")
    return 0


if __name__ == "__main__":
    sys.exit(main())
