#!/usr/bin/env python3
"""make_npu_input.py —— 生成 npu-only 单元用的预录输入帧

用途:
  npu-only 单元必须完全排除 MPP 与 RGA, 所以输入不能来自实时解码,
  而是一份**预先录好**的帧数据。这样单元里跑的就是纯 RKNN 推理。

产物:
  <out>/frame_<W>x<H>.nv12   NV12 原始帧 (W*H*3/2 字节)
  <out>/frame_<W>x<H>.rgb    RGB888 原始帧 (W*H*3 字节)
  两者来自同一张源图, 内容等价, 供两种输入模式使用。

处理方式: letterbox(等比缩放 + 114 灰边), 与 YOLOv8 训练时的预处理一致。

用法:
  python3 make_npu_input.py --src <图片路径> --out <输出目录> [--width 640] [--height 640]

依赖: 需要 OpenCV 与 numpy。板端可用工程 venv:
  /home/seeed/rk_deploy/_bench/venv/bin/python3.12
"""

import argparse
import os
import sys

import cv2
import numpy as np


def letterbox(img, out_w, out_h, pad=114):
    """等比缩放 + 居中填充(灰边)。返回 (out_img, scale, pad_x, pad_y)。"""
    h, w = img.shape[:2]
    scale = min(out_w / w, out_h / h)
    nw, nh = int(round(w * scale)), int(round(h * scale))
    resized = cv2.resize(img, (nw, nh), interpolation=cv2.INTER_LINEAR)
    canvas = np.full((out_h, out_w, 3), pad, dtype=np.uint8)
    x0, y0 = (out_w - nw) // 2, (out_h - nh) // 2
    canvas[y0:y0 + nh, x0:x0 + nw] = resized
    return canvas, scale, x0, y0


def rgb_to_nv12(rgb):
    """RGB888 -> NV12 (BT.601 有限范围)

    与单元内 unit_npu.cc 的 nv12_to_rgb888() 严格互为逆运算:
      - UV 采用 2x2 最近邻抽点(与单元侧读取方式一致)
      - UV 在行内交织: 偶数列放 U, 奇数列放 V
    返回 H*W*3/2 字节的 uint8 数组。
    """
    yuv = cv2.cvtColor(rgb, cv2.COLOR_RGB2YUV)   # 全分辨率 YUV, 三通道
    h, w = rgb.shape[:2]
    y = yuv[:, :, 0]
    u = yuv[0::2, 0::2, 1]                        # 2x2 抽一个
    v = yuv[0::2, 0::2, 2]
    uv = np.empty((h // 2, w), dtype=np.uint8)
    uv[:, 0::2] = u
    uv[:, 1::2] = v
    nv12 = np.empty(h * w * 3 // 2, dtype=np.uint8)
    nv12[:h * w] = y.ravel()
    nv12[h * w:] = uv.ravel()
    return nv12


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="源图片路径")
    ap.add_argument("--out", required=True, help="输出目录")
    ap.add_argument("--width", type=int, default=640)
    ap.add_argument("--height", type=int, default=640)
    args = ap.parse_args()

    if not os.path.isfile(args.src):
        print(f"[FATAL] 源图不存在: {args.src}", file=sys.stderr)
        return 1

    bgr = cv2.imread(args.src, cv2.IMREAD_COLOR)
    if bgr is None:
        print(f"[FATAL] 无法解码: {args.src}", file=sys.stderr)
        return 1
    rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
    out_img, scale, px, py = letterbox(rgb, args.width, args.height)

    os.makedirs(args.out, exist_ok=True)
    base = f"frame_{args.width}x{args.height}"

    nv12 = rgb_to_nv12(out_img)
    nv12_path = os.path.join(args.out, base + ".nv12")
    with open(nv12_path, "wb") as f:
        f.write(nv12.tobytes())

    rgb_path = os.path.join(args.out, base + ".rgb")
    with open(rgb_path, "wb") as f:
        f.write(out_img.tobytes())

    print(f"[OK] 源图      : {args.src}  {bgr.shape[1]}x{bgr.shape[0]}")
    print(f"[OK] letterbox : scale={scale:.4f} pad_x={px} pad_y={py}")
    print(f"[OK] NV12      : {nv12_path}  ({len(nv12)} 字节)")
    print(f"[OK] RGB888    : {rgb_path}  ({out_img.size} 字节)")
    print()
    print("用 npu-only 单元跑:")
    print(f"  ./build_minimal/rk_minimal --npu-only --src nv12:{nv12_path} "
          f"--model <model.rknn> --frames 100")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
