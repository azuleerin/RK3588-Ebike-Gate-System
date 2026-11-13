#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""convert_models.py —— 精度回归要用的模型转换(统一入口)

产出(都在 out/ 下):
  yolov8n_fp16.rknn              已有, 直接软链
  yolov8n_int8_calib24.rknn      已有(现有 24 张校准集)
  yolov8n_int8_calib200.rknn     新转: 200 张校准集
  ppocr_fp16.rknn                新转: 不量化, 做 FP 基准
  ppocr_int8_calib24.rknn        新转: 24 张车牌校准集
  ppocr_int8_calib200.rknn       新转: 200 张车牌校准集

★ PP-OCR 的官方口径 (rknn_model_zoo/examples/PPOCR/PPOCR-Rec/python/convert.py):
     do_quantization=False 建 FP 模型; 量化版用 do_quantization=True + dataset。
★ YOLO 的官方口径 (rknn_model_zoo/examples/yolov8/python/convert.py):
     mean_values=[[0,0,0]], std_values=[[255,255,255]], target_platform='rk3588'
"""
import os
import sys

from rknn.api import RKNN

OUT = 'out'


def build(in_onnx, out_rknn, dataset=None, fp16=False, mean_std=True, mean=None, std=None):
    print('>>> %s -> %s  (quant=%s, dataset=%s)' %
          (in_onnx, out_rknn, not fp16, dataset), flush=True)
    os.makedirs(os.path.dirname(out_rknn) or '.', exist_ok=True)
    rknn = RKNN(verbose=False)
    cfg = {'target_platform': 'rk3588'}
    if mean_std:
        # 把归一化固化进模型, 板端只喂 0~255 原图
        #   YOLO  : 官方口径 mean=0/std=255
        #   PP-OCR: 官方 C++ 示例是 (x-127.5)/127.5 -> mean=127.5/std=127.5
        cfg['mean_values'] = [mean if mean else [0, 0, 0]]
        cfg['std_values'] = [std if std else [255, 255, 255]]
    if rknn.config(**cfg) != 0:
        print('config failed')
        return False
    if rknn.load_onnx(model=in_onnx) != 0:
        print('load_onnx failed')
        return False
    if rknn.build(do_quantization=not fp16, dataset=dataset) != 0:
        print('build failed')
        return False
    if rknn.export_rknn(out_rknn) != 0:
        print('export failed')
        return False
    rknn.release()
    print('    OK %s (%d 字节)' % (out_rknn, os.path.getsize(out_rknn)), flush=True)
    return True


def main():
    jobs = []
    # ---- PP-OCR(识别) ----
    if os.path.exists('models/ppocrv4_rec.onnx'):
        jobs += [
            # (onnx, out, dataset, fp16, mean, std)
            ('models/ppocrv4_rec.onnx', OUT + '/ppocr_fp16.rknn', None, True,
             [127.5, 127.5, 127.5], [127.5, 127.5, 127.5]),
            ('models/ppocrv4_rec.onnx', OUT + '/ppocr_int8_calib24.rknn',
             'calib/calib_rec_24.txt', False,
             [127.5, 127.5, 127.5], [127.5, 127.5, 127.5]),
            ('models/ppocrv4_rec.onnx', OUT + '/ppocr_int8_calib200.rknn',
             'calib/calib_rec_200.txt', False,
             [127.5, 127.5, 127.5], [127.5, 127.5, 127.5]),
        ]
    # ---- YOLO(检测) ----
    yolov8n_onnx = '../_bench/dl/yolov8n_optimized.onnx'
    if os.path.exists(yolov8n_onnx):
        jobs.append((yolov8n_onnx, OUT + '/yolov8n_int8_calib200.rknn',
                     'calib/calib_yolo_200.txt', False, None, None))

    # ---- 已有的模型: 软链过去, 免得重复转 ----
    for src, dst in (('../_bench/yolov8n_rk3588_fp16.rknn', OUT + '/yolov8n_fp16.rknn'),
                     ('../_bench/yolov8n_rk3588_int8.rknn', OUT + '/yolov8n_int8_calib24.rknn')):
        if os.path.exists(src) and not os.path.exists(dst):
            os.makedirs(OUT, exist_ok=True)
            os.symlink(os.path.abspath(src), dst)
            print('>>> 复用已有模型: %s -> %s' % (src, dst))

    ok = True
    for job in jobs:
        in_onnx, out_rknn, ds, fp16 = job[0], job[1], job[2], job[3]
        mean = job[4] if len(job) > 4 else None
        std = job[5] if len(job) > 5 else None
        if os.path.exists(out_rknn):
            print('>>> 已存在, 跳过: %s' % out_rknn)
            continue
        ok = build(in_onnx, out_rknn, ds, fp16, mean=mean, std=std) and ok
    print('全部转换完成' if ok else '有转换失败')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
