#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_dfl_equiv.py —— 验证「Arm A(DFL 在 NPU 图内)」与「fork9 + CPU 侧 DFL」数值等价

做法:
  1) onnxruntime 跑 fork9 ONNX -> 拿 9 个原始输出
  2) Python 里按 yolo_post.cc 的 compute_dfl 复刻两种解码:
       a) 单位正确版: x1 = ((j+0.5) - d0) * stride      <- 与 ultralytics 一致
       b) 生产代码现状: x1 = (j+0.5)*stride - d0         <- 像素/格点混用
  3) onnxruntime 跑 Arm A ONNX -> 图内已解好的 cx,cy,w,h
  4) 对比三者

用法: verify_dfl_equiv.py <fork9.onnx> <armA.onnx> <图片> [框阈值]
"""
import sys
import numpy as np
import onnxruntime as ort
import cv2

DFL_LEN = 16
STRIDES = [8, 16, 32]


def load_inputs(path):
    img = cv2.imread(path)
    if img is None:
        raise SystemExit(f"读不了图片: {path}")
    img = cv2.resize(img, (640, 640))
    rgb = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
    x = rgb.astype(np.float32).transpose(2, 0, 1)[None] / 255.0
    return x


def decode_fork9(outs, unit_correct=True):
    """复刻 yolo_post.cc 的 fork9(形态B) 解码 -> 返回 (N,4) 的 cx,cy,w,h (像素)"""
    boxes = []
    for s, stride in enumerate(STRIDES):
        bt = outs[s * 3 + 0]          # [1,64,H,W]
        H, W = bt.shape[2], bt.shape[3]
        grid_len = H * W
        # bt[0, b*16+k, i, j] -> reshape 成 [4,16,H,W]
        d = bt[0].reshape(4, DFL_LEN, H, W)
        e = np.exp(d - d.max(axis=1, keepdims=True))
        p = e / e.sum(axis=1, keepdims=True)
        dist = (p * np.arange(DFL_LEN, dtype=np.float32)[None, :, None, None]).sum(axis=1)  # [4,H,W] 格点单位

        jj, ii = np.meshgrid(np.arange(W), np.arange(H))
        if unit_correct:
            cx = (jj.astype(np.float32) + 0.5) * stride
            cy = (ii.astype(np.float32) + 0.5) * stride
            x1 = (jj.astype(np.float32) + 0.5 - dist[0]) * stride
            y1 = (ii.astype(np.float32) + 0.5 - dist[1]) * stride
            x2 = (jj.astype(np.float32) + 0.5 + dist[2]) * stride
            y2 = (ii.astype(np.float32) + 0.5 + dist[3]) * stride
        else:
            # 生产代码 yolo_post.cc: cx=(j+0.5)*stride; x1 = cx - d[0]
            cx = (jj.astype(np.float32) + 0.5) * stride
            cy = (ii.astype(np.float32) + 0.5) * stride
            x1 = cx - dist[0]
            y1 = cy - dist[1]
            x2 = cx + dist[2]
            y2 = cy + dist[3]
        boxes.append(np.stack([(x1 + x2) / 2, (y1 + y2) / 2, x2 - x1, y2 - y1], axis=-1).reshape(-1, 4))
    return np.concatenate(boxes, axis=0)


def main():
    fork9_onnx, arma_onnx, img = sys.argv[1], sys.argv[2], sys.argv[3]
    conf = float(sys.argv[4]) if len(sys.argv) > 4 else 0.25

    x = load_inputs(img)
    print(f"输入: {img}  shape={x.shape}  range=[{x.min():.3f},{x.max():.3f}]")

    so_f = ort.SessionOptions()
    so_f.log_severity_level = 3
    sf = ort.InferenceSession(fork9_onnx, so_f, providers=["CPUExecutionProvider"])
    of = sf.run(None, {"images": x})
    print(f"fork9 输出数: {len(of)}  shapes={[list(o.shape) for o in of]}")

    so_a = ort.SessionOptions()
    so_a.log_severity_level = 3
    sa = ort.InferenceSession(arma_onnx, so_a, providers=["CPUExecutionProvider"])
    oa = sa.run(None, {"images": x})
    print(f"ArmA 输出: {[list(o.shape) for o in oa]}")

    correct = decode_fork9(of, unit_correct=True)
    buggy = decode_fork9(of, unit_correct=False)
    arma = oa[0][0].T  # [1,4,8400] -> [8400,4]

    # 只在有意义的框上比较(conf 高的 anchor)
    cls = oa[1][0]                       # [nc, 8400]
    score = cls.max(axis=0)
    keep = score >= conf
    print(f"conf>={conf} 的 anchor: {int(keep.sum())} / {len(score)}")

    if keep.sum() == 0:
        print("没有高置信 anchor, 换个图或降阈值")
        return

    d_correct = np.abs(correct[keep] - arma[keep])
    d_buggy = np.abs(buggy[keep] - arma[keep])

    print()
    print("=== ArmA(图内 DFL) vs fork9+CPU DFL ===")
    print(f"  单位正确版: max|Δ|={d_correct.max():.4f} px   mean|Δ|={d_correct.mean():.4f} px")
    print(f"  生产代码现状: max|Δ|={d_buggy.max():.4f} px   mean|Δ|={d_buggy.mean():.4f} px")
    print()
    print(f"  判定: 单位正确版 {'✅ 一致 (ArmA 构造正确)' if d_correct.max() < 0.05 else '❌ 不一致'}")
    print(f"        生产代码现状 {'✅ 一致' if d_buggy.max() < 0.05 else '❌ 不一致 -> yolo_post.cc 形态B 存在像素/格点单位混用 bug'}")


if __name__ == "__main__":
    main()
