#!/usr/bin/env python3
"""hw_load_chart.py —— 把 hw_load.csv 画成一张 SVG 监控图(rktop 风格的面板)

用法:
    python3 scripts/hw_load_chart.py <csv> <out.svg> [标题]

输出: 四个堆叠面板(共用时间轴)
    1) NPU 三核 (Core0/Core1/Core2, 0~100%)
    2) VPU (rkvdec 解码核, 0~100%)
    3) CPU 总体占用 (0~100%)
    4) 流水线 FPS (自适应量程)
"""
from __future__ import annotations

import csv
import html
import sys

# CSV 列名: time,npu0,npu1,npu2,vpu_dec,cpu_pct,fps,frame
PANELS = [
    ("NPU 三核占用 (%)", [("NPU Core0", "npu0", "#e4572e"),
                          ("NPU Core1", "npu1", "#f2a541"),
                          ("NPU Core2", "npu2", "#8d99ae")], 100.0),
    ("VPU 硬解码核 rkvdec (%)", [("rkvdec", "vpu_dec", "#3d8b8b")], 100.0),
    ("CPU 总占用 (%)", [("CPU", "cpu_pct", "#4c6ef5")], 100.0),
    ("流水线 FPS", [("FPS", "fps", "#2f9e44")], None),
]


def load(path):
    rows = []
    with open(path) as f:
        for r in csv.DictReader(f):
            try:
                rows.append({k: float(v) for k, v in r.items()})
            except (TypeError, ValueError):
                continue
    return rows


def panel_svg(rows, x0, y0, w, h, title, series, ymax):
    """画一个面板, 返回 SVG 片段"""
    out = [f'<rect x="{x0}" y="{y0}" width="{w}" height="{h}" fill="#12151c" stroke="#2b313d"/>']
    out.append(f'<text x="{x0 + 10}" y="{y0 + 18}" fill="#c9d1d9" '
               f'font-family="DejaVu Sans, sans-serif" font-size="13" font-weight="bold">'
               f'{html.escape(title)}</text>')

    vals = [max((row.get(col, 0.0) for row in rows), default=0.0)
            for _, col, _ in series] or [0]
    if ymax is None:
        ymax = max(vals) * 1.15 or 1.0

    # 网格 + 刻度
    for i in range(5):
        gy = y0 + h - 10 - (h - 30) * i / 4.0
        out.append(f'<line x1="{x0 + 45}" y1="{gy:.1f}" x2="{x0 + w - 8}" y2="{gy:.1f}" '
                   f'stroke="#242a35" stroke-width="1"/>')
        label = f"{ymax * i / 4.0:.0f}"
        out.append(f'<text x="{x0 + 40}" y="{gy + 4:.1f}" fill="#7d8590" text-anchor="end" '
                   f'font-family="DejaVu Sans, sans-serif" font-size="10">{label}</text>')

    n = len(rows)
    if n >= 2:
        for name, col, color in series:
            pts = []
            for i, r in enumerate(rows):
                px = x0 + 45 + (w - 60) * (i / (n - 1))
                py = y0 + h - 10 - (h - 30) * min(max(r.get(col, 0.0) / ymax, 0.0), 1.0)
                pts.append(f"{px:.1f},{py:.1f}")
            out.append(f'<polyline points="{" ".join(pts)}" fill="none" stroke="{color}" '
                       f'stroke-width="2" stroke-linejoin="round"/>')
            # 图例(放右侧, 免得压住左边的刻度)
            ly = y0 + 34 + series.index((name, col, color)) * 15
            lx = x0 + w - 130
            out.append(f'<rect x="{lx}" y="{ly - 9}" width="9" height="9" fill="{color}"/>')
            out.append(f'<text x="{lx + 13}" y="{ly}" fill="#9aa4b2" '
                       f'font-family="DejaVu Sans, sans-serif" font-size="10">{name}</text>')
    return "\n".join(out)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    csv_path, out_path = sys.argv[1], sys.argv[2]
    title = sys.argv[3] if len(sys.argv) > 3 else "RK3588 硬件负载实时监控"
    rows = load(csv_path)
    if not rows:
        print("没有数据")
        return 1

    W, H = 900, 760
    x0, w = 60, 800
    ph = 140
    gap = 18
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">',
        f'<rect width="{W}" height="{H}" fill="#0b0e13"/>',
        f'<text x="{x0}" y="30" fill="#e6edf3" font-family="DejaVu Sans, sans-serif" '
        f'font-size="17" font-weight="bold">{html.escape(title)}</text>',
        f'<text x="{x0}" y="48" fill="#7d8590" font-family="DejaVu Sans, sans-serif" font-size="11">'
        f'{html.escape(csv_path)}  |  {len(rows)} 个采样点</text>',
    ]
    y = 62
    for title_, series, ymax in PANELS:
        parts.append(panel_svg(rows, x0, y, w, ph, title_, series, ymax))
        y += ph + gap

    # 时间轴
    parts.append(f'<text x="{x0 + 45}" y="{y + 14}" fill="#7d8590" '
                 f'font-family="DejaVu Sans, sans-serif" font-size="10">时间(秒级采样)</text>')
    parts.append("</svg>")

    with open(out_path, "w") as f:
        f.write("\n".join(parts))
    print(f"已生成: {out_path}  ({len(rows)} 个采样点)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
