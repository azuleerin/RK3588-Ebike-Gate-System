#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""py_rga_cpu_reference.py —— 「RGA CPU 兜底」的 Python 参考实现, 用于对拍。

与 C++ 的 rk::rga_nv12_to_rgb888() 在 RK_FORCE_CPU_RGA=1 下走同一条 CPU 路径。

输入: C++ 测试写出的 <prefix>_cpu.bin / <prefix>_meta.txt
      (meta: "sw sh s_wstride s_hstride dw dh d_wstride")

流程(C++ 也必须完全一致):
  1) 从带 padding 的源里裁出连续区域 -> 连续 NV12
  2) cvtColor(NV12 -> RGB)
  3) resize 到 (dw, dh)
  4) 逐行写进 stride 为 d_wstride 的目标缓冲

用法: python3 py_rga_cpu_reference.py <prefix>
"""
import sys

import numpy as np


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    prefix = sys.argv[1]

    with open(prefix + '_meta.txt') as f:
        sw, sh, s_wstride, s_hstride, dw, dh, d_wstride = [int(x) for x in f.read().split()]

    cpp = np.fromfile(prefix + '_cpu.bin', dtype=np.uint8)
    cpp = cpp.reshape(dh, d_wstride)

    # ---- 1) 造同样的带 padding 的 NV12 源 ----
    src = np.zeros((s_hstride * 3 // 2, s_wstride), dtype=np.uint8)
    ys, xs = np.mgrid[0:sh, 0:sw]
    src[0:sh, 0:sw] = ((xs + ys * 3) & 0xFF).astype(np.uint8)
    uv_off = s_hstride
    # NV12 的 UV plane 每行 sw 字节(= sw/2 个 chroma 采样, U/V 交织)
    yy, xx = np.mgrid[0:sh // 2, 0:sw // 2]
    src[uv_off:uv_off + sh // 2, 0:sw:2] = ((xx * 2) & 0xFF).astype(np.uint8)
    src[uv_off:uv_off + sh // 2, 1:sw:2] = ((yy * 5) & 0xFF).astype(np.uint8)

    # ---- 2) 裁出连续 NV12(全幅, 与 C++ 的 (0,0,sw,sh) 一致) ----
    contig = np.zeros((sh, sw), dtype=np.uint8)
    contig[:] = src[0:sh, 0:sw]
    ucontig = np.zeros((sh // 2, sw), dtype=np.uint8)
    ucontig[:] = src[uv_off:uv_off + sh // 2, 0:sw]
    nv12 = np.empty((sh * 3 // 2, sw), dtype=np.uint8)
    nv12[0:sh] = contig
    nv12[sh:] = ucontig

    # ---- 3) NV12 -> RGB ----
    import cv2
    rgb = cv2.cvtColor(nv12, cv2.COLOR_YUV2RGB_NV12)

    # ---- 4) resize ----
    scaled = cv2.resize(rgb, (dw, dh), interpolation=cv2.INTER_LINEAR)

    # ---- 5) 写进带 stride 的目标缓冲 ----
    dst = np.full((dh, d_wstride), 0xAB, dtype=np.uint8)
    dst[0:dh, 0:dw * 3] = scaled.reshape(dh, dw * 3)

    diff = np.abs(dst.astype(np.int16) - cpp.astype(np.int16))
    maxd = int(diff.max())
    bad = int((diff > 2).sum())
    print('最大像素差: %d   超过 2 的像素数: %d / %d' % (maxd, bad, diff.size))
    if maxd <= 2:
        print('对拍通过 (CPU 兜底与参考实现一致)')
        return 0
    print('对拍失败')
    ys2, xs2 = np.where(diff > 2)
    for i in range(min(5, len(ys2))):
        y, x = ys2[i], xs2[i]
        print('  (%d,%d) cpp=%d ref=%d' % (y, x, cpp[y, x], dst[y, x]))
    return 1


if __name__ == '__main__':
    sys.exit(main())
