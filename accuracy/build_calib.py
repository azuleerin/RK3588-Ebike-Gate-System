#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""build_calib.py —— 造量化校准集(用来回答"校准集够不够")

产四份清单:
  calib_yolo_24.txt    YOLO 用: 现有的 24 张(工程里已经在用的那套)
  calib_yolo_200.txt   YOLO 用: 200 张, 从视频里均匀抽(覆盖镜头移动/远近变化)
  calib_rec_24.txt     识别用: 24 张车牌小图
  calib_rec_200.txt    识别用: 200 张车牌小图

★ 为什么要"24 vs 200"两组:
  现有校准集只有 24 张, 而且全是同一个场景的相邻帧(实测 24 张的平均亮度
  标准差极小)—— 量化时这种校准集会把激活范围估得很窄, 一换场景就掉点。
  做成 24 vs 200 的对照, 量化掉点的差值就能直接归因到"校准集"。
"""
import argparse
import glob
import os
import sys


def write_list(path, files):
    with open(path, 'w') as f:
        for x in files:
            f.write(os.path.abspath(x) + '\n')
    print('  %-24s %d 张' % (os.path.basename(path), len(files)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--outdir', default='calib')
    ap.add_argument('--frames-glob', default='data/frames/*.jpg')
    ap.add_argument('--plates-glob', default='data/plates_serial/*.jpg')
    ap.add_argument('--current-calib', default='../_bench/calib_list.txt')
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)

    # ---- YOLO: 现有 24 张(直接复用工程里的清单) ----
    cur = []
    if os.path.exists(args.current_calib):
        with open(args.current_calib) as f:
            cur = [l.strip() for l in f if l.strip()]
    write_list(os.path.join(args.outdir, 'calib_yolo_24.txt'), cur)

    # ---- YOLO: 200 张(从 1724 张里均匀抽, 覆盖镜头移动) ----
    frames = sorted(glob.glob(args.frames_glob))
    step = max(1, len(frames) // 200)
    write_list(os.path.join(args.outdir, 'calib_yolo_200.txt'), frames[::step][:200])

    # ---- 识别: 24 / 200 张车牌小图 ----
    plates = sorted(glob.glob(args.plates_glob))
    write_list(os.path.join(args.outdir, 'calib_rec_24.txt'), plates[:24])
    step = max(1, len(plates) // 200)
    write_list(os.path.join(args.outdir, 'calib_rec_200.txt'), plates[::step][:200])
    return 0


if __name__ == '__main__':
    sys.exit(main())
