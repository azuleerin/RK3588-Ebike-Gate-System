#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""analyze_convention.py —— 判定 2 输出形态的 box 四通道到底是什么约定

输入: probe_raw_outputs.py dump 的 .npy
  prod_f0_o0.npy  [1,4,8400]   生产模型
  formC_f0_o0.npy [1,4,8400]   自建孪生
  fork9_f0_o{0..8}.npy         fork9 的 9 个输出

输出:
  1) prod vs formC 的通道级差异(同权重? 同量化?)
  2) formC/prod 的 ch0..3 在【各种解释】下与 fork9 的 CPU 解码的一致性
"""
import numpy as np
import os

STR = [8, 16, 32]
D = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "out", "probe")


def load(name):
    return np.load(os.path.join(D, name)).astype(np.float64)


def corr(a, b):
    a = np.asarray(a, dtype=np.float64).ravel()
    b = np.asarray(b, dtype=np.float64).ravel()
    if a.std() < 1e-12 or b.std() < 1e-12:
        return float("nan")
    return float(np.corrcoef(a, b)[0, 1])


def dfl_cpu(box):
    """[1,64,H,W] -> d[4,HW] (格点单位)"""
    H, W = box.shape[-2:]
    b = box[0].reshape(4, 16, H * W)
    e = np.exp(b - b.max(axis=1, keepdims=True))
    p = e / e.sum(axis=1, keepdims=True)
    return (p * np.arange(16).reshape(16, 1)).sum(axis=1)


def main():
    prod_b, prod_c = load("prod_f0_o0.npy")[0], load("prod_f0_o1.npy")[0]
    fc_b, fc_c = load("formC_f0_o0.npy")[0], load("formC_f0_o1.npy")[0]

    d9 = np.zeros((4, 8400))
    cx9 = np.zeros(8400)
    cy9 = np.zeros(8400)
    cls9 = np.zeros((2, 8400))
    off = 0
    for si, st in enumerate(STR):
        box = load("fork9_f0_o%d.npy" % (si * 3))
        d = dfl_cpu(box)
        H, W = box.shape[-2:]
        n = H * W
        ys, xs = np.meshgrid(np.arange(H), np.arange(W), indexing="ij")
        cx9[off:off + n] = (xs.reshape(-1) + 0.5) * st
        cy9[off:off + n] = (ys.reshape(-1) + 0.5) * st
        d9[:, off:off + n] = d * st
        cls9[:, off:off + n] = load("fork9_f0_o%d.npy" % (si * 3 + 1)).reshape(2, -1)
        off += n

    w_full = d9[0] + d9[2]
    h_full = d9[1] + d9[3]
    x1_9, y1_9 = cx9 - d9[0], cy9 - d9[1]
    x2_9, y2_9 = cx9 + d9[2], cy9 + d9[3]

    print("=" * 92)
    print("① prod vs formC —— 同权重吗? (prod=算法组编译, formC=我们自建孪生)")
    for i, nm in enumerate(["ch0", "ch1", "ch2", "ch3"]):
        a, b = prod_b[i], fc_b[i]
        print("   %s: max|Δ|=%.4f  mean|Δ|=%.4f  corr=%.5f"
              % (nm, np.abs(a - b).max(), np.abs(a - b).mean(), corr(a, b)))
    print("   cls : max|Δ|=%.6f  corr=%.5f"
          % (np.abs(prod_c - fc_c).max(), corr(prod_c, fc_c)))

    print()
    print("=" * 92)
    print("② formC/prod 的 ch0..3 在各种解释下, 与 fork9 CPU 解码的一致性")
    tests = [
        ("ch0 = cx9", fc_b[0], cx9),
        ("ch0 = x1_9", fc_b[0], x1_9),
        ("ch1 = cy9", fc_b[1], cy9),
        ("ch1 = y1_9", fc_b[1], y1_9),
        ("ch2 = w_full", fc_b[2], w_full),
        ("ch2 = w_full/2", fc_b[2], w_full / 2),
        ("ch2 = x2_9", fc_b[2], x2_9),
        ("ch3 = h_full", fc_b[3], h_full),
        ("ch3 = h_full/2", fc_b[3], h_full / 2),
        ("ch3 = y2_9", fc_b[3], y2_9),
        ("cls = cls9", fc_c, cls9),
    ]
    for nm, a, b in tests:
        print("   corr(%-14s) = %+.5f   max|Δ|=%9.3f  rel_err=%.4f"
              % (nm, corr(a, b), np.abs(a - b).max(),
                 np.abs(a - b).mean() / (np.abs(b).mean() + 1e-9)))

    print()
    print("=" * 92)
    print("③ 高分 anchor 逐个对照 (formC / prod 四通道 vs fork9 CPU 解码)")
    best = cls9.max(axis=0)
    order = np.argsort(-best)[:8]
    print("   %6s %6s | %8s %8s %8s | %8s %8s %8s %8s | %8s %8s %8s %8s"
          % ("anchor", "cls", "9.cx", "fc.ch0", "pd.ch0", "9.wfull",
             "fc.ch2", "pd.ch2", "wfull/2", "9.hfull", "fc.ch3", "pd.ch3",
             "hfull/2"))
    for a in order:
        print("   %6d %6.3f | %8.2f %8.2f %8.2f | %8.2f %8.2f %8.2f %8.2f | "
              "%8.2f %8.2f %8.2f %8.2f"
              % (a, best[a], cx9[a], fc_b[0, a], prod_b[0, a],
                 w_full[a], fc_b[2, a], prod_b[2, a], w_full[a] / 2,
                 h_full[a], fc_b[3, a], prod_b[3, a], h_full[a] / 2))

    print()
    print("=" * 92)
    print("④ prod 的 ch2/ch3 是不是【半宽/半高】(高置信 anchor 子集)")
    sel = best > 0.25
    print("   高置信 anchor 数 = %d" % int(sel.sum()))
    for nm, a, b in [("corr(pd.ch2, w_full)", prod_b[2][sel], w_full[sel]),
                     ("corr(pd.ch2, w_full/2)", prod_b[2][sel], (w_full / 2)[sel]),
                     ("corr(pd.ch3, h_full)", prod_b[3][sel], h_full[sel]),
                     ("corr(pd.ch3, h_full/2)", prod_b[3][sel], (h_full / 2)[sel]),
                     ("corr(fc.ch2, w_full)", fc_b[2][sel], w_full[sel]),
                     ("corr(fc.ch3, h_full)", fc_b[3][sel], h_full[sel])]:
        print("   %-26s = %+.5f" % (nm, corr(a, b)))
    print("   高置信 anchor 上的比值中位数:")
    for nm, a, b in [("prod.ch2 / w_full", prod_b[2][sel], w_full[sel]),
                     ("prod.ch3 / h_full", prod_b[3][sel], h_full[sel]),
                     ("fc.ch2 / w_full", fc_b[2][sel], w_full[sel]),
                     ("fc.ch3 / h_full", fc_b[3][sel], h_full[sel])]:
        m = np.abs(b) > 1e-6
        r = (a[m] / b[m])
        print("      %-18s median=%.4f  mean=%.4f" % (nm, np.median(r), r.mean()))


if __name__ == "__main__":
    main()
