#!/usr/bin/env python3
"""plate_roi_resolution_sim.py —— 换输入分辨率后, 车牌 ROI 还能不能看清

问题:
    检测模型从 640x640 换到 416/320 之后, 车牌 ROI(按固定比例从检测框里切)
    实际拿到的像素少了多少? 放大回识别模型输入尺寸之后还锐不锐?

做法(模拟, 不需要真的转三个模型):
    1. 拿一张真实帧 + 从它上面切下来的车牌 ROI 裁剪图(生产流水线 --save-roi 存的)
    2. 用模板匹配把 ROI 的位置在原图里找出来 -> 得到 ROI 矩形(原图坐标)
    3. 对每个输入尺寸 S:
         把整帧缩到 S/640 (检测器看到的就是这个分辨率)
         在缩放后的帧里按同样比例切 ROI
         放大回识别模型的输入尺寸(PP-OCR 320x48)
    4. 拼图 + 用拉普拉斯方差量化清晰度

用法:
    python3 scripts/plate_roi_resolution_sim.py \
        --full logs/plate_crops/full_000120.jpg \
        --roi  logs/plate_crops/bottom_000120.jpg \
        --out  /tmp/plate_sim.png
"""
from __future__ import annotations

import argparse
import os
import sys

try:
    import cv2
    import numpy as np
except ImportError:
    print("需要 opencv + numpy", file=sys.stderr)
    raise


def sharpness(img) -> float:
    """拉普拉斯方差 = 清晰度(越大越锐)"""
    g = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    return float(cv2.Laplacian(g, cv2.CV_64F).var())


def find_rect(full, roi):
    """在 full 里模板匹配出 roi 的位置"""
    for method in (cv2.TM_CCOEFF_NORMED, cv2.TM_SQDIFF_NORMED):
        res = cv2.matchTemplate(full, roi, method)
        _, maxv, _, maxl = cv2.minMaxLoc(res)
        if method == cv2.TM_CCOEFF_NORMED and maxv < 0.3:
            continue
        x, y = maxl
        return x, y, x + roi.shape[1], y + roi.shape[0]
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--full", required=True, help="原始帧")
    ap.add_argument("--roi", required=True, help="从原帧切下来的车牌 ROI")
    ap.add_argument("--out", default="/tmp/plate_sim.png")
    ap.add_argument("--in-w", type=int, default=320, help="识别模型输入宽")
    ap.add_argument("--in-h", type=int, default=48, help="识别模型输入高")
    ap.add_argument("--sizes", default="640,416,320")
    args = ap.parse_args()

    full = cv2.imread(args.full)
    roi = cv2.imread(args.roi)
    if full is None or roi is None:
        print("读图失败", file=sys.stderr)
        return 1

    rect = find_rect(full, roi)
    if rect is None:
        print("在原图里找不到 ROI 的位置, 换一张 ROI 图试试", file=sys.stderr)
        return 1
    x0, y0, x1, y1 = rect
    print(f">>> ROI 在原图({full.shape[1]}x{full.shape[0]})里的位置: "
          f"({x0},{y0})-({x1},{y1})  {x1-x0}x{y1-y0}px")
    base_w = full.shape[1]
    print(f">>> 识别模型输入: {args.in_w}x{args.in_h}")

    tiles = []
    for s in [int(v) for v in args.sizes.split(",")]:
        scale = s / 640.0
        small = cv2.resize(full, (int(full.shape[1] * scale), int(full.shape[0] * scale)),
                           interpolation=cv2.INTER_AREA)
        rx0, ry0 = int(x0 * scale), int(y0 * scale)
        rx1, ry1 = int(x1 * scale), int(y1 * scale)
        rx0 = max(0, rx0); ry0 = max(0, ry0)
        rx1 = min(small.shape[1], rx1); ry1 = min(small.shape[0], ry1)
        crop = small[ry0:ry1, rx0:rx1]
        # 放大回识别模型输入尺寸(流水线里就是这么做的)
        up = cv2.resize(crop, (args.in_w, args.in_h), interpolation=cv2.INTER_LINEAR)
        # 再放大 4 倍只是为了人眼看
        big = cv2.resize(up, (args.in_w * 4, args.in_h * 4), interpolation=cv2.INTER_NEAREST)
        sh = sharpness(up)
        cv2.putText(big, f"input {s}x{s}  roi={crop.shape[1]}x{crop.shape[0]}px  "
                        f"sharp={sh:.0f}",
                    (10, 28), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 0, 255), 2)
        tiles.append(big)
        print(f"    {s}x{s}: ROI 像素 {crop.shape[1]}x{crop.shape[0]}  "
              f"清晰度(拉普拉斯方差)={sh:.0f}")

    wmax = max(t.shape[1] for t in tiles)
    pad = []
    for t in tiles:
        if t.shape[1] < wmax:
            t = cv2.copyMakeBorder(t, 0, 0, 0, wmax - t.shape[1], cv2.BORDER_CONSTANT, value=(0, 0, 0))
        pad.append(t)
    out = np.vstack(pad)
    cv2.imwrite(args.out, out)
    print(f">>> 对比图: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
