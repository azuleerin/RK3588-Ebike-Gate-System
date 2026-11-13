#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""aggregate.py —— 汇总精度回归结果, 出 CSV + ACCURACY_REPORT.md

输入:
  out/boxes_<tag>.csv      检测结果(每个模型一份)
  out/rec_<tag>.csv        识别结果(每个模型一份)
输出:
  out/summary_detection.csv  检测侧: 每个模型对的精度对比
  out/summary_recognition.csv 识别侧: 每个模型对的精度对比
  ACCURACY_REPORT.md        人看的报告
"""
import csv
import os
import sys

OUT = 'out'


# ---------------------------------------------------------------------------
def read_boxes(path):
    """-> {img: [(cls, score, x1,y1,x2,y2), ...]}"""
    res = {}
    if not os.path.exists(path):
        return res
    with open(path) as f:
        for row in csv.reader(f):
            if not row or row[0] == 'img':
                continue
            img = row[0]
            n = int(row[1]) if len(row) > 1 and row[1] else 0
            boxes = []
            for i in range(n):
                base = 2 + i * 7
                try:
                    boxes.append((int(row[base]), float(row[base + 1]),
                                  float(row[base + 2]), float(row[base + 3]),
                                  float(row[base + 4]), float(row[base + 5])))
                except (IndexError, ValueError):
                    break
            res[img] = boxes
    return res


def iou(a, b):
    ax1, ay1, ax2, ay2 = a[2], a[3], a[4], a[5]
    bx1, by1, bx2, by2 = b[2], b[3], b[4], b[5]
    ix1, iy1 = max(ax1, bx1), max(ay1, by1)
    ix2, iy2 = min(ax2, bx2), min(ay2, by2)
    iw, ih = max(0.0, ix2 - ix1), max(0.0, iy2 - iy1)
    inter = iw * ih
    aa = max(0.0, ax2 - ax1) * max(0.0, ay2 - ay1)
    ba = max(0.0, bx2 - bx1) * max(0.0, by2 - by1)
    u = aa + ba - inter
    return inter / u if u > 1e-9 else 0.0


def match_boxes(ref, test, iou_thr=0.5):
    """把 test 框匹配到 ref 框(IoU>=thr, 不看类别, 类别一致性单独统计)。
    返回 (匹配对列表[(ref_box, test_box, iou)], ref 未匹配数, test 未匹配数)"""
    pairs = []
    used = set()
    for rb in sorted(ref, key=lambda b: -b[1]):
        best, best_i = None, iou_thr
        for i, tb in enumerate(test):
            if i in used:
                continue
            v = iou(rb, tb)
            if v >= best_i:
                best, best_i = i, v
        if best is not None:
            used.add(best)
            pairs.append((rb, test[best], best_i))
    return pairs, len(ref) - len(pairs), len(test) - len(pairs)


def pseudo_map50(ref_map, test_map, iou_thr=0.5):
    """以 FP16 的框当"真值", 算 INT8 的 mAP@0.5 (COCO 式 AP: 按分数排序, 贪心匹配)。
    ★ 没有人工标注时的替代口径 —— 量化掉点主要体现在"边界框附近的分数翻转",
      这个指标能把它量化出来。"""
    # 收集所有类别
    classes = set()
    for boxes in ref_map.values():
        for b in boxes:
            classes.add(b[0])
    aps = []
    for c in sorted(classes):
        # 真值(FP16 的框)
        gts = []
        for img, boxes in ref_map.items():
            for b in boxes:
                if b[0] == c:
                    gts.append((img, b))
        # 预测(INT8 的框), 全局按分数降序
        preds = []
        for img, boxes in test_map.items():
            for b in boxes:
                if b[0] == c:
                    preds.append((img, b))
        if not gts:
            continue
        preds.sort(key=lambda x: -x[1][1])
        used = set()
        tp, fp = [], []
        for img, b in preds:
            best, best_i = None, iou_thr
            for gi, (gimg, gb) in enumerate(gts):
                if gi in used or gimg != img:
                    continue
                v = iou(gb, b)
                if v >= best_i:
                    best, best_i = gi, v
            if best is None:
                tp.append(0); fp.append(1)
            else:
                used.add(best)
                tp.append(1); fp.append(0)
        if not tp:
            aps.append(0.0)
            continue
        ctp = cfp = 0
        prec = []
        rec = []
        for i in range(len(tp)):
            ctp += tp[i]; cfp += fp[i]
            prec.append(ctp / max(1, ctp + cfp))
            rec.append(ctp / len(gts))
        # 11 点插值法(和 COCO 的 area 近似)
        ap = 0.0
        for t in [0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0]:
            p = max([pp for pp, rr in zip(prec, rec) if rr >= t] or [0.0])
            ap += p / 11.0
        aps.append(ap)
    return (sum(aps) / len(aps) if aps else 0.0), len(aps)


def detection_compare(ref_map, test_map, iou_thr=0.5):
    """以 ref 为准, 统计 test 的量化损失"""
    stats = {
        'frames': 0, 'ref_boxes': 0, 'test_boxes': 0, 'matched': 0,
        'ref_missed': 0, 'test_extra': 0, 'cls_agree': 0,
        'score_delta_sum': 0.0, 'iou_sum': 0.0,
        'per_class': {}, 'frames_ref_has_test_none': 0, 'frames_test_has_ref_none': 0,
        'missed_ref_score_sum': 0.0, 'missed_ref_score_n': 0,
        'extra_score_sum': 0.0, 'extra_score_n': 0,
        'missed_near_thr': 0, 'extra_near_thr': 0,
        'missed_best_iou_sum': 0.0, 'missed_best_iou_n': 0,
        'iou_bucket_shifted': 0, 'iou_bucket_partial': 0,
        'iou_bucket_different': 0, 'iou_bucket_gone': 0,
    }
    for img, ref_boxes in ref_map.items():
        test_boxes = test_map.get(img, [])
        stats['frames'] += 1
        stats['ref_boxes'] += len(ref_boxes)
        stats['test_boxes'] += len(test_boxes)
        if ref_boxes and not test_boxes:
            stats['frames_ref_has_test_none'] += 1
        if test_boxes and not ref_boxes:
            stats['frames_test_has_ref_none'] += 1
        pairs, rm, tm = match_boxes(ref_boxes, test_boxes, iou_thr)
        stats['matched'] += len(pairs)
        stats['ref_missed'] += rm
        stats['test_extra'] += tm
        # 漏检的框在 FP16 里的分数 —— 如果集中在阈值附近, 说明量化只是把
        # "擦边框"推过了阈值, 属于可接受的小扰动, 不是"目标彻底看不见了"
        matched_ref = set()
        for rb, tb, v in pairs:
            matched_ref.add(id(rb))
        for rb in ref_boxes:
            if id(rb) not in matched_ref:
                stats['missed_ref_score_sum'] += rb[1]
                stats['missed_ref_score_n'] += 1
                if rb[1] < 0.35:
                    stats['missed_near_thr'] += 1
        # 漏检的框跟 INT8 的框到底差多少?(区分"挪了几像素"和"彻底没了")
        for rb in ref_boxes:
            if id(rb) in matched_ref:
                continue
            best = 0.0
            for tb in test_boxes:
                v = iou(rb, tb)
                if v > best:
                    best = v
            stats['missed_best_iou_sum'] += best
            stats['missed_best_iou_n'] += 1
            if best >= 0.45:
                stats['iou_bucket_shifted'] += 1      # 几乎重合, IoU 差一点点
            elif best >= 0.2:
                stats['iou_bucket_partial'] += 1
            elif best >= 0.05:
                stats['iou_bucket_different'] += 1
            else:
                stats['iou_bucket_gone'] += 1        # 完全对不上
        # 多检的框在 INT8 里的分数
        matched_test = set()
        for rb, tb, v in pairs:
            matched_test.add(id(tb))
        for tb in test_boxes:
            if id(tb) not in matched_test:
                stats['extra_score_sum'] += tb[1]
                stats['extra_score_n'] += 1
                if tb[1] < 0.35:
                    stats['extra_near_thr'] += 1
        for rb, tb, v in pairs:
            stats['iou_sum'] += v
            stats['score_delta_sum'] += tb[1] - rb[1]
            if rb[0] == tb[0]:
                stats['cls_agree'] += 1
            pc = stats['per_class'].setdefault(rb[0], {'n': 0, 'dsum': 0.0, 'ref_sum': 0.0,
                                                        'agree': 0})
            pc['n'] += 1
            pc['dsum'] += tb[1] - rb[1]
            pc['ref_sum'] += rb[1]
            pc['agree'] += (rb[0] == tb[0])
    return stats


def fmt_pct(x):
    return '%.2f%%' % (x * 100.0)


# ---------------------------------------------------------------------------
# 识别: 整牌准确率 + 单字符准确率(Levenshtein 对齐)
# ---------------------------------------------------------------------------
def levenshtein(a, b):
    """返回 (对齐后的 [(a_char, b_char 或 None)], 编辑距离)"""
    n, m = len(a), len(b)
    dp = [[0] * (m + 1) for _ in range(n + 1)]
    for i in range(n + 1):
        dp[i][0] = i
    for j in range(m + 1):
        dp[0][j] = j
    for i in range(1, n + 1):
        for j in range(1, m + 1):
            cost = 0 if a[i - 1] == b[j - 1] else 1
            dp[i][j] = min(dp[i - 1][j] + 1, dp[i][j - 1] + 1, dp[i - 1][j - 1] + cost)
    # 回溯
    aln = []
    i, j = n, m
    while i > 0 or j > 0:
        if i > 0 and j > 0 and dp[i][j] == dp[i - 1][j - 1] + (0 if a[i - 1] == b[j - 1] else 1):
            aln.append((a[i - 1], b[j - 1]))
            i -= 1; j -= 1
        elif i > 0 and dp[i][j] == dp[i - 1][j] + 1:
            aln.append((a[i - 1], None))
            i -= 1
        else:
            aln.append((None, b[j - 1]))
            j -= 1
    aln.reverse()
    return aln, dp[n][m]


def read_rec(path, pred_col='ppocr_text'):
    """-> [(crop_path, gt, pred, score)]
    ★ 注意: 一个 CSV 里同时有 lpr_text 和 ppocr_text 两列, 要按列名取,
      取错列会得到"全是空"的假象。"""
    res = []
    if not os.path.exists(path):
        return res
    with open(path) as f:
        for row in csv.DictReader(f):
            pred = row.get(pred_col, '') or ''
            if not pred and pred_col != 'lpr_text':
                pred = row.get('lpr_text', '') or ''
            res.append((row['crop_path'], row.get('gt', ''), pred,
                        float(row.get('ppocr_score', 0) or 0)))
    return res


def rec_stats(rows):
    n = len(rows)
    if n == 0:
        return {'n': 0}
    exact = 0
    char_ok = 0
    char_total = 0
    empty = 0
    dist = {}
    for path, gt, pred, score in rows:
        gt = (gt or '').strip()
        pred = (pred or '').strip()
        if not pred:
            empty += 1
        if gt and pred == gt:
            exact += 1
        if gt:
            aln, _ = levenshtein(gt, pred)
            for g, p in aln:
                char_total += 1
                if g is not None and g == p:
                    char_ok += 1
        dist[pred] = dist.get(pred, 0) + 1
    return {
        'n': n, 'exact': exact, 'exact_rate': exact / n,
        'char_ok': char_ok, 'char_total': char_total,
        'char_rate': (char_ok / char_total) if char_total else 0.0,
        'empty_rate': empty / n, 'dist': dist,
    }


# ---------------------------------------------------------------------------
def main():
    os.makedirs(OUT, exist_ok=True)

    # ---------------- 检测侧 ----------------
    det_models = {
        'yolov8n_FP16(基准)': OUT + '/boxes_yolov8n_fp16.csv',
        'yolov8n_INT8_calib24(现有校准集)': OUT + '/boxes_yolov8n_int8_calib24.csv',
        'yolov8n_INT8_calib200(新校准集)': OUT + '/boxes_yolov8n_int8_calib200.csv',
        'ebike_INT8(在用模型)': OUT + '/boxes_ebike_int8.csv',
    }
    maps = {k: read_boxes(v) for k, v in det_models.items()}
    ref = maps.get('yolov8n_FP16(基准)')
    rows = []
    for name, m in maps.items():
        if not m:
            continue
        if ref is None or name == 'yolov8n_FP16(基准)':
            continue
        st = detection_compare(ref, m)
        rows.append((name, st))

    with open(OUT + '/summary_detection.csv', 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['model', 'frames', 'ref_boxes', 'test_boxes', 'matched',
                    'match_rate', 'ref_missed', 'test_extra', 'cls_agree_rate',
                    'mean_iou', 'mean_score_delta', 'frames_ref_has_test_none',
                    'map50_vs_fp16', 'missed_ref_mean_score', 'missed_near_thr(0.35)',
                    'extra_mean_score', 'extra_near_thr(0.35)',
                    'missed_best_iou', 'bucket_shifted(>=0.45)',
                    'bucket_partial(0.2-0.45)', 'bucket_different(0.05-0.2)',
                    'bucket_gone(<0.05)'])
        for name, st in rows:
            mp, ncls = pseudo_map50(ref, maps[name]) if ref is not None else (0.0, 0)
            w.writerow([name, st['frames'], st['ref_boxes'], st['test_boxes'],
                        st['matched'],
                        '%.4f' % (st['matched'] / max(1, st['ref_boxes'])),
                        st['ref_missed'], st['test_extra'],
                        '%.4f' % (st['cls_agree'] / max(1, st['matched'])),
                        '%.4f' % (st['iou_sum'] / max(1, st['matched'])),
                        '%.4f' % (st['score_delta_sum'] / max(1, st['matched'])),
                        st['frames_ref_has_test_none'],
                        '%.4f' % mp,
                        '%.4f' % (st['missed_ref_score_sum'] / max(1, st['missed_ref_score_n'])),
                        st['missed_near_thr'],
                        '%.4f' % (st['extra_score_sum'] / max(1, st['extra_score_n'])),
                        st['extra_near_thr'],
                        '%.4f' % (st['missed_best_iou_sum'] / max(1, st['missed_best_iou_n'])),
                        st['iou_bucket_shifted'], st['iou_bucket_partial'],
                        st['iou_bucket_different'], st['iou_bucket_gone']])

    # 每个类别的置信度变化(用户明确要的)
    with open(OUT + '/summary_detection_by_class.csv', 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['model', 'cls_id', 'matched', 'ref_mean_score',
                    'test_mean_score', 'score_delta', 'cls_agree_rate'])
        for name, st in rows:
            for cid, pc in sorted(st['per_class'].items()):
                w.writerow([name, cid, pc['n'],
                            '%.4f' % (pc['ref_sum'] / max(1, pc['n'])),
                            '%.4f' % ((pc['ref_sum'] + pc['dsum']) / max(1, pc['n'])),
                            '%.4f' % (pc['dsum'] / max(1, pc['n'])),
                            '%.4f' % (pc['agree'] / max(1, pc['n']))])

    # ---------------- 识别侧 ----------------
    rec_models = [
        ('LPRNet_INT8(在用模型)', OUT + '/rec_lprnet_int8.csv', 'lpr_text'),
        ('PPOCR_FP16(基准)', OUT + '/rec_ppocr_fp16.csv', 'ppocr_text'),
        ('PPOCR_INT8_calib24', OUT + '/rec_ppocr_int8_calib24.csv', 'ppocr_text'),
        ('PPOCR_INT8_calib200', OUT + '/rec_ppocr_int8_calib200.csv', 'ppocr_text'),
    ]
    rec_rows = []
    for name, path, col in rec_models:
        rr = read_rec(path, col)
        if not rr:
            continue
        rec_rows.append((name, rec_stats(rr)))

    with open(OUT + '/summary_recognition.csv', 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['model', 'n', 'exact', 'exact_rate', 'char_total', 'char_ok',
                    'char_rate', 'empty_rate'])
        for name, st in rec_rows:
            w.writerow([name, st['n'], st['exact'], '%.4f' % st['exact_rate'],
                        st['char_total'], st['char_ok'], '%.4f' % st['char_rate'],
                        '%.4f' % st['empty_rate']])

    print('检测侧:')
    for name, st in rows:
        mp, ncls = pseudo_map50(ref, maps[name]) if ref is not None else (0.0, 0)
        print('  %-38s 匹配率 %.1f%%  mAP50 %.1f%%  漏检 %d(均分%.3f, 阈值附近%d)  多检 %d  '
              '类别一致 %.1f%%  置信度变化 %+.4f' %
              (name, 100.0 * st['matched'] / max(1, st['ref_boxes']), 100 * mp,
               st['ref_missed'],
               st['missed_ref_score_sum'] / max(1, st['missed_ref_score_n']),
               st['missed_near_thr'], st['test_extra'],
               100.0 * st['cls_agree'] / max(1, st['matched']),
               st['score_delta_sum'] / max(1, st['matched'])))
        print('      漏检框的最佳 IoU 分布: 几乎重合 %d / 部分重合 %d / 完全不同 %d / 完全消失 %d' %
              (st['iou_bucket_shifted'], st['iou_bucket_partial'],
               st['iou_bucket_different'], st['iou_bucket_gone']))
    print('识别侧:')
    for name, st in rec_rows:
        print('  %-38s 整牌 %.1f%%  单字 %.1f%%  空输出 %.1f%%' %
              (name, 100 * st['exact_rate'], 100 * st['char_rate'], 100 * st['empty_rate']))
    print('\n已写: %s/summary_detection.csv, %s/summary_detection_by_class.csv, %s/summary_recognition.csv' %
          (OUT, OUT, OUT))
    return 0


if __name__ == '__main__':
    sys.exit(main())
