#!/usr/bin/env python3
"""quant_compare.py —— INT8 vs FP16 检测结果对比(量化精度回归)

用法:
    python3 scripts/quant_compare.py <int8.json> <fp16.json> [--iou 0.5] [--csv out.csv]

两份 JSON 来自 rk_eval(同一个 ONNX 编出来的 INT8 / FP16 两个模型,
喂完全相同的图片)。

指标说明(重要):
    * 【检测 mAP】= 以 FP16 的检测结果为"真值", 算 INT8 的 mAP。
      这是量化回归的标准做法(teacher-student 一致性): 没有人工标注时,
      它衡量的是"量化把检测结果改动了多少"。
    * 同时输出 IoU 分布、置信度漂移、漏检/多检数量 —— 这几个指标比 mAP
      更能说明"量化到底伤在哪"。
"""
from __future__ import annotations

import argparse
import csv
import json
import sys
from collections import defaultdict


def load(path):
    d = json.load(open(path))
    return {r["file"]: r["dets"] for r in d["results"]}


def iou(a, b):
    ax1, ay1, ax2, ay2 = a
    bx1, by1, bx2, by2 = b
    ix1, iy1 = max(ax1, bx1), max(ay1, by1)
    ix2, iy2 = min(ax2, bx2), min(ay2, by2)
    iw, ih = max(0.0, ix2 - ix1), max(0.0, iy2 - iy1)
    inter = iw * ih
    area_a = max(0.0, ax2 - ax1) * max(0.0, ay2 - ay1)
    area_b = max(0.0, bx2 - bx1) * max(0.0, by2 - by1)
    union = area_a + area_b - inter
    return inter / union if union > 0 else 0.0


def match(dets_a, dets_b, thr):
    """把 a 的框和 b 的框按 IoU 匹配(同类 + 贪心按分数降序)。

    返回 (匹配对列表[(a_idx, b_idx, iou)], 未匹配的 a, 未匹配的 b)
    """
    pairs = []
    used_b = set()
    order = sorted(range(len(dets_a)), key=lambda i: -dets_a[i]["s"])
    for i in order:
        best, best_j = 0.0, -1
        for j, d in enumerate(dets_b):
            if j in used_b or d["c"] != dets_a[i]["c"]:
                continue
            v = iou(dets_a[i]["b"], d["b"])
            if v > best:
                best, best_j = v, j
        if best_j >= 0 and best >= thr:
            used_b.add(best_j)
            pairs.append((i, best_j, best))
        else:
            pass
    matched_a = {p[0] for p in pairs}
    unmatched_a = [i for i in range(len(dets_a)) if i not in matched_a]
    unmatched_b = [j for j in range(len(dets_b)) if j not in used_b]
    return pairs, unmatched_a, unmatched_b


def average_precision(prec_rec):
    """VOC 风格 AP(积分), prec_rec = [(precision, recall), ...] 升序 recall"""
    if not prec_rec:
        return 0.0
    # 按 recall 升序做插值
    mrec = [0.0] + [r for _, r in prec_rec] + [prec_rec[-1][1]]
    mpre = [0.0] + [p for p, _ in prec_rec] + [0.0]
    for i in range(len(mpre) - 2, -1, -1):
        mpre[i] = max(mpre[i], mpre[i + 1])
    idx = 0
    ap = 0.0
    for i in range(1, len(mrec)):
        for j in range(idx, len(prec_rec)):
            if prec_rec[j][1] >= mrec[i]:
                idx = j
                break
        else:
            idx = len(prec_rec)
        ap += (mrec[i] - mrec[i - 1]) * mpre[i]
    return ap


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("int8")
    ap.add_argument("fp16")
    ap.add_argument("--iou", type=float, default=0.5)
    ap.add_argument("--csv", default="")
    ap.add_argument("--min-score", type=float, default=0.0,
                    help="只统计置信度 >= 该值的框(滤掉量化噪声)")
    args = ap.parse_args()

    i8 = load(args.int8)
    f16 = load(args.fp16)
    files = sorted(set(i8) | set(f16))

    tp = fp = fn = 0
    ious = []
    dconf = []
    per_class = defaultdict(lambda: {"tp": 0, "fp": 0, "fn": 0})
    n_i8 = n_f16 = 0
    # 置信度排序的 PR 曲线(以 FP16 为真值)
    scores = []
    for f in files:
        a = [d for d in i8.get(f, []) if d["s"] >= args.min_score]
        b = [d for d in f16.get(f, []) if d["s"] >= args.min_score]
        n_i8 += len(a)
        n_f16 += len(b)
        pairs, ua, ub = match(a, b, args.iou)
        for (i, j, v) in pairs:
            tp += 1
            ious.append(v)
            dconf.append(abs(a[i]["s"] - b[j]["s"]))
            per_class[a[i]["c"]]["tp"] += 1
        fp += len(ua)
        fn += len(ub)
        for i in ua:
            per_class[a[i]["c"]]["fp"] += 1
        for j in ub:
            per_class[b[j]["c"]]["fn"] += 1
        # PR 曲线用点
        for i in ua:
            scores.append((a[i]["s"], 0, 0))   # 假阳
        for (i, j, v) in pairs:
            scores.append((a[i]["s"], 1, 0))   # 真阳
    scores.sort(key=lambda x: -x[0])

    # 按分数累计算 PR 与 AP
    cum_tp = cum_fp = 0
    pr_points = []
    for (sc, is_tp, _) in scores:
        if is_tp:
            cum_tp += 1
        else:
            cum_fp += 1
        prec = cum_tp / max(1, cum_tp + cum_fp)
        rec = cum_tp / max(1, n_f16)
        pr_points.append((prec, rec))
    ap = average_precision(pr_points)

    prec = tp / max(1, tp + fp)
    recall = tp / max(1, tp + fn)
    f1 = 2 * prec * recall / max(1e-9, prec + recall)
    mean_iou = sum(ious) / len(ious) if ious else 0.0
    mean_dconf = sum(dconf) / len(dconf) if dconf else 0.0

    print("=" * 68)
    print("INT8 vs FP16 检测一致性(FP16 作为参照真值)")
    print("=" * 68)
    print(f"  图片数            : {len(files)}")
    print(f"  IoU 匹配阈值     : {args.iou}")
    print(f"  INT8 检测框       : {n_i8}")
    print(f"  FP16 检测框       : {n_f16}")
    print(f"  匹配(TP) / 多检(FP) / 漏检(FN) : {tp} / {fp} / {fn}")
    print(f"  精确率 / 召回率 / F1 : {prec*100:.2f}% / {recall*100:.2f}% / {f1*100:.2f}%")
    print(f"  mAP (VOC 积分, 以 FP16 为真值) : {ap*100:.2f}%")
    print(f"  匹配框平均 IoU    : {mean_iou:.4f}")
    print(f"  匹配框平均置信度漂移: {mean_dconf:.4f}")
    print()
    print("  逐类:")
    for c in sorted(per_class):
        d = per_class[c]
        p = d["tp"] / max(1, d["tp"] + d["fp"])
        r = d["tp"] / max(1, d["tp"] + d["fn"])
        print(f"    cls {c}: TP={d['tp']:6d} FP={d['fp']:6d} FN={d['fn']:6d}  "
              f"P={p*100:6.2f}%  R={r*100:6.2f}%")

    if args.csv:
        with open(args.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["metric", "value"])
            w.writerow(["images", len(files)])
            w.writerow(["iou_threshold", args.iou])
            w.writerow(["int8_dets", n_i8])
            w.writerow(["fp16_dets", n_f16])
            w.writerow(["tp", tp])
            w.writerow(["fp", fp])
            w.writerow(["fn", fn])
            w.writerow(["precision", f"{prec:.6f}"])
            w.writerow(["recall", f"{recall:.6f}"])
            w.writerow(["f1", f"{f1:.6f}"])
            w.writerow(["mAP_vs_fp16", f"{ap:.6f}"])
            w.writerow(["mean_iou", f"{mean_iou:.6f}"])
            w.writerow(["mean_conf_drift", f"{mean_dconf:.6f}"])
        print(f"\n  已写入: {args.csv}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
