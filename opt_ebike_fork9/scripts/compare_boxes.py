#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""compare_boxes.py —— 精度回归对比(复用仓库 accuracy/aggregate.py 的判定口径)

★ 为什么不直接改 accuracy/aggregate.py
  aggregate.py 的 det_models 是写死的字典, 加模型就要改那个文件 —— 那是共享文件。
  本脚本 import 它的 read_boxes / iou / match_boxes / pseudo_map50 / detection_compare,
  判定口径【逐行一致】, 但模型对由命令行指定, 不动共享文件。

★ 判定口径(与 accuracy/aggregate.py 完全一致)
  · 匹配率 = matched / ref_boxes, 匹配条件 IoU >= 0.5, 一对一贪心(按 ref 分数降序)
  · 类别一致率 = 类别相同的匹配对数 / matched
  · 平均 IoU、mAP@0.5 也走 aggregate.py 的同一套实现

用法:
    compare_boxes.py --ref A.csv --test B.csv [--label "名字"] [--json out.json]
"""
import argparse
import importlib.util
import json
import os
import sys

AGG = '/home/seeed/rk_deploy/accuracy/aggregate.py'
spec = importlib.util.spec_from_file_location('agg', AGG)
agg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(agg)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ref', required=True)
    ap.add_argument('--test', required=True)
    ap.add_argument('--label', default=None)
    ap.add_argument('--ref-label', default=None)
    ap.add_argument('--iou', type=float, default=0.5)
    ap.add_argument('--json', default=None)
    args = ap.parse_args()
    if not args.label:
        args.label = os.path.basename(args.test)
    if not args.ref_label:
        args.ref_label = os.path.basename(args.ref)

    ref = agg.read_boxes(args.ref)
    test = agg.read_boxes(args.test)
    st = agg.detection_compare(ref, test, iou_thr=args.iou)
    mp, ncls = agg.pseudo_map50(ref, test, iou_thr=args.iou)

    print('=' * 78)
    print('参考模型: %s   (%d 帧, %d 框)' % (args.ref_label, len(ref), st['ref_boxes']))
    print('被测模型: %s   (%d 帧, %d 框)' % (args.label, len(test), st['test_boxes']))
    print('-' * 78)
    print('  匹配率(match@IoU>=%.2f) : %.2f%%  (%d/%d)'
          % (args.iou, 100.0 * st['matched'] / max(1, st['ref_boxes']),
             st['matched'], st['ref_boxes']))
    print('  类别一致率              : %.2f%%  (%d)'
          % (100.0 * st['cls_agree'] / max(1, st['matched']), st['cls_agree']))
    print('  平均 IoU                : %.4f' % (st['iou_sum'] / max(1, st['matched'])))
    print('  参考漏掉 / 被测多出     : %d / %d' % (st['ref_missed'], st['test_extra']))
    print('  平均分数差(被测-参考)   : %+.4f'
          % (st['score_delta_sum'] / max(1, st['matched'])))
    print('  mAP@0.5 vs 参考         : %.4f' % mp)
    print('  参考有框但被测检不出的帧: %d' % st['frames_ref_has_test_none'])
    print('  漏框最佳 IoU(均值)      : %.4f'
          % (st['missed_best_iou_sum'] / max(1, st['missed_best_iou_n'])))
    print('  漏框 IoU 分桶 >=0.45/0.2-0.45/0.05-0.2/<0.05 : %d / %d / %d / %d'
          % (st['iou_bucket_shifted'], st['iou_bucket_partial'],
             st['iou_bucket_different'], st['iou_bucket_gone']))
    print('=' * 78)

    if args.json:
        with open(args.json, 'w') as f:
            json.dump({'ref': args.ref, 'test': args.test,
                       'label': args.label, 'stats': st}, f, indent=2)
        print('JSON ->', args.json)


if __name__ == '__main__':
    main()
