#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""convert_ppocr_rec.py —— PP-OCRv4_mobile_rec 的 ONNX -> RKNN 转换脚本

来源: rknn_model_zoo v2.5.0 examples/PPOCR/PPOCR-Rec/python/convert.py
      (官方只有 FP 版本; 这里补了 INT8 + UINT8 输入的量化流程, 因为车牌识别
       模型很小, 量化后 NPU 耗时能再降一截, 而且 12 路并发时收益很明显)

★ 转换必须在【PC/x86】上跑(rknn-toolkit2 的编译器只有 x86 版本),
  板子上跑不了。板子只负责跑 librknnrt。

用法:
    # ① FP(官方默认): 输入张量是 FLOAT32, 板端自己做 (x-127.5)/127.5
    python3 convert_ppocr_rec.py ppocrv4_rec.onnx rk3588 fp ../model/ppocrv4_rec.rknn

    # ② INT8 + UINT8 输入(推荐): 归一化固化进模型, 板端【不用做归一化】
    #    板端喂原始 0~255 像素即可, 归一化和量化的开销全在 NPU 侧
    python3 convert_ppocr_rec.py ppocrv4_rec.onnx rk3588 int8 ../model/ppocrv4_rec_int8.rknn \
        --dataset dataset.txt --mean 127.5 127.5 127.5 --std 127.5 127.5 127.5

dataset.txt 里每行一个图片路径(校准集, 建议 100~300 张真实车牌小图,
覆盖不同光照/角度/车牌底色)。没有校准集就别做量化, FP 也能跑。

★ 转完之后板端怎么用:
    rk::PlateRecConfig cfg;
    cfg.model_path = "./model/ppocrv4_rec_int8.rknn";
    cfg.dict_path  = "./model/ppocr_keys_v1.txt";
    cfg.norm_mode  = rk::PlateNormMode::kMinus127Div127;  // UINT8 输入时这个不生效
    // 模型输入是 UINT8/INT8 时, 类内部按模型自己的 zp/scale 生成量化码(现查现用)
"""
import os
import sys

from rknn.api import RKNN

RKNPU1_PLATFORM = ['rk1808', 'rv1109', 'rv1126']
DEFAULT_RKNN_PATH = './ppocrv4_rec.rknn'


def parse_arg():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    model_path = sys.argv[1]
    platform = sys.argv[2]
    model_type = sys.argv[3] if len(sys.argv) > 3 else 'fp'
    if model_type not in ('fp', 'int8'):
        print("ERROR: model type 只能是 fp / int8")
        sys.exit(1)
    output_path = sys.argv[4] if len(sys.argv) > 4 else DEFAULT_RKNN_PATH

    dataset = None
    mean = std = None
    i = 5
    while i < len(sys.argv) - 1:
        if sys.argv[i] == '--dataset':
            dataset = sys.argv[i + 1]
        elif sys.argv[i] == '--mean':
            mean = [float(v) for v in sys.argv[i + 1].split(',')]
        elif sys.argv[i] == '--std':
            std = [float(v) for v in sys.argv[i + 1].split(',')]
        i += 2
    return model_path, platform, model_type, output_path, dataset, mean, std


def main():
    model_path, platform, model_type, output_path, dataset, mean, std = parse_arg()

    rknn = RKNN(verbose=True)

    print('--> Config model')
    if platform in RKNPU1_PLATFORM:
        rknn.config(target_platform=platform)
    else:
        cfg = {'target_platform': platform}
        if mean is not None and std is not None:
            # 归一化固化进模型: 板端喂原始 0~255 像素, 运行时内部做 (x-mean)/std
            # ★ 这样板端的 CPU 预处理只剩"裁剪+缩放", 归一化那 46KB 的浮点转换
            #   可以整段省掉(见 src/license_plate_rec.cc 的 UINT8 分支)。
            cfg['mean_values'] = [mean]
            cfg['std_values'] = [std]
        rknn.config(**cfg)
    print('done')

    print('--> Loading model')
    ret = rknn.load_onnx(model=model_path)
    if ret != 0:
        print('Load model failed!')
        sys.exit(ret)
    print('done')

    print('--> Building model')
    do_quant = (model_type == 'int8')
    if do_quant and not dataset:
        print('ERROR: int8 需要 --dataset 校准集')
        sys.exit(1)
    ret = rknn.build(do_quantization=do_quant, dataset=dataset)
    if ret != 0:
        print('Build model failed!')
        sys.exit(ret)
    print('done')

    print('--> Exporting model')
    ret = rknn.export_rknn(output_path)
    if ret != 0:
        print('Export model failed!')
        sys.exit(ret)
    print('done: %s (%d 字节)' % (output_path, os.path.getsize(output_path)))


if __name__ == '__main__':
    main()
