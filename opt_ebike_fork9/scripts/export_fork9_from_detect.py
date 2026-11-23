#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""export_fork9_from_detect.py —— 把【原生 Detect(DFL 在图内)】的 ONNX 改成 fork9(9 输出)

★ 这是任务书"第一步"的正式实现: 输入 `best_split.onnx`(或任何原生 Detect 的
  ultralytics ONNX), 输出 9 输出的 fork9 ONNX。

★ 改造做了什么
  原生 Detect 的图长这样(每个尺度 i ∈ {0,1,2}):

      cv2.{i}.0.conv ─► cv2.{i}.1.conv ─► cv2.{i}.2 ─► [64ch 原始 DFL logits] ─┐
                                                                            ├─► Concat(66) ─► … ─► DFL ─► dist2bbox ─► box[1,4,8400]
      cv3.{i}.0.conv ─► cv3.{i}.1.conv ─► cv3.{i}.2 ─► Sigmoid ─► [ncch 概率] ──┘        └─► Sigmoid ─► cls[1,nc,8400]

  fork9 只要在【DFL 之前】下刀:

      输出 3i+0 = [1, 64, H, W]   cv2.{i}.2 的原始 DFL logits   (DFL 搬到 CPU 解)
      输出 3i+1 = [1, nc, H, W]  cv3.{i}.2 过 Sigmoid 的概率
      输出 3i+2 = [1,  1, H, W]  Clip(ReduceSum(cls, axis=1), 0, 1)

  ⇒ 图内 DFL 子图、dist2bbox、66 通道的 Concat/Reshape/Split 全部【不再需要】,
    DCE 掉即可; 骨干与检测头卷积一个字节都不动。

★ 怎么找到那三个张量
  优先按【节点名】找(ultralytics 导出的命名是稳定的, 生产 .rknn 里也保留着):
      /model.{m}/cv2.{i}/cv2.{i}.2/Conv     -> box logits
      /model.{m}/cv3.{i}/cv3.{i}.2/Conv     -> cls logits
  名字对不上就退回【按形状】找(64 通道 = 4*reg_max)。

用法:
    export_fork9_from_detect.py <in.onnx> <out.onnx> [--dfl-len 16]
"""
import argparse
import re
import sys

import numpy as np
import onnx
from onnx import helper, numpy_helper, TensorProto

STRIDES = [8, 16, 32]


def shape_of(m, name):
    for vi in list(m.graph.output) + list(m.graph.value_info) + list(m.graph.input):
        if vi.name == name:
            return [d.dim_value for d in vi.type.tensor_type.shape.dim]
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--dfl-len', type=int, default=16)
    ap.add_argument('--imgsz', type=int, default=640)
    args = ap.parse_args()

    m = onnx.load(args.src)
    g = m.graph
    opset = max(o.version for o in m.opset_import if o.domain in ('', 'ai.onnx'))
    print(f"  源 ONNX opset = {opset}")

    # ---------- 已经是 fork9 就直接返回 ----------
    n_out = len(g.output)
    if n_out == 9:
        print("  源模型已经是 9 输出(fork9), 直接用")
        onnx.save(m, args.dst)
        return

    # ---------- 找检测头 ----------
    by_name = {n.name: n for n in g.node if n.name}
    producers = {}
    for n in g.node:
        for o in n.output:
            producers[o] = n

    # ---------- 找检测头 ----------
    # ★ 不能取"第一个 /model.N/" —— 那是 backbone(model.0)。检测头的标志是它有
    #   cv2./cv3. 分支。ultralytics 导出的节点名稳定:
    #       /model.{m}/cv2.{i}/cv2.{i}.2/Conv    -> box 分支最后一层(出 4*reg_max 通道)
    #       /model.{m}/cv3.{i}/cv3.{i}.2/Conv    -> cls 分支最后一层(出 nc 通道)
    #   生产 .rknn 里保留的就是这套名字(见 docs/01_ebike_topology_evidence.md)
    RX_BOX = re.compile(r'^/model\.(\d+)/cv2\.(\d+)/cv2\.\d+\.\d+/Conv$')
    RX_CLS = re.compile(r'^/model\.(\d+)/cv3\.(\d+)/cv3\.\d+\.\d+/Conv$')
    box_logits, cls_logits = {}, {}
    head_idx = None
    for n in g.node:
        nm = n.name or ''
        mb = RX_BOX.match(nm)
        mc = RX_CLS.match(nm)
        if mb:
            box_logits[int(mb.group(2))] = n.output[0]
            head_idx = int(mb.group(1))
        if mc:
            cls_logits[int(mc.group(2))] = n.output[0]
            head_idx = head_idx if head_idx is not None else int(mc.group(1))
    if len(box_logits) == 3 and len(cls_logits) == 3:
        print(f"  检测头 = model.{head_idx}")
        for i in range(3):
            print(f"  尺度{i}: box={box_logits[i]}  cls={cls_logits[i]}")

    if len(box_logits) != 3 or len(cls_logits) != 3:
        # 退回按形状找, 但【必须在 cv2/cv3 分支里】——骨干里也有 64 通道的卷积
        print("  节点名对不上, 改按形状找 (限定 cv2/cv3 分支内)")
        cands_box, cands_cls = [], []
        for n in g.node:
            if n.op_type != 'Conv':
                continue
            nm = n.name or ''
            if '/cv2' not in nm and '/cv3' not in nm:
                continue
            for o in n.output:
                s = shape_of(m, o)
                if s and len(s) == 4:
                    if s[1] == 4 * args.dfl_len:
                        cands_box.append(o)
                    elif 1 < s[1] < 64:
                        cands_cls.append(o)
        if len(cands_box) < 3 or len(cands_cls) < 3:
            raise SystemExit(f"按形状也没找全: box={len(cands_box)} cls={len(cands_cls)}")

        def by_hw(cands):
            d = {}
            for c in cands:
                s = shape_of(m, c)
                d.setdefault((s[2], s[3]), []).append(c)
            return d
        bb, cc = by_hw(cands_box), by_hw(cands_cls)
        for i, st in enumerate(STRIDES):
            hw = (args.imgsz // st, args.imgsz // st)
            if hw not in bb or hw not in cc:
                raise SystemExit(f"尺度 {hw} 找不到 box/cls")
            box_logits[i] = bb[hw][0]
            cls_logits[i] = cc[hw][0]
            print(f"  尺度{i}: box={box_logits[i]}  cls={cls_logits[i]}")

    # ---------- 组装 9 个输出 ----------
    nodes, inits = [], []

    def const_i(name, arr):
        t = numpy_helper.from_array(np.asarray(arr, dtype=np.int64), name)
        inits.append(t)
        return name

    def const_f(name, arr):
        t = numpy_helper.from_array(np.asarray(arr, dtype=np.float32), name)
        inits.append(t)
        return name

    outs = []
    for i in range(3):
        h = w = args.imgsz // STRIDES[i]
        bt, ct = box_logits[i], cls_logits[i]
        bs, cs = shape_of(m, bt), shape_of(m, ct)
        nc = cs[1]

        # cls: 如果下游已经有 Sigmoid 就直接用它的输出, 否则自己加一个
        sig_out = None
        for n in g.node:
            if n.op_type == 'Sigmoid' and ct in n.input:
                sig_out = n.output[0]
                break
        if sig_out is None:
            sig_out = f'fork9/cls_sigmoid_{i}'
            nodes.append(helper.make_node('Sigmoid', [ct], [sig_out],
                                          name=f'fork9/Sigmoid_{i}'))

        # sum = Clip(ReduceSum(cls, axis=1), 0, 1)
        rs = f'fork9/sum_rs_{i}'
        if 13 <= opset <= 17:
            ax = const_i(f'fork9/sum_axes_{i}', [1])
            nodes.append(helper.make_node('ReduceSum', [sig_out, ax], [rs],
                                          name=f'fork9/ReduceSum_{i}'))
        else:
            nodes.append(helper.make_node('ReduceSum', [sig_out], [rs], axes=[1],
                                          name=f'fork9/ReduceSum_{i}'))
        lo = const_f(f'fork9/sum_min_{i}', 0.0)
        hi = const_f(f'fork9/sum_max_{i}', 1.0)
        sm = f'fork9/sum_{i}'
        nodes.append(helper.make_node('Clip', [rs, lo, hi], [sm], name=f'fork9/Clip_{i}'))

        outs.append(helper.make_tensor_value_info(bt, TensorProto.FLOAT, [1, 4 * args.dfl_len, h, w]))
        outs.append(helper.make_tensor_value_info(sig_out, TensorProto.FLOAT, [1, nc, h, w]))
        outs.append(helper.make_tensor_value_info(sm, TensorProto.FLOAT, [1, 1, h, w]))

    del g.output[:]
    g.output.extend(outs)
    g.node.extend(nodes)
    g.initializer.extend(inits)

    # ---------- DCE: 只保留能走到新输出的节点 ----------
    produced = {o.name for o in g.output}
    keep, stack = set(), list(produced)
    for n in g.node:
        for o in n.output:
            producers[o] = n
    while stack:
        name = stack.pop()
        n = producers.get(name)
        if n is None or id(n) in keep:
            continue
        keep.add(id(n))
        stack.extend(n.input)
    dead = [n for n in g.node if id(n) not in keep]
    if dead:
        print(f"  DCE 掉 {len(dead)} 个无用节点 (DFL/dist2bbox/Concat/Reshape 等)")
        kn = [n for n in g.node if id(n) in keep]
        del g.node[:]
        g.node.extend(kn)

    used = set()
    for n in g.node:
        used.update(n.input)
    used.update(i.name for i in g.input)
    used.update(o.name for o in g.output)
    inits_new = [t for t in g.initializer if t.name in used]
    print(f"  initializer: {len(g.initializer)} -> {len(inits_new)}")
    del g.initializer[:]
    g.initializer.extend(inits_new)

    keep_in = [vi for vi in g.input if vi.name == 'images']
    if keep_in:
        del g.input[:]
        g.input.extend(keep_in)

    m = onnx.shape_inference.infer_shapes(m, strict_mode=False)
    onnx.checker.check_model(m)
    onnx.save(m, args.dst)
    print(f"  -> {args.dst}")
    for i, o in enumerate(m.graph.output):
        role = ['box', 'cls', 'sum'][i % 3]
        print(f"  输出[{i}] {role}: {o.name} {[d.dim_value for d in o.type.tensor_type.shape.dim]}")


if __name__ == '__main__':
    main()
