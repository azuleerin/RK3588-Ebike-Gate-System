#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""compare_dfl_paths.py —— 量化【DFL 放 NPU】 vs 【DFL 放 CPU】的坐标误差

输入: probe_raw_outputs.py 的 dump (同一帧、同一预处理)
  fork9_f0_o{0..8}.npy  -> 9 输出 (原始 DFL logits + cls + sum)
  formC_f0_o0.npy       -> [1,4,8400] (cx, cy, w_full, h_full) —— DFL 在 NPU 里
  prod_f0_o0.npy        -> [1,4,8400]                    —— 生产模型(DFL 在 NPU)

对比:
  fork9-CPU:  x1 = anchor_x - l*stride, x2 = anchor_x + r*stride
  formC-NPU:  x1 = cx - w/2,            x2 = cx + w/2
输出: 高置信 anchor 上的坐标误差(px)/IoU 分布
"""
import os

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
PROBE = os.path.join(HERE, "..", "out", "probe")
STR = [8, 16, 32]


def load(n):
    return np.load(os.path.join(PROBE, n)).astype(np.float64)


def fork9_boxes():
    l = np.zeros(8400); t = np.zeros(8400); r = np.zeros(8400); b = np.zeros(8400)
    cls = np.zeros((2, 8400))
    off = 0
    for si, st in enumerate(STR):
        box = load("fork9_f0_o%d.npy" % (si * 3))
        H, W = box.shape[-2:]
        bb = box[0].reshape(4, 16, H * W)
        e = np.exp(bb - bb.max(axis=1, keepdims=True))
        p = e / e.sum(axis=1, keepdims=True)
        d = (p * np.arange(16).reshape(16, 1)).sum(axis=1) * st   # 像素
        n = H * W
        ys, xs = np.meshgrid(np.arange(H), np.arange(W), indexing="ij")
        l[off:off + n] = (xs.reshape(-1) + 0.5) * st - d[0]
        t[off:off + n] = (ys.reshape(-1) + 0.5) * st - d[1]
        r[off:off + n] = (xs.reshape(-1) + 0.5) * st + d[2]
        b[off:off + n] = (ys.reshape(-1) + 0.5) * st + d[3]
        cls[:, off:off + n] = load("fork9_f0_o%d.npy" % (si * 3 + 1)).reshape(2, -1)
        off += n
    return l, t, r, b, cls


def iou(a, b):
    ix1 = np.maximum(a[0], b[0]); iy1 = np.maximum(a[1], b[1])
    ix2 = np.minimum(a[2], b[2]); iy2 = np.minimum(a[3], b[3])
    iw = np.clip(ix2 - ix1, 0, None); ih = np.clip(iy2 - iy1, 0, None)
    inter = iw * ih
    aa = (a[2] - a[0]) * (a[3] - a[1]); bb = (b[2] - b[0]) * (b[3] - b[1])
    uni = aa + bb - inter
    return np.where(uni > 0, inter / np.maximum(uni, 1e-9), 0.0)


def main():
    l, t, r, b, cls = fork9_boxes()
    score = cls.max(axis=0)
    sel = score > 0.25
    print("高置信 anchor = %d" % int(sel.sum()))
    for tag, fn in (("formC(自建孪生)", "formC_f0_o0.npy"), ("prod(生产模型)", "prod_f0_o0.npy")):
        m = load(fn).reshape(4, 8400)
        cx, cy, w, h = m[0], m[1], m[2], m[3]
        x1, y1, x2, y2 = cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2
        dx1 = x1[sel] - l[sel]; dy1 = y1[sel] - t[sel]
        dx2 = x2[sel] - r[sel]; dy2 = y2[sel] - b[sel]
        ious = iou((x1[sel], y1[sel], x2[sel], y2[sel]),
                   (l[sel], t[sel], r[sel], b[sel]))
        print()
        print("【%s】DFL 在 NPU 内 vs fork9 的 CPU DFL (高置信 anchor)" % tag)
        print("   坐标误差 |Δx1| mean=%.2f px  max=%.2f px" % (np.abs(dx1).mean(), np.abs(dx1).max()))
        print("            |Δy1| mean=%.2f px  max=%.2f px" % (np.abs(dy1).mean(), np.abs(dy1).max()))
        print("            |Δx2| mean=%.2f px  max=%.2f px" % (np.abs(dx2).mean(), np.abs(dx2).max()))
        print("            |Δy2| mean=%.2f px  max=%.2f px" % (np.abs(dy2).mean(), np.abs(dy2).max()))
        print("   逐 anchor IoU: mean=%.4f  min=%.4f  <0.5 的个数=%d/%d"
              % (ious.mean(), ious.min(), int((ious < 0.5).sum()), int(sel.sum())))
        print("   宽度差 |Δw| mean=%.2f px (宽度中位数 %.1f px)"
              % (np.abs(w[sel] - (r[sel] - l[sel])).mean(), np.median(r[sel] - l[sel])))
        print("   高度差 |Δh| mean=%.2f px (高度中位数 %.1f px)"
              % (np.abs(h[sel] - (b[sel] - t[sel])).mean(), np.median(b[sel] - t[sel])))


if __name__ == "__main__":
    main()
