#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""export_quantize.py —— YOLOv8 模型改造 / 量化 / 导出(可复现全部实验组)

不重训, 只做【结构替换】与【输入分辨率调整】:

  A0  原模型      640×640  SiLU      (对照组)
  B1  SiLU→ReLU   640×640  ReLU
  C1  原模型      576×576  SiLU
  C2  SiLU→ReLU   576×576  ReLU
  D1  SiLU→ReLU   512×512  ReLU
  REF 参考(非 5 组之一): A0 的 per-channel 量化版 —— 用来量化"per-tensor vs per-channel"的代价

两条改造路径:
  ① SiLU → ReLU  : ONNX 图手术。YOLOv8 的 SiLU 导出成 Mul(X, Sigmoid(X)),
                    把它换成 Relu(X) 即可, 权重完全不动。
                    ★ 输出端的 3 个 Sigmoid (/model.22/Sigmoid*) 是分类头输出激活, 不动。
  ② 分辨率       : rknn.load_onnx(input_size_list=...) 覆盖输入形状。
                    ★ YOLOv8 内部的 Resize 用的是【相对 scale】(1,1,2,2), 不是绝对尺寸,
                      所以特征图尺寸会自动跟着变 —— 这是本脚本可行性的前提。
                    ★ 改完必须核对输出形状, 见 verify_output_shapes()。

量化口径(全部 5 组完全一致, 变量唯一):
  · RKNN Toolkit 2.3.2
  · INT8 权值 + INT8 激活 (w8a8), 量化算法 normal
  · per-tensor 量化 (Rockchip 叫 quantized_method='layer', 即每个张量一组 scale/zp)
  · optimization_level=3 (最高等级图优化: 算子融合 / 内存复用 / 等效替换)
  · mean=[[0,0,0]] std=[[255,255,255]] (Rockchip YOLOv8 官方口径), 归一化固化进模型
  · 同一份校准集 (--calib)

用法:
  ./export_quantize.py --all                    # 生成全部 5 组 + 参考组 + 量化误差分析
  ./export_quantize.py --all --skip-analysis    # 只出模型, 不跑量化误差分析
  ./export_quantize.py --variants A0,B1         # 只出指定组
"""

from __future__ import annotations

import argparse
import ast
import collections
import ctypes
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DEFAULT_SRC = os.path.join(REPO, "_bench", "dl", "yolov8n_optimized.onnx")
DEFAULT_CALIB = os.path.join(REPO, "_bench", "calib_list.txt")
DEFAULT_OUT = HERE

# ---------------------------------------------------------------- 实验组定义
# (key, 分辨率, 激活, 说明)
VARIANTS = [
    ("A0", 640, "silu", "原模型对照 (SiLU)"),
    ("B1", 640, "relu", "SiLU→ReLU (同分辨率)"),
    ("C1", 576, "silu", "分辨率下调 (同激活)"),
    ("C2", 576, "relu", "SiLU→ReLU + 576"),
    ("D1", 512, "relu", "SiLU→ReLU + 512"),
]

# ---------------------------------------------------------------- 板端推理 (ctypes)
# 说明: rknn-toolkit2 的 init_runtime() 在本机找不到 /usr/lib64/librknnrt.so,
#       所以量化误差分析直接用 ctypes 调板端运行库, 与 model_audit.py 同一套绑定。


class RknnTensorAttr(ctypes.Structure):
    _fields_ = [
        ("index", ctypes.c_uint32), ("n_dims", ctypes.c_uint32),
        ("dims", ctypes.c_uint32 * 16), ("name", ctypes.c_char * 256),
        ("n_elems", ctypes.c_uint32), ("size", ctypes.c_uint32),
        ("fmt", ctypes.c_int), ("type", ctypes.c_int), ("qnt_type", ctypes.c_int),
        ("fl", ctypes.c_int8), ("zp", ctypes.c_int32), ("scale", ctypes.c_float),
        ("w_stride", ctypes.c_uint32), ("size_with_stride", ctypes.c_uint32),
        ("pass_through", ctypes.c_uint8), ("h_stride", ctypes.c_uint32),
    ]


class RknnInputOutputNum(ctypes.Structure):
    _fields_ = [("n_input", ctypes.c_uint32), ("n_output", ctypes.c_uint32)]


class RknnInput(ctypes.Structure):
    _fields_ = [("index", ctypes.c_uint32), ("buf", ctypes.c_void_p),
                ("size", ctypes.c_uint32), ("pass_through", ctypes.c_uint8),
                ("type", ctypes.c_int), ("fmt", ctypes.c_int)]


class RknnOutput(ctypes.Structure):
    _fields_ = [("want_float", ctypes.c_uint8), ("is_prealloc", ctypes.c_uint8),
                ("index", ctypes.c_uint32), ("buf", ctypes.c_void_p),
                ("size", ctypes.c_uint32)]


def _load_lib():
    for cand in ("/usr/lib/librknnrt.so",
                 os.path.join(REPO, "rk3588_zero_copy", "third_party", "lib", "librknnrt.so")):
        if os.path.exists(cand):
            lib = ctypes.CDLL(cand)
            lib.rknn_init.restype = ctypes.c_int
            lib.rknn_init.argtypes = [ctypes.POINTER(ctypes.c_uint64), ctypes.c_void_p,
                                      ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
            lib.rknn_query.restype = ctypes.c_int
            lib.rknn_query.argtypes = [ctypes.c_uint64, ctypes.c_uint32,
                                       ctypes.c_void_p, ctypes.c_uint32]
            lib.rknn_inputs_set.restype = ctypes.c_int
            lib.rknn_inputs_set.argtypes = [ctypes.c_uint64, ctypes.c_uint32,
                                            ctypes.POINTER(RknnInput)]
            lib.rknn_run.restype = ctypes.c_int
            lib.rknn_run.argtypes = [ctypes.c_uint64, ctypes.c_void_p]
            lib.rknn_outputs_get.restype = ctypes.c_int
            lib.rknn_outputs_get.argtypes = [ctypes.c_uint64, ctypes.c_uint32,
                                             ctypes.POINTER(RknnOutput), ctypes.c_void_p]
            lib.rknn_outputs_release.restype = ctypes.c_int
            lib.rknn_outputs_release.argtypes = [ctypes.c_uint64, ctypes.c_uint32,
                                                 ctypes.POINTER(RknnOutput)]
            lib.rknn_destroy.restype = ctypes.c_int
            lib.rknn_destroy.argtypes = [ctypes.c_uint64]
            return lib, cand
    raise RuntimeError("找不到 librknnrt.so")


def rknn_infer(model_path, images):
    """在板端 NPU 上跑一个模型, 返回 list[np.ndarray] (每个输出一个 float32 数组)。"""
    import numpy as np

    lib, _ = _load_lib()
    ctx = ctypes.c_uint64(0)
    ret = lib.rknn_init(ctypes.byref(ctx), ctypes.c_char_p(model_path.encode()),
                        0, 0, None)
    if ret != 0:
        raise RuntimeError("rknn_init 失败: %d" % ret)
    try:
        io = RknnInputOutputNum()
        lib.rknn_query(ctx, 0, ctypes.byref(io), ctypes.sizeof(io))
        attr = RknnTensorAttr()
        attr.index = 0
        lib.rknn_query(ctx, 1, ctypes.byref(attr), ctypes.sizeof(attr))

        out_meta = []
        for i in range(io.n_output):
            oa = RknnTensorAttr()
            oa.index = i
            lib.rknn_query(ctx, 2, ctypes.byref(oa), ctypes.sizeof(oa))
            out_meta.append((oa.n_elems, oa.size))

        all_out = []
        for img in images:
            buf = ctypes.create_string_buffer(img.nbytes)
            ctypes.memmove(buf, img.tobytes(), img.nbytes)
            inp = RknnInput()
            inp.index = 0
            inp.buf = ctypes.cast(buf, ctypes.c_void_p)
            inp.size = img.nbytes
            inp.pass_through = 0
            inp.type = 3      # UINT8
            inp.fmt = 1       # NHWC
            if lib.rknn_inputs_set(ctx, 1, ctypes.byref(inp)) != 0:
                raise RuntimeError("rknn_inputs_set 失败")
            lib.rknn_run(ctx, None)
            outs = (RknnOutput * io.n_output)()
            bufs = []
            for i in range(io.n_output):
                n_elems, _ = out_meta[i]
                nb = max(n_elems * 4, 4)
                b = (ctypes.c_float * (nb // 4))()
                bufs.append(b)
                outs[i].want_float = 1
                outs[i].is_prealloc = 1
                outs[i].index = i
                outs[i].buf = ctypes.cast(b, ctypes.c_void_p)
                outs[i].size = nb
            if lib.rknn_outputs_get(ctx, io.n_output, outs, None) != 0:
                raise RuntimeError("rknn_outputs_get 失败")
            all_out.append([np.array(b[:n]) for (n, _), b in zip(out_meta, bufs)])
        return all_out
    finally:
        lib.rknn_destroy(ctx)


# ---------------------------------------------------------------- ① ONNX 手术
def silu_to_relu(src_onnx, dst_onnx, verbose=True):
    """把图里所有 SiLU (Mul(X, Sigmoid(X))) 换成 Relu(X)。

    只动激活函数, 权重/结构/输出节点全部不变。
    输出端的分类 Sigmoid (/model.22/Sigmoid*) 不是 SiLU, 自然不会被匹配到。
    """
    import onnx

    m = onnx.load(src_onnx)
    g = m.graph

    sig = {n.output[0]: n for n in g.node if n.op_type == "Sigmoid"}
    consumers = collections.Counter()
    for n in g.node:
        for i in n.input:
            consumers[i] += 1

    kept, removed_sig, relu_nodes, pairs = [], set(), [], []
    for n in g.node:
        if n.op_type != "Mul":
            kept.append(n)
            continue
        partner = None
        for a, b in ((n.input[0], n.input[1]), (n.input[1], n.input[0])):
            s = sig.get(a)
            if s is not None and b == s.input[0]:
                partner = (a, b, s)
                break
        if partner is None:
            kept.append(n)
            continue
        a, b, s = partner
        if consumers[a] != 1:
            # Sigmoid 的输出还有别的消费者 -> 不能安全删除, 放弃这一处
            kept.append(n)
            continue
        relu = onnx.helper.make_node("Relu", inputs=[b], outputs=list(n.output),
                                      name=n.name + "_ReLU")
        kept.append(relu)
        removed_sig.add(s.name)
        pairs.append((n.name, s.name, b))

    del g.node[:]
    g.node.extend(kept)

    # 清理被删掉节点的中间张量 value_info
    dead_tensors = set()
    for n in g.node:
        if n.name in removed_sig:
            dead_tensors.update(n.output)
    keep_vi = [vi for vi in g.value_info if vi.name not in dead_tensors]
    del g.value_info[:]
    g.value_info.extend(keep_vi)

    os.makedirs(os.path.dirname(dst_onnx) or ".", exist_ok=True)
    onnx.save(m, dst_onnx)
    onnx.checker.check_model(onnx.load(dst_onnx))

    if verbose:
        print("    ONNX 手术: SiLU→ReLU 替换 %d 处, 删除 Sigmoid %d 个"
              % (len(pairs), len(removed_sig)))
    return {"relu_count": len(pairs), "removed_sigmoid": len(removed_sig),
            "pairs": pairs}


# ---------------------------------------------------------------- ①b 分辨率改造
def resize_onnx_input(src_onnx, dst_onnx, size, verbose=True):
    """把 ONNX 的输入分辨率从 640 改成 size (正方形), 并清空输出形状让推理引擎重算。

    ★ 为什么不能只改输入的 dim:
      YOLOv8 头部的 9 个输出在 ONNX 里写死了形状([1,64,80,80] 等)。
      只改输入不改输出, 形状就自相矛盾了。
    ★ 正确做法(与项目已有的 _bench/dl/yolov8n_320.onnx 完全同构):
        1. 输入 images 的 dim 改成 [1,3,size,size]
        2. 把所有 graph.output 的 shape dim 清空 —— 让 onnxruntime / RKNN 的
           形状推理按新输入尺寸重算
      节点数、初始化器(权重)、算子类型【一个都不动】, 纯尺寸改造。
    ★ 可行性前提: YOLOv8 内部的 Resize 用相对 scale(1,1,2,2) 不是绝对尺寸,
      所以特征图尺寸会跟着输入自动缩放。
    """
    import onnx

    m = onnx.load(src_onnx)
    g = m.graph

    # 1) 输入形状
    hit = False
    for i in g.input:
        if i.name == "images" or len(g.input) == 1:
            dims = i.type.tensor_type.shape.dim
            if len(dims) == 4:
                dims[0].dim_value = 1
                dims[1].dim_value = 3
                dims[2].dim_value = size
                dims[3].dim_value = size
                hit = True
    if not hit:
        raise RuntimeError("没找到输入张量 images")

    # 2) 清空输出形状
    for o in g.output:
        del o.type.tensor_type.shape.dim[:]

    os.makedirs(os.path.dirname(dst_onnx) or ".", exist_ok=True)
    onnx.save(m, dst_onnx)

    if verbose:
        print("    ONNX 分辨率改造: 640 → %d×%d (节点/权重不变, 清空输出形状让引擎重算)"
              % (size, size))
    return {"size": size, "out": dst_onnx}


# ---------------------------------------------------------------- ② 构建 RKNN
def build_rknn(onnx_path, out_rknn, size, calib, per_tensor=True, fp16=False,
               log_path=None):
    """用 RKNN Toolkit 2.3.2 构建并导出模型。返回构建摘要。"""
    from rknn.api import RKNN

    rknn = RKNN(verbose=True)
    cfg = dict(
        mean_values=[[0, 0, 0]],
        std_values=[[255, 255, 255]],
        target_platform="rk3588",
        quantized_dtype="w8a8",            # 权值 INT8 + 激活 INT8
        quantized_algorithm="normal",      # 不用 hybrid(混合精度)
        # ★ Rockchip 的命名: 'layer' = 每个张量一组 scale/zp, 即业界说的 per-tensor;
        #   'channel' = 逐输出通道, 即 per-channel。任务要求 per-tensor -> 'layer'。
        quantized_method="layer" if per_tensor else "channel",
        optimization_level=3,              # 最高等级图优化(算子融合/内存复用/等效替换)
        float_dtype="float16",
        quant_img_RGB2BGR=False,
    )
    print("    config:", cfg)
    if rknn.config(**cfg) != 0:
        raise RuntimeError("config 失败")
    # ★ 不用 input_size_list: 实测本 toolkit 2.3.2 对本模型【忽略】该参数
    #   (传 512 出来的还是 640)。输入尺寸由 ONNX 图本身决定, 见 resize_onnx_input()。
    ret = rknn.load_onnx(model=onnx_path)
    if ret != 0:
        raise RuntimeError("load_onnx 失败")
    if rknn.build(do_quantization=not fp16, dataset=calib) != 0:
        raise RuntimeError("build 失败")
    os.makedirs(os.path.dirname(out_rknn) or ".", exist_ok=True)
    if rknn.export_rknn(out_rknn) != 0:
        raise RuntimeError("export 失败")
    rknn.release()

    size_mb = os.path.getsize(out_rknn) / 1048576.0
    print("    导出: %s (%.2f MB)" % (out_rknn, size_mb))
    return {"rknn": out_rknn, "size_mb": size_mb,
            "per_tensor": per_tensor, "fp16": fp16, "input_size": size}


# ---------------------------------------------------------------- ③ 形状核对
def verify_output_shapes(model_path, expect_size):
    """在板端跑一次, 打印输入/输出形状。

    ★ 分辨率改造后【必须】核对: 模型转出来了不等于形状对, 形状错了检测框全是乱的。
    """
    lib, _ = _load_lib()
    ctx = ctypes.c_uint64(0)
    ret = lib.rknn_init(ctypes.byref(ctx), ctypes.c_char_p(model_path.encode()),
                        0, 0, None)
    if ret != 0:
        raise RuntimeError("rknn_init 失败")
    try:
        attr = RknnTensorAttr()
        attr.index = 0
        lib.rknn_query(ctx, 1, ctypes.byref(attr), ctypes.sizeof(attr))
        dims = [attr.dims[i] for i in range(attr.n_dims)]
        fmt = attr.fmt                       # 0=NCHW, 1=NHWC
        io = RknnInputOutputNum()
        lib.rknn_query(ctx, 0, ctypes.byref(io), ctypes.sizeof(io))
        outs = []
        for i in range(io.n_output):
            oa = RknnTensorAttr()
            oa.index = i
            lib.rknn_query(ctx, 2, ctypes.byref(oa), ctypes.sizeof(oa))
            outs.append((oa.name.decode(errors="replace"),
                         [oa.dims[k] for k in range(oa.n_dims)]))
        # NCHW -> dims[2],[3] 是 H,W ; NHWC -> dims[1],[2] 是 H,W
        if fmt == 1:
            h, w = (dims[1], dims[2]) if len(dims) == 4 else (0, 0)
        else:
            h, w = (dims[2], dims[3]) if len(dims) == 4 else (0, 0)
        ok = (expect_size == 0) or (h == expect_size and w == expect_size)
        return {"input_dims": dims, "input_fmt": fmt, "input_hw": [h, w],
                "outputs": outs, "input_ok": ok, "n_output": io.n_output}
    finally:
        lib.rknn_destroy(ctx)


# ---------------------------------------------------------------- ④ 量化误差分析
def _out_role(name):
    """按输出名判断这个张量在检测头里的角色。"""
    n = name.lower()
    if "reducesum" in n:
        return "cls"          # 80 个类别的原始得分
    if n in ("331", "350", "369"):
        return "aux"          # Clip(ReduceSum) —— 辅助输出, 量程退化, 单独看
    return "box"              # 318/338/357: DFL 框分布


def quantization_error_report(int8_model, fp16_model, calib_list, n_images=8):
    """INT8 vs FP16(同图同分辨率)的逐输出量化误差。

    ★ 用同一脚本、同分辨率、不量化的 FP16 模型做参考, 这样比出来的差异
      【纯粹是量化引入的】, 不掺杂结构/分辨率的差异。

    ★ 归一化用 FP16 的【最大绝对值】而不是量程(range):
      这些输出里有 Clip(ReduceSum) 这种量程退化的辅助输出, 用 range 归一化
      会算出 100 倍以上没有物理意义的百分比。
    """
    import numpy as np
    import cv2

    paths = [l.strip() for l in open(calib_list) if l.strip()][:n_images]
    meta = verify_output_shapes(int8_model, 0)
    size = meta["input_hw"][0]

    prepared = []
    for p in paths:
        im = cv2.imread(p)
        if im is None:
            continue
        im = cv2.resize(im, (size, size), interpolation=cv2.INTER_LINEAR)
        prepared.append(im[:, :, ::-1].copy())          # BGR -> RGB
    if not prepared:
        return {"error": "校准图片读不出来"}

    out_i8 = rknn_infer(int8_model, prepared)
    out_f16 = rknn_infer(fp16_model, prepared)

    names = meta["outputs"]
    per_out = []
    groups = {}
    for oi in range(len(out_i8[0])):
        a = np.stack([x[oi] for x in out_i8]).astype(np.float64)
        b = np.stack([x[oi] for x in out_f16]).astype(np.float64)
        d = np.abs(a - b)
        nm = names[oi][0] if oi < len(names) else "out%d" % oi
        role = _out_role(nm)
        peak = float(np.abs(b).max()) or 1.0
        rec = {
            "output_index": oi,
            "name": nm,
            "role": role,
            "shape": list(a.shape[1:]),
            "max_abs_err": float(d.max()),
            "mean_abs_err": float(d.mean()),
            "fp16_peak_abs": peak,
            "fp16_range": float(b.max() - b.min()),
            "max_err_over_peak": float(d.max() / peak),
            "mean_err_over_peak": float(d.mean() / peak),
        }
        per_out.append(rec)
        g = groups.setdefault(role, {"max_err_over_peak": 0.0, "mean_abs_err": 0.0,
                                     "max_abs_err": 0.0, "n": 0})
        g["max_err_over_peak"] = max(g["max_err_over_peak"], rec["max_err_over_peak"])
        g["max_abs_err"] = max(g["max_abs_err"], rec["max_abs_err"])
        g["mean_abs_err"] += rec["mean_abs_err"]
        g["n"] += 1
    for role, g in groups.items():
        g["mean_abs_err"] /= max(g["n"], 1)

    return {"n_images": len(prepared), "input_size": size,
            "per_output": per_out, "by_role": groups,
            "agg_max_err_over_peak": max(
                (o["max_err_over_peak"] for o in per_out
                 if o["role"] in ("box", "cls")), default=0.0)}


# ---------------------------------------------------------------- ⑥ 精度风险指标
CLS_OUT_IDX = [1, 4, 7]          # 3 个尺度的类别得分张量(onnx::ReduceSum_*)


def accuracy_risk_report(model_path, calib_list, n_images=8, onnx_ref=None):
    """检测置信度指标 —— 用来提示"改结构/改分辨率"对精度的影响。

    ★ 本任务不负责精度回归(业务侧做), 这里只给一个【可量化的健康信号】:
        max_cls : 全图最大类别得分
        n>0.15  : 得分超过 0.15 的位置数(本项目 conf 阈值就是 0.15)
      如果 n>0.15 塌到 0, 说明模型已经【检不出任何东西】, 这不是"精度略降"。
    """
    import numpy as np
    import cv2

    meta = verify_output_shapes(model_path, 0)
    size = meta["input_hw"][0]
    paths = [l.strip() for l in open(calib_list) if l.strip()][:n_images]
    imgs = []
    for p in paths:
        im = cv2.imread(p)
        if im is None:
            continue
        im = cv2.resize(im, (size, size), interpolation=cv2.INTER_LINEAR)
        imgs.append(im[:, :, ::-1].copy())
    if not imgs:
        return {"error": "校准图片读不出来"}

    outs = rknn_infer(model_path, imgs)
    mx, n15, n25, mean = [], [], [], []
    for o in outs:
        c = np.concatenate([o[i].ravel() for i in CLS_OUT_IDX if i < len(o)])
        mx.append(c.max())
        n15.append(int((c > 0.15).sum()))
        n25.append(int((c > 0.25).sum()))
        mean.append(c.mean())

    res = {"size": size, "n_images": len(imgs),
           "max_cls_mean": float(np.mean(mx)), "max_cls_best": float(np.max(mx)),
           "mean_cls": float(np.mean(mean)),
           "n_gt_015_mean": float(np.mean(n15)), "n_gt_025_mean": float(np.mean(n25))}

    # 可选: FP32 ONNX 参考(同一批图)
    if onnx_ref and os.path.exists(onnx_ref):
        try:
            import onnxruntime as ort
            so = ort.InferenceSession(onnx_ref, providers=["CPUExecutionProvider"])
            mx2, n15b = [], []
            for im in imgs:
                x = (im.astype(np.float32) / 255.0).transpose(2, 0, 1)[None]
                o = so.run(None, {"images": x})
                c = np.concatenate([o[i].ravel() for i in CLS_OUT_IDX if i < len(o)])
                mx2.append(c.max())
                n15b.append(int((c > 0.15).sum()))
            res["fp32_max_cls_mean"] = float(np.mean(mx2))
            res["fp32_n_gt_015_mean"] = float(np.mean(n15b))
        except Exception as e:                                    # pragma: no cover
            res["fp32_error"] = str(e)
    return res


# ---------------------------------------------------------------- ⑤ 算子普查
def op_census(model_path):
    """用驱动自报的逐算子表统计算子类型构成(看 SiLU 是否真的被融合掉了)。"""
    import re

    sys.path.insert(0, REPO)
    try:
        from audit.model_audit import runtime_op_table
    except Exception:
        return None
    try:
        rt = runtime_op_table(model_path)
    except Exception as e:
        return {"error": str(e)}
    ops = rt.get("ops", [])
    if not ops:
        return {"error": "没取到逐算子表"}
    census = collections.Counter(o["op_type"] for o in ops)
    by_target = collections.Counter(o["target"] for o in ops)
    cpu_us = sum(o["time_us"] for o in ops if o["target"] == "CPU")
    total_us = sum(o["time_us"] for o in ops)
    return {"n_ops": len(ops), "op_types": dict(census),
            "by_target": dict(by_target),
            "cpu_us": cpu_us, "total_us": total_us,
            "cpu_ratio": (cpu_us / total_us) if total_us else 0.0}


# ---------------------------------------------------------------- 主流程
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=DEFAULT_SRC, help="源 ONNX (640 SiLU)")
    ap.add_argument("--calib", default=DEFAULT_CALIB, help="校准集列表")
    ap.add_argument("--out-dir", default=DEFAULT_OUT)
    ap.add_argument("--variants", default=",".join(v[0] for v in VARIANTS))
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--skip-analysis", action="store_true")
    ap.add_argument("--analysis-only", action="store_true",
                    help="模型已经建好时, 只重跑量化误差分析(不重建)")
    ap.add_argument("--with-ref-perchannel", action="store_true",
                    help="额外构建 A0 的 per-channel 参考版")
    args = ap.parse_args()

    want = set(v.strip() for v in args.variants.split(",") if v.strip())
    if args.all:
        want = set(v[0] for v in VARIANTS)
    if args.with_ref_perchannel:
        want.add("REF")

    onnx_dir = os.path.join(args.out_dir, "onnx")
    model_dir = os.path.join(args.out_dir, "models")
    ref_dir = os.path.join(args.out_dir, "models_ref")
    out_log = os.path.join(args.out_dir, "out")
    for d in (onnx_dir, model_dir, ref_dir, out_log):
        os.makedirs(d, exist_ok=True)

    print("=" * 78)
    print("源模型    : %s" % args.src)
    print("校准集    : %s" % args.calib)
    print("输出目录  : %s" % args.out_dir)
    print("=" * 78)

    summary = {"src": args.src, "calib": args.calib, "models": {}}

    # ---- 先做 ONNX 手术 (SiLU→ReLU) ----
    print("\n[1/4] ONNX 手术 (SiLU → ReLU + 分辨率改造)")
    relu_onnx = os.path.join(onnx_dir, "yolov8n_relu_640.onnx")
    surgery = None
    if any(v[2] == "relu" for v in VARIANTS if v[0] in want):
        surgery = silu_to_relu(args.src, relu_onnx)
        summary["surgery"] = {k: v for k, v in surgery.items() if k != "pairs"}
        print("    替换明细(前 3 条): %s" % surgery["pairs"][:3])

    # ---- 为每个需要的分辨率准备一份 ONNX ----
    #  (激活, 分辨率) -> ONNX 路径
    onnx_for = {}
    for key, size, act, desc in VARIANTS:
        if key not in want:
            continue
        if act == "relu":
            base = relu_onnx
        else:
            base = args.src
        if size == 640:
            onnx_for[(key, size, act)] = base
        else:
            p = os.path.join(onnx_dir, "yolov8n_%s_%d.onnx" % (act, size))
            resize_onnx_input(base, p, size)
            onnx_for[(key, size, act)] = p

    # ---- 逐组构建 ----
    print("\n[2/4] 构建 + 量化 + 导出")
    for key, size, act, desc in VARIANTS:
        if key not in want:
            continue
        if args.analysis_only:
            name = "%s_%s_%d.rknn" % (key, act, size)
            rknn_path = os.path.join(model_dir, name)
            if not os.path.exists(rknn_path):
                print("  !! %s 不存在, 跳过" % rknn_path)
                continue
            info = {"rknn": rknn_path, "size_mb": os.path.getsize(rknn_path) / 1048576.0,
                    "per_tensor": True, "fp16": False, "input_size": size,
                    "variant": key, "activation": act, "desc": desc}
            sh = verify_output_shapes(rknn_path, size)
            info["input_dims"] = sh["input_dims"]
            info["outputs"] = sh["outputs"]
            info["shape_ok"] = sh["input_ok"]
            ref_path = os.path.join(ref_dir, "%s_%s_%d_fp16.rknn" % (key, act, size))
            info["fp16_ref"] = ref_path if os.path.exists(ref_path) else None
            summary["models"][key] = info
            print("  === %s (已有) %s ===" % (key, rknn_path))
            continue
        print("\n  === %s  %s  (%d×%d, %s) ===" % (key, desc, size, size, act))
        onnx_path = onnx_for[(key, size, act)]
        name = "%s_%s_%d.rknn" % (key, act, size)
        rknn_path = os.path.join(model_dir, name)
        t0 = time.time()
        info = build_rknn(onnx_path, rknn_path, size, args.calib, per_tensor=True)
        info["build_sec"] = round(time.time() - t0, 1)
        info["variant"] = key
        info["activation"] = act
        info["desc"] = desc

        # 形状核对
        sh = verify_output_shapes(rknn_path, size)
        info["input_dims"] = sh["input_dims"]
        info["outputs"] = sh["outputs"]
        info["shape_ok"] = sh["input_ok"]
        print("    输入形状 %s (fmt=%s) | 输出 %d 个 | 形状核对 %s"
              % (sh["input_dims"], "NHWC" if sh["input_fmt"] == 1 else "NCHW",
                 sh["n_output"], "OK" if sh["input_ok"] else "★不一致★"))
        for n, d in sh["outputs"]:
            print("        out %-26s %s" % (n, d))
        summary["models"][key] = info

    # ---- FP16 参考模型 (量化误差分析用) ----
    if not args.skip_analysis and not args.analysis_only:
        print("\n[3/4] 构建 FP16 参考模型 (量化误差分析的基准)")
        for key, size, act, desc in VARIANTS:
            if key not in want:
                continue
            onnx_path = onnx_for[(key, size, act)]
            ref_path = os.path.join(ref_dir, "%s_%s_%d_fp16.rknn" % (key, act, size))
            if not os.path.exists(ref_path):
                build_rknn(onnx_path, ref_path, size, args.calib, fp16=True)
            summary["models"][key]["fp16_ref"] = ref_path

    # ---- 参考组: per-channel ----
    if "REF" in want:
        print("\n[3b/4] 构建参考组 REF: A0 的 per-channel 量化版")
        ref_path = os.path.join(ref_dir, "A0_silu_640_perchannel.rknn")
        info = build_rknn(args.src, ref_path, 640, args.calib, per_tensor=False)
        info["variant"] = "REF"
        info["desc"] = "A0 的 per-channel 量化版 (仅用于量化 per-tensor 的代价)"
        summary["models"]["REF"] = info

    # ---- 量化误差分析 ----
    if not args.skip_analysis:
        print("\n[4/4] 量化误差分析 (INT8 vs 同图同分辨率 FP16)")
        for key, size, act, desc in VARIANTS:
            if key not in want:
                continue
            m = summary["models"][key]
            if not m.get("fp16_ref") or not os.path.exists(m["fp16_ref"]):
                print("  %s: 缺 FP16 参考模型, 跳过" % key)
                continue
            qe = quantization_error_report(m["rknn"], m["fp16_ref"], args.calib)
            m["quant_error"] = qe
            br = qe.get("by_role", {})
            print("  %s: box 最大误差 %6.2f%% | cls 最大误差 %6.2f%% | box 平均绝对误差 %.4f"
                  % (key, 100 * br.get("box", {}).get("max_err_over_peak", 0),
                     100 * br.get("cls", {}).get("max_err_over_peak", 0),
                     br.get("box", {}).get("mean_abs_err", 0)))

    # ---- 参考组 per-channel 的量化误差 (对照) ----
    if "REF" in want and not args.skip_analysis:
        ref_i8 = os.path.join(ref_dir, "A0_silu_640_perchannel.rknn")
        ref_f16 = os.path.join(ref_dir, "A0_silu_640_fp16.rknn")
        if os.path.exists(ref_i8) and os.path.exists(ref_f16):
            qe = quantization_error_report(ref_i8, ref_f16, args.calib)
            summary["models"].setdefault("REF", {})["quant_error"] = qe
            br = qe.get("by_role", {})
            print("  REF(per-channel): box 最大误差 %6.2f%% | cls 最大误差 %6.2f%%"
                  % (100 * br.get("box", {}).get("max_err_over_peak", 0),
                     100 * br.get("cls", {}).get("max_err_over_peak", 0)))

    # ---- 算子普查 ----
    print("\n[附加] 算子普查 (驱动自报)")
    for key, size, act, desc in VARIANTS:
        if key not in want:
            continue
        c = op_census(summary["models"][key]["rknn"])
        summary["models"][key]["op_census"] = c
        if c and "op_types" in c:
            top = sorted(c["op_types"].items(), key=lambda kv: -kv[1])[:6]
            print("  %-4s 算子 %3d 个 | %s | CPU 占比 %.2f%%"
                  % (key, c["n_ops"], ", ".join("%s×%d" % (k, v) for k, v in top),
                     100 * c["cpu_ratio"]))

    # ---- 落盘 ----
    with open(os.path.join(out_log, "export_summary.json"), "w") as f:
        json.dump(summary, f, indent=2, ensure_ascii=False)
    print("\n汇总已写入: %s" % os.path.join(out_log, "export_summary.json"))

    # ---- 精度风险指标 ----
    print("\n[附加] 检测置信度指标 (精度风险提示, 不替代业务侧回归)")
    ar = {}
    for key, size, act, desc in VARIANTS:
        if key not in want:
            continue
        m = summary["models"][key]
        if not os.path.exists(m["rknn"]):
            continue
        onnx_ref = onnx_for[(key, size, act)]
        r = accuracy_risk_report(m["rknn"], args.calib, onnx_ref=onnx_ref)
        ar[key] = r
        m["accuracy_risk"] = r
        print("  %-3s %-9s %-4d INT8 max_cls=%.4f | #>0.15=%5.1f | #>0.25=%5.1f %s"
              % (key, act, size, r.get("max_cls_mean", 0), r.get("n_gt_015_mean", 0),
                 r.get("n_gt_025_mean", 0),
                 ("★ 检不出任何东西★" if r.get("n_gt_015_mean", 1) < 1 else "")))
    if ar:
        with open(os.path.join(out_log, "accuracy_risk.json"), "w") as f:
            json.dump(ar, f, indent=2, ensure_ascii=False)

    # ---- 汇总表 ----
    print("\n" + "=" * 78)
    print("%-4s %-9s %-6s %-10s %-12s %-10s" %
          ("组", "激活", "分辨率", "RKNN(MB)", "输出数", "形状"))
    for key, size, act, desc in VARIANTS:
        if key not in want:
            continue
        m = summary["models"][key]
        print("%-4s %-9s %-6d %-10.2f %-12d %-10s"
              % (key, act, size, m["size_mb"], m.get("n_output", len(m["outputs"])),
                 "OK" if m["shape_ok"] else "★异常★"))
    print("=" * 78)
    return 0


if __name__ == "__main__":
    sys.exit(main())
