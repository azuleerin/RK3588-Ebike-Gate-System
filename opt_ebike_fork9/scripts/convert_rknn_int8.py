#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""convert_rknn_int8.py —— ONNX -> RKNN INT8 (rknn-toolkit2 2.3.2, rk3588)

★ 配方来自仓库里已有的两处口径(不是自创):
  · accuracy/convert_models.py      —— 官方 YOLO 口径 mean=0 / std=255
  · npu_layer_bench/scripts/make_res_models.sh —— toolkit 必须是 2.3.2
    (板端 runtime 就是 2.3.2, 版本不一致模型加载不了)

★ 归一化已固化进模型
  mean=0/std=255 等价于图里有一个 Div255 节点, 所以板端喂【原始 0~255 像素】,
  不要再除 255(见 EBIKE_MODEL_RUNBOOK.md 第二节又二分之一)。

用法:
    convert_rknn_int8.py <in.onnx> <out.rknn> [--calib calib/ebike_calib.txt] [--fp16]
"""
import argparse
import os
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--calib', default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), '..', 'calib', 'ebike_calib.txt'))
    ap.add_argument('--fp16', action='store_true', help='不量化(FP 基准)')
    args = ap.parse_args()

    import rknn
    # rknn 是 namespace package, __file__ 可能是 None -> 用 __path__
    pkg_dir = os.path.dirname(list(rknn.__path__)[0])
    vfile = os.path.join(pkg_dir, 'VERSION')
    ver = open(vfile).read().strip() if os.path.exists(vfile) else 'unknown'
    print(f">>> rknn-toolkit2 {ver}")
    if ver != '2.3.2':
        print(f"    [警告] 板端 runtime 是 2.3.2, 这里不是 {ver}", file=sys.stderr)

    from rknn.api import RKNN
    calib = os.path.abspath(args.calib)
    if not args.fp16:
        if not os.path.exists(calib):
            raise SystemExit(f"标定集不存在: {calib}")
        n = sum(1 for _ in open(calib))
        print(f">>> 标定集 {calib} ({n} 张)")

    r = RKNN(verbose=False)
    cfg = {'target_platform': 'rk3588',
           'mean_values': [[0, 0, 0]],
           'std_values': [[255, 255, 255]]}
    if r.config(**cfg) != 0:
        raise SystemExit('config failed')
    if r.load_onnx(model=os.path.abspath(args.src)) != 0:
        raise SystemExit('load_onnx failed')
    if r.build(do_quantization=not args.fp16,
               dataset=None if args.fp16 else calib) != 0:
        raise SystemExit('build failed')
    out = os.path.abspath(args.dst)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    if r.export_rknn(out) != 0:
        raise SystemExit('export_rknn failed')
    r.release()
    print(f">>> OK {out} ({os.path.getsize(out)} 字节)")


if __name__ == '__main__':
    main()
