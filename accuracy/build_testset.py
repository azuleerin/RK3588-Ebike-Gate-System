#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""build_testset.py —— 精度回归测试集的构建

从 DJI 视频里出两套数据:
  1) frames/   整帧(给 YOLO 检测用)      -> 每 --every 帧存一张
  2) plates/   车牌裁剪图(给识别模型用)  -> 按颜色定位绿牌后裁出来

★ 为什么要自己裁: 流水线里的 ROI 几何(--roi-ratio/--roi-y)是按"远景小车"调的,
  在这个近景视频上裁到的是地面, 不是车牌 —— 直接拿它做识别精度测试毫无意义。
  这里用【颜色定位】: 新能源绿牌是饱和的青绿色(H 70~95, S>150, V>90),
  和画面里的树叶(H 35~40, S~110)分得很开。

用法:
    build_testset.py --video V.mp4 --outdir data [--every 1] [--max-frames 0]
"""
import argparse
import csv
import os
import sys

import cv2
import numpy as np

# 绿牌的颜色窗口(实测: 牌 H=83 S=216 V=147; 树叶 H=38 S=110)
HSV_LO = (70, 150, 90)
HSV_HI = (95, 255, 255)


def detect_plate(frame):
    """返回绿牌的 bbox (x, y, w, h); 找不到返回 None。"""
    hsv = cv2.cvtColor(frame, cv2.COLOR_BGR2HSV)
    m = cv2.inRange(hsv, HSV_LO, HSV_HI)
    m = cv2.morphologyEx(m, cv2.MORPH_CLOSE, np.ones((7, 7), np.uint8))
    m = cv2.morphologyEx(m, cv2.MORPH_OPEN, np.ones((5, 5), np.uint8))
    cnts, _ = cv2.findContours(m, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    best = None
    for c in cnts:
        x, y, w, h = cv2.boundingRect(c)
        if w < 60 or h < 18:            # 太小的不可能是牌
            continue
        ar = w / float(h)
        if ar < 1.4 or ar > 6.0:       # 车牌长宽比(透视下会变)
            continue
        area = w * h
        if best is None or area > best[0]:
            best = (area, x, y, w, h)
    if best is None:
        return None
    _, x, y, w, h = best
    return (x, y, w, h)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--video', required=True)
    ap.add_argument('--outdir', required=True)
    ap.add_argument('--every', type=int, default=1, help='每 N 帧取一张')
    ap.add_argument('--max-frames', type=int, default=0, help='0=全部')
    ap.add_argument('--save-frames', action='store_true', help='是否存整帧(给 YOLO 用)')
    ap.add_argument('--jpeg-quality', type=int, default=92)
    args = ap.parse_args()

    frames_dir = os.path.join(args.outdir, 'frames')
    plates_dir = os.path.join(args.outdir, 'plates')
    man_dir = os.path.join(args.outdir, 'manifests')
    for d in (frames_dir, plates_dir, man_dir):
        os.makedirs(d, exist_ok=True)

    cap = cv2.VideoCapture(args.video)
    if not cap.isOpened():
        print('打不开视频:', args.video)
        return 1
    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    fps = cap.get(cv2.CAP_PROP_FPS) or 25.0

    plate_rows = []
    frame_rows = []
    idx = 0
    kept = 0
    while True:
        ok, fr = cap.read()
        if not ok:
            break
        if idx % args.every != 0:
            idx += 1
            continue
        if args.max_frames and kept >= args.max_frames:
            break

        stem = 'f%05d' % idx
        if args.save_frames:
            fp = os.path.join(frames_dir, stem + '.jpg')
            cv2.imwrite(fp, fr, [cv2.IMWRITE_JPEG_QUALITY, args.jpeg_quality])
            frame_rows.append((idx, fp, len(plate_rows)))

        box = detect_plate(fr)
        if box is not None:
            x, y, w, h = box
            # 外扩一点: 绿色掩膜会把牌的边缘(阴影/白色边框)漏掉,
            # 压太紧会把首尾字符切掉(实测 "8" 被切了一半)。左右多留点。
            mx, my = int(w * 0.08), int(h * 0.10)
            x0 = max(0, x - mx); y0 = max(0, y - my)
            x1 = min(fr.shape[1], x + w + mx); y1 = min(fr.shape[0], y + h + my)
            crop = fr[y0:y1, x0:x1]
            if crop.size > 0:
                cp = os.path.join(plates_dir, stem + '.jpg')
                cv2.imwrite(cp, crop, [cv2.IMWRITE_JPEG_QUALITY, args.jpeg_quality])
                plate_rows.append((idx, round(idx / fps, 3), cp, x, y, w, h, x1 - x0, y1 - y0))
        kept += 1
        idx += 1
        if kept % 200 == 0:
            print('  已处理 %d 帧, 裁到 %d 张车牌' % (idx, len(plate_rows)), flush=True)
    cap.release()

    # ★ 清单格式: 第一列必须是【路径】(acc_batch 按第一列读)
    #   plates.csv : crop_path, gt_alnum, gt_serial, frame_idx, ts_sec, x,y,w,h
    #   GT 是【人工确认】的(看联系表确认过): 这块绿牌 = 苏N / A339058
    #   gt_alnum  = 整块的字母数字(苏N + A339058, 去掉省份字"苏") = NA339058
    #   gt_serial = 下面一行的流水号 = A339058
    #   ★ LPRNet 的字符集里【没有汉字】(只有 0-9 A-Z), 所以它只能输出字母数字部分,
    #     省份要靠外部拼(主程序里的 --plate-prefix 就是干这个的)。
    GT_ALNUM = 'NA339058'
    GT_SERIAL = 'A339058'
    with open(os.path.join(man_dir, 'plates.csv'), 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['crop_path', 'gt_alnum', 'gt_serial', 'frame_idx', 'ts_sec',
                    'x', 'y', 'w', 'h', 'crop_w', 'crop_h'])
        for r in plate_rows:
            w.writerow([r[2], GT_ALNUM, GT_SERIAL] + list(r))
    if args.save_frames:
        with open(os.path.join(man_dir, 'frames_list.txt'), 'w') as f:
            for r in frame_rows:
                f.write(r[1] + '\n')
        with open(os.path.join(man_dir, 'frames.csv'), 'w', newline='') as f:
            w = csv.writer(f)
            w.writerow(['frame_idx', 'frame_path', 'n_plate'])
            for r in frame_rows:
                w.writerow(list(r))

    print('完成: 视频总帧数 %d, 处理 %d 帧' % (total, idx))
    print('  整帧 %d 张 -> %s' % (len(frame_rows), frames_dir))
    print('  车牌裁剪 %d 张 -> %s' % (len(plate_rows), plates_dir))
    return 0


if __name__ == '__main__':
    sys.exit(main())
