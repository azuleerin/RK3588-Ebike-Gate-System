#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""model_complexity.py —— 从 ONNX 图算参数量 / MACs / 各层通道宽度

用来量化"剪枝到底砍掉了多少计算量"(任务要求: 进一步削减模型计算量)。
MACs = sum over Conv of  out_c * in_c * kh * kw * H_out * W_out

用法:
    ./model_complexity.py a.onnx b.onnx ...
"""
import json
import sys

import numpy as np
import onnx
from onnx import numpy_helper


def analyse(path):
    m = onnx.load(path)
    g = m.graph
    init = {i.name: numpy_helper.to_array(i) for i in g.initializer}
    prod = {o: n for n in g.node for o in n.output}
    # 形状: 优先用图里声明的 value_info / output
    vi = {}
    try:
        inf = onnx.shape_inference.infer_shapes(m, strict_mode=False)
        for v in inf.graph.value_info:
            vi[v.name] = [d.dim_value for d in v.type.tensor_type.shape.dim]
        for o in inf.graph.output:
            vi[o.name] = [d.dim_value for d in o.type.tensor_type.shape.dim]
    except Exception:
        pass
    # 逐节点算输出空间大小
    hw = {}
    for n in g.node:
        if n.op_type == "Conv":
            t = n.output[0]
            sh = vi.get(t)
            if sh and len(sh) == 4 and all(x > 0 for x in sh):
                hw[t] = sh[2] * sh[3]
            else:
                hw[t] = 0
        elif n.op_type in ("Relu", "Sigmoid", "Mul", "Add", "Clip", "MaxPool",
                           "AveragePool", "Reshape", "Transpose", "Identity"):
            for t in n.output:
                src = n.input[0]
                hw[t] = hw.get(src, 0)
        elif n.op_type == "Concat":
            tot = 0
            for i in n.input:
                tot += hw.get(i, 0)
            for t in n.output:
                hw[t] = tot
        elif n.op_type == "Split":
            src = n.input[0]
            per = hw.get(src, 0) // max(len(n.output), 1)
            for t in n.output:
                hw[t] = per
        elif n.op_type == "Resize":
            for t in n.output:
                hw[t] = hw.get(n.input[0], 0)
        else:
            for t in n.output:
                hw[t] = hw.get(n.input[0], 0) if n.input else 0

    params = 0
    macs = 0
    convs = []
    for n in g.node:
        if n.op_type != "Conv":
            continue
        w = None
        for i in n.input:
            if i in init and init[i].ndim == 4:
                w = init[i]
                break
        if w is None:
            continue
        oc, ic, kh, kw = w.shape
        params += int(oc * ic * kh * kw)
        px = 1
        for i in n.input:
            if i in init and init[i].ndim == 1:
                params += int(init[i].size)
                break
        out_hw = hw.get(n.output[0], 0)
        mac = int(oc * ic * kh * kw * out_hw)
        macs += mac
        convs.append({"name": n.name, "out_c": int(oc), "in_c": int(ic),
                      "kh": int(kh), "kw": int(kw), "hw": out_hw, "macs": mac})
    return {"file": path, "params": params, "macs": macs, "convs": convs,
            "n_conv": len(convs)}


def main():
    out = []
    for p in sys.argv[1:]:
        a = analyse(p)
        out.append(a)
        print("%-46s params %7.3f M | MACs %8.2f M | conv %d" %
              (p.split("/")[-1], a["params"] / 1e6, a["macs"] / 1e6, a["n_conv"]))
    if len(out) > 1:
        base = out[0]
        print("\n%-46s %-14s %-14s" % ("模型", "params减少", "MACs减少"))
        for a in out[1:]:
            print("%-46s %-14.2f%% %-14.2f%%" % (
                a["file"].split("/")[-1],
                100 * (1 - a["params"] / base["params"]),
                100 * (1 - a["macs"] / base["macs"])))
    with open("/home/seeed/rk_deploy/prune_finetune/out/complexity.json", "w") as f:
        json.dump(out, f, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
