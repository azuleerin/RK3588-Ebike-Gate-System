#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""diagnose_prod_model.py —— 生产模型 ebike_int8_rk2332.rknn 到底是什么

现象: prod 的 box 四通道与 fork9 的 CPU DFL 解码差 300+ px(IoU≈0.08),
      但 cls 又是高度相关(corr 0.99) —— 说明"分类像、框不像"。

要回答三件事:
  1) prod 的 cls 与 fork9 的 cls 在高置信 anchor 上差多少? -> 权重同不同?
  2) prod 的 4 通道按哪种约定解释, 才能与 fork9 的框对上?
     (cx,cy,w,h) / (x1,y1,x2,y2) / (cx,cy,w/2,h/2) / (l,t,r,b) ...
  3) 如果都对不上, prod 的框能否与 fork9 的框【一一重排】后对上?
     (即: 是"约定不同"还是"anchor 排列不同")
"""
import os

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
PROBE = os.path.join(HERE, "..", "out", "probe")
STR = [8, 16, 32]


def load(n):
    return np.load(os.path.join(PROBE, n)).astype(np.float64)


def fork9_all():
    l = np.zeros(8400); t = np.zeros(8400); r = np.zeros(8400); b = np.zeros(8400)
    ax = np.zeros(8400); ay = np.zeros(8400)
    cls = np.zeros((2, 8400))
    off = 0
    for si, st in enumerate(STR):
        box = load("fork9_f0_o%d.npy" % (si * 3))
        H, W = box.shape[-2:]
        bb = box[0].reshape(4, 16, H * W)
        e = np.exp(bb - bb.max(axis=1, keepdims=True))
        p = e / e.sum(axis=1, keepdims=True)
        d = (p * np.arange(16).reshape(16, 1)).sum(axis=1) * st
        n = H * W
        ys, xs = np.meshgrid(np.arange(H), np.arange(W), indexing="ij")
        ax[off:off + n] = (xs.reshape(-1) + 0.5) * st
        ay[off:off + n] = (ys.reshape(-1) + 0.5) * st
        l[off:off + n] = ax[off:off + n] - d[0]
        t[off:off + n] = ay[off:off + n] - d[1]
        r[off:off + n] = ax[off:off + n] + d[2]
        b[off:off + n] = ay[off:off + n] + d[3]
        cls[:, off:off + n] = load("fork9_f0_o%d.npy" % (si * 3 + 1)).reshape(2, -1)
        off += n
    return l, t, r, b, ax, ay, cls


def iou_mat(A, B):
    """A,B: [N,4] 与 [M,4] -> [N,M]"""
    ix1 = np.maximum(A[:, None, 0], B[None, :, 0])
    iy1 = np.maximum(A[:, None, 1], B[None, :, 1])
    ix2 = np.minimum(A[:, None, 2], B[None, :, 2])
    iy2 = np.minimum(A[:, None, 3], B[None, :, 3])
    iw = np.clip(ix2 - ix1, 0, None); ih = np.clip(iy2 - iy1, 0, None)
    inter = iw * ih
    aa = (A[:, 2] - A[:, 0]) * (A[:, 3] - A[:, 1])
    bb = (B[:, 2] - B[:, 0]) * (B[:, 3] - B[:, 1])
    uni = aa[:, None] + bb[None, :] - inter
    return np.where(uni > 0, inter / np.maximum(uni, 1e-9), 0.0)


def main():
    l, t, r, b, ax, ay, cls = fork9_all()
    score = cls.max(axis=0)
    f9_boxes = np.stack([l, t, r, b], axis=1)

    prod_b = load("prod_f0_o0.npy").reshape(4, 8400)
    prod_c = load("prod_f0_o1.npy").reshape(2, 8400)
    fc_b = load("formC_f0_o0.npy").reshape(4, 8400)
    fc_c = load("formC_f0_o1.npy").reshape(2, 8400)

    print("=" * 92)
    print("① cls 对比 (高置信 anchor, 前 10)")
    idx = np.argsort(-score)[:10]
    print("   %6s %9s %9s %9s | %9s %9s"
          % ("anchor", "fork9", "formC", "prod", "formC-f9", "prod-f9"))
    for a in idx:
        print("   %6d %9.4f %9.4f %9.4f | %+9.4f %+9.4f"
              % (a, score[a], fc_c[:, a].max(), prod_c[:, a].max(),
                 fc_c[:, a].max() - score[a], prod_c[:, a].max() - score[a]))
    print("   全 8400 anchor: corr(prod_cls, fork9_cls)=%.5f  max|Δ|=%.4f"
          % (np.corrcoef(prod_c.ravel(), cls.ravel())[0, 1],
             np.abs(prod_c - cls).max()))
    print("   全 8400 anchor: corr(formC_cls, fork9_cls)=%.5f  max|Δ|=%.4f"
          % (np.corrcoef(fc_c.ravel(), cls.ravel())[0, 1],
             np.abs(fc_c - cls).max()))
    print("   prod_cls 的最大值 = %.4f, 最小值 = %.4f" % (prod_c.max(), prod_c.min()))

    print()
    print("=" * 92)
    print("② prod 的 4 通道按各约定解成框, 与 fork9 的框【同 anchor】比 IoU")
    cands = {
        "(cx,cy,w,h)":      (prod_b[0], prod_b[1], prod_b[2] / 2, prod_b[3] / 2),
        "(x1,y1,x2,y2)":    (prod_b[0], prod_b[1], prod_b[2], prod_b[3]),
        "(l,t,r,b)格点":     (ax - prod_b[0] * 8, ay - prod_b[1] * 8,
                          ax + prod_b[2] * 8, ay + prod_b[3] * 8),
    }
    for nm, (x1, y1, x2, y2) in cands.items():
        pb = np.stack([x1, y1, x2, y2], axis=1)
        i = iou_mat(pb[idx], f9_boxes[idx])
        print("   %-14s  同 anchor IoU: mean=%.4f  max=%.4f  >=0.5 的比例=%.2f"
              % (nm, i.mean(), i.max(), (i.max(axis=1) >= 0.5).mean()))

    print()
    print("=" * 92)
    print("③ 允许【任意重排】时, prod 的框能否对上 fork9 的框?")
    for nm, (x1, y1, x2, y2) in cands.items():
        pb = np.stack([x1, y1, x2, y2], axis=1)
        M = iou_mat(pb[idx], f9_boxes)
        best = M.max(axis=1)
        print("   %-14s  自由重排后 IoU: mean=%.4f  >=0.5 比例=%.2f  >=0.3 比例=%.2f"
              % (nm, best.mean(), (best >= 0.5).mean(), (best >= 0.3).mean()))

    print()
    print("=" * 92)
    print("④ prod 的 ch0/ch1 是【什么】? 与 fork9 的各种量做相关")
    tests = [("ax 锚点x", ax), ("ay 锚点y", ay), ("x1", l), ("y1", t),
             ("x2", r), ("y2", b), ("w=x2-x1", r - l), ("h=y2-y1", b - t),
             ("cx 框中心x", (l + r) / 2), ("cy 框中心y", (t + b) / 2)]
    for ch, nm in ((0, "prod.ch0"), (1, "prod.ch1"), (2, "prod.ch2"), (3, "prod.ch3")):
        s = prod_b[ch]
        best = max(tests, key=lambda kv: abs(np.corrcoef(s, kv[1])[0, 1]))
        c = np.corrcoef(s, best[1])[0, 1]
        print("   %s  最像的是 %-10s (corr=%+.4f)" % (nm, best[0], c))

    print()
    print("=" * 92)
    print("⑤ prod 与 formC 的 4 通道: 是不是 prod = formC 但被换了排列/半宽")
    for ch in range(4):
        print("   prod.ch%d vs formC.ch%d: corr=%+.5f  比值中位数=%.4f"
              % (ch, ch, np.corrcoef(prod_b[ch], fc_b[ch])[0, 1],
                 np.median(prod_b[ch] / np.where(np.abs(fc_b[ch]) > 1e-6, fc_b[ch], np.nan))))


if __name__ == "__main__":
    main()
