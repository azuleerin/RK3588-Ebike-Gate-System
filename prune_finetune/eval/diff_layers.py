#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""diff_layers.py —— 逐层对比 原模型 vs 剪枝模型, 定位第一处数值分叉

做法: 把两个模型的所有中间张量都加成图输出, 跑一遍, 逐个比对
      (剪枝模型的第 k 个通道 == 原模型的第 keep[k] 个通道)。
      第一处 cos 明显掉下去的层, 就是剪枝逻辑出错的地方。

用法:
    ./diff_layers.py --orig a.onnx --pruned b.onnx --prune-report b_prune.json [--limit N]
"""
import argparse
import json
import sys

import numpy as np
import onnx
import onnxruntime as ort

REPO = "/home/seeed/rk_deploy"


def add_all_outputs(m, names):
    """把中间张量加成图输出(带正确的类型, 否则 ORT 会直接忽略)。"""
    import onnx.helper as oh
    inferred = onnx.shape_inference.infer_shapes(m, strict_mode=False)
    vi_map = {v.name: v for v in inferred.graph.value_info}
    vi_map.update({o.name: o for o in inferred.graph.output})
    g = m.graph
    have = {o.name for o in g.output}
    for n in names:
        if n in have or n not in vi_map:
            continue
        v = vi_map[n]
        tt = v.type.tensor_type
        shape = [d.dim_value if d.dim_value > 0 else (d.dim_param or 1)
                 for d in tt.shape.dim]
        g.output.append(oh.make_tensor_value_info(n, tt.elem_type, shape))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--orig", required=True)
    ap.add_argument("--pruned", required=True)
    ap.add_argument("--prune-report", required=True)
    ap.add_argument("--limit", type=int, default=60)
    ap.add_argument("--size", type=int, default=576)
    args = ap.parse_args()

    rep = json.load(open(args.prune_report))
    keep_out = rep["keep_out"]

    mo = onnx.load(args.orig)
    mp = onnx.load(args.pruned)

    # 中间张量名(两边一致, 因为没改名)
    names = [t.name for t in mp.graph.value_info]
    # 也把 Conv 的输出都加上
    for n in mp.graph.node:
        for o in n.output:
            if o not in names:
                names.append(o)
    add_all_outputs(mo, names)
    add_all_outputs(mp, names)

    # ★ 必须把改过的模型存成临时文件再加载: 直接从原路径建 session 会用回原始图
    import tempfile
    fo = tempfile.NamedTemporaryFile(suffix=".onnx", delete=False)
    fp = tempfile.NamedTemporaryFile(suffix=".onnx", delete=False)
    onnx.save(mo, fo.name)
    onnx.save(mp, fp.name)
    fo.close()
    fp.close()

    import cv2
    img = cv2.imread(REPO + "/_bench/calib/calib_000.jpg")
    img = cv2.resize(img, (args.size, args.size))[:, :, ::-1].copy()
    x = (img.astype(np.float32) / 255.0).transpose(2, 0, 1)[None]

    so = ort.InferenceSession(fo.name, providers=["CPUExecutionProvider"])
    sp = ort.InferenceSession(fp.name, providers=["CPUExecutionProvider"])
    iname = mo.graph.input[0].name
    oo = so.run(None, {iname: x})
    po = sp.run(None, {iname: x})
    onames = [o.name for o in so.get_outputs()]
    pnames = [o.name for o in sp.get_outputs()]
    omap = dict(zip(onames, oo))
    pmap = dict(zip(pnames, po))

    print("%-46s %-8s %-9s %-9s %-10s" % ("张量", "剪后ch", "cos", "max|Δ|", "原/剪 RMS"))
    bad = 0
    prod = {o: n for n in mo.graph.node for o in n.output}
    for t in names:
        if t not in omap or t not in pmap:
            continue
        a = omap[t]
        b = pmap[t]
        if a.ndim < 2 or b.ndim < 2:
            continue
        # 通道在 axis=1
        if a.shape[1] == b.shape[1]:
            idx = list(range(a.shape[1]))
        else:
            pn = prod.get(t)
            if pn is None or pn.name not in keep_out:
                print("%-46s %-8d %-9s %-9s %-10s" % (t[:46], b.shape[1],
                                                       "SKIP", "-", "-"))
                continue
            ko = keep_out[pn.name]
            if len(ko) != b.shape[1]:
                print("%-46s %-8d %-9s %-9s %-10s" % (t[:46], b.shape[1],
                                                       "SKIPlen", "-", "-"))
                continue
            idx = ko
        aa = a[:, idx]
        bb = b
        if aa.shape != bb.shape:
            print("%-46s %-8d %-9s %-9s %-10s" % (t[:46], bb.shape[1],
                                                   "SHAPE", "-", "-"))
            continue
        af = aa.ravel().astype(np.float64)
        bf = bb.ravel().astype(np.float64)
        na, nb = np.linalg.norm(af), np.linalg.norm(bf)
        cos = float(af.dot(bf) / (na * nb)) if na > 1e-12 and nb > 1e-12 else float("nan")
        rms = float(np.sqrt(np.mean((af - bf) ** 2)))
        mark = "" if cos >= 0.999 else "  <<<"
        if cos < 0.999:
            bad += 1
        print("%-46s %-8d %-9.4f %-9.4f %-10.4f%s" %
              (t[:46], bb.shape[1], cos, float(np.abs(af - bf).max()), rms, mark))
        if bad >= args.limit:
            print("  ... (超过 limit, 停止)")
            break
    return 0


if __name__ == "__main__":
    sys.exit(main())
