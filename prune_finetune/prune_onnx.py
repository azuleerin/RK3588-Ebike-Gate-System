#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""prune_onnx.py —— YOLOv8 ONNX 结构化通道剪枝(不重训版)

======================================================================
【为什么在 ONNX 上剪, 而不是按常规做法在 PyTorch 上剪 BN gamma】
======================================================================
常规做法: 在 PyTorch 里给 BN 的 gamma 加 L1 正则 -> 训练 -> 按 |gamma| 排序剪通道。
本项目的现实:
  * 仓库里【没有】yolov8n 的 .pt 权重, 也没有 ultralytics, 板子也没有 GPU, 没有外网;
  * 手上唯一可训练级的 YOLOv8 载体是已【优化过】的 ONNX;
  * 这个 ONNX 里【已经没有 BatchNorm 节点】(BN 已经被折叠进 Conv 的权重/偏置)。

所以本脚本用【折叠后卷积核的逐通道 L1 范数】作为重要性度量:
    BN 折叠关系   W_folded = gamma * W / sqrt(sigma^2 + eps)
    => 折叠后第 c 个输出通道的权��尺度 ∝ |gamma_c| * ||W_c||
    => 【按折叠后权重的逐通道 L1 范数排序, 等价于按 |gamma| 排序】
两者排序高度一致(相差一个逐层常数), 这就是本脚本的判据依据。
真要复现"BN gamma + L1 稀疏化训练"那条路, 见 sparse_train.py (需 PyTorch 环境)。

======================================================================
【剪枝准则与结构约束】
======================================================================
1. 重要性: 每个 Conv 输出通道的 L1 范数 ||W[o]||_1, 越小越先被剪。
2. 结构规则(保证剪完还是规则结构, RKNN 兼容):
   R1 受保护 Conv 不剪输出通道:
      - 输出直接是图输出的 Conv (box 分支 cv2.*.2, 64 通道)
      - 输出经 Sigmoid 后成为图输出的 Conv (cls 分支 cv3.*.2, 80 通道)
        -> 这两个决定图输出的形状, 剪了就对不上
   R2 C2f 块: cv1 的输出被 Split 成 n 份, 其中一份与 bottleneck 的输出做 Add。
      -> Add 两个操作数通道数必须相等, 且 Split 的 n 份必须等宽
      => 设块内宽度 w' = round(w*(1-r)), 则 cv1 输出 = n*w', bottleneck 链各层 = w'
   R3 Concat: 各分支宽度可以不同(Concat 只做拼接),
      但下游消费者 Conv 的输入通道数 = 各分支【实际】通道数之和(由传播决定, 不是各自 round)
   R4 其它 Conv: 输出通道数 = round(out_c*(1-r)), 至少留 1 个
   R5 通道对齐: bottleneck 链的各层沿用【同一组】幸存通道下标,
      保证 Add 的两个操作数是同下标对应的(语义不错位)
3. 输入通道剪枝: 消费者 Conv 的输入按生产者的【幸存下标映射】切片。

用法:
    ./prune_onnx.py --onnx in.onnx --out out.onnx --ratio 0.3
    ./prune_onnx.py --onnx in.onnx --out out.onnx --ratio 0.3 --report report.json
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import sys

import numpy as np
import onnx
from onnx import numpy_helper

# 通道数保持不变的算子(逐元素 / 空间类)
CHANNEL_PRESERVING = {
    "Sigmoid", "Relu", "LeakyRelu", "Tanh", "Clip", "MaxPool", "AveragePool",
    "Resize", "Upsample", "Pad", "Identity", "Flatten", "Reshape", "Transpose",
    "Split", "Concat", "Add", "Mul", "Sub", "Div",
}


def _producer_map(graph):
    return {o: n for n in graph.node for o in n.output}


def _consumer_map(graph):
    c = collections.defaultdict(list)
    for n in graph.node:
        for i in n.input:
            c[i].append(n)
    return c


def _conv_weight(graph, node):
    """取 Conv 的权重 initializer。"""
    init = {i.name: i for i in graph.initializer}
    for i in node.input:
        if i in init and len(init[i].dims) == 4:
            return init[i]
    return None


def _split_axis(node):
    for a in node.attribute:
        if a.name == "axis":
            return a.i
    return 0


def _reduce_axes(node):
    for a in node.attribute:
        if a.name == "axes":
            return list(a.ints)
    return None


class Pruner:
    def __init__(self, model, ratio, min_keep=8, verbose=True,
                 onnx_path=None, act_stats=None, align=16):
        self.m = model
        # ★ 对齐粒度: RK3588 的 NPU 按 NC1HWC2 处理, 卷积通道会补齐到 16 的倍数。
        #   不对齐的话, 理论 MACs 砍掉 35% 但硬件上只省 14%(补齐把宽度又填回去了)。
        self.align = align if align and align > 1 else 1
        self.onnx_path = onnx_path
        self.act_stats = act_stats or {}
        self.g = model.graph
        self.ratio = ratio
        self.min_keep = min_keep
        self.verbose = verbose
        self.init = {i.name: numpy_helper.to_array(i) for i in self.g.initializer}
        self.prod = _producer_map(self.g)
        self.cons = _consumer_map(self.g)

        # 每个 Conv 节点: {node_name: (out_c, in_c)}
        self.convs = [n for n in self.g.node if n.op_type == "Conv"]
        self.protected = set()
        self.k_out = {}          # conv name -> 目标输出通道数
        self.keep_out = {}       # conv name -> 幸存下标(list)
        self.keep_in = {}        # conv name -> 输入幸存下标(list)

    # ---------------------------------------------------------------- 保护集
    def find_protected(self):
        """R1: 决定哪些 Conv 的输出通道不能剪。"""
        gouts = {o.name for o in self.g.output}

        def reaches_graph_out(tensor, depth=0):
            """沿通道数不变的算子往下走, 看能不能到图输出。"""
            if depth > 4:
                return False
            for n in self.cons.get(tensor, []):
                if n.op_type in ("Conv", "Concat", "Split", "Add", "Mul"):
                    continue                     # 这些会改通道数, 不能直接判
                for o in n.output:
                    if o in gouts:
                        return True
                    if reaches_graph_out(o, depth + 1):
                        return True
            return False

        for n in self.convs:
            w = self.init.get(self._w_name(n))
            if w is None:
                continue
            out_c = w.shape[0]
            # (a) 输出直接就是图输出
            if n.output[0] in gouts:
                self.protected.add(n.name)
                continue
            # (b) 输出经 Sigmoid/Relu 等逐元素算子后成为图输出
            if reaches_graph_out(n.output[0]):
                self.protected.add(n.name)
                continue
            # (c) 输出喂给了 ReduceSum(axis=1) —— 类别得分就是这些通道求和来的
            for c in self.cons.get(n.output[0], []):
                if c.op_type == "ReduceSum":
                    axes = _reduce_axes(c)
                    if axes and 1 in axes:
                        self.protected.add(n.name)
                        break
        if self.verbose:
            print("    受保护 Conv(不剪输出通道): %d 个 -> %s"
                  % (len(self.protected),
                     ", ".join(sorted(x.split("/")[-2] for x in self.protected))[:160]))

    def _w_name(self, node):
        for i in node.input:
            if i in self.init and self.init[i].ndim == 4:
                return i
        return None

    def _conv_ancestor(self, tensor, depth=0):
        """往回找产生该张量的 Conv(穿过逐元素算子)。

        ★ SiLU 图里激活是 Mul(X, Sigmoid(X)), 所以 Mul 也要能穿过去;
          穿的时候取"通道数多"的那个输入, 避免穿到广播用的标量上。
        """
        if depth > 4:
            return None
        p = self.prod.get(tensor)
        if p is None:
            return None
        if p.op_type == "Conv":
            return p
        if p.op_type in ("Relu", "Sigmoid", "LeakyRelu", "Tanh", "Identity"):
            return self._conv_ancestor(p.input[0], depth + 1)
        if p.op_type == "Mul":
            best, best_c = None, -1
            for i in p.input:
                c = self._tensor_channels(i)
                if c is not None and c > best_c:
                    best, best_c = i, c
            if best is None:
                best = p.input[0]
            return self._conv_ancestor(best, depth + 1)
        return None

    # ------------------------------------------------------- R2: C2f 块宽度
    def compute_counts(self):
        """先定每个 Conv 的目标输出通道数 k_out。"""
        for n in self.convs:
            w = self.init[self._w_name(n)]
            out_c = w.shape[0]
            if n.name in self.protected:
                self.k_out[n.name] = out_c
            else:
                k = int(round(out_c * (1.0 - self.ratio)))
                k = max(self.min_keep if out_c >= self.min_keep else 1, k)
                # ★ 对齐到 NPU 粒度, 否则补齐会把省下来的宽度又填回去
                if out_c % self.align == 0:
                    k = min(out_c, self.align_c(k))
                self.k_out[n.name] = k

        # R2: Split 的各份必须等宽, 且 Split 的生产者输出 = n * 份宽
        self.splits = []
        self.add_pairs = []
        for n in self.g.node:
            if n.op_type != "Split":
                continue
            axis = _split_axis(n)
            if axis != 1:
                continue
            p = self._conv_ancestor(n.input[0])
            if p is None:
                continue
            nchunk = len(n.output)
            pw = self.init[self._w_name(p)]
            chunk_w = pw.shape[0] // nchunk
            # 块内宽度: 按 chunk_w 打折
            new_chunk_w = max(self.min_keep if chunk_w >= self.min_keep else 1,
                              int(round(chunk_w * (1.0 - self.ratio))))
            if chunk_w % self.align == 0:
                new_chunk_w = min(chunk_w, self.align_c(new_chunk_w))
            # 让 Split 生产者输出 = nchunk * new_chunk_w
            self.k_out[p.name] = nchunk * new_chunk_w
            self.splits.append({"node": n, "producer": p, "nchunk": nchunk,
                                "chunk_w": chunk_w, "new_chunk_w": new_chunk_w})
            # 找到与某一 chunk 做 Add 的那个 Conv(bottleneck 末端), 它的输出必须 = new_chunk_w
            for chunk in n.output:
                for c in self.cons.get(chunk, []):
                    if c.op_type != "Add":
                        continue
                    other = [i for i in c.input if i != chunk]
                    for oi in other:
                        op = self._conv_ancestor(oi)
                        if op is not None:
                            # bottleneck 末端: 输出宽度 = 块内宽度
                            self.k_out[op.name] = new_chunk_w
                            # bottleneck 前一层的输出也要 = 块内宽度(它喂给末端)
                            prev = self._conv_ancestor(op.input[0])
                            if prev is not None and prev.name not in self.protected:
                                self.k_out[prev.name] = new_chunk_w
                            self.add_pairs.append((c, chunk, oi))
            if self.verbose:
                print("    C2f 块 %-24s 宽度 %d -> %d (Split %d 份)"
                      % (p.name.split("/")[-2], chunk_w, new_chunk_w, nchunk))

    def align_c(self, c):
        """把通道数对齐到 NPU 的粒度(默认 16 的倍数)。"""
        if self.align <= 1:
            return max(1, int(c))
        a = self.align
        v = int(round(c / a)) * a
        return max(a, v)

    # ------------------------------------------------------- 激活统计
    def collect_activation_stats(self, calib_list, n_images=24, size=None):
        """用校准图跑一遍原模型, 统计每个 Conv 输出通道的平均绝对激活。

        ★ 为什么必须带激活: 光看 |W| 会挑错通道。
          一个通道的"权重 L1 大"不代表它对结果重要 —— 还要看它下游的激活多大。
          重要性 = ||W[o]||_1 * E|act[o]| 才是"这个通道对输出的实际贡献"。
          ★ 这一步等价于 BN 稀疏化训练里 gamma 的角色, 但不需要重训就能拿到激活侧信息。
        """
        try:
            import cv2
            import onnxruntime as ort
            import onnx.helper as oh
        except Exception as e:                                    # pragma: no cover
            print("    (拿不到激活统计: %s, 退回纯权重判据)" % e)
            return {}
        if size is None:
            # 从模型输入形状取边长(NCHW 或 NHWC 都认)
            d = [x.dim_value for x in self.g.input[0].type.tensor_type.shape.dim]
            size = d[2] if len(d) == 4 and d[2] > 0 else 640
        paths = [l.strip() for l in open(calib_list) if l.strip()][:n_images]
        imgs = []
        for p in paths:
            im = cv2.imread(p)
            if im is None:
                continue
            imgs.append(cv2.resize(im, (size, size), interpolation=cv2.INTER_LINEAR)
                       [:, :, ::-1].copy())
        if not imgs:
            return {}

        # ★ 必须在【副本】上做: 否则会把"加了中间输出 + 旧形状声明"留在这个模型里,
        #   后面剪枝完的图里这些陈旧形状会把 onnxruntime 的形状推理带偏
        #   (典型症状: Add 报 Incompatible dimensions)。
        import copy
        probe = copy.deepcopy(self.m)
        pg = probe.graph
        inferred = onnx.shape_inference.infer_shapes(probe, strict_mode=False)
        vi = {v.name: v for v in inferred.graph.value_info}
        want = []
        have = {o.name for o in pg.output}
        for n in probe.graph.node:
            if n.op_type != "Conv":
                continue
            t = n.output[0]
            if t in vi and t not in have:
                tt = vi[t].type.tensor_type
                shape = [d.dim_value if d.dim_value > 0 else 1 for d in tt.shape.dim]
                pg.output.append(oh.make_tensor_value_info(t, tt.elem_type, shape))
                want.append(t)
        try:
            import tempfile
            tf = tempfile.NamedTemporaryFile(suffix=".onnx", delete=False)
            onnx.save(probe, tf.name)
            tf.close()
            so = ort.InferenceSession(tf.name, providers=["CPUExecutionProvider"])
        except Exception as e:
            print("    (onnxruntime 起不来: %s, 退回纯权重判据)" % e)
            return {}
        iname = self.g.input[0].name
        onm = [o.name for o in so.get_outputs()]
        acc = {}
        cnt = 0
        for im in imgs:
            x = (im.astype(np.float32) / 255.0).transpose(2, 0, 1)[None]
            outs = so.run(None, {iname: x})
            for t in want:
                if t not in onm:
                    continue
                a = np.abs(outs[onm.index(t)])
                # (1, C, H, W) -> 每通道的平均绝对值
                v = a.reshape(a.shape[0], a.shape[1], -1).mean(axis=2).ravel()
                acc[t] = acc.get(t, 0) + v
            cnt += 1
        if cnt == 0:
            return {}
        return {t: v / cnt for t, v in acc.items()}

    # ------------------------------------------------------- 幸存下标的选择
    def select_survivors(self):
        """按重要性选幸存下标。

        ★ 重要性 = ||W[o]||_1 * E|act[o]|  (激活感知; 没拿到激活就退回纯权重)
        ★ Split 的生产者必须【对称剪】: 它的输出被切成 n 份, 每份必须等宽,
          否则 Split 不合法。所以每一份里选相同数量的通道。
        ★ R5: bottleneck 末端(喂给 Add 的那个)的输出幸存下标, 直接沿用
          Add 另一侧(Split chunk)的幸存下标, 保证逐元素 Add 的两个操作数
          下标一一对应(语义不错位)。
        """
        act = self.act_stats or {}
        # (a) 先给每个 Conv 选幸存下标
        for n in self.convs:
            w = self.init[self._w_name(n)]
            out_c = w.shape[0]
            if n.name in self.protected:
                self.keep_out[n.name] = list(range(out_c))
                continue
            k = min(self.k_out[n.name], out_c)
            imp = np.abs(w).reshape(out_c, -1).sum(axis=1)
            a = act.get(n.output[0])
            if a is not None and len(a) == out_c:
                imp = imp * np.maximum(a, 1e-8)
            order = np.argsort(imp)[::-1]          # 重要 -> 不重要
            self.keep_out[n.name] = sorted(order[:k].tolist())

        # (b) Split 生产者改成对称剪
        self.chunk_map = {}          # split 输出张量名 -> 幸存下标(该份内的空间)
        for sp in self.splits:
            p = sp["producer"]
            w = self.init[self._w_name(p)]
            out_c = w.shape[0]
            per = out_c // sp["nchunk"]
            target = self.k_out[p.name] // sp["nchunk"]
            imp = np.abs(w).reshape(out_c, -1).sum(axis=1)
            a = act.get(p.output[0])
            if a is not None and len(a) == out_c:
                imp = imp * np.maximum(a, 1e-8)
            sel = []
            for c in range(sp["nchunk"]):
                lo, hi = c * per, (c + 1) * per
                cand = np.arange(lo, hi)
                local = imp[lo:hi]
                take = np.argsort(local)[::-1][:target]
                sel.extend(cand[take].tolist())
            self.keep_out[p.name] = sorted(sel)
            for c, o in enumerate(sp["node"].output):
                lo = c * per
                self.chunk_map[o] = sorted(i - lo for i in sel if lo <= i < lo + per)

        # (c) R5: bottleneck 末端沿用 chunk 的幸存下标
        for add, chunk, other in self.add_pairs:
            oc = self._conv_ancestor(other)
            if oc is None or oc.name in self.protected:
                continue
            cm = self.chunk_map.get(chunk)
            if cm is None or len(cm) != len(self.keep_out[oc.name]):
                continue
            self.keep_out[oc.name] = list(cm)

    # ------------------------------------------------------- 输入通道剪枝
    def compute_input_keeps(self):
        """沿图拓扑传播每个张量的【幸存下标映射】(原始通道空间)。"""
        # keep_map[tensor] = 幸存下标列表(在该张量原始通道空间里)
        keep_map = {}

        # 图输入
        for i in self.g.input:
            keep_map[i.name] = None           # None = 不剪

        # 先把 Conv 的输出映射定下来(输出通道空间是 Conv 自己的)
        for n in self.g.node:
            if n.op_type == "Conv":
                keep_map[n.output[0]] = list(self.keep_out[n.name])

        # 拓扑序传播非 Conv 节点
        for n in self.g.node:
            if n.op_type == "Conv":
                continue
            ot = n.output
            if n.op_type == "Split":
                axis = _split_axis(n)
                src = keep_map.get(n.input[0])
                nchunk = len(n.output)
                if src is None:
                    for o in ot:
                        keep_map[o] = None
                    continue
                # 用 select_survivors 里算好的【对称】chunk 映射
                for o in ot:
                    if o in self.chunk_map:
                        keep_map[o] = list(self.chunk_map[o])
                    else:
                        total = self._tensor_channels(n.input[0])
                        per = total // nchunk
                        keep_map[o] = None
            elif n.op_type == "Concat":
                axis = _split_axis(n)
                out_map = []
                base = 0
                for i in n.input:
                    m = keep_map.get(i)
                    total = self._tensor_channels(i)
                    if m is None:
                        out_map.extend(range(base, base + total))
                    else:
                        out_map.extend(base + x for x in m)
                    base += total
                keep_map[ot[0]] = out_map
            elif n.op_type in ("Add", "Mul", "Sub", "Div"):
                # 逐元素: 两个操作数必须同宽; 取交集语义(用第一个的映射)
                m0 = keep_map.get(n.input[0])
                m1 = keep_map.get(n.input[1]) if len(n.input) > 1 else None
                if m0 is not None and m1 is not None and len(m0) != len(m1):
                    # 数量对不上 —— 以较短的那个为准(Add 逐元素要求等宽)
                    if len(m1) < len(m0):
                        keep_map[ot[0]] = list(m1)
                    else:
                        keep_map[ot[0]] = list(m0)
                else:
                    keep_map[ot[0]] = m0
            else:
                # 通道数不变的算子: 映射原样传递
                m = keep_map.get(n.input[0]) if n.input else None
                keep_map[ot[0]] = m

        self.keep_map = keep_map

        # 给每个 Conv 算输入幸存下标
        for n in self.convs:
            w = self.init[self._w_name(n)]
            in_c = w.shape[1]
            src = self.prod.get(n.input[0]) if n.input else None
            m = keep_map.get(n.input[0]) if n.input else None
            if m is None:
                self.keep_in[n.name] = list(range(in_c))
            else:
                self.keep_in[n.name] = list(m)

    def _tensor_channels(self, tensor, depth=0):
        """张量的通道数(从生产者的权重 / 图输入推, 穿过逐元素算子)。"""
        if tensor in self._ch_cache:
            return self._ch_cache[tensor]
        if depth > 6:
            return 0
        p = self.prod.get(tensor)
        c = 0
        if p is not None and p.op_type == "Conv":
            c = self.init[self._w_name(p)].shape[0]
        elif p is not None and p.op_type == "Split":
            c = self._tensor_channels(p.input[0], depth + 1) // max(len(p.output), 1)
        elif p is not None and p.op_type == "Concat":
            c = sum(self._tensor_channels(i, depth + 1) for i in p.input)
        elif p is not None and p.op_type in (
                "Relu", "Sigmoid", "LeakyRelu", "Tanh", "Clip", "MaxPool",
                "AveragePool", "Resize", "Upsample", "Pad", "Identity",
                "Flatten", "Reshape", "Transpose"):
            c = self._tensor_channels(p.input[0], depth + 1)
        elif p is not None and p.op_type == "Mul":
            # ★ SiLU 的 Mul(X, Sigmoid(X)) 通道数不变; 取最宽的那个输入
            best = 0
            for i in p.input:
                cc = self._tensor_channels(i, depth + 1)
                best = max(best, cc or 0)
            c = best
        elif p is not None and p.op_type in ("Add", "Mul", "Sub", "Div"):
            c = self._tensor_channels(p.input[0], depth + 1)
        else:
            for i in self.g.input:
                if i.name == tensor:
                    c = 3
                    break
        self._ch_cache[tensor] = c
        return c

    # ------------------------------------------------------- 重写权重
    def rewrite(self):
        init_map = {i.name: i for i in self.g.initializer}
        new_init = []
        removed = 0
        for n in self.convs:
            wname = self._w_name(n)
            w = self.init[wname].copy()
            ki = self.keep_in[n.name]
            ko = self.keep_out[n.name]
            if len(ki) < w.shape[1] or len(ko) < w.shape[0]:
                removed += w.shape[0] * w.shape[1] * w.shape[2] * w.shape[3] - \
                    len(ko) * len(ki) * w.shape[2] * w.shape[3]
            w2 = w[np.ix_(ko, ki)]
            new_init.append(numpy_helper.from_array(w2, wname))
            # 偏置
            for i in n.input:
                if i in init_map and self.init[i].ndim == 1 and \
                        len(self.init[i]) == w.shape[0]:
                    new_init.append(numpy_helper.from_array(
                        self.init[i][np.array(ko)], i))
                    break
        # 保留其它 initializer
        conv_w = set()
        for n in self.convs:
            conv_w.add(self._w_name(n))
            for i in n.input:
                if i in init_map and self.init[i].ndim == 1:
                    conv_w.add(i)
        for i in self.g.initializer:
            if i.name not in conv_w:
                new_init.append(i)
        del self.g.initializer[:]
        self.g.initializer.extend(new_init)
        return removed

    # ------------------------------------------------------- Split 属性修正
    def fix_split_attrs(self):
        """Split 节点上带着显式的 split=[16,16] 属性, 剪完必须同步改, 否则图不合法。"""
        self._ch_cache = {}            # ★ 权重已被改写, 通道数缓存必须失效
        for n in self.g.node:
            if n.op_type != "Split":
                continue
            sizes = []
            for o in n.output:
                if o in self.chunk_map:
                    sizes.append(len(self.chunk_map[o]))
                else:
                    sizes.append(self._tensor_channels(o))
            if not sizes or None in sizes or 0 in sizes:
                continue
            new_attr = None
            for a in n.attribute:
                if a.name == "split":
                    new_attr = a
                    break
            if new_attr is not None:
                del new_attr.ints[:]
                new_attr.ints.extend(int(s) for s in sizes)
            else:
                attr = n.attribute.add()
                attr.name = "split"
                attr.type = onnx.AttributeProto.INTS
                attr.ints.extend(int(s) for s in sizes)

    # ------------------------------------------------------- 输出形状修正
    def fix_output_shapes(self):
        """图输出的形状里, 通道维可能因为剪枝而变(受保护的不会变)。"""
        # 重新做形状推断
        try:
            inferred = onnx.shape_inference.infer_shapes(self.m, strict_mode=False)
            vi = {v.name: v for v in inferred.graph.value_info}
            for o in self.g.output:
                if o.name in vi:
                    o.type.tensor_type.shape.CopyFrom(vi[o.name].type.tensor_type.shape)
        except Exception:
            pass

    def run(self):
        self._ch_cache = {}
        self.find_protected()
        self.compute_counts()
        self.select_survivors()
        self.compute_input_keeps()
        removed = self.rewrite()
        self.fix_split_attrs()
        self.fix_output_shapes()
        return removed


def channel_report(model):
    """统计剪枝后的参数量/通道分布。"""
    total = 0
    for i in model.graph.initializer:
        if i.dims and int(i.dims[0]) > 1:
            n = 1
            for d in i.dims:
                n *= int(d)
            total += n
    return total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--onnx", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--ratio", type=float, required=True, help="剪枝比例, 0.2/0.3/0.4")
    ap.add_argument("--min-keep", type=int, default=8)
    ap.add_argument("--report", default=None)
    ap.add_argument("--calib", default="/home/seeed/rk_deploy/_bench/calib_list.txt",
                    help="校准图列表(用于激活感知的重要性)")
    ap.add_argument("--calib-n", type=int, default=24)
    ap.add_argument("--no-act-aware", action="store_true")
    ap.add_argument("--align", type=int, default=16,
                    help="通道宽度对齐粒度(RK3588 NPU 按 16 对齐; 0=不对齐)")
    args = ap.parse_args()

    print("=" * 74)
    print("结构化通道剪枝: %s  ratio=%.0f%%" % (os.path.basename(args.onnx),
                                              args.ratio * 100))
    print("=" * 74)
    m = onnx.load(args.onnx)
    before = channel_report(m)
    print("  剪枝前参数量: %.3f M" % (before / 1e6))

    # 激活感知的重要性统计(可选)
    act = {}
    if not args.no_act_aware and args.calib and os.path.exists(args.calib):
        print("  统计激活分布 (激活感知的重要性判据) ...")
        tmp = Pruner(m, args.ratio, onnx_path=args.onnx, align=args.align)
        act = tmp.collect_activation_stats(args.calib, args.calib_n)
        print("    拿到 %d 个 Conv 的激活统计" % len(act))
    p = Pruner(m, args.ratio, min_keep=args.min_keep, onnx_path=args.onnx,
               act_stats=act, align=args.align)
    removed = p.run()

    after = channel_report(m)
    print("  剪枝后参数量: %.3f M  (减少 %.2f%%)"
          % (after / 1e6, 100.0 * (1 - after / before)))

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    onnx.save(m, args.out)
    onnx.checker.check_model(onnx.load(args.out))
    print("  已保存: %s  (onnx.checker 通过)" % args.out)

    # 统计每个 Conv 的通道变化
    convs = []
    for n in m.graph.node:
        if n.op_type != "Conv":
            continue
        for i in n.input:
            if i in {x.name for x in m.graph.initializer}:
                arr = None
                for x in m.graph.initializer:
                    if x.name == i and len(x.dims) == 4:
                        arr = x.dims
                if arr is not None:
                    convs.append((n.name, int(arr[0]), int(arr[1])))
                break
    if args.report:
        rep = {"onnx": args.onnx, "out": args.out, "ratio": args.ratio,
               "params_before": before, "params_after": after,
               "param_reduction_pct": 100.0 * (1 - after / before),
               "keep_out": p.keep_out, "keep_in": p.keep_in,
               "chunk_map": p.chunk_map,
               "convs": [{"name": n, "out_c": o, "in_c": i} for n, o, i in convs]}
        with open(args.report, "w") as f:
            json.dump(rep, f, indent=2, ensure_ascii=False)
        print("  报告: %s" % args.report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
