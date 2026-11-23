#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""analyze_layout.py —— 判定 RKNN 2输出模型的 box 张量到底怎么排

背景(2026-11-23 P1):
  对拍时 fork9 vs formC 匹配率只有 91.19%, fork9 vs prod 只有 34.43%,
  但类别一致率 100%、分数差 ±0.0007 —— 框坐标系统性对不上。
  原始张量显示 prod 的 ch3 出现 0.00 / 10.19, formC 的 ch0 不落在 anchor 网格上
  ⇒ 说明【两个 2输出模型的输出张量物理排布与我们以为的不一样】。

本脚本把 4 种排布假设全试一遍, 并用 ONNX 运行时做基准:
  A) [4,8400] 通道优先, (cx, cy, w, h)
  B) [4,8400] 通道优先, (x1, y1, x2, y2)
  C) [8400,4] anchor 交织, (cx, cy, w, h)
  D) [8400,4] anchor 交织, (x1, y1, x2, y2)
判据: 与 fork9 的 CPU DFL 解码(已由 verify_formC_equiv 证明等价)逐 anchor 对齐后的误差。
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PROBE = os.path.join(ROOT, "out", "probe")
STR = [8, 16, 32]
IMG = "/home/seeed/rk_deploy/accuracy/data/frames/f00000.jpg"


def load(name):
    return np.load(os.path.join(PROBE, name)).astype(np.float64)


def fork9_cpu_decode():
    """返回 (cx, cy, w_full, h_full, x1, y1, x2, y2, cls) —— 全部按 8400 展开"""
    d9 = np.zeros((4, 8400))
    cx = np.zeros(8400)
    cy = np.zeros(8400)
    cls = np.zeros((2, 8400))
    off = 0
    for si, st in enumerate(STR):
        box = load("fork9_f0_o%d.npy" % (si * 3))
        H, W = box.shape[-2:]
        b = box[0].reshape(4, 16, H * W)
        e = np.exp(b - b.max(axis=1, keepdims=True))
        p = e / e.sum(axis=1, keepdims=True)
        d = (p * np.arange(16).reshape(16, 1)).sum(axis=1)      # 格点单位
        n = H * W
        ys, xs = np.meshgrid(np.arange(H), np.arange(W), indexing="ij")
        cx[off:off + n] = (xs.reshape(-1) + 0.5) * st
        cy[off:off + n] = (ys.reshape(-1) + 0.5) * st
        d9[:, off:off + n] = d * st
        cls[:, off:off + n] = load("fork9_f0_o%d.npy" % (si * 3 + 1)).reshape(2, -1)
        off += n
    return dict(cx=cx, cy=cy, w=d9[0] + d9[2], h=d9[1] + d9[3],
                x1=cx - d9[0], y1=cy - d9[1], x2=cx + d9[2], y2=cy + d9[3],
                cls=cls)


def interp(raw, kind):
    """raw: [1,4,8400] 的扁平化 float —— 返回 {name: [8400]}"""
    v = raw.reshape(-1)
    out = {}
    if kind == "chan":
        m = v.reshape(4, 8400)
        out["cx"], out["cy"], out["w"], out["h"] = m[0], m[1], m[2], m[3]
        out["x1"], out["y1"], out["x2"], out["y2"] = m[0], m[1], m[2], m[3]
    else:  # anchor 交织
        m = v.reshape(8400, 4)
        out["cx"], out["cy"], out["w"], out["h"] = m[:, 0], m[:, 1], m[:, 2], m[:, 3]
        out["x1"], out["y1"], out["x2"], out["y2"] = m[:, 0], m[:, 1], m[:, 2], m[:, 3]
    return out


def score(got, ref, sel):
    """在高置信 anchor 上算平均相对误差"""
    errs = {}
    for k in ("cx", "cy", "w", "h"):
        a, b = got[k][sel], ref[k][sel]
        denom = np.abs(b).mean() + 1e-9
        errs[k] = np.abs(a - b).mean() / denom
    return errs


def main():
    ref = fork9_cpu_decode()
    best = ref["cls"].max(axis=0)
    sel = best > 0.25
    print("高置信 anchor 数 = %d / 8400" % int(sel.sum()))
    print()
    for tag, fn in (("formC", "formC_f0_o0.npy"), ("prod", "prod_f0_o0.npy")):
        raw = load(fn)
        print("=" * 92)
        print("【%s】原始张量 %s" % (tag, raw.shape))
        for kind in ("chan", "inter"):
            got = interp(raw, kind)
            # 假设 1: (cx, cy, w, h)
            e1 = score(got, ref, sel)
            # 假设 2: (x1, y1, x2, y2)
            e2 = score({"cx": got["x1"], "cy": got["y1"], "w": got["x2"] - got["x1"],
                        "h": got["y2"] - got["y1"]}, ref, sel)
            print("  [%s] 当 (cx,cy,w,h) 解释: " % kind
                  + "  ".join("%s rel_err=%.4f" % (k, v) for k, v in e1.items())
                  + "  => 均值 %.4f" % (sum(e1.values()) / 4))
            print("  [%s] 当 (x1,y1,x2,y2)解释: " % kind
                  + "  ".join("%s rel_err=%.4f" % (k, v) for k, v in e2.items())
                  + "  => 均值 %.4f" % (sum(e2.values()) / 4))
        print()

    # ---- 与 ONNX 运行时对照: formC ONNX 的输出是什么排布 ----
    try:
        import onnxruntime as ort
        import cv2
    except Exception as e:
        print("onnxruntime/cv2 不可用, 跳过 ONNX 基准:", e)
        return
    img = cv2.imread(IMG, cv2.IMREAD_COLOR)
    small = cv2.resize(img, (640, 640), interpolation=cv2.INTER_LINEAR)
    small = cv2.cvtColor(small, cv2.COLOR_BGR2RGB).astype(np.float32)
    x = (small / 255.0).transpose(2, 0, 1)[None]      # NCHW [0,1]
    for name, onnx_path in (("formC", os.path.join(ROOT, "onnx", "ebike_real_formC.onnx")),
                            ("fork9", os.path.join(ROOT, "onnx", "ebike_real_fork9.onnx"))):
        s = ort.InferenceSession(onnx_path, providers=["CPUExecutionProvider"])
        outs = s.run(None, {s.get_inputs()[0].name: x})
        if name == "formC":
            b_onnx = outs[0][0]     # [4,8400]
            c_onnx = outs[1][0]
            print("=" * 92)
            print("【formC ONNX 运行时】输出 shape=%s" % (outs[0].shape,))
            e = score({"cx": b_onnx[0], "cy": b_onnx[1], "w": b_onnx[2], "h": b_onnx[3]},
                      ref, sel)
            print("  ONNX(ch0,ch1,ch2,ch3) 对 fork9-CPU: "
                  + "  ".join("%s rel_err=%.5f" % (k, v) for k, v in e.items()))
            print("  ONNX 前 6 个 anchor 的 ch0..3:")
            for a in range(6):
                print("     a=%d  %s" % (a, np.round(b_onnx[:, a], 3)))
            print("  fork9-CPU 对应 anchor 的 (cx,cy,w,h):")
            for a in range(6):
                print("     a=%d  %s" % (a, np.round([ref["cx"][a], ref["cy"][a],
                                                      ref["w"][a], ref["h"][a]], 3)))
            np.save(os.path.join(PROBE, "formC_onnx_box.npy"), b_onnx)
            np.save(os.path.join(PROBE, "formC_onnx_cls.npy"), c_onnx)
        else:
            print("=" * 92)
            print("【fork9 ONNX 运行时】输出数=%d" % len(outs))
            for i, o in enumerate(outs):
                print("   out[%d] %s" % (i, o.shape))


if __name__ == "__main__":
    main()
