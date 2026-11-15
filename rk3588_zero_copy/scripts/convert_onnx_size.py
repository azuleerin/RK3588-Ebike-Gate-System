#!/usr/bin/env python3
"""convert_onnx_size.py —— 按指定输入尺寸把 ONNX 转成 RKNN(板端, rknn-toolkit2)

用途:
    生产模型是 640x640, 单帧 NPU 要 23.5ms。城中村路口目标大, 想换 416/320。
    RKNN 是编译后的二进制, 输入尺寸烤死了 —— 想换尺寸【只能从 ONNX 重转】。

用法(在板子上, ��先用 rknn-toolkit2 的虚拟环境):
    source _bench/venv/bin/activate
    python3 scripts/convert_onnx_size.py \
        --onnx _bench/dl/yolov8n_optimized.onnx \
        --out  _bench/yolov8n_rk3588_int8_416.rknn \
        --size 416 --dataset _bench/calib_list.txt

★ 关键点: 输入尺寸怎么改?
    rknn-toolkit2 的 load_onnx(input_size_list=...) 可以覆盖 ONNX 的输入形状。
    对 YOLOv8 这种全卷积网络(内部按 scale 做 Resize, 不是固定尺寸)可以直接改;
    改完必须【在板端核对输出形状】—— 模型转出来了不等于形状对,
    形状不对的话检测框全是乱的(看框数/框位置就知道)。
"""
from __future__ import annotations

import argparse
import os
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--onnx", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--size", type=int, required=True, help="输入边长(正方形)")
    ap.add_argument("--dataset", required=True, help="量化校准图片列表")
    ap.add_argument("--platform", default="rk3588")
    ap.add_argument("--fp16", action="store_true", help="不做量化, 建 FP16 模型")
    ap.add_argument("--batch", type=int, default=1,
                    help="Batch 大小(>1 时用 toolkit 的 batch 扩展, 模型输入变成 [B,3,H,W])")
    ap.add_argument("--mean", default="0,0,0")
    ap.add_argument("--std", default="255,255,255")
    args = ap.parse_args()

    try:
        from rknn.api import RKNN
    except ImportError:
        print("没装 rknn-toolkit2。先装:\n"
              "  pip3 install _bench/wheel/rknn_toolkit2-2.3.2-*-aarch64.whl",
              file=sys.stderr)
        return 2

    mean = [[float(x) for x in args.mean.split(",")]]
    std = [[float(x) for x in args.std.split(",")]]

    rknn = RKNN(verbose=True)
    print(f">>> config: mean={mean} std={std} platform={args.platform}")
    if rknn.config(mean_values=mean, std_values=std,
                   target_platform=args.platform) != 0:
        print("config 失败")
        return 1

    # ★ 覆盖输入尺寸: ONNX 里写死的是 640x640
    print(f">>> load_onnx: {args.onnx}  (输入改成 {args.size}x{args.size})")
    try:
        ret = rknn.load_onnx(model=args.onnx,
                             input_size_list=[(1, 3, args.size, args.size)])
    except Exception as e:                                  # noqa: BLE001
        print(f"load_onnx(input_size_list) 失败: {e}\n"
              f"退回不改输入尺寸的加载", file=sys.stderr)
        ret = rknn.load_onnx(model=args.onnx)
    if ret != 0:
        print("load_onnx 失败")
        return 1

    # ★ Batch: 用 toolkit 官方的 batch 扩展(它会在图里插 batch 展开算子),
    #   比自己改 ONNX 的输入 dim 更稳 —— 不用管图内部有没有写死的 batch=1。
    #   注意: ONNX 本身要是 batch 无关的(YOLOv8 就是), 否则扩展出来是错的。
    rknn_batch_size = args.batch if args.batch > 1 else None
    print(f">>> build: do_quantization={not args.fp16}  batch={args.batch}")
    if rknn.build(do_quantization=not args.fp16, dataset=args.dataset,
                  rknn_batch_size=rknn_batch_size) != 0:
        print("build 失败")
        return 1

    print(f">>> export: {args.out}")
    if rknn.export_rknn(args.out) != 0:
        print("export 失败")
        return 1
    rknn.release()
    print(f">>> DONE: {args.out}  ({os.path.getsize(args.out)} 字节)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
