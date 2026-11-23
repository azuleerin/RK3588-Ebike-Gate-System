#!/usr/bin/env python3
"""resize_onnx.py —— 把固定尺寸的 ONNX 改成另一个输入尺寸

为什么需要它:
    RKNN 是编译后的二进制, 输入尺寸烤死在模型里 —— 想从 640x640 换到
    576x576, 只能回到 ONNX 重转。而 rknn-toolkit2 的
    load_onnx(input_size_list=...) 对【固定形状】的 ONNX 是不生效的
    (实测: 转出来的模型输入还是 640x640, 输出形状也没变 —— 静默失败)。

    办法: 直接改 ONNX 图里输入的 dim_value, 再把图里那些"写死的旧形状"
    (value_info) 清掉。对 YOLOv8 这类全卷积网络(内部按 scale 做 Resize,
    不是按绝对尺寸), 改完就是对的。

用法:
    python3 resize_onnx.py in.onnx out.onnx --size 576 [--verify]
"""
from __future__ import annotations

import argparse
import sys


def resize(src: str, dst: str, size: int, verify: bool) -> bool:
    try:
        import onnx
    except ImportError:
        print("没装 onnx", file=sys.stderr)
        return False

    m = onnx.load(src)

    # ★ 这个 ONNX 是老 exporter 导出的: 有一批【权重】也挂在 graph.input 上
    #   (同时又在 initializer 里)。改尺寸时必须跳过它们, 否则会把权重
    #   的形状改成 [C,C,size,size] —— 之后 onnxruntime 直接报
    #   "Shape of initializer ... does not match"。
    #   注意: initializer 是 TensorProto 列表, 要取它的【名字】来比对。
    init_names = {t.name for t in m.graph.initializer}

    changed = 0
    for i in m.graph.input:
        if i.name in init_names:
            continue
        dims = i.type.tensor_type.shape.dim
        if len(dims) == 4:
            dims[2].dim_value = size
            dims[3].dim_value = size
            changed += 1
    if changed == 0:
        print("这个 ONNX 没有可改的 4 维输入", file=sys.stderr)
        return False

    # 清掉写死的旧形状(value_info): 留着会让形状推断报一堆 MergeShapeInfo
    # 警告, 也可能让 RKNN 按旧形状编译。
    del m.graph.value_info[:]
    for o in m.graph.output:
        o.type.tensor_type.shape.ClearField("dim")

    onnx.save(m, dst)
    print(f">>> 已写出 {dst} (输入改成 {size}x{size})")

    if not verify:
        return True
    try:
        import numpy as np
        import onnxruntime as ort
    except ImportError:
        print(">>> 没装 onnxruntime, 跳过校验")
        return True

    sess = ort.InferenceSession(dst, providers=["CPUExecutionProvider"])
    x = np.random.rand(1, 3, size, size).astype(np.float32)
    outs = sess.run(None, {sess.get_inputs()[0].name: x})
    print(">>> onnxruntime 跑通, 输出形状:")
    for o in outs:
        print(f"      {tuple(o.shape)}")
    head = outs[0].shape
    exp = size // 8
    if head[-1] != exp:
        print(f"!!! 输出头尺寸 {head[-1]} != 期望 {exp} —— 模型不对, 别拿去转 RKNN")
        return False
    print(f">>> 校验通过 (输出头 {head[-1]}x{head[-1]} = {size}/8)")
    return True


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--size", type=int, required=True)
    ap.add_argument("--verify", action="store_true")
    args = ap.parse_args()
    return 0 if resize(args.src, args.dst, args.size, args.verify) else 1


if __name__ == "__main__":
    sys.exit(main())
