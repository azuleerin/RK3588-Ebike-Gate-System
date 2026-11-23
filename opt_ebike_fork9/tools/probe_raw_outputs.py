#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""probe_raw_outputs.py —— 把 RKNN 模型的【原始输出张量】dump 成 .npy

★ 为什么需要它
  2026-11-23 P1: fork9(9输出, DFL 在 CPU) 与 formC/prod(2输出, DFL 在 NPU) 对拍时,
  匹配率只有 91.19% / 34.43%, 而【类别一致率 100%、平均分数差 ±0.0007】 ——
  典型的"检出同一个东西, 但框坐标系统性差一截"。
  要判断这是"模型精度问题"还是"两边解码约定不一致", 必须看原始张量:
  直接比对 [1,4,8400] 的四个通道到底是什么(cxywh? xyxy? w 是全宽还是半宽?)。

★ 口径必须和 acc_batch_nc.cc 完全一致, 否则比的不是同一件事
  · cv::imread(IMREAD_COLOR) -> cv::resize(640x640, INTER_LINEAR) -> cvtColor(BGR2RGB)
  · 量化模型喂 uint8 NHWC 原始[0,255] (运行时自己量化)

用法:
    probe_raw_outputs.py --model M.rknn --img a.jpg --img b.jpg --dump out_prefix
"""
import argparse
import ctypes
import os
import sys

import cv2
import numpy as np

RKNN_MAX_DIMS = 16
RKNN_MAX_NAME_LEN = 256
RKNN_QUERY_OUTPUT_ATTR = 2
RKNN_TENSOR_UINT8 = 3      # rknn_tensor_type: FLOAT32=0,FLOAT16=1,INT8=2,UINT8=3
RKNN_TENSOR_NHWC = 1        # rknn_tensor_format: NCHW=0,NHWC=1


class RknnInput(ctypes.Structure):
    _fields_ = [("index", ctypes.c_uint32), ("buf", ctypes.c_void_p),
                ("size", ctypes.c_uint32), ("pass_through", ctypes.c_uint8),
                ("type", ctypes.c_int), ("fmt", ctypes.c_int)]


class RknnOutput(ctypes.Structure):
    _fields_ = [("want_float", ctypes.c_uint8), ("is_prealloc", ctypes.c_uint8),
                ("index", ctypes.c_uint32), ("buf", ctypes.c_void_p),
                ("size", ctypes.c_uint32)]


class RknnInputOutputNum(ctypes.Structure):
    _fields_ = [("n_input", ctypes.c_uint32), ("n_output", ctypes.c_uint32)]


class RknnAttr(ctypes.Structure):
    _fields_ = [
        ("index", ctypes.c_uint32),
        ("n_dims", ctypes.c_uint32),
        ("dims", ctypes.c_uint32 * RKNN_MAX_DIMS),
        ("name", ctypes.c_char * RKNN_MAX_NAME_LEN),
        ("n_elems", ctypes.c_uint32),
        ("size", ctypes.c_uint32),
        ("fmt", ctypes.c_int),
        ("type", ctypes.c_int),
        ("qnt_type", ctypes.c_int),
        ("fl", ctypes.c_int8),
        ("zp", ctypes.c_int32),
        ("scale", ctypes.c_float),
        ("w_stride", ctypes.c_uint32),
        ("size_with_stride", ctypes.c_uint32),
        ("pass_through", ctypes.c_uint8),
        ("h_stride", ctypes.c_uint32),
    ]


def load_rknnrt():
    for cand in ("/usr/lib/librknnrt.so",
                 "/home/seeed/rk_deploy/rk3588_zero_copy/third_party/lib/librknnrt.so"):
        if os.path.exists(cand):
            return ctypes.CDLL(cand), cand
    raise RuntimeError("找不到 librknnrt.so")


def preprocess(path, w=640, h=640):
    img = cv2.imread(path, cv2.IMREAD_COLOR)
    if img is None:
        raise SystemExit(f"读不出图: {path}")
    small = cv2.resize(img, (w, h), interpolation=cv2.INTER_LINEAR)
    small = cv2.cvtColor(small, cv2.COLOR_BGR2RGB)   # 与 C++ 的 in-place 版等价
    return np.ascontiguousarray(small)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--img", action="append", required=True)
    ap.add_argument("--dump", required=True, help="输出前缀")
    ap.add_argument("--size", default="640x640")
    args = ap.parse_args()
    mw, mh = (int(x) for x in args.size.split("x"))

    lib, libpath = load_rknnrt()
    lib.rknn_init.restype = ctypes.c_int
    # rknn_api.h(v1 风格): rknn_init(ctx*, void* model, uint32_t size, uint32_t flag, extend*)
    #   size=0 -> model 传的是【文件路径】
    lib.rknn_init.argtypes = [ctypes.POINTER(ctypes.c_uint64), ctypes.c_void_p,
                              ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    lib.rknn_query.restype = ctypes.c_int
    lib.rknn_query.argtypes = [ctypes.c_uint64, ctypes.c_int, ctypes.c_void_p,
                               ctypes.c_uint32]
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

    ctx = ctypes.c_uint64(0)
    path = os.path.abspath(args.model)
    ret = lib.rknn_init(ctypes.byref(ctx), ctypes.c_char_p(path.encode()), 0, 0, None)
    if ret != 0:
        raise SystemExit(f"rknn_init 失败: {ret}")
    print(f"rknn_init OK: {path}  (lib={libpath})")

    # 输出个数 (RKNN_QUERY_IN_OUT_NUM 需要 8 字节的结构体, 不是 uint32)
    io_num = RknnInputOutputNum()
    lib.rknn_query(ctx, 0, ctypes.byref(io_num), ctypes.sizeof(io_num))
    n_out = io_num.n_output
    print(f"  输入 {io_num.n_input} 个 / 输出 {n_out} 个")

    attrs = []
    for i in range(n_out):
        a = RknnAttr()
        a.index = i
        if lib.rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, ctypes.byref(a),
                          ctypes.sizeof(a)) != 0:
            raise SystemExit(f"rknn_query OUTPUT_ATTR[{i}] 失败")
        dims = list(a.dims[:a.n_dims])
        attrs.append((a.name.decode(), dims, a.n_elems, a.type))
        print(f"  输出[{i}] {a.name.decode():45s} dims={dims} n_elems={a.n_elems} type={a.type}")

    for fi, img_path in enumerate(args.img):
        small = preprocess(img_path, mw, mh)
        buf = small.reshape(-1)
        arr = (ctypes.c_uint8 * buf.size).from_buffer_copy(buf.tobytes())
        inp = RknnInput()
        inp.index = 0
        inp.buf = ctypes.cast(arr, ctypes.c_void_p)
        inp.size = buf.size
        inp.pass_through = 0
        inp.type = RKNN_TENSOR_UINT8
        inp.fmt = RKNN_TENSOR_NHWC
        if lib.rknn_inputs_set(ctx, 1, ctypes.byref(inp)) != 0:
            raise SystemExit("rknn_inputs_set 失败")
        if lib.rknn_run(ctx, None) != 0:
            raise SystemExit("rknn_run 失败")

        outs = (RknnOutput * n_out)()
        for i in range(n_out):
            outs[i].want_float = 1
            outs[i].is_prealloc = 0
            outs[i].index = i
        if lib.rknn_outputs_get(ctx, n_out, outs, None) != 0:
            raise SystemExit("rknn_outputs_get 失败")
        for i in range(n_out):
            name, dims, n_elems, _ = attrs[i]
            data = ctypes.cast(outs[i].buf, ctypes.POINTER(ctypes.c_float))
            vec = np.ctypeslib.as_array(data, shape=(n_elems,)).copy()
            outp = f"{args.dump}_f{fi}_o{i}.npy"
            np.save(outp, vec.reshape(dims))
            print(f"  -> {outp}  shape={dims}  min={vec.min():.4f} max={vec.max():.4f}")
        lib.rknn_outputs_release(ctx, n_out, outs)

    lib.rknn_destroy(ctx)
    print("完成")


if __name__ == "__main__":
    main()
