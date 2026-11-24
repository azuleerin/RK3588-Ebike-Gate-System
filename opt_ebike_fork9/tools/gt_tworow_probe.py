#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gt_tworow_probe.py —— 上限验证: GT 车牌特写按【两行切分】喂 PP-OCR 能不能读出来

★ 为什么必须做这一步:
  直接把 GT 特写(453x297, 两行牌)当一行喂 PP-OCR -> 216/217 空输出(必然: 一行模型读两行)。
  这不能说明"OCR 读不了这块牌", 只能说明"整块牌不能当一行喂"。
  生产代码的做法是: 上行试 3 个切分点 + 下行切 1 次, 每行单独喂。
  所以这里必须复刻【两行切分】再喂, 才能知道:
    · 如果能读出 "A339058" -> ROI 几何是唯一瓶颈, 标定 ROI 就能把识别率拉起来
    · 如果还是读不出        -> PP-OCRv4 这个模型本身吃不下这块两行牌, ROI 标定救不了
"""
import argparse
import csv
import os

import cv2
import numpy as np

PPOCR_IN_H = 48
PPOCR_IN_W = 320
SPLITS = [0.40, 0.45, 0.50]


def resize_ppocr(img):
    h, w = img.shape[:2]
    if h <= 0 or w <= 0:
        return None, 0
    ow = int(np.ceil(PPOCR_IN_H * (w / h)))
    ow = min(ow, PPOCR_IN_W)
    ow = max(ow, 1)
    return cv2.resize(img, (ow, PPOCR_IN_H), interpolation=cv2.INTER_LINEAR), ow


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plates", default="/home/seeed/rk_deploy/accuracy/data/manifests/plates.csv")
    ap.add_argument("--crops", default="/home/seeed/rk_deploy/accuracy/data/plates")
    ap.add_argument("--out", default="/home/seeed/rk_deploy/opt_ebike_fork9/out/gt_tworow")
    ap.add_argument("--limit", type=int, default=40)
    args = ap.parse_args()

    os.makedirs(os.path.join(args.out, "crops"), exist_ok=True)
    rows = []
    n = 0
    with open(args.plates) as fh:
        for r in csv.reader(fh):
            if not r or r[0] == "crop_path":
                continue
            if n >= args.limit:
                break
            crop_path = r[0]
            gt = r[2] if len(r) > 2 else ""
            p = crop_path if os.path.isabs(crop_path) else os.path.join(
                "/home/seeed/rk_deploy/accuracy", crop_path)
            img = cv2.imread(p)
            if img is None:
                continue
            n += 1
            H, W = img.shape[:2]
            for sp in SPLITS:
                cut = int(H * sp)
                if cut <= 0 or cut >= H:
                    continue
                for kind, y0, y1 in (("top", 0, cut), ("bot", cut, H)):
                    strip = img[y0:y1]
                    small, ow = resize_ppocr(strip)
                    if small is None:
                        continue
                    name = "f%05d_%s_%.2f.jpg" % (n, kind, sp)
                    op = os.path.join(args.out, "crops", name)
                    cv2.imwrite(op, small)
                    rows.append([op, gt, n, kind, sp, ow, small.shape[1], small.shape[0]])
    lst = os.path.join(args.out, "crops.csv")
    with open(lst, "w", newline="") as fh:
        wr = csv.writer(fh)
        wr.writerow(["crop_path", "gt_alnum", "frame_idx", "kind", "split",
                     "out_w", "crop_w", "crop_h"])
        wr.writerows(rows)
    print("GT 牌 %d 张, 切分候选 %d 张 -> %s" % (n, len(rows), lst))


if __name__ == "__main__":
    main()
