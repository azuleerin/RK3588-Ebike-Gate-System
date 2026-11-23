#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""eval_ocr.py —— OCR 候选模型的精度对比 + 逐位错误率

★ 直接复用 accuracy/aggregate.py 的 read_rec / rec_stats / levenshtein,
  保证口径和 2026-11-13 那份 ACCURACY_REPORT.md【完全一致】
  (整牌准确率 / 单字符准确率 / 空输出率 都是同一套算法算的)。

额外加: 逐位错误率 (第 1~7 位各错多少), 用来定位"混合量化把哪一位弄坏了"。

用法: eval_ocr.py <acc_dir> <tag1> [tag2 ...]
      tag 对应 <acc_dir>/out/rec_<tag>.csv
"""
import collections
import csv
import os
import sys

sys.path.insert(0, "/home/seeed/rk_deploy/accuracy")
import aggregate as AG  # noqa: E402


def pos_errors(rows, maxlen=10):
    """逐位错误率: 真值第 i 位 vs 识别第 i 位"""
    ok = [0] * maxlen
    tot = [0] * maxlen
    for path, gt, pred, score in rows:
        gt = (gt or '').strip()
        pred = (pred or '').strip()
        aln, _ = AG.levenshtein(gt, pred)
        # 对齐结果里 g 是真值字符, p 是预测字符; 按真值顺序重建
        gi = 0
        for g, p in aln:
            if g is None:
                continue          # 插入(多出来的字符)
            if gi < maxlen:
                tot[gi] += 1
                if p == g:
                    ok[gi] += 1
            gi += 1
    return ok, tot


def main():
    acc = sys.argv[1]
    tags = sys.argv[2:]
    out_dir = os.path.join(acc, "out")

    print("=" * 96)
    print(f"OCR 精度对比  (样本 = accuracy/data/manifests/plates_serial.csv, 真值 A339058)")
    print("=" * 96)
    print(f"{'模型':<26} {'样本':>5} {'整牌准确率':>12} {'单字符准确率':>13} {'空输出率':>10}")

    results = {}
    for tag in tags:
        path = os.path.join(out_dir, f"rec_{tag}.csv")
        if not os.path.exists(path):
            print(f"{tag:<26}  (缺 {path})")
            continue
        rows = AG.read_rec(path, 'ppocr_text')
        st = AG.rec_stats(rows)
        results[tag] = (st, rows)
        print(f"{tag:<26} {st['n']:>5} {st['exact_rate'] * 100:>11.2f}% "
              f"{st['char_rate'] * 100:>12.2f}% {st['empty_rate'] * 100:>9.2f}%")

    # 逐位错误率
    if len(results) >= 1:
        print()
        print("=== 逐位错误率 (每位的正确率 %) ===")
        hdr = "".join(f"{'第'+str(i+1)+'位':>9}" for i in range(7))
        print(f"{'模型':<26}{hdr}")
        for tag, (st, rows) in results.items():
            ok, tot = pos_errors(rows)
            cells = ""
            for i in range(7):
                cells += f"{(ok[i] / tot[i] * 100 if tot[i] else 0):>9.1f}" if tot[i] else f"{'-':>9}"
            print(f"{tag:<26}{cells}")

        print()
        print("=== 输出分布 (前 5 种) ===")
        for tag, (st, rows) in results.items():
            top = sorted(st['dist'].items(), key=lambda kv: -kv[1])[:5]
            print(f"  {tag}: " + ", ".join(f"{k!r}×{v}" for k, v in top))


if __name__ == "__main__":
    main()
