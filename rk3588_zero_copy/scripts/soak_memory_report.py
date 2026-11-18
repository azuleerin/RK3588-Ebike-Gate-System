#!/usr/bin/env python3
# ===========================================================================
# soak_memory_report.py —— 长跑(烤机)的内存报告
#
# ★ 为什么不用"首尾相减"看活堆
#   4 路视频源会【循环播放】, 每循环一次 MPP 解码器就重新申请/释放一批帧缓冲,
#   活堆因此呈【锯齿】: 涨 25MB -> 掉回来 -> 再涨。
#   首尾相减会把"锯齿的相位"当成增长(实测能算出 +2.1KB/帧 的假泄漏)。
#   正确做法: 取【锯齿的谷底】(局部极小)作为基线, 再看基线的趋势。
#   谷底序列本身在"每个循环一次台阶"的尺度上也是台阶 —— 那才是真问题。
#
# 用法:
#   python3 scripts/soak_memory_report.py <logs/xxx> [--out report.md]
#   (目录里要有 gate.log 和 load.csv)
# ===========================================================================
import argparse
import json
import os
import re
import sys

RE_STATS = re.compile(
    r"\[(ch\d)\]\[frame\s+(\d+)\].*?fps=([\d.]+)\s+deliv=([\d.]+).*?"
    r"detwait=([\d.]+)ms\s+ocrwait=([\d.]+)ms.*?"
    r"objs=(\d+)\s+tracks=(\d+).*?rss=(\d+)KB\s+heap=(\d+)KB")


def parse_stats(path):
    """-> {ch: [(frame, fps, deliv, detwait, ocrwait, objs, tracks, rss_kb, heap_kb), ...]}"""
    per = {}
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            m = RE_STATS.search(line)
            if not m:
                continue
            ch = m.group(1)
            per.setdefault(ch, []).append((
                int(m.group(2)), float(m.group(3)), float(m.group(4)),
                float(m.group(5)), float(m.group(6)), int(m.group(7)),
                int(m.group(8)), int(m.group(9)), int(m.group(10))))
    for v in per.values():
        v.sort(key=lambda x: x[0])
    return per


def minima(series):
    """锯齿的谷底(局部极小)"""
    out = []
    for i in range(1, len(series) - 1):
        if series[i][8] <= series[i - 1][8] and series[i][8] <= series[i + 1][8]:
            out.append(series[i])
    return out


def linfit(xs, ys):
    """最小二乘斜率 + 截距"""
    n = len(xs)
    if n < 2:
        return 0.0, ys[0] if ys else 0.0
    mx = sum(xs) / n
    my = sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    if sxx <= 0:
        return 0.0, my
    k = sxy / sxx
    return k, my - k * mx


def parse_load(path):
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            p = line.strip().split(",")
            if len(p) < 12:
                continue
            try:
                rows.append([float(x) for x in p])
            except ValueError:
                continue
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir", help="长跑输出目录(含 gate.log / load.csv)")
    ap.add_argument("--out", default="", help="输出 Markdown 路径(默认打到 stdout)")
    args = ap.parse_args()

    gate_log = os.path.join(args.dir, "gate.log")
    load_csv = os.path.join(args.dir, "load.csv")
    if not os.path.exists(gate_log):
        print("找不到 gate.log:", gate_log, file=sys.stderr)
        return 1

    per = parse_stats(gate_log)
    if not per:
        print("gate.log 里没有带 heap= 的统计行", file=sys.stderr)
        return 1

    out = []
    out.append("# 长跑内存报告")
    out.append("")
    out.append("- 目录: `%s`" % args.dir)
    out.append("- 口径: 活堆 = `mallinfo2().uordblks`(正在使用的堆字节); "
               "RSS = `/proc/self/statm`")
    out.append("- ★ 活堆呈**锯齿**(视频源循环 -> MPP 解码器反复申请/释放帧缓冲), "
               "所以下面看的是**锯齿谷底**的基线趋势, 不是首尾相减。")
    out.append("")

    # ---- 总体 ----
    all_ch = sorted(per)
    out.append("## 1. 各通道概况")
    out.append("")
    out.append("| 通道 | 采样点 | 帧数区间 | 吞吐(FPS) | e2e等待(ms) | RSS 峰值(MB) | "
               "活堆起/止(MB) | 活堆谷底起/止(MB) | 谷底斜率 |")
    out.append("|---|---|---|---|---|---|---|---|---|")
    for ch in all_ch:
        s = per[ch]
        mins = minima(s)
        if not s:
            continue
        fps = sum(x[2] for x in s) / len(s)
        dtw = sum(x[4] for x in s) / len(s)
        rss_pk = max(x[7] for x in s) / 1024.0
        h0, h1 = s[0][8] / 1024.0, s[-1][8] / 1024.0
        if len(mins) >= 2:
            mf, ml = mins[0], mins[-1]
            k, _ = linfit([m[0] for m in mins], [m[8] / 1024.0 for m in mins])
            base = "%.1f -> %.1f" % (mf[8] / 1024.0, ml[8] / 1024.0)
            slope = "%+.3f KB/帧" % (k * 1024.0 / max(1, (ml[0] - mf[0])))
        else:
            base = "-"
            slope = "-"
        out.append("| %s | %d | %d -> %d | %.1f | %.1f | %.0f | %.1f -> %.1f | %s | %s |" % (
            ch, len(s), s[0][0], s[-1][0], fps, dtw, rss_pk, h0, h1, base, slope))
    out.append("")

    # ---- 谷底序列(台阶视图) ----
    out.append("## 2. 活堆的锯齿谷底序列(看台阶, 不看斜率)")
    out.append("")
    for ch in all_ch:
        mins = minima(per[ch])
        if len(mins) < 3:
            continue
        out.append("**%s** (%d 个谷底):" % (ch, len(mins)))
        out.append("")
        out.append("```")
        seq = []
        for m in mins:
            seq.append("%d:%.1f" % (m[0], m[8] / 1024.0))
        out.append("  ".join(seq))
        out.append("```")
        out.append("")

    # ---- 线性拟合(全样本, 对锯齿稳健) ----
    out.append("## 3. 活堆趋势(全样本最小二乘, 对锯齿稳健)")
    out.append("")
    xs_all, ys_all = [], []
    for ch in all_ch:
        for x in per[ch]:
            xs_all.append(x[0])
            ys_all.append(x[8] / 1024.0)
    k, b = linfit(xs_all, ys_all)
    out.append("- 全样本回归斜率: **%+.4f MB/帧**(正=涨)")
    out.append("- 折算: 80 fps 下 **%+.1f KB/s**, 一天 **%+.1f MB**"
               % (k * 80 * 1024, k * 80 * 1024 * 86400 / 1024 / 1024))
    out.append("")

    # ---- RSS ----
    rows = parse_load(load_csv)
    if rows:
        out.append("## 4. RSS(整机口径, 来自 load.csv)")
        out.append("")
        rss = [r[7] for r in rows if r[7] > 0]
        if rss:
            out.append("- 采样 %d 点, 起 %.0f MB, 末 %.0f MB, 峰值 %.0f MB"
                       % (len(rss), rss[0] / 1024.0, rss[-1] / 1024.0, max(rss) / 1024.0))
        cpu = [r[6] for r in rows if r[6] > 0]
        if cpu:
            out.append("- CPU: 平均 %.1f%%, 峰值 %.1f%%" % (sum(cpu) / len(cpu), max(cpu)))
        npu = [r[1:4] for r in rows if any(r[1:4])]
        if npu:
            for c in range(3):
                vals = [x[c] for x in npu]
                out.append("- NPU Core%d(debugfs, 需 root, 只统计采到非 0 的点): 平均 %.1f%%, 峰值 %.0f%%"
                           % (c, sum(vals) / len(vals), max(vals)))
        out.append("")

    # ---- 每核占用(应用侧) ----
    out.append("## 5. 每核 NPU 占用(应用内自测口径)")
    out.append("")
    with open(gate_log, "r", encoding="utf-8", errors="ignore") as f:
        txt = f.read()
    blk = re.search(r"===== 每核 NPU 占用.*?\n(.*?)\n\s*\(对照命令", txt, re.S)
    if blk:
        out.append("```")
        out.append(blk.group(1).strip())
        out.append("```")
    out.append("")

    md = "\n".join(out) + "\n"
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(md)
        print("已写出:", args.out)
    else:
        print(md)
    return 0


if __name__ == "__main__":
    sys.exit(main())
