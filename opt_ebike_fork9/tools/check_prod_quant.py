#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_prod_quant.py —— 生产模型 box 张量的量化参数 + want_float 转换保真度

要排除的假设: "运行时 want_float=1 的反量化对 prod 的 box 张量不忠实"
              (比如 per-axis 量化只按 per-tensor scale 转 -> w/h 通道被缩错)
做法: 同一个张量取两次 —— ① want_float=1 的 float 值 ② want_float=0 的原始 int8
      再用 attr 里的 scale/zp 手动反量化, 两者逐位对照。
"""
import ctypes
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_raw_outputs import (load_rknnrt, RknnAttr, RknnInput, RknnOutput,
                               RknnInputOutputNum, RKNN_QUERY_OUTPUT_ATTR,
                               preprocess)
import cv2

IMGS = ["/home/seeed/rk_deploy/accuracy/data/frames/f00000.jpg"]


def run(model):
    lib, _ = load_rknnrt()
    lib.rknn_init.restype = ctypes.c_int
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

    ctx = ctypes.c_uint64(0)
    lib.rknn_init(ctypes.byref(ctx), ctypes.c_char_p(os.path.abspath(model).encode()),
                  0, 0, None)
    io = RknnInputOutputNum()
    lib.rknn_query(ctx, 0, ctypes.byref(io), ctypes.sizeof(io))

    attrs = []
    for i in range(io.n_output):
        a = RknnAttr(); a.index = i
        lib.rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, ctypes.byref(a), ctypes.sizeof(a))
        attrs.append(a)
        print("   输出[%d] %-42s dims=%s type=%d qnt_type=%d scale=%.8f zp=%d n_elems=%d"
              % (i, a.name.decode(), list(a.dims[:a.n_dims]), a.type, a.qnt_type,
                 a.scale, a.zp, a.n_elems))

    small = preprocess(IMGS[0])
    buf = small.reshape(-1)
    arr = (ctypes.c_uint8 * buf.size).from_buffer_copy(buf.tobytes())
    inp = RknnInput()
    inp.index = 0; inp.buf = ctypes.cast(arr, ctypes.c_void_p)
    inp.size = buf.size; inp.pass_through = 0
    inp.type = 3; inp.fmt = 1
    lib.rknn_inputs_set(ctx, 1, ctypes.byref(inp))
    lib.rknn_run(ctx, None)

    res = {}
    for want_float in (1, 0):
        outs = (RknnOutput * io.n_output)()
        for i in range(io.n_output):
            outs[i].want_float = want_float
            outs[i].is_prealloc = 0
            outs[i].index = i
        lib.rknn_outputs_get(ctx, io.n_output, outs, None)
        for i in range(io.n_output):
            a = attrs[i]
            if want_float:
                p = ctypes.cast(outs[i].buf, ctypes.POINTER(ctypes.c_float))
                v = np.ctypeslib.as_array(p, shape=(a.n_elems,)).copy()
            else:
                p = ctypes.cast(outs[i].buf, ctypes.POINTER(ctypes.c_int8))
                v = np.ctypeslib.as_array(p, shape=(a.n_elems,)).copy()
            res[(want_float, i)] = v
        lib.rknn_outputs_release(ctx, io.n_output, outs)
    lib.rknn_destroy(ctx)
    return res, attrs


def main():
    for model in ["/home/seeed/rk_deploy/models/ebike_int8_rk2332.rknn",
                  "/home/seeed/rk_deploy/opt_ebike_fork9/models/ebike_real_formC_int8.rknn"]:
        print("=" * 92)
        print("【%s】" % os.path.basename(model))
        res, attrs = run(model)
        for i, a in enumerate(attrs):
            f = res[(1, i)]
            q = res[(0, i)]
            manual = (q.astype(np.float32) - np.float32(a.zp)) * np.float32(a.scale)
            diff = np.abs(f - manual)
            print("   输出[%d] %s:" % (i, a.name.decode()))
            print("      want_float=1 vs 手动反量化: max|Δ|=%.6g  mean|Δ|=%.6g"
                  % (diff.max(), diff.mean()))
            print("      int8 原始: min=%d max=%d   float: min=%.4f max=%.4f"
                  % (q.min(), q.max(), f.min(), f.max()))
            if a.n_elems == 33600:
                m = f.reshape(4, 8400)
                for ch in range(4):
                    print("         ch%d: float min=%.3f max=%.3f mean=%.3f | int8 min=%d max=%d"
                          % (ch, m[ch].min(), m[ch].max(), m[ch].mean(),
                             q.reshape(4, 8400)[ch].min(), q.reshape(4, 8400)[ch].max()))


if __name__ == "__main__":
    main()
