#!/usr/bin/env python3
"""compare_boxes.py —— 逐框 IoU 对比两个 CSV (格式: img,n_boxes,cls,score,x1,y1,x2,y2,...)"""
import csv, sys

def load(p):
    out = {}
    for r in csv.reader(open(p)):
        if not r or r[0] == "img":
            continue
        nb = int(r[1]); boxes = []
        for i in range(nb):
            b = r[2+6*i:8+6*i]
            boxes.append((int(b[0]), float(b[1]), float(b[2]), float(b[3]), float(b[4]), float(b[5])))
        import os
        out[os.path.basename(r[0])] = boxes
    return out

def iou(a, b):
    ax1, ay1, ax2, ay2 = a[2:6]; bx1, by1, bx2, by2 = b[2:6]
    ix = max(0.0, min(ax2, bx2) - max(ax1, bx1))
    iy = max(0.0, min(ay2, by2) - max(ay1, by1))
    inter = ix*iy
    aa = max(0.0, ax2-ax1)*max(0.0, ay2-ay1)
    bb = max(0.0, bx2-bx1)*max(0.0, by2-by1)
    u = aa+bb-inter
    return inter/u if u > 0 else 0.0

def compare(pa, pb, label_a, label_b, iou_thr=0.5):
    A, B = load(pa), load(pb)
    keys = sorted(set(A) & set(B))
    matched = 0; total_a = 0; total_b = 0; best_iou = []
    for k in keys:
        ba, bb = A[k], B[k]
        total_a += len(ba); total_b += len(bb)
        used = set()
        for a in ba:
            bi, bj = 0.0, -1
            for j, b in enumerate(bb):
                if j in used: continue
                v = iou(a, b)
                if v > bi: bi, bj = v, j
            if bj >= 0 and bi >= iou_thr:
                used.add(bj); matched += 1
                best_iou.append(bi)
    print(f"{label_a}  vs  {label_b}")
    print(f"  帧数={len(keys)}  {label_a}框数={total_a}  {label_b}框数={total_b}")
    print(f"  IoU>={iou_thr} 匹配框数 = {matched}")
    if best_iou:
        best_iou.sort()
        print(f"  匹配框 IoU: min={best_iou[0]:.4f} 中位={best_iou[len(best_iou)//2]:.4f} max={best_iou[-1]:.4f}")
    else:
        print("  匹配框 IoU: 无")
    return matched, total_a, total_b

if __name__ == "__main__":
    compare(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4],
            float(sys.argv[5]) if len(sys.argv) > 5 else 0.5)
