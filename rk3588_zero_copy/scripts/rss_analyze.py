#!/usr/bin/env python3
"""rss_analyze.py —— RSS 曲线分析: 漂移速率 + 线性度 + 台阶

用法:
  python3 scripts/rss_analyze.py <csv> [列名...]

CSV 格式(第一行表头):
  rss_ab.csv     : time,rss_a_kb,vsz_a_kb,rss_b_kb,vsz_b_kb
  soak*.csv      : time,rss_kb,vsz_kb,...

输出每个 RSS 列的:
  · 样本数 / 跨度
  · min / max / 首 / 末
  · 线性回归斜率(KB/h)  <- 客户要的就是这个: 必须是 0
  · 首尾差 (KB)
  · 相对基线的最大偏移(KB)
"""
import csv
import sys


def slope_per_hour(ts, vs):
    """最小二乘斜率, 单位 KB/h"""
    n = len(ts)
    if n < 2:
        return 0.0
    sx = sum(ts)
    sy = sum(vs)
    sxx = sum(t * t for t in ts)
    sxy = sum(t * v for t, v in zip(ts, vs))
    den = n * sxx - sx * sx
    if den == 0:
        return 0.0
    return (n * sxy - sx * sy) / den * 3600.0


def analyze(path, cols):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        print(f"{path}: 无数据")
        return

    print(f"=== {path} ===")
    print(f"样本数: {len(rows)}")

    for c in cols:
        if c not in rows[0]:
            continue
        pts = []
        for r in rows:
            try:
                v = int(float(r.get(c) or 0))
            except (TypeError, ValueError):
                continue
            if v <= 0:
                continue
            try:
                t = int(float(r.get("time") or 0))
            except (TypeError, ValueError):
                continue
            pts.append((t, v))
        if len(pts) < 2:
            continue

        ts = [p[0] for p in pts]
        vs = [p[1] for p in pts]
        span_h = (ts[-1] - ts[0]) / 3600.0
        sl = slope_per_hour(ts, vs)
        drift = vs[-1] - vs[0]
        lo, hi = min(vs), max(vs)
        # 相对第一个样本的最大偏离(看有没有台阶)
        max_dev = max(abs(v - vs[0]) for v in vs)

        print(f"\n  列 {c}:")
        print(f"    跨度      : {span_h:.3f} 小时 ({span_h*60:.1f} 分钟)")
        print(f"    RSS       : 首={vs[0]} 末={vs[-1]} min={lo} max={hi} (KB)")
        print(f"    首尾差    : {drift:+d} KB ({drift/1024:+.2f} MB)")
        print(f"    回归斜率  : {sl:+.1f} KB/h   <-- 客户指标(要求 ~0)")
        print(f"    最大偏离  : {max_dev} KB (相对首样本)")

        # 分段速率: 看是"缓慢爬坡"还是"台阶"
        if len(pts) >= 6:
            half = len(pts) // 2
            s1 = slope_per_hour(ts[:half], vs[:half])
            s2 = slope_per_hour(ts[half:], vs[half:])
            print(f"    前半段斜率: {s1:+.1f} KB/h   后半段: {s2:+.1f} KB/h")
    print()


if __name__ == "__main__":
    path = sys.argv[1]
    cols = sys.argv[2:] or None
    if cols is None:
        with open(path, newline="") as f:
            hdr = next(csv.reader(f))
        cols = [h for h in hdr if h.startswith("rss_")]
    analyze(path, cols)
