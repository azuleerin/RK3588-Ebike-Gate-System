#!/usr/bin/env python3
"""RSS 曲线分析: 改动前 vs 改动后(A/B 同机同时跑)

用法:
    python3 scripts/rss_analyze.py <after.csv> [<before.csv>]

输出:
    - 斜率(KB/h)、首尾差、峰值偏离基线
    - 锯齿检测: 统计"回落 > 1MB"的次数(= malloc_trim 起作用的次数)
    - ASCII 曲线
"""
from __future__ import annotations

import csv
import sys


def load(path: str):
    rows = []
    with open(path) as f:
        for r in csv.DictReader(f):
            try:
                rss = int(r.get("rss_kb", 0))
                ts = int(r.get("time", 0))
                fr = r.get("frame", "")
            except (ValueError, TypeError):
                continue
            if rss <= 0 or ts <= 0:
                continue
            rows.append((ts, rss, fr))
    return rows


def floor_from_log(path: str):
    """从主程序日志里取【每次 trim 之后的 RSS】= 地板。

    地板才是客户关心的"总体内存": 它剔除了锯齿的毛刺, 直接反映
    "有没有真泄漏"。RSS 采样点(30s)会落在锯齿的不同相位上, 看起来像单调上涨。
    """
    import re
    vals = []
    try:
        for line in open(path, errors='ignore'):
            m = re.search(r'malloc_trim @帧(\d+):.*?rss=(\d+)KB', line)
            if m:
                vals.append((int(m.group(1)), int(m.group(2))))
    except OSError:
        return []
    return vals


def analyse(name: str, rows):
    if len(rows) < 2:
        print(f"{name}: 数据点不足({len(rows)})")
        return None
    t0, r0 = rows[0][0], rows[0][1]
    dur_h = (rows[-1][0] - t0) / 3600.0
    slope = (rows[-1][1] - r0) / dur_h if dur_h > 0 else 0.0
    rss = [r[1] for r in rows]
    base = rss[0]
    peak = max(rss)
    trough = min(rss)

    # 锯齿: 相邻采样点回落 > 1MB 的次数
    drops = 0
    biggest = 0
    for a, b in zip(rss, rss[1:]):
        if a - b > 1024:
            drops += 1
            biggest = max(biggest, a - b)
    over_base = peak - base
    print(f"--- {name} ---")
    print(f"  样本 {len(rows)} 个, 时长 {dur_h*60:.1f} 分钟")
    print(f"  RSS 首 {base} KB -> 末 {rss[-1]} KB   净变化 {rss[-1]-base:+d} KB")
    print(f"  斜率 {slope:+.1f} KB/h    峰值 {peak} KB (高于基线 {over_base} KB = {over_base/1024:.2f} MB)"
          f"   谷值 {trough} KB")
    print(f"  回落>1MB 的次数: {drops}    单次最大回落 {biggest} KB")
    verdict = "✔ 平直/锯齿(合格)" if (over_base <= 1024 or drops > 0) else "✘ 持续爬升"
    print(f"  判定: {verdict}")
    return {"name": name, "slope": slope, "base": base, "peak": peak, "over": over_base,
            "drops": drops, "rows": rows, "dur_h": dur_h}


def curve(rows, width=100, height=14):
    if not rows:
        return
    rss = [r[1] for r in rows]
    lo, hi = min(rss), max(rss)
    if hi == lo:
        hi = lo + 1
    step = max(1, len(rss) // width)
    pts = rss[::step][:width]
    print(f"  RSS 曲线 (纵轴 {lo}~{hi} KB, 横轴 {len(rows)} 个采样点)")
    for row in range(height, 0, -1):
        thr = lo + (hi - lo) * row / (height + 1)
        line = "".join("█" if v >= thr else " " for v in pts)
        print(f"   {thr:9.0f} |{line}|")


def main() -> int:
    after_path = sys.argv[1] if len(sys.argv) > 1 else "logs/rss_zerogrowth.csv"
    before_path = sys.argv[2] if len(sys.argv) > 2 else "logs/soak.csv"
    print("=" * 72)
    print(" RSS Zero-Growth 验收 (改动前 vs 改动后)")
    print("=" * 72)
    before = analyse("改动前 (10000 帧间隔)", load(before_path)) if before_path else None
    after = analyse("改动后 (500 帧间隔)", load(after_path))

    # 地板口径(优先用日志里 trim 后的 RSS)
    for tag, path in (("改动前", before_path), ("改动后", after_path)):
        log = path.replace("rss_", "zerogrowth_").replace(".csv", ".log")
        fl = floor_from_log(log)
        if len(fl) < 3:
            continue
        f0, r0 = fl[0]
        f1, r1 = fl[-1]
        df = max(1, f1 - f0)
        print(f"--- {tag} 地板(trim 后的 RSS, {len(fl)} 次) ---")
        print(f"  帧 {f0} -> {f1}: 地板 {r0} -> {r1} KB  漂移 {r1-r0:+d} KB"
              f"  = {(r1-r0)/df:+.3f} KB/帧")
    if after:
        print()
        curve(after["rows"])
    if before:
        print()
        curve(before["rows"])
    print("=" * 72)
    if before and after:
        print(f" 斜率: {before['slope']:+.1f} KB/h  ->  {after['slope']:+.1f} KB/h "
              f"({after['slope']-before['slope']:+.1f} KB/h)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
