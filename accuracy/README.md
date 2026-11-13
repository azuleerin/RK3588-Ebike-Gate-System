# accuracy —— INT8 精度回归测试(独立工具, 不改主程序源码)

夜间跑的离线批处理: 遍历视频抽出的图片, 分别用 YOLO 模型和识别模型推理,
和 FP 基准逐帧/逐张比对, 出 CSV + `ACCURACY_REPORT.md`。

## 目录

| 文件 | 作用 |
|---|---|
| `build_testset.py` | 从 DJI 视频抽整帧 + 按颜色定位绿牌裁出车牌图 + 生成清单(含人工确认的真值) |
| `build_calib.py` | 造量化校准集(24 张 vs 200 张, 检测侧和识别侧各一套) |
| `convert_models.py` | 统一转换入口(PP-OCR fp16/int8×2, yolov8n int8 用新校准集) |
| `acc_batch.cc` | 独立推理工具: `acc_batch yolo` / `acc_batch rec` → CSV |
| `aggregate.py` | 算指标(匹配率 / mAP@0.5 / IoU 分布 / 整牌+单字符准确率) |
| `make_report.py` | 生成 `ACCURACY_REPORT.md` |
| `run_all.sh` | 全流程编排(每步都写 `logs/`, 断了能接着跑) |

## 跑一遍

```bash
cd /home/seeed/rk_deploy/accuracy
../_bench/venv/bin/python build_testset.py --video ../rk3588_zero_copy/DJI_20261102155401_0126_D.MP4 \
    --outdir data --every 1 --save-frames
../_bench/venv/bin/python build_calib.py --outdir calib
./run_all.sh
../_bench/venv/bin/python make_report.py
```

## 产物

- `ACCURACY_REPORT.md` —— 结论报告
- `out/summary_detection.csv` —— 检测侧: 匹配率 / mAP@0.5 / 置信度变化 / 漏检 IoU 分布
- `out/summary_detection_by_class.csv` —— 每个类别的平均置信度变化
- `out/summary_recognition.csv` —— 识别侧: 整牌准确率 / 单字符准确率 / 空输出率
- `out/boxes_*.csv`, `out/rec_*.csv` —— 逐帧/逐张的原始结果

## ★ 踩过的坑(改代码前先看)

1. **喂输入必须和主程序一致**(`src/main.cc:1490-1524`):
   - 量化模型 → 喂 **uint8 NHWC 原始 [0,255]**, `type=UINT8`, 由运行时自己量化。
     ★ 喂 `type=INT8` + 原始字节 = 把像素当成量化码, 输出全乱。
   - 真浮点模型(FLOAT32/FLOAT16) → 喂 float32 **NCHW**。
     ★ 判定要看【逻辑】attr 的 type, FLOAT16 也要算浮点(漏判 → 每帧几千个假框)。
2. **标准 ultralytics 导出的 9 输出, DFL 距离是格子单位, 要乘 stride**。
   工程里 `yolo_post.cc` 的"形态B"是给瑞芯微 fork 导出用的(那个把 stride 烤进图里了,
   所以不乘)。直接拿来解标准导出 → 框小 8/16/32 倍, 全是几像素的碎框。
   本工具自带 `decode_9out_standard()` 处理这种情况。
3. **PP-OCR 的字符白名单会把结果毁掉**: 车牌裁剪图上还有上面一行"苏N",
   模型在那儿会输出一个汉字; 白名单把这个汉字换成一个字母, 整串就废了
   (实测输出 `TLA390580C10`)。不加白名单时 FP16 读出来就是干净的 `A339058`。
4. **LPRNet 的输入是原始 uint8 RGB**(`/255` 和量化码都由 `LprNet::infer()` 自己做),
   外面再归一化一次就全解不出来。
