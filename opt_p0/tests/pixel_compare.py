#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""pixel_compare.py —— 同源对拍: 逐框逐坐标比对两个 CSV 的偏差(像素)

两个 CSV 必须是【同一份模型原始输出】经两条不同解码路径得到的:
  · 生产代码的形态B (瑞芯微 fork 9 输出, CPU 侧 DFL)
  · 生产代码的形态C (双输出, DFL 已经被塞进张量里)
逐框配对(按 IoU 贪心), 报告:
  · 框数是否一致
  · 类别/置信度是否一致
  · 坐标最大偏差(像素) / 平均偏差
"""
import csv
import os
import sys


def load(p):
    out = {}
    for r in csv.reader(open(p)):
        if not r or r[0] == "img":
            continue
        nb = int(r[1])
        bs = []
        for i in range(nb):
            b = r[2+6*i:8+6*i]
            bs.append((int(b[0]), float(b[1]), float(b[2]), float(b[3]), float(b[4]), float(b[5])))
        out[os.path.basename(r[0])] = bs
    return out


def iou(a, b):
    ax1, ay1, ax2, ay2 = a[2:6]
    bx1, by1, bx2, by2 = b[2:6]
    ix = max(0.0, min(ax2, bx2) - max(ax1, bx1))
    iy = max(0.0, min(ay2, by2) - max(ay1, by1))
    inter = ix*iy
    aa = max(0.0, ax2-ax1)*max(0.0, ay2-ay1)
    bb = max(0.0, bx2-bx1)*max(0.0, by2-by1)
    u = aa+bb-inter
    return inter/u if u > 0 else 0.0


def main():
    pa, pb = sys.argv[1], sys.argv[2]
    la, lb = sys.argv[3], sys.argv[4]
    W = H = 1.0
    if len(sys.argv) > 5:
        W, H = [float(x) for x in sys.argv[5].split("x")]
    A, B = load(pa), load(pb)
    keys = sorted(set(A) | set(B))

    n_a = sum(len(A.get(k, [])) for k in keys)
    n_b = sum(len(B.get(k, [])) for k in keys)
    matched = 0
    cls_mismatch = 0
    score_maxdiff = 0.0
    dmax = 0.0
    dsum = 0.0
    dcount = 0
    per_frame = []

    for k in keys:
        ba, bb = A.get(k, []), B.get(k, [])
        used = set()
        fm = 0
        for a in ba:
            bi, bj = 0.0, -1
            for j, b in enumerate(bb):
                if j in used:
                    continue
                v = iou(a, b)
                if v > bi:
                    bi, bj = v, j
            if bj < 0 or bi < 0.5:
                continue
            used.add(bj)
            b = bb[bj]
            matched += 1
            fm += 1
            if a[0] != b[0]:
                cls_mismatch += 1
            score_maxdiff = max(score_maxdiff, abs(a[1]-b[1]))
            # 坐标偏差(像素): x 用图宽, y 用图高
            d = max(abs(a[2]-b[2])*W, abs(a[3]-b[3])*H,
                    abs(a[4]-b[4])*W, abs(a[5]-b[5])*H)
            dmax = max(dmax, d)
            dsum += d
            dcount += 1
        per_frame.append((k, len(ba), len(bb), fm))

    print("=" * 78)
    print(f"同源对拍: {la}  vs  {lb}")
    print("=" * 78)
    print(f"  帧数            : {len(keys)}")
    print(f"  框数            : {la}={n_a}   {lb}={n_b}   {'一致 ✅' if n_a == n_b else '不一致 ❌'}")
    print(f"  配对框数        : {matched}  (IoU>=0.5)")
    print(f"  类别不一致      : {cls_mismatch}   {'✅' if cls_mismatch == 0 else '❌'}")
    print(f"  置信度最大差    : {score_maxdiff:.6f}")
    if dcount:
        print(f"  坐标最大偏差    : {dmax:.6f} px   {'✅ (<1px)' if dmax < 1.0 else '❌ (>=1px)'}")
        print(f"  坐标平均偏差    : {dsum/dcount:.6f} px")
    else:
        print("  坐标偏差        : 无配对框")
    bad = [f for f in per_frame if f[1] != f[2]]
    if bad:
        print(f"  框数不一致的帧  : {len(bad)}  例: {bad[:3]}")
    return 0 if (n_a == n_b and cls_mismatch == 0 and dmax < 1.0 and dcount > 0) else 1


if __name__ == "__main__":
    sys.exit(main())
