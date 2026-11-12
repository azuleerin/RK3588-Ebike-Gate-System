#!/usr/bin/env python3
# convert_yolov8n.py —— 按 Rockchip 官方口径在 RK3588 板端把 YOLOv8n ONNX 转成 RKNN
#
# 官方配方 (rknn_model_zoo/examples/yolov8/python/convert.py):
#     rknn.config(mean_values=[[0,0,0]], std_values=[[255,255,255]], target_platform='rk3588')
#     rknn.build(do_quantization=True, dataset=...)
#
# 用法: convert_yolov8n.py <in.onnx> <out.rknn> <dataset.txt> [fp16]
#   传 fp16 作为第 4 个参数则 do_quantization=False (建 FP16 模型做对照)

import sys

from rknn.api import RKNN

def main():
    in_onnx = sys.argv[1]
    out_rknn = sys.argv[2]
    dataset = sys.argv[3]
    fp16 = len(sys.argv) > 4 and sys.argv[4] == "fp16"

    rknn = RKNN(verbose=True)

    print(">>> config: mean=[[0,0,0]] std=[[255,255,255]] target_platform='rk3588'")
    ret = rknn.config(mean_values=[[0, 0, 0]],
                      std_values=[[255, 255, 255]],
                      target_platform="rk3588")
    if ret != 0:
        print("config failed:", ret)
        sys.exit(1)

    print(">>> load_onnx:", in_onnx)
    ret = rknn.load_onnx(model=in_onnx)
    if ret != 0:
        print("load_onnx failed:", ret)
        sys.exit(1)

    print(">>> build: do_quantization =", not fp16)
    ret = rknn.build(do_quantization=not fp16, dataset=dataset)
    if ret != 0:
        print("build failed:", ret)
        sys.exit(1)

    print(">>> export:", out_rknn)
    ret = rknn.export_rknn(out_rknn)
    if ret != 0:
        print("export failed:", ret)
        sys.exit(1)

    rknn.release()
    print(">>> DONE:", out_rknn)

if __name__ == "__main__":
    main()
