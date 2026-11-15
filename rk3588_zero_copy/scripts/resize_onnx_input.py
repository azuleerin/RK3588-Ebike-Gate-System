#!/usr/bin/env python3
"""resize_onnx_input.py —— 把固定尺寸的 ONNX 改成另一个输入尺寸

为什么需要它:
    RKNN 是编译后的二进制, 输入尺寸烤死在模型里 —— 想从 640x640 换到
    416x416, 只能回到 ONNX 重转。而 rknn-toolkit2 的
    load_onnx(input_size_list=...) 对【固定形状】的 ONNX 是【不生效】的
    (实测: 转出来的模型输入还是 640x640, 输出形状也没变 —— 静默失败,
     最坑的是它不报错)。

    办法: 直接改 ONNX 图里输入的 dim_value, 再把图里那些"写死的旧形状"
    (value_info) 清掉。对 YOLOv8 这类全卷积网络(内部按 scale 做 Resize,
    不是按绝对尺寸), 改完就是对的 —— 实测 onnxruntime 能跑通,
    输出头尺寸也按新尺寸走(416 -> 52x52, 320 -> 40x40)。

用法:
    python3 scripts/resize_onnx_input.py in.onnx out.onnx --size 416

校验(强烈建议做这一步):
    python3 scripts/resize_onnx_input.py in.onnx out.onnx --size 416 --verify
"""
from __future__ import annotations

import argparse
import sys


def resize(src: str, dst: str, size: int, verify: bool) -> bool:
    try:
        import onnx
    except ImportError:
        print("没装 onnx: pip3 install onnx", file=sys.stderr)
        return False

    m = onnx.load(src)

    # ---- 1. 改输入的形状 ----
    changed = 0
    for i in m.graph.input:
        dims = i.type.tensor_type.shape.dim
        if len(dims) == 4:
            dims[2].dim_value = size
            dims[3].dim_value = size
            changed += 1
    if changed == 0:
        print("这个 ONNX 没有 4 维输入, 改不了", file=sys.stderr)
        return False

    # ---- 2. 清掉写死的旧形状(value_info) ----
    #   它们是 640 那次导出留下的, 留着会让 onnxruntime/工具链在形状推断时
    #   报一堆 "MergeShapeInfo" 警告, 也可能让 RKNN 按旧形状编译。
    del m.graph.value_info[:]
    for o in m.graph.output:
        o.type.tensor_type.shape.ClearField("dim")

    onnx.save(m, dst)
    print(f">>> 已写出 {dst} (输入改成 {size}x{size}, 清掉了 {len(m.graph.value_info)} 条旧 shape)")

    # ---- 3. 用 onnxruntime 校验(形状对不对, 能不能跑) ----
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
    print(f">>> 校验通过 (输出头 {head[-1]}x{head[-1]} = {size}/8 ✓)")
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
