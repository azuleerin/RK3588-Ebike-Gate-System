#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""make_plate_crops.py —— 按候选 GATE_ROI_* 参数离线生成【喂给 PP-OCR 的原图】

★ 与生产逐字一致的链(全部来自 src/main.cc / src/gate_main.cc / include/rk/plate_roi.h):
  1) plate_roi_from_box:  w=bw*ratio, h=bh*height, cx=框中心+bw*shift, top=y1+bh*roi_y+bh*below
  2) ppocr_pad(0.10) 撑开 -> clamp_roi -> 偶对齐 -> clamp -> 最小尺寸(rh>=16/rw>=32) -> clamp
  3) 两行牌判定 rw/rh < kTwoRowAspect(2.5):
       上行 3 个切分点 ry+rh*{0.40,0.45,0.50}, 下行 ry+rh*0.45 往下
     否则整块当一行
  4) 每块裁剪缩放到 (ceil(48*rw/rh) 上限 320, 48) —— 与 crop_plate_roi / calc_resized_w 同式
  5) 水平方向裁剪范围取 ROI 内部(切分点也在 ROI 内), 所以省份字行被切掉与否
     完全取决于 ROI 的纵向位置 —— 这就是 O9 的落点

用法:
  make_plate_crops.py --boxes <boxes.csv> --params 0.8,0.25,0.4,0.0,0.0 \
      --out out/plate_crops/<tag> [--limit N]
输出:
  <out>/crops/*.jpg          每个候选一张(与生产 --save-roi 落的是同一批图)
  <out>/crops.csv            清单: crop_path,gt_alnum,frame_idx,kind,slot,roi_x,roi_y,roi_w,roi_h
"""
import argparse
import csv
import os
import sys

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sweep_roi_params import (FW, FH, TWO_ROW_ASPECT, clamp_roi, load_boxes,
                              load_gt, roi_of_box)

PPOCR_IN_H = 48
PPOCR_IN_W = 320
PPOCR_PAD = 0.10
ROW_SPLIT_TRIALS = 3
ROW_SPLIT_STEP = 0.05
ROW_SPLIT_DEFAULT = 0.45
ACC = "/home/seeed/rk_deploy/accuracy"
GT_ALNUM = "NA339058"      # plates.csv 的 gt_alnum
GT_SERIAL = "A339058"      # plates.csv 的 gt_serial


def resize_to_ppocr(img, w, h):
    """按生产 crop_plate_roi 的比例缩放到 (w, 48)"""
    out_w = int(np.ceil(PPOCR_IN_H * (w / max(1, h))))
    out_w = min(out_w, PPOCR_IN_W)
    out_w = max(out_w, 1)
    return cv2.resize(img, (out_w, PPOCR_IN_H), interpolation=cv2.INTER_LINEAR), out_w


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--boxes", required=True)
    ap.add_argument("--params", required=True, help="ratio,roi_y,height,shift,below")
    ap.add_argument("--out", required=True)
    ap.add_argument("--gt", default=os.path.join(ACC, "data/manifests/plates.csv"))
    ap.add_argument("--frames", default=os.path.join(ACC, "data/frames"))
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()

    ratio, roi_y, height, shift, below = [float(v) for v in args.params.split(",")]
    os.makedirs(os.path.join(args.out, "crops"), exist_ok=True)
    boxes = load_boxes(args.boxes)
    gt = load_gt(args.gt)

    rows = []
    n_frames = 0
    for key, bs in boxes.items():
        base = os.path.basename(key)
        if not base.endswith(".jpg"):
            continue
        fi = int(base[1:6])
        g = gt.get(fi)
        if g is None:
            continue
        if args.limit and n_frames >= args.limit:
            break
        n_frames += 1
        frame_path = os.path.join(args.frames, base)
        img = cv2.imread(frame_path)
        if img is None:
            continue
        H, W = img.shape[:2]
        gx, gy, gw, gh = g
        # 该帧里选【最贴住 GT】的检测: ROI 与 GT 的横向/纵向交集最大
        best = None
        best_score = -1
        for c, sc, x1, y1, x2, y2 in bs:
            r = roi_of_box((x1 * W, y1 * H, x2 * W, y2 * H),
                           ratio, roi_y, height, shift, below)
            if r is None:
                continue
            rx, ry, rw, rh = r
            ix = max(0, min(rx + rw, gx + gw) - max(rx, gx))
            iy = max(0, min(ry + rh, gy + gh) - max(ry, gy))
            score = ix * iy
            if score > best_score:
                best_score = score
                best = (rx, ry, rw, rh)
        if best is None:
            continue
        rx, ry, rw, rh = best
        two_row = (rw / max(rh, 1)) < TWO_ROW_ASPECT
        cands = []
        if two_row:
            for t in range(ROW_SPLIT_TRIALS):
                sp = ROW_SPLIT_DEFAULT + (t - (ROW_SPLIT_TRIALS - 1) / 2) * ROW_SPLIT_STEP
                cut = ry + int(rh * sp)
                if cut <= ry or cut >= ry + rh:
                    continue
                cands.append(("top%d" % t, rx, ry, rw, cut - ry))
            mid = ry + int(rh * ROW_SPLIT_DEFAULT)
            if mid > ry and mid < ry + rh:
                cands.append(("bot", rx, mid, rw, ry + rh - mid))
        else:
            cands.append(("all", rx, ry, rw, rh))
        for kind, cx, cy, cw, ch in cands:
            if cw <= 0 or ch <= 0:
                continue
            x0 = max(0, min(W - 1, cx))
            y0 = max(0, min(H - 1, cy))
            x1 = max(x0 + 1, min(W, cx + cw))
            y1 = max(y0 + 1, min(H, cy + ch))
            crop = img[y0:y1, x0:x1]
            if crop.size == 0:
                continue
            small, out_w = resize_to_ppocr(crop, x1 - x0, y1 - y0)
            name = "%s_f%05d_%s.jpg" % (os.path.basename(args.out), fi, kind)
            path = os.path.join(args.out, "crops", name)
            cv2.imwrite(path, small)
            rows.append([path, GT_ALNUM, fi, kind, out_w, cx, cy, cw, ch,
                         small.shape[1], small.shape[0]])
    lst = os.path.join(args.out, "crops.csv")
    with open(lst, "w", newline="") as fh:
        wr = csv.writer(fh)
        wr.writerow(["crop_path", "gt_alnum", "frame_idx", "kind", "out_w",
                     "roi_x", "roi_y", "roi_w", "roi_h", "crop_w", "crop_h"])
        wr.writerows(rows)
    print("[%s] 参数 ratio=%s roi_y=%s height=%s shift=%s below=%s" %
          (os.path.basename(args.out), ratio, roi_y, height, shift, below))
    print("  真牌帧=%d  候选裁剪=%d 张  清单=%s" % (n_frames, len(rows), lst))
    return 0


if __name__ == "__main__":
    sys.exit(main())
