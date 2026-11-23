#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""decide_prod_layout.py —— 生产模型 box 张量的物理排布, 最终判定

参考系 = fork9 的 CPU DFL 解码(边坐标 x1,y1,x2,y2), 已由 verify_formC_equiv 证明
与 ONNX 逐像素等价(0.000079 px)。

对每个模型 × 每种读法, 算:
  · 同 anchor IoU (布局与 anchor 顺序都一致时应该 ~1.0)
  · 自由重排后的最大 IoU (只允许 anchor 换序时应该 ~1.0)
"""
import os

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
PROBE = os.path.join(HERE, "..", "out", "probe")
STR = [8, 16, 32]


def L(n):
    return np.load(os.path.join(PROBE, n)).astype(np.float64)


def fork9_edges():
    """返回 8400 个 anchor 的 (x1,y1,x2,y2) 边坐标 + cls"""
    x1 = np.zeros(8400); y1 = np.zeros(8400); x2 = np.zeros(8400); y2 = np.zeros(8400)
    cls = np.zeros((2, 8400))
    off = 0
    for si, st in enumerate(STR):
        box = L("fork9_f0_o%d.npy" % (si * 3))
        H, W = box.shape[-2:]
        bb = box[0].reshape(4, 16, H * W)
        e = np.exp(bb - bb.max(axis=1, keepdims=True))
        p = e / e.sum(axis=1, keepdims=True)
        d = (p * np.arange(16).reshape(16, 1)).sum(axis=1) * st
        n = H * W
        ys, xs = np.meshgrid(np.arange(H), np.arange(W), indexing="ij")
        cx = (xs.reshape(-1) + 0.5) * st
        cy = (ys.reshape(-1) + 0.5) * st
        x1[off:off + n] = cx - d[0]; y1[off:off + n] = cy - d[1]
        x2[off:off + n] = cx + d[2]; y2[off:off + n] = cy + d[3]
        cls[:, off:off + n] = L("fork9_f0_o%d.npy" % (si * 3 + 1)).reshape(2, -1)
        off += n
    return np.stack([x1, y1, x2, y2], axis=1), cls


def iou_mat(A, B):
    ix1 = np.maximum(A[:, None, 0], B[None, :, 0])
    iy1 = np.maximum(A[:, None, 1], B[None, :, 1])
    ix2 = np.minimum(A[:, None, 2], B[None, :, 2])
    iy2 = np.minimum(A[:, None, 3], B[None, :, 3])
    iw = np.clip(ix2 - ix1, 0, None)
    ih = np.clip(iy2 - iy1, 0, None)
    inter = iw * ih
    aa = (A[:, 2] - A[:, 0]) * (A[:, 3] - A[:, 1])
    bb = (B[:, 2] - B[:, 0]) * (B[:, 3] - B[:, 1])
    uni = aa[:, None] + bb[None, :] - inter
    return np.where(uni > 0, inter / np.maximum(uni, 1e-9), 0.0)


def main():
    ref, cls = fork9_edges()
    score = cls.max(axis=0)
    idx = np.argsort(-score)[:10]
    print("参考系 = fork9 CPU DFL (边坐标); 高置信 anchor %d 个" % len(idx))
    print()
    for tag, fn in (("formC(自建孪生)", "formC_f0_o0.npy"), ("prod(生产模型)", "prod_f0_o0.npy")):
        raw = L(fn).reshape(-1)
        print("=" * 90)
        print("【%s】" % tag)
        # 读法 1: 通道优先 [4,8400], 语义 (cx,cy,w,h) —— 生产解码器的读法
        m = raw.reshape(4, 8400)
        pb1 = np.stack([m[0] - m[2] / 2, m[1] - m[3] / 2,
                        m[0] + m[2] / 2, m[1] + m[3] / 2], axis=1)
        # 读法 2: anchor 交织 [8400,4], 语义 (cx,cy,w,h)
        m2 = raw.reshape(8400, 4)
        pb2 = np.stack([m2[:, 0] - m2[:, 2] / 2, m2[:, 1] - m2[:, 3] / 2,
                        m2[:, 0] + m2[:, 2] / 2, m2[:, 1] + m2[:, 3] / 2], axis=1)
        # 读法 3: 通道优先, 语义 (x1,y1,x2,y2)
        pb3 = np.stack([m[0], m[1], m[2], m[3]], axis=1)
        # 读法 4: anchor 交织, 语义 (x1,y1,x2,y2)
        pb4 = m2.copy()
        for nm, pb in (("chan (cx,cy,w,h)", pb1), ("inter (cx,cy,w,h)", pb2),
                       ("chan (x1,y1,x2,y2)", pb3), ("inter (x1,y1,x2,y2)", pb4)):
            same = iou_mat(pb[idx], ref[idx]).max(axis=1)
            free = iou_mat(pb[idx], ref).max(axis=1)
            print("   %-20s 同 anchor: mean=%.4f (>=0.5: %d/%d) | 自由重排: mean=%.4f (>=0.5: %d/%d)"
                  % (nm, same.mean(), int((same >= 0.5).sum()), len(idx),
                     free.mean(), int((free >= 0.5).sum()), len(idx)))
        # 逐通道与参考量的相关
        print("   [通道优先读法] 逐通道相关:")
        for ch, nm in ((0, "cx"), (1, "cy"), (2, "w"), (3, "h")):
            print("      ch%d vs %-3s : corr=%+.5f" % (ch, nm, np.corrcoef(m[ch], ref[:, ch])[0, 1]))
        print()


if __name__ == "__main__":
    main()
