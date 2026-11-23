#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""validate_pruned_onnx.py —— 剪枝后 ONNX 的结构与数值一致性校验

校验 4 件事:
  1. 结构自洽: Add 两个操作数等宽 / Split 各份等宽 / Concat 分支数不变 / Conv 输入通道数
     与生产者输出通道数一致
  2. 图输出形状: 与原模型【完全一致】(剪枝只改内部宽度, 不该改输出)
  3. 数值健全: onnxruntime 能跑通, 且输出没有 NaN/Inf
  4. 数值漂移: 与原模型的输出余弦相似度 —— 这是"没微调就剪"的精度损失度量

用法:
    ./validate_pruned_onnx.py --orig a.onnx --pruned b.onnx [--images img1 img2 ...]
"""

import argparse
import collections
import json
import os
import sys

import numpy as np
import onnx
from onnx import numpy_helper

CHANNEL_PRESERVING = {"Sigmoid", "Relu", "LeakyRelu", "Tanh", "Clip",
                       "MaxPool", "AveragePool", "Resize", "Upsample", "Pad",
                       "Identity", "Flatten", "Reshape", "Transpose"}


def analyse(path):
    m = onnx.load(path)
    g = m.graph
    init = {i.name: numpy_helper.to_array(i) for i in g.initializer}
    prod = {o: n for n in g.node for o in n.output}
    cons = collections.defaultdict(list)
    for n in g.node:
        for i in n.input:
            cons[i].append(n)

    def chan(tensor, depth=0):
        if depth > 6:
            return None
        p = prod.get(tensor)
        if p is None:
            for i in g.input:
                if i.name == tensor:
                    return 3
            return None
        if p.op_type == "Conv":
            for i in p.input:
                if i in init and init[i].ndim == 4:
                    return int(init[i].shape[0])
            return None
        if p.op_type == "Split":
            a = 1
            for at in p.attribute:
                if at.name == "axis":
                    a = at.i
            c = chan(p.input[0], depth + 1)
            return c // len(p.output) if c else None
        if p.op_type == "Concat":
            a = 1
            for at in p.attribute:
                if at.name == "axis":
                    a = at.i
            tot = 0
            for i in p.input:
                c = chan(i, depth + 1)
                if c is None:
                    return None
                tot += c
            return tot
        if p.op_type in ("Add", "Mul", "Sub", "Div"):
            return chan(p.input[0], depth + 1)
        return chan(p.input[0], depth + 1)

    issues = []
    # 1) Add 两个操作数等宽
    for n in g.node:
        if n.op_type in ("Add", "Sub", "Div"):
            cs = [chan(i) for i in n.input]
            if None not in cs and len(set(cs)) != 1:
                issues.append("Add %s 两个操作数宽度不等: %s" % (n.name, cs))
    # 2) Split 各份等宽
    for n in g.node:
        if n.op_type == "Split":
            cs = [chan(o) for o in n.output]
            if None not in cs and len(set(cs)) != 1:
                issues.append("Split %s 各份宽度不等: %s" % (n.name, cs))
    # 3) Conv 输入通道数 == 生产者输出通道数
    for n in g.node:
        if n.op_type != "Conv":
            continue
        wname = None
        for i in n.input:
            if i in init and init[i].ndim == 4:
                wname = i
                break
        if wname is None:
            continue
        in_c = int(init[wname].shape[1])
        src = chan(n.input[0])
        if src is not None and src != in_c:
            issues.append("Conv %s 输入通道 %d != 生产者输出 %d" % (n.name, in_c, src))
    return m, issues, init


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--orig", required=True)
    ap.add_argument("--pruned", required=True)
    ap.add_argument("--images", nargs="*", default=None)
    ap.add_argument("--size", type=int, default=0, help="推理用输入边长(0=按原模型)")
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    print("=" * 74)
    print("剪枝模型校验")
    print("=" * 74)

    mo, io, _ = analyse(args.orig)
    mp, ip, _ = analyse(args.pruned)
    print("[1] 结构自洽性")
    if io:
        print("    原模型问题 %d 处(可能有, 剪枝前就有)" % len(io))
        for x in io[:5]:
            print("      - %s" % x)
    if ip:
        print("    ✗ 剪枝模型有 %d 处结构问题:" % len(ip))
        for x in ip[:12]:
            print("      - %s" % x)
    else:
        print("    ✓ 剪枝模型结构自洽 (Add 等宽 / Split 等宽 / Conv 输入匹配)")

    print("[2] 图输出形状")
    oo = {o.name: [d.dim_value for d in o.type.tensor_type.shape.dim] for o in mo.graph.output}
    po = {o.name: [d.dim_value for d in o.type.tensor_type.shape.dim] for o in mp.graph.output}
    same = True
    for k in oo:
        a, b = oo[k], po.get(k)
        flag = "✓" if a == b else "✗"
        if a != b:
            same = False
        print("    %s %-24s 原=%s  剪后=%s" % (flag, k, a, b))

    print("[3][4] 数值健全性 + 与原模型的漂移")
    try:
        import onnxruntime as ort
        import cv2
    except Exception as e:
        print("    (跳过, onnxruntime/cv2 不可用: %s)" % e)
        return 0 if not ip else 1

    # 找原模型的输入尺寸
    size = args.size
    if not size:
        for o in mo.graph.input:
            d = [x.dim_value for x in o.type.tensor_type.shape.dim]
            if len(d) == 4:
                size = d[2]
    imgs = args.images or []
    if not imgs:
        default = "/home/seeed/rk_deploy/_bench/calib/calib_000.jpg"
        if os.path.exists(default):
            imgs = [default]

    so = ort.InferenceSession(args.orig, providers=["CPUExecutionProvider"])
    sp = ort.InferenceSession(args.pruned, providers=["CPUExecutionProvider"])
    iname = mo.graph.input[0].name

    cos_all = []
    for p in imgs:
        im = cv2.imread(p)
        if im is None:
            continue
        im = cv2.resize(im, (size, size), interpolation=cv2.INTER_LINEAR)
        x = (im[:, :, ::-1].copy().astype(np.float32) / 255.0).transpose(2, 0, 1)[None]
        o_out = so.run(None, {iname: x})
        p_out = sp.run(None, {iname: x})
        for k, (a, b) in enumerate(zip(o_out, p_out)):
            a = a.ravel().astype(np.float64)
            b = b.ravel().astype(np.float64)
            if b.size != a.size:
                print("    ✗ 输出 %d 元素数不一致: %d vs %d" % (k, a.size, b.size))
                continue
            na, nb = np.linalg.norm(a), np.linalg.norm(b)
            cos = float(a.dot(b) / (na * nb)) if na > 0 and nb > 0 else float("nan")
            finite = bool(np.isfinite(b).all())
            cos_all.append(cos)
            print("    %-24s cos=%.4f  max|Δ|=%.4f  finite=%s"
                  % (mo.graph.output[k].name, cos, np.abs(a - b).max(), finite))
    if cos_all:
        print("    平均余弦相似度: %.4f  (1.0=完全一致; 越低说明剪枝伤得越狠, 需要微调补回来)"
              % float(np.mean(cos_all)))

    if args.json:
        with open(args.json, "w") as f:
            json.dump({"orig": args.orig, "pruned": args.pruned,
                       "struct_issues": ip,
                       "output_shapes_same": same,
                       "cos": cos_all}, f, indent=2, ensure_ascii=False)
    return 0 if (not ip and same) else 1


if __name__ == "__main__":
    sys.exit(main())
