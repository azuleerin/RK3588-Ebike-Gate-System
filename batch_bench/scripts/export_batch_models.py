#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""export_batch_models.py —— 导出 Batch=1/2/4 的 RKNN 模型

★ 关键: Batch 推理的 batch 维是【烤进模型里】的, 不是运行期参数。
  做法: rknn.build(rknn_batch_size=N) —— toolkit 会往图里插 batch 展开算子,
  输入张量随之变成 [N,3,H,W]。运行期只能用 rknn_set_io_mem 喂整块 batch 缓冲。

★ 量化口径与第二步(model_opt/export_quantize.py)完全一致, 保证对比链条干净:
  w8a8 / per-tensor(layer) / normal / optimization_level=3 /
  mean=[[0,0,0]] std=[[255,255,255]] / 同一份 24 张校准集

用法:
    ./export_batch_models.py [--onnx <源ONNX>] [--batches 1,2,4] [--out-dir models]
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))          # scripts/
BASE = os.path.dirname(HERE)                                  # batch_bench/
REPO = os.path.dirname(BASE)


def build_batch(onnx, out_rknn, batch, calib):
    from rknn.api import RKNN

    r = RKNN(verbose=False)
    cfg = dict(
        mean_values=[[0, 0, 0]],
        std_values=[[255, 255, 255]],
        target_platform="rk3588",
        quantized_dtype="w8a8",
        quantized_algorithm="normal",
        quantized_method="layer",          # per-tensor
        optimization_level=3,
        float_dtype="float16",
        quant_img_RGB2BGR=False,
    )
    if r.config(**cfg) != 0:
        raise RuntimeError("config 失败")
    if r.load_onnx(model=onnx) != 0:
        raise RuntimeError("load_onnx 失败")
    if r.build(do_quantization=True, dataset=calib,
               rknn_batch_size=batch) != 0:
        raise RuntimeError("build 失败")
    os.makedirs(os.path.dirname(out_rknn) or ".", exist_ok=True)
    if r.export_rknn(out_rknn) != 0:
        raise RuntimeError("export 失败")
    r.release()
    return os.path.getsize(out_rknn)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--onnx", default=os.path.join(
        REPO, "model_opt", "onnx", "yolov8n_relu_512.onnx"))
    ap.add_argument("--calib", default=os.path.join(REPO, "_bench", "calib_list.txt"))
    ap.add_argument("--batches", default="1,2,4")
    ap.add_argument("--out-dir", default=os.path.join(BASE, "models"))
    args = ap.parse_args()

    batches = [int(b) for b in args.batches.split(",") if b.strip()]
    os.makedirs(args.out_dir, exist_ok=True)

    print("源 ONNX  : %s" % args.onnx)
    print("校准集    : %s" % args.calib)
    print("输出目录  : %s\n" % args.out_dir)

    info = {"onnx": args.onnx, "calib": args.calib, "models": {}}
    for b in batches:
        out = os.path.join(args.out_dir, "batch%d.rknn" % b)
        # ★ toolkit 会把中间优化产物写到 CWD, 切到临时目录免得污染
        cwd = os.getcwd()
        tmpd = tempfile.mkdtemp(prefix="rknn_batch_")
        try:
            os.chdir(tmpd)
            n = build_batch(args.onnx, out, b, args.calib)
        finally:
            os.chdir(cwd)
            shutil.rmtree(tmpd, ignore_errors=True)
        info["models"][str(b)] = {"path": out, "bytes": n, "batch": b}
        print("  Batch=%d -> %s (%.2f MB)" % (b, out, n / 1048576.0))

    with open(os.path.join(args.out_dir, "batch_models.json"), "w") as f:
        json.dump(info, f, indent=2, ensure_ascii=False)
    print("\n完成, 清单: %s" % os.path.join(args.out_dir, "batch_models.json"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
