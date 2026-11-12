#!/usr/bin/env python3
"""sim_quant_compare.py —— INT8 vs FP16 量化损失【张量级】对比(RKNN 模拟器)

为什么用模拟器而不是板子:
    板子上两个 .rknn 的预处理喂法不同(INT8 收 uint8 原始像素, FP16 收 float32/255),
    像素经过两次不同的插值, 差异会被混进来, 分不清是"量化损失"还是"预处理差异"。
    模拟器里两个模型吃的是【同一个 numpy 数组】, 差异 100% 来自量化。

方法:
    1) 同一个 ONNX 编出两份模型: do_quantization=True(INT8) / False(FP16)
    2) 模拟器里跑同一批真实图片, 逐张比较 9 个输出张量:
         - sum 张量  (= 每格的最大类别概率) -> 类别分数漂移
         - cls 张量  -> 逐类别概率漂移
         - box 张量  -> DFL 解码后的框坐标漂移
    3) 输出: 类别一致率 / 平均框坐标误差 / 分数漂移 / IoU 分布

用法:
    _bench/venv/bin/python scripts/sim_quant_compare.py \
        --onnx _bench/dl/yolov8n_optimized.onnx \
        --calib _bench/calib_list.txt \
        --images dataset_mp4 --count 200 --out logs/sim_quant.json
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import sys

import numpy as np
import cv2

STRIDES = [8, 16, 32]
SIG_THR = 0.01          # "模型有把握"的门槛: 最大类别概率 > 0.01(这两个场景的分数都在 0.01~0.05)
DFL_LEN = 16


def load_images(paths, model_hw):
    out = []
    for p in paths:
        img = cv2.imread(p)
        if img is None:
            continue
        img = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
        img = cv2.resize(img, (model_hw[1], model_hw[0]))
        out.append(img.astype(np.float32) / 255.0)
    return out


def build(onnx, calib, quant):
    from rknn.api import RKNN
    r = RKNN(verbose=False)
    r.config(mean_values=[[0, 0, 0]], std_values=[[255, 255, 255]], target_platform="rk3588")
    r.load_onnx(model=onnx)
    ret = r.build(do_quantization=quant, dataset=calib if quant else None)
    if ret != 0:
        raise RuntimeError(f"build(quant={quant}) 失败: {ret}")
    ret = r.init_runtime()
    if ret != 0:
        raise RuntimeError(f"init_runtime 失败: {ret}")
    return r


def dfl_decode(box):
    """box: [4*DFL_LEN, H, W] -> [4, H, W] (l,t,r,b, 单位=格)"""
    b = box.reshape(4, DFL_LEN, -1)
    b = b - b.max(axis=1, keepdims=True)
    e = np.exp(b)
    p = e / e.sum(axis=1, keepdims=True)
    proj = np.arange(DFL_LEN, dtype=np.float32)
    return (p * proj[:, None]).sum(axis=1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--onnx", required=True)
    ap.add_argument("--calib", required=True)
    ap.add_argument("--images", required=True)
    ap.add_argument("--count", type=int, default=200)
    ap.add_argument("--stride", type=int, default=4, help="每隔几张取一张")
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(args.images, "*.jpg")))
    files = files[::args.stride][:args.count]
    print(f"[sim] 取 {len(files)} 张图: {args.images}")

    r_int8 = build(args.onnx, args.calib, True)
    r_fp16 = build(args.onnx, args.calib, False)
    print("[sim] 两个模型都已在模拟器里就绪")

    # 推断输入布局(NCHW 还是 NHWC)
    import onnx
    m = onnx.load(args.onnx)
    inp = m.graph.input[0]
    shape = [d.dim_value if d.dim_value > 0 else d.dim_param for d in inp.type.tensor_type.shape.dim]
    nchw = (len(shape) == 4 and isinstance(shape[1], int) and shape[1] == 3)
    model_hw = (shape[2], shape[3]) if nchw else (shape[1], shape[2])
    print(f"[sim] ONNX 输入 shape={shape} -> {'NCHW' if nchw else 'NHWC'}, 模型输入 {model_hw[1]}x{model_hw[0]}")

    imgs = load_images(files, model_hw)
    if not imgs:
        print("没有可用图片")
        return 1

    n_cls_same = n_cls_total = 0
    n_sig_same = n_sig_total = 0      # 只统计"模型有把握"的格子(最大类别概率 > 阈值)
    sum_drift = []
    cls_drift = []
    box_drift = []
    ious = []
    per_image = []

    for i, im in enumerate(imgs):
        # 模拟器的输入一律要 NHWC(1,H,W,C) —— 它内部会按模型需要再转
        x = im[None, ...].astype(np.float32)
        o8 = r_int8.inference(inputs=[x])
        o16 = r_fp16.inference(inputs=[x])
        if not o8 or not o16:
            continue
        img_cls_same = 0
        img_cls_total = 0
        for s in range(3):
            if s * 3 + 2 >= len(o8):
                break
            b8, c8, s8 = o8[s * 3 + 0], o8[s * 3 + 1], o8[s * 3 + 2]
            b16, c16, s16 = o16[s * 3 + 0], o16[s * 3 + 1], o16[s * 3 + 2]
            # 类别一致率(逐格 argmax)
            a8 = np.argmax(c8, axis=1 if c8.ndim == 4 else 0)
            a16 = np.argmax(c16, axis=1 if c16.ndim == 4 else 0)
            same = int((a8 == a16).sum())
            n_cls_same += same
            n_cls_total += a8.size
            # 有把握的格子: 两侧的最大类别概率都 > SIG_THR
            conf8 = np.max(c8, axis=1 if c8.ndim == 4 else 0)
            conf16 = np.max(c16, axis=1 if c16.ndim == 4 else 0)
            sig = (conf8 > SIG_THR) | (conf16 > SIG_THR)
            if sig.any():
                n_sig_same += int(((a8 == a16) & sig).sum())
                n_sig_total += int(sig.sum())
            img_cls_same += same
            img_cls_total += a8.size
            # sum(最大类别概率)漂移
            sum_drift.append(np.abs(s8.astype(np.float64) - s16.astype(np.float64)).mean())
            # cls 概率漂移(只看双方都认为"有东西"的格子)
            mask = (s8 > 0.05) | (s16 > 0.05)
            mask = np.broadcast_to(mask, c8.shape)
            if mask.any():
                cls_drift.append(np.abs(c8.astype(np.float64) - c16.astype(np.float64))[mask].mean())
            # DFL 框坐标漂移(只比较有目标的格子)
            d8 = dfl_decode(b8)
            d16 = dfl_decode(b16)
            m2 = (s8.reshape(-1) > 0.05) | (s16.reshape(-1) > 0.05)
            if m2.any():
                d8f = d8.reshape(4, -1)
                d16f = d16.reshape(4, -1)
                diff = np.abs(d8f[:, m2] - d16f[:, m2])
                box_drift.append(diff.mean())
        per_image.append({"file": os.path.basename(files[i]),
                           "cls_same_ratio": img_cls_same / max(1, img_cls_total)})
        if (i + 1) % 50 == 0:
            print(f"[sim] {i+1}/{len(imgs)}")

    r_int8.release()
    r_fp16.release()

    def mean(v):
        return float(np.mean(v)) if v else 0.0

    res = {
        "images": len(per_image),
        "cls_agree_ratio": n_cls_same / max(1, n_cls_total),
        "cls_agree_ratio_significant": n_sig_same / max(1, n_sig_total),
        "significant_cells": n_sig_total,
        "sig_threshold": SIG_THR,
        "mean_sum_drift": mean(sum_drift),
        "mean_cls_drift": mean(cls_drift),
        "mean_box_drift_cells": mean(box_drift),
        "note": "框漂移单位=格(stride 8/16/32), 乘上 stride 就是像素",
    }
    print("\n" + "=" * 64)
    print("INT8 vs FP16 张量级量化损失(模拟器, 同一输入)")
    print("=" * 64)
    print(f"  图片数            : {res['images']}")
    print(f"  类别逐格一致率    : {res['cls_agree_ratio']*100:.3f}%  (所有格子)")
    print(f"  有把握格子的一致率 : {res['cls_agree_ratio_significant']*100:.3f}%  "
          f"(最大类别概率 > {SIG_THR} 的格子, 共 {res['significant_cells']} 个)")
    print(f"  最大类别概率平均漂移: {res['mean_sum_drift']:.6f}")
    print(f"  类别概率平均漂移  : {res['mean_cls_drift']:.6f}")
    print(f"  DFL 框坐标平均漂移: {res['mean_box_drift_cells']:.4f} 格")
    if args.out:
        with open(args.out, "w") as f:
            json.dump({**res, "per_image": per_image[:50]}, f, ensure_ascii=False, indent=2)
        print(f"\n  已写入 {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
