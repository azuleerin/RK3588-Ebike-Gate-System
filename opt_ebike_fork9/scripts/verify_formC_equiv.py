#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_formC_equiv.py —— 形态C ONNX 的数值自检

★ 为什么必须有这个脚本
  造"图内 DFL"的 ONNX 时, 只要有一个算子的语义不对(比如 opset<=12 的 Softmax
  legacy 2D 强制转换), 结果就是【框全变成零宽】, 而 onnx.checker 照样通过、
  RKNN 编译器也照样编译成功、跑起来也不报错 —— 静默失败。
  所以必须有一个【数值级】的自检: 把 fork9 的原始 DFL logits 在 CPU 上手工解码,
  和形态C 图内解出来的框逐像素对比。

★ 判定
  形态C 输出的是 (cx, cy, w, h) 像素(生产解码器 post_process_dual_output 的口径),
  所以对比前先把 CPU 解出来的 (x1,y1,x2,y2) 转成 (cx,cy,w,h)。

用法:
    verify_formC_equiv.py <fork9.onnx> <formC.onnx>
"""
import sys
import numpy as np
import onnxruntime as ort

STRIDES = [8, 16, 32]
DFL_LEN = 16


def dfl_decode_cpu(box, H, W, stride):
    """[1,64,H,W] 原始 DFL logits -> (x1,y1,x2,y2) 像素"""
    b = box[0].reshape(4, DFL_LEN, H * W)
    p = np.exp(b - b.max(axis=1, keepdims=True))
    p /= p.sum(axis=1, keepdims=True)
    d = (p * np.arange(DFL_LEN).reshape(DFL_LEN, 1)).sum(axis=1)   # [4, HW] 格点单位
    ys, xs = np.meshgrid(np.arange(H), np.arange(W), indexing='ij')
    cx = (xs.reshape(-1) + 0.5) * stride
    cy = (ys.reshape(-1) + 0.5) * stride
    l, t, r, btm = d[0], d[1], d[2], d[3]
    return np.stack([cx - l * stride, cy - t * stride,
                     cx + r * stride, cy + btm * stride])


def main():
    f9, fc = sys.argv[1], sys.argv[2]
    s9 = ort.InferenceSession(f9, providers=['CPUExecutionProvider'])
    sc = ort.InferenceSession(fc, providers=['CPUExecutionProvider'])
    rng = np.random.default_rng(20261122)
    worst = 0.0
    for trial in range(3):
        x = (rng.random((1, 3, 640, 640)).astype(np.float32) * 255.0)
        o9 = s9.run(None, {s9.get_inputs()[0].name: x})
        oc = sc.run(None, {sc.get_inputs()[0].name: x})
        assert len(o9) == 9, f"fork9 输出数不对: {len(o9)}"
        assert len(oc) == 2, f"形态C 输出数不对: {len(oc)}"
        box_flat = oc[0][0]          # [4, 8400]
        cls_flat = oc[1][0]          # [nc, 8400]
        off = 0
        for si, st in enumerate(STRIDES):
            H = W = 640 // st
            xyxy = dfl_decode_cpu(o9[si * 3], H, W, st)
            seg = box_flat[:, off:off + H * W]
            cx = (xyxy[0] + xyxy[2]) / 2.0
            cy = (xyxy[1] + xyxy[3]) / 2.0
            w = xyxy[2] - xyxy[0]
            h = xyxy[3] - xyxy[1]
            ref = np.stack([cx, cy, w, h])
            d = float(np.abs(seg - ref).max())
            worst = max(worst, d)
            print('  trial%d stride=%-3d max|Δ(cx,cy,w,h)| = %.6f px' % (trial, st, d))
            # cls 也要对
            cs = float(np.abs(cls_flat[:, off:off + H * W] - o9[si * 3 + 1][0].reshape(
                o9[si * 3 + 1].shape[1], -1)).max())
            worst = max(worst, cs)
            print('  trial%d stride=%-3d max|Δcls|          = %.3e' % (trial, st, cs))
            off += H * W
    print('=' * 70)
    if worst < 1e-2:
        print('VERDICT: PASS ✔  形态C 图内 DFL 与 CPU 解码逐像素一致 (max|Δ|=%.6f)' % worst)
        return 0
    print('VERDICT: FAIL ✘  max|Δ|=%.6f —— 图内解码与 CPU 解码对不上' % worst)
    return 1


if __name__ == '__main__':
    sys.exit(main())
