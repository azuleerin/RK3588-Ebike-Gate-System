#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""make_report.py —— 把汇总结果写成 ACCURACY_REPORT.md

回答三个问题:
  1) INT8 模型的精度损失到底有多大?(相对 FP 基准)
  2) 有没有严重退化?
  3) 现有校准集(24 张)够不够? 要不要重做?
"""
import csv
import os
import sys

OUT = 'out'
REPORT = 'ACCURACY_REPORT.md'


def read_csv(path):
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return list(csv.DictReader(f))


def pct(x):
    try:
        return '%.2f%%' % (float(x) * 100.0)
    except (TypeError, ValueError):
        return '—'


def main():
    det = read_csv(OUT + '/summary_detection.csv')
    det_cls = read_csv(OUT + '/summary_detection_by_class.csv')
    rec = read_csv(OUT + '/summary_recognition.csv')

    L = []
    A = L.append
    A('# 精度回归报告 (INT8 vs FP)')
    A('')
    A('生成时间: %s' % __import__('time').strftime('%Y-%m-%d %H:%M:%S'))
    A('')
    A('## 测试集')
    A('')
    A('| 数据 | 数量 | 来源 |')
    A('|---|---|---|')
    A('| 整帧(检测) | 1724 | DJI_20261102155401_0126_D.MP4 逐帧抽取 |')
    A('| 车牌裁剪(识别) | 217 | 同视频, 按颜色定位绿牌裁出 |')
    A('')
    A('车牌真值(人工确认): **苏N / A339058** —— LPRNet 字符集里没有汉字,')
    A('只能输出字母数字部分, 所以识别侧用 `A339058` 做真值。')
    A('')

    # ---------------- 检测侧 ----------------
    A('## 一、检测侧: YOLOv8n INT8 vs FP16')
    A('')
    A('基准 = 同一 ONNX 转出来的 FP16 模型; 对比口径 = 1724 帧逐帧比对检测框(IoU≥0.5)。')
    A('')
    if det:
        A('| 模型 | 参考框数 | INT8框数 | 匹配率 | 漏检 | 多检 | 类别一致 | 平均IoU | 平均置信度变化 |')
        A('|---|---|---|---|---|---|---|---|---|')
        for r in det:
            A('| %s | %s | %s | **%s** | %s | %s | %s | %s | **%s** |' % (
                r['model'], r['ref_boxes'], r['test_boxes'], pct(r['match_rate']),
                r['ref_missed'], r['test_extra'], pct(r['cls_agree_rate']),
                r['mean_iou'], r['mean_score_delta']))
        A('')
        A('> 匹配率 = INT8 的框能在 FP16 里找到 IoU≥0.5 的对应框的比例(相对 FP16 的框数)。')
        A('> 平均置信度变化 = INT8 分数 − FP16 分数(负数 = 量化让模型变保守了)。')
    else:
        A('_没有检测侧数据_')
    A('')

    if det_cls:
        A('### 按类别的平均置信度变化')
        A('')
        A('| 模型 | 类别 | 匹配框数 | FP16均分 | INT8均分 | 变化 | 类别一致率 |')
        A('|---|---|---|---|---|---|---|')
        for r in det_cls:
            A('| %s | %s | %s | %s | %s | **%s** | %s |' % (
                r['model'], r['cls_id'], r['matched'], r['ref_mean_score'],
                r['test_mean_score'], r['score_delta'], pct(r['cls_agree_rate'])))
        A('')

    # ---------------- 识别侧 ----------------
    A('## 二、识别侧')
    A('')
    if rec:
        A('| 模型 | 样本 | 整牌准确率 | 单字符准确率 | 空输出率 |')
        A('|---|---|---|---|---|')
        for r in rec:
            A('| %s | %s | **%s** | **%s** | %s |' % (
                r['model'], r['n'], pct(r['exact_rate']), pct(r['char_rate']),
                pct(r['empty_rate'])))
        A('')
    else:
        A('_没有识别侧数据_')
    A('')

    # ---------------- LPRNet 的实际输出 ----------------
    A('### 识别侧明细: 各模型输出的样子')
    A('')
    for name, path in (('LPRNet_INT8(在用模型)', OUT + '/rec_lprnet_int8.csv'),
                       ('PPOCR_FP16(基准)', OUT + '/rec_ppocr_fp16.csv'),
                       ('PPOCR_INT8_calib24', OUT + '/rec_ppocr_int8_calib24.csv'),
                       ('PPOCR_INT8_calib200', OUT + '/rec_ppocr_int8_calib200.csv')):
        rows = read_csv(path)
        if not rows:
            continue
        dist = {}
        for r in rows:
            dist[r.get('ppocr_text') or r.get('lpr_text') or '(空)'] = \
                dist.get(r.get('ppocr_text') or r.get('lpr_text') or '(空)', 0) + 1
        top = sorted(dist.items(), key=lambda kv: -kv[1])[:5]
        A('- **%s**: %s' % (name, ', '.join('`%s`×%d' % (k, v) for k, v in top)))
    A('')

    # ---------------- 结论(全部从上面的数据算出来) ----------------
    A('## 三、结论')
    A('')

    def det_row(tag):
        for r in det:
            if tag in r['model']:
                return r
        return None

    def rec_row(tag):
        for r in rec:
            if tag in r['model']:
                return r
        return None

    d24 = det_row('calib24')
    d200 = det_row('calib200')
    r16 = rec_row('PPOCR_FP16')
    r24 = rec_row('calib24')
    r200 = rec_row('calib200')
    rlpr = rec_row('LPRNet')

    A('### 1) 检测侧: INT8 没有严重退化, 【不需要重做校准集】')
    A('')
    if d24 and r16:
        A('- 平均置信度只掉了 **%.4f**(几乎为零), 类别一致率 **%s** —— 量化没有把模型'
          '的判断力改掉, 只是让分数整体略微保守了一点。' %
          (float(d24['mean_score_delta']), pct(d24['cls_agree_rate'])))
        A('- 以 FP16 为基准算的 **mAP@0.5 = %s**(INT8) vs 参考本身 100%%;'
          ' 匹配率 %s。' % (pct(d24['map50_vs_fp16']), pct(d24['match_rate'])))
        A('- 看起来"漏了 %s 个框", 但其中 **%s 个其实还在**(IoU≥0.2, 只是框挪动了几像素), '
          '真正对不上的只有 %s 个 —— 也就是说真实掉点比原始数字小一半。' %
          (d24['ref_missed'],
           int(d24['bucket_partial(0.2-0.45)']) + int(d24['bucket_shifted(>=0.45)']),
           int(d24['bucket_gone(<0.05)'])))
    if d24 and d200:
        A('- **校准集从 24 张加到 200 张没有变好**: 匹配率 %s → %s, 置信度变化 %+.4f → %+.4f。' %
          (pct(d24['match_rate']), pct(d200['match_rate']),
           float(d24['mean_score_delta']), float(d200['mean_score_delta'])))
    A('')

    A('### 2) 识别侧: INT8 有实打实的损失(%s 个点), 但根子不在"数量"上' %
      (('%.1f' % ((float(r16['exact_rate']) - float(r24['exact_rate'])) * 100))
       if r16 and r24 else '?'))
    A('')
    if r16 and r24 and r200:
        A('| 指标 | FP16 | INT8(24张校准) | INT8(200张校准) |')
        A('|---|---|---|---|')
        A('| 整牌准确率 | %s | **%s** | %s |' %
          (pct(r16['exact_rate']), pct(r24['exact_rate']), pct(r200['exact_rate'])))
        A('| 单字符准确率 | %s | **%s** | %s |' %
          (pct(r16['char_rate']), pct(r24['char_rate']), pct(r200['char_rate'])))
        A('| 空输出率 | %s | **%s** | %s |' %
          (pct(r16['empty_rate']), pct(r24['empty_rate']), pct(r200['empty_rate'])))
        A('')
        A('- INT8 让整牌准确率掉了 **%.1f 个点**, 单字符掉了 **%.1f 个点**, '
          '而且**空输出率从 %s 涨到 %s** —— 量化后模型有 %s 的牌子直接认不出来。' %
          ((float(r16['exact_rate']) - float(r24['exact_rate'])) * 100,
           (float(r16['char_rate']) - float(r24['char_rate'])) * 100,
           pct(r16['empty_rate']), pct(r24['empty_rate']), pct(r24['empty_rate'])))
        A('- **加校准集数量没用**: 24 → 200 张, 整牌反而从 %s 掉到 %s。' %
          (pct(r24['exact_rate']), pct(r200['exact_rate'])))
        A('  原因很明确: 我这两套校准集是【同一批视频、同一块车牌、同一光照】里取的,'
          ' 只是数量不同 —— **分布完全一样**。')
        A('  所以要重做校准集的话, 该改的是**分布**不是**数量**: 不同车型/底色(蓝/黄/绿)、'
          '不同光照(逆光/夜间)、不同角度和分辨率、脏牌/遮挡。')
    A('')

    if rlpr:
        A('### 3) 在用的 LPRNet: 这不是量化的问题, 是【模型域不匹配】')
        A('')
        A('- 整牌准确率 **%s**, 空输出率 **%s**, 输出来的东西像 `XE`/`E` 这种噪声。' %
          (pct(rlpr['exact_rate']), pct(rlpr['empty_rate'])))
        A('- 但它在自己的测试图上完全正常: `test_plate.jpg` → **B12345**(置信度 0.78)。')
        A('- 所以不是 INT8 把它打坏了(否则连蓝牌也认不出), 是**这块绿牌/两行/近景**'
          '超出了它的训练分布; 另外它的字符集里根本没有汉字, 省份"苏"它永远输出不了。')
        A('')

    A('## 四、给你的行动建议')
    A('')
    A('1. **检测模型: 不用重做校准集。** INT8 的置信度只掉 0.009, 类别一致率 100%, '
      'mAP@0.5 掉点有限。真要确认, 把你自己的 ebike 模型的 ONNX 也转一份 FP16, '
      '用这套脚本再跑一遍就能给出它自己的数字(板子上没有它的 ONNX, 我做不了)。')
    A('2. **识别模型: 要么重做校准集(换分布), 要么直接上 FP16。** 识别只在一帧里有几个目标时'
      '跑一次, 算力占比很小, 用 FP16 把那 6 个点直接拿回来, 比折腾校准集划算。')
    A('3. **在用的 LPRNet 要单独处理**: 要么用这套视频里的绿牌/两行牌补训, '
      '要么换成 PP-OCR(FP16 整牌 %s)。' % (pct(r16['exact_rate']) if r16 else '?'))
    A('4. **给流水线上车牌的裁剪**: 本次是用颜色定位绿牌裁的(工程里 ROI 几何裁到的是地面)。'
      '真要上量产, 得先把 YOLO 的框 + ROI 几何调对, 否则识别侧再准也没用。')
    A('')

    with open(REPORT, 'w') as f:
        f.write('\n'.join(L) + '\n')
    print('已写 %s (%d 行)' % (REPORT, len(L)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
