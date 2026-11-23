#!/usr/bin/env python3
"""ab_report.py —— 汇总 A/B 结果: 各臂取中位数, 输出对比表"""
import glob
import json
import os
import statistics
import sys

OUT = "/home/seeed/rk_deploy/opt_audit/ab"


def load(arm, f):
    files = sorted(glob.glob(os.path.join(OUT, f"{arm}_r*_f{f}.json")))
    return [(os.path.basename(x), json.load(open(x))) for x in files]


def med(vals):
    vals = [v for v in vals if v is not None]
    return statistics.median(vals) if vals else None


def spread(vals):
    vals = [v for v in vals if v is not None]
    if not vals:
        return None
    return max(vals) - min(vals)


def main():
    f = sys.argv[1] if len(sys.argv) > 1 else "0"
    b = load("base", f)
    n = load("new", f)
    if not b or not n:
        print(f"没有结果 (f={f})")
        return

    def agg(rows, key):
        return [r[1].get(key) for r in rows]

    def ch_agg(rows, ch, key):
        return [r[1]["channels"][ch][key] for r in rows
                if ch in r[1].get("channels", {})]

    print(f"\n{'='*78}")
    print(f"  A/B 汇总   (max_fps={f},  {len(b)} 轮 base / {len(n)} 轮 new)")
    print(f"{'='*78}")
    print(f"{'指标':<26}{'base 中位数':>16}{'new 中位数':>16}{'变化':>14}{'极差(base)':>12}")
    print("-" * 78)

    def row(name, kb, kn, unit=""):
        mb, mn = med(kb), med(kn)
        if mb is None or mn is None:
            return
        d = mn - mb
        pct = (d / mb * 100.0) if mb else 0.0
        print(f"{name:<26}{mb:>12.2f}{unit}{mn:>12.2f}{unit}"
              f"{d:>+10.2f}{unit} {pct:>+5.1f}%{spread(kb):>11.2f}")

    row("总 FPS(4 路帧计数器差分)", agg(b, "total_fps"), agg(n, "total_fps"))
    for ch in ["ch1", "ch2", "ch3", "ch4"]:
        row(f"  {ch} FPS", ch_agg(b, ch, "fps_delta"), ch_agg(n, ch, "fps_delta"))
    print("-" * 78)
    for ch in ["ch1", "ch2", "ch3", "ch4"]:
        row(f"  {ch} detwait ms", ch_agg(b, ch, "detwait_ms"), ch_agg(n, ch, "detwait_ms"))
    for ch in ["ch1", "ch3"]:
        row(f"  {ch} ocrwait ms", ch_agg(b, ch, "ocrwait_ms"), ch_agg(n, ch, "ocrwait_ms"))
    for ch in ["ch1", "ch2", "ch3", "ch4"]:
        row(f"  {ch} npu ms", ch_agg(b, ch, "npu_ms"), ch_agg(n, ch, "npu_ms"))
    print("-" * 78)
    # NPU 每核
    cores = ["core0", "core1", "core2"]
    for c in cores:
        kb = [r[1].get("npu_core_pct", {}).get(c) for r in b]
        kn = [r[1].get("npu_core_pct", {}).get(c) for r in n]
        row(f"  NPU {c} 占用 %", kb, kn)
    print("-" * 78)
    row("活堆斜率 KB/min", [r[1].get("heap_global", {}).get("slope_kb_per_min") for r in b],
        [r[1].get("heap_global", {}).get("slope_kb_per_min") for r in n])
    row("RSS 斜率 KB/min", [r[1].get("rss_global", {}).get("slope_kb_per_min") for r in b],
        [r[1].get("rss_global", {}).get("slope_kb_per_min") for r in n])
    row("活堆末值 KB", [r[1].get("heap_global", {}).get("last_kb") for r in b],
        [r[1].get("heap_global", {}).get("last_kb") for r in n])
    row("RSS 末值 KB", [r[1].get("rss_global", {}).get("last_kb") for r in b],
        [r[1].get("rss_global", {}).get("last_kb") for r in n])
    print("-" * 78)
    print("每轮原始值:")
    for tag, rows in (("base", b), ("new", n)):
        for name, r in rows:
            print(f"  {tag:<5} {name:<22} total_fps={r['total_fps']:<8} "
                  f"npu_core={r.get('npu_core_pct')} "
                  f"heap_slope={r.get('heap_global',{}).get('slope_kb_per_min')}")


if __name__ == "__main__":
    main()
