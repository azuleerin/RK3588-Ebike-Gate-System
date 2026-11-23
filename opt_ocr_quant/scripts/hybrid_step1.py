#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""hybrid_step1.py —— PP-OCR 混合量化的第 1 步: 敏感度分析

用 rknn-toolkit2 2.3.2 的 hybrid_quantization_step1(proposal=True):
  · 跑一遍校准集, 由工具链自己算出【每个候选层的量化敏感度】
  · 产出一份 proposal(建议哪些层该保留 FP16) + 中间模型 + 数据文件

产物落在 OUT 目录里, 第 2 步(hybrid_step2.py)拿它们建最终模型。

用法: hybrid_step1.py <onnx> <calib_list> <out_dir>
"""
import json
import os
import shutil
import sys

from rknn.api import RKNN

MEAN = [[127.5, 127.5, 127.5]]
STD = [[127.5, 127.5, 127.5]]


def main():
    # ★ 先转绝对路径: 下面要 chdir 到工作目录, 相对路径会失效
    onnx = os.path.abspath(sys.argv[1])
    calib = os.path.abspath(sys.argv[2])
    out = os.path.abspath(sys.argv[3])
    os.makedirs(out, exist_ok=True)
    work = os.path.join(out, "step1_work")
    os.makedirs(work, exist_ok=True)
    os.chdir(work)

    print(f">>> config: target=rk3588 mean={MEAN[0]} std={STD[0]}")
    rknn = RKNN(verbose=True)
    ret = rknn.config(mean_values=MEAN, std_values=STD, target_platform="rk3588")
    if ret != 0:
        sys.exit("config failed")

    print(f">>> load_onnx: {onnx}")
    if rknn.load_onnx(model=onnx) != 0:
        sys.exit("load_onnx failed")

    print(f">>> hybrid_quantization_step1 (dataset={calib}, proposal=True)")
    ret = rknn.hybrid_quantization_step1(dataset=calib, proposal=True)
    if ret != 0:
        sys.exit("step1 failed")
    rknn.release()

    print()
    print("=== step1 产出的文件 ===")
    for f in sorted(os.listdir(work)):
        p = os.path.join(work, f)
        print(f"  {os.path.getsize(p):>10d}  {f}")

    # 把 json 类产物单独摘出来看看结构
    for f in sorted(os.listdir(work)):
        if not f.endswith(".json"):
            continue
        p = os.path.join(work, f)
        print()
        print(f"=== {f} 的结构 ===")
        try:
            d = json.load(open(p))
        except Exception as e:
            print(f"  读不了当 json: {e}")
            continue
        if isinstance(d, dict):
            print(f"  顶层 key: {list(d.keys())}")
            for k, v in list(d.items())[:3]:
                if isinstance(v, list):
                    print(f"  {k}: list[{len(v)}], 首元素 = {json.dumps(v[0], ensure_ascii=False)[:400] if v else '空'}")
                elif isinstance(v, dict):
                    print(f"  {k}: dict, key 若干 = {list(v.keys())[:5]}")
                else:
                    print(f"  {k}: {v}")
        elif isinstance(d, list):
            print(f"  list[{len(d)}], 首元素 = {json.dumps(d[0], ensure_ascii=False)[:400] if d else '空'}")


if __name__ == "__main__":
    main()
