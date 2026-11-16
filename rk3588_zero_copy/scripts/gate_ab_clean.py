#!/usr/bin/env python3
"""gate_ab_clean.py —— 4 路大门 A/B 的【干净窗口】统计

背景: 压测板上有时会有别的进程(rk_minimal 的 RGA 压测、另一个会话的 rk_gate)
在跑, 它们的负载会计进 CPU% 和 NPU 占用。本脚本按【干扰结束之后】的时间窗口
统计, 避免把别人的负载算到自己头上。

用法:
    python3 scripts/gate_ab_clean.py <A目录> <B目录> [--from-ts T1] [--from-ts T2]
"""
import csv
import os
import sys


def load(path):
    rows = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for r in csv.reader(f):
            if len(r) < 7 or not r[0].strip().isdigit():
                continue
            try:
                rows.append([float(x) for x in r])
            except ValueError:
                pass
    return rows


def stats(rows, idx):
    v = [r[idx] for r in rows if idx < len(r)]
    if not v:
        return 0.0, 0.0
    return sum(v) / len(v), max(v)


def report(path, label, from_ts, fps_cols, rss_cols):
    rows = load(path)
    if not rows:
        print(f"[{label}] 没有数据: {path}")
        return None
    total = len(rows)
    clean = [r for r in rows if r[0] >= from_ts] if from_ts else rows
    print("=" * 72)
    print(f" {label}   ({path})")
    print("=" * 72)
    print(f" 采样点 {total} 个; 干净窗口(from ts={from_ts:.0f}) {len(clean)} 个"
          f"{'' if len(clean) == total else '   ← 前段有别的进程干扰, 已剔除'}")
    if not clean:
        clean = rows
    out = {}
    for i, lbl in ((1, "NPU Core0"), (2, "NPU Core1"), (3, "NPU Core2"),
                   (4, "VPU 解码"), (5, "RGA"), (6, "CPU 总占用%")):
        a, m = stats(clean, i)
        out[lbl] = (a, m)
        print(f"   {lbl:<14} 平均 {a:6.1f}%   峰值 {m:6.1f}%")
    rss = 0.0
    for c in rss_cols:
        a, _ = stats(clean, c)
        rss += a
    out["RSS(MB)"] = (rss, 0.0)
    print(f"   {'RSS 合计':<14} {rss:6.0f} MB")
    fps = []
    for c in fps_cols:
        a, m = stats(clean, c)
        fps.append(a)
    out["fps"] = fps
    print(f"   各路 FPS        {' / '.join(f'{x:.2f}' for x in fps)}")
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    froms = [float(a.split("=", 1)[1]) for a in sys.argv[1:] if a.startswith("--from-ts")]
    base = args[0] if len(args) > 0 else "/home/seeed/rk_deploy/logs/gate_4ch_base_ab"
    new = args[1] if len(args) > 1 else "/home/seeed/rk_deploy/logs/gate_4ch_single_ab"
    ts_a = froms[0] if len(froms) > 0 else 0
    ts_b = froms[1] if len(froms) > 1 else 0

    a = report(os.path.join(base, "load.csv"), "A: 4 个独立进程 (rk_zero_copy x4)",
               ts_a, (11, 12, 13, 14), (7, 8, 9, 10))
    b = report(os.path.join(new, "load.csv"), "B: 1 个进程 (rk_gate, NPU 全局线程池)",
               ts_b, (8, 9, 10, 11), (7,))
    if not a or not b:
        return 1

    print("=" * 72)
    print(" 前后对比")
    print("=" * 72)
    print(f"{'指标':<16}{'A(4进程)':>12}{'B(单进程)':>12}{'变化':>12}")
    print("-" * 72)
    for k in ("NPU Core0", "NPU Core1", "NPU Core2", "CPU 总占用%", "RSS(MB)"):
        va, vb = a[k][0], b[k][0]
        d = f"{(vb - va) / va * 100:+.0f}%" if va > 0.01 else "-"
        print(f"{k:<16}{va:>12.1f}{vb:>12.1f}{d:>12}")
    fa, fb = a["fps"], b["fps"]
    for i in range(min(len(fa), len(fb))):
        d = f"{(fb[i] - fa[i]) / fa[i] * 100:+.0f}%" if fa[i] > 0.01 else "-"
        print(f"{'ch%d FPS' % (i + 1):<16}{fa[i]:>12.2f}{fb[i]:>12.2f}{d:>12}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
