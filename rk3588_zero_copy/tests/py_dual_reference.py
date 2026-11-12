#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""py_dual_reference.py —— 「形态 C」box/cls 双输出的 Python 参考实现。

与 C++ 的 post_process_dual_output() 做数值对拍用。

输入(模拟器 dump):
    <prefix>_box.bin   [1, 4, N] float32  cx,cy,w,h (像素)
    <prefix>_cls.bin   [1, nc, N] float32 概率
    <prefix>_meta.txt  "原图宽 原图高 box通道数 num_anchors"

输出每行: cls score x1 y1 x2 y2

用法: python3 py_dual_reference.py <prefix> [conf] [nms]
"""
import sys

import numpy as np

MODEL_SIZE = 640


def nms_per_class(objs, conf_thr, iou_thr):
    """分类别 NMS(与 C++ nms_per_class 同语义: 逐类内按分数降序贪心抑制)。"""
    out = []
    if not objs:
        return out
    for c in sorted({o[0] for o in objs}):
        cand = [o for o in objs if o[0] == c and o[1] >= conf_thr]
        cand.sort(key=lambda o: -o[1])
        dead = [False] * len(cand)
        for i in range(len(cand)):
            if dead[i]:
                continue
            out.append(cand[i])
            ax1, ay1, ax2, ay2 = cand[i][2], cand[i][3], cand[i][4], cand[i][5]
            a_area = max(0.0, ax2 - ax1) * max(0.0, ay2 - ay1)
            for j in range(i + 1, len(cand)):
                if dead[j]:
                    continue
                bx1, by1, bx2, by2 = cand[j][2], cand[j][3], cand[j][4], cand[j][5]
                iw = min(ax2, bx2) - max(ax1, bx1)
                ih = min(ay2, by2) - max(ay1, by1)
                if iw <= 0 or ih <= 0:
                    continue
                inter = iw * ih
                b_area = max(0.0, bx2 - bx1) * max(0.0, by2 - by1)
                union = a_area + b_area - inter
                iou = inter / union if union > 0 else 0.0
                if iou > iou_thr:
                    dead[j] = True
    out.sort(key=lambda o: -o[1])
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    prefix = sys.argv[1]
    conf = float(sys.argv[2]) if len(sys.argv) > 2 else 0.25
    iou = float(sys.argv[3]) if len(sys.argv) > 3 else 0.45

    with open(prefix + '_meta.txt') as f:
        img_w, img_h, box_ch, num_anchors = [int(x) for x in f.read().split()]
    box = np.fromfile(prefix + '_box.bin', dtype=np.float32).reshape(1, box_ch, num_anchors)
    cls_flat = np.fromfile(prefix + '_cls.bin', dtype=np.float32)
    nc = cls_flat.size // num_anchors
    cls = cls_flat.reshape(1, nc, num_anchors)

    print('# 图 %dx%d box通道 %d num_anchors %d nc %d conf %.2f nms %.2f'
          % (img_w, img_h, box_ch, num_anchors, nc, conf, iou), file=sys.stderr)

    objs = []
    for a in range(num_anchors):
        scores = cls[0, :, a]
        c = int(np.argmax(scores))
        s = float(scores[c])
        if s < conf:
            continue
        cx = float(box[0, 0, a])
        cy = float(box[0, 1, a])
        w = float(box[0, 2, a])
        h = float(box[0, 3, a])
        objs.append([c, s, cx - w * 0.5, cy - h * 0.5, cx + w * 0.5, cy + h * 0.5])

    objs = nms_per_class(objs, conf, iou)

    sx = img_w / float(MODEL_SIZE)
    sy = img_h / float(MODEL_SIZE)
    for o in objs:
        x1 = min(max(o[2] * sx, 0.0), img_w)
        x2 = min(max(o[4] * sx, 0.0), img_w)
        y1 = min(max(o[3] * sy, 0.0), img_h)
        y2 = min(max(o[5] * sy, 0.0), img_h)
        print('%d %.6f %.2f %.2f %.2f %.2f' % (o[0], o[1], x1, y1, x2, y2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
