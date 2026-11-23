#!/usr/bin/env python3
"""活堆(heap=uordblks)斜率分析 —— 判断"真泄漏"的唯一指标。

为什么看 heap 而不是 RSS:
    RSS 会被 glibc 的 arena 保留/碎片干扰(锯齿), malloc_trim 之后还会回落,
    看 RSS 会把"碎片"误判成"泄漏"。
    mallinfo2().uordblks = 进程【正在使用】的堆字节数, 只增不减才是真泄漏。

用法:
    python3 scripts/heap_slope.py <app.log> [<app.log> ...]
    python3 scripts/heap_slope.py --csv <out.csv> <app.log>

输出:
    每个日志一行: 帧数区间、活堆起止、斜率(KB/帧 / KB/h / MB/24h)、判定
    + 【逐路分解】: 见下面的"为什么要逐路"。

★★ 为什么要逐路分解(2026-11-23 实测踩出来的):
    活堆 mallinfo2().uordblks 是【进程级】的 —— 4 路通道打印的是同一个值,
    但每路的【帧号】是各自独立的。把 4 路混成一个序列再做回归, x 轴
    (帧号)就不是单调的(ch1 跑到 135 万时 ch2 才 142 万, 两路的帧号还
    会互相穿插), 这个回归的斜率是【有偏的】。
    实测对照(2026-11-23):
        旧二进制 19 小时:  混合口径 +0.3022 KB/帧, 逐路口径 +0.30 KB/帧  (接近)
        新二进制:          混合口径 +0.0272 KB/帧, 逐路【净变化】-40 ~ +92 KB
    ⇒ 逐路口径才是判据, 混合口径只能当参考。
    所以本脚本【两个都出】: 原来的汇总行保持不变(不破坏既有口径),
    下面再补一段逐路分解, 以逐路的为准。
"""
from __future__ import annotations

import re
import sys

# [frame 60] fps=19.6 src=25.0 gap=40.0ms wait=... heap=20633KB trim=0 ...
RE_FRAME = re.compile(r"\[frame (\d+)\]")
RE_HEAP = re.compile(r"\bheap=(\d+)KB")
RE_FPS = re.compile(r"\bfps=([\d.]+)")
# rk_gate 的统计行长这样: [ch1][frame 1331600] fps=... heap=495757KB
RE_CH = re.compile(r"\[ch(\d+)\]")
# deliv = 【实际交付的帧率】—— 帧号是按它递增的, 折算 KB/min 必须用它,
# 不能用 fps=(源帧率): max_fps=20 时源是 60fps 但帧号只按 20/s 涨。
RE_DELIV = re.compile(r"\bdeliv=([\d.]+)")


def parse(path: str):
    """返回 [(frame, heap_kb, fps, ch, deliv), ...]

    支持两种输入:
      1) 主程序日志(带 [frame N] ... heap=NNNKB 行)
      2) 看护进程的采样 CSV(time,frame,rss_kb,heap_kb,...)
    CSV 里没有通道列, ch 一律记 0(表示"不区分通道")。
    """
    out = []
    if path.endswith(".csv"):
        import csv as _csv
        with open(path) as f:
            for r in _csv.DictReader(f):
                try:
                    fr = int(float(r.get("frame", -1)))
                    hp = int(float(r.get("heap_kb", 0)))
                    fp = float(r.get("fps", 0) or 0)
                except (TypeError, ValueError):
                    continue
                if fr >= 0 and hp > 0:
                    out.append((fr, hp, fp, 0, fp))
        return out
    with open(path, errors="replace") as f:
        for line in f:
            if "[frame " not in line:
                continue
            mf = RE_FRAME.search(line)
            mh = RE_HEAP.search(line)
            if not mf or not mh:
                continue
            fps = 0.0
            mfps = RE_FPS.search(line)
            if mfps:
                try:
                    fps = float(mfps.group(1))
                except ValueError:
                    fps = 0.0
            mch = RE_CH.search(line)
            ch = int(mch.group(1)) if mch else 0
            deliv = 0.0
            mdl = RE_DELIV.search(line)
            if mdl:
                try:
                    deliv = float(mdl.group(1))
                except ValueError:
                    deliv = 0.0
            out.append((int(mf.group(1)), int(mh.group(1)), fps, ch, deliv))
    return out


def slope_kb_per_frame(pts):
    """最小二乘斜率 (KB/帧)。"""
    n = len(pts)
    if n < 2:
        return 0.0
    sx = sum(p[0] for p in pts)
    sy = sum(p[1] for p in pts)
    sxx = sum(p[0] * p[0] for p in pts)
    sxy = sum(p[0] * p[1] for p in pts)
    den = n * sxx - sx * sx
    if den == 0:
        return 0.0
    return (n * sxy - sx * sy) / den


def per_channel(pts):
    """按通道拆分, 每路用自己的帧号做回归。

    返回 [(ch, n, f0, f1, h0, h1, slope, slope_tail, fps), ...], 按 ch 排序。
    """
    by = {}
    for p in pts:
        by.setdefault(p[3], []).append(p)
    out = []
    for ch, ps in sorted(by.items()):
        ps = sorted(ps, key=lambda x: x[0])
        # 同一帧号可能重复出现(日志重放), 只保留最后一次
        ded = {}
        for p in ps:
            ded[p[0]] = p
        ps = [ded[k] for k in sorted(ded)]
        if len(ps) < 3:
            continue
        f0, h0 = ps[0][0], ps[0][1]
        f1, h1 = ps[-1][0], ps[-1][1]
        k = slope_kb_per_frame(ps)
        k_tail = slope_kb_per_frame(ps[len(ps) // 2:])
        # 折算 KB/min 用 deliv(帧号实际递增的速率), 没有就退回 fps
        fps = ps[-1][4] or ps[-1][2] or 20.0
        out.append((ch, len(ps), f0, f1, h0, h1, k, k_tail, fps))
    return out


def report(path: str):
    pts = parse(path)
    if len(pts) < 2:
        print(f"{path}: 没有足够的 [frame] 采样点 (共 {len(pts)})")
        return None
    f0, h0 = pts[0][0], pts[0][1]
    f1, h1 = pts[-1][0], pts[-1][1]
    df = f1 - f0
    k = slope_kb_per_frame(pts)
    dh = h1 - h0
    fps = pts[-1][4] or pts[-1][2] or 20.0
    kb_per_s = k * fps
    mb24 = kb_per_s * 86400 / 1024.0

    # ---- 尾段斜率(后一半): 区分"真泄漏(线性)"和"高水位一次性分配(阶跃后打平)" ----
    half = pts[len(pts) // 2:]
    k_tail = slope_kb_per_frame(half)
    # 最大单次阶跃(KB)
    steps = [pts[i][1] - pts[i - 1][1] for i in range(1, len(pts))]
    max_step = max(steps) if steps else 0

    # 判定: 以尾段为准 —— 尾段是线性的才算泄漏; 阶跃后打平 = 高水位分配, 不是泄漏
    if abs(k_tail) < 0.01:
        verdict = "PASS 活堆打平(尾段线性度≈0)"
    elif k_tail > 0:
        verdict = f"FAIL 泄漏 +{k_tail:.4f} KB/帧 (~{k_tail * fps * 86400 / 1024:.1f} MB/24h)"
    else:
        verdict = "PASS 活堆不增反降"

    print(f"{path}")
    print(f"  帧区间   : {f0} -> {f1}  ({len(pts)} 个采样点)")
    print(f"  活堆     : {h0} -> {h1} KB   (净增 {dh} KB)")
    print(f"  全程斜率 : {k:+.4f} KB/帧 | {kb_per_s:+.2f} KB/s | {mb24:+.1f} MB/24h")
    print(f"  尾段斜率 : {k_tail:+.4f} KB/帧  (后 {len(half)} 个点)  最大阶跃 {max_step:+d} KB")
    print(f"  判定     : {verdict}")

    # ---- 逐路分解(判据以这一段为准) ----
    ch_rows = per_channel(pts)
    if len(ch_rows) > 1:
        print(f"  ---- 逐路分解(判据以这一段为准; 活堆是进程级的, 4 路打印的是同一个值) ----")
        worst = None
        for (ch, n, cf0, cf1, ch0, ch1, ck, ckt, cfps) in ch_rows:
            d = ch1 - ch0
            print(f"    ch{ch}: n={n:<5} 帧 {cf0} -> {cf1}  活堆 {ch0} -> {ch1} KB (净 {d:+d})"
                  f"  斜率 {ck:+.5f} KB/帧 -> {ck * cfps * 60:+.1f} KB/min")
            if worst is None or ck > worst[6]:
                worst = (ch, n, cf0, cf1, ch0, ch1, ck, ckt, cfps)
        if worst:
            ch, n, cf0, cf1, ch0, ch1, ck, ckt, cfps = worst
            net = ch1 - ch0
            if abs(ck) < 0.01 and abs(net) < 4096:
                print(f"    逐路判定: PASS 各路净变化都在 ±4MB 内且斜率≈0 -> 无可测的持续增长"
                      f" (最大的一路 ch{ch}: {ck:+.5f} KB/帧, 净 {net:+d} KB)")
            else:
                print(f"    逐路判定: FAIL 最大的一路 ch{ch}: {ck:+.5f} KB/帧"
                      f" ({ck * cfps * 60:+.1f} KB/min, 净 {net:+d} KB)")

    return dict(path=path, f0=f0, f1=f1, h0=h0, h1=h1, slope=k, slope_tail=k_tail,
                fps=fps, mb24=mb24, verdict=verdict, pts=pts)


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    csv_out = None
    if args[0] == "--csv":
        csv_out = args[1]
        args = args[2:]
    results = []
    for p in args:
        r = report(p)
        if r:
            results.append(r)
    if csv_out and results:
        import csv as _csv
        with open(csv_out, "w", newline="") as f:
            w = _csv.writer(f)
            w.writerow(["log", "frame0", "frame1", "heap0_kb", "heap1_kb",
                        "slope_kb_per_frame", "fps", "mb_per_24h", "verdict"])
            for r in results:
                w.writerow([r["path"], r["f0"], r["f1"], r["h0"], r["h1"],
                            f"{r['slope']:.6f}", r["fps"], f"{r['mb24']:.2f}",
                            r["verdict"]])
        print(f"\n对比表已写入: {csv_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
