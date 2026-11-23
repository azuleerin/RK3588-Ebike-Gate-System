#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""build_ebike_calib.py —— 造 ebike 的 INT8 量化校准集

★ 为什么要自己造
  任务书要求用算法组的 `calib/ebike_calib.txt`。那份文件跟 best_split.onnx 一样
  在算法组的训练仓里, 本机拿不到(见 docs/00_source_not_available.md)。
  本脚本用【仓库里现成的 ebike 域视频帧】造一份等价的校准集。

★ 帧从哪来
  候选池:
    · accuracy/data/frames/   1724 帧 —— 精度回归的测试集(门禁相机域, 含电动车)
    · dataset/                2001 帧
    · dataset_mp4/            1664 帧
  默认用 dataset/ + dataset_mp4/ 采样, 【与回归测试集不相交】——
  避免"校准集 = 测试集"带来的乐观偏差。

★ 域的判定不靠肉眼, 靠生产模型自己
  scripts/check_calib_domain.py 会用生产 ebike 模型在这批帧上跑一遍, 报最高分:
  门禁相机域最高分 0.79~0.87, 行车记录仪域 0.008~0.068(见
  rk3588_zero_copy/docs/EBIKE_MODEL_RUNBOOK.md 第五节)。

用法:
    build_ebike_calib.py --out calib/ebike_calib.txt --n 200
"""
import argparse
import glob
import os
import random

ROOT = '/home/seeed/rk_deploy'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.dirname(os.path.abspath(__file__)) +
                    '/../calib/ebike_calib.txt')
    ap.add_argument('--n', type=int, default=200)
    ap.add_argument('--seed', type=int, default=20261122)
    ap.add_argument('--pool', default='dataset,dataset_mp4',
                    help='逗号分隔的帧目录名(相对 rk_deploy 根目录)')
    ap.add_argument('--include-test', action='store_true',
                    help='把 accuracy/data/frames 也算进候选池(默认不算)')
    args = ap.parse_args()

    pools = args.pool.split(',')
    files = []
    for p in pools:
        pat = os.path.join(ROOT, p, '*.jpg')
        got = sorted(glob.glob(pat))
        print(f"  候选池 {p}: {len(got)} 帧")
        files += got
    if args.include_test:
        got = sorted(glob.glob(os.path.join(ROOT, 'accuracy/data/frames/*.jpg')))
        print(f"  候选池 accuracy/data/frames: {len(got)} 帧")
        files += got
    if not files:
        raise SystemExit('候选池为空')

    random.Random(args.seed).shuffle(files)
    take = files[:args.n]
    out = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'w') as f:
        for x in take:
            f.write(x + '\n')
    print(f"  -> {out}  ({len(take)} 张)")


if __name__ == '__main__':
    main()
