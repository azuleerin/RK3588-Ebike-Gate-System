#!/bin/bash
# ============================================================================
# run_all.sh —— 精度回归的完整流程(夜间跑)
#
# 阶段:
#   0) 造校准集(24 张 vs 200 张)
#   1) 转换模型(PP-OCR fp16/int8×2, yolov8n int8 用新校准集)
#   2) 批量推理(YOLO 4 个模型 × 1724 帧; 识别 4 个模型 × 217 张车牌)
#   3) 汇总 -> summary CSV + ACCURACY_REPORT.md
#
# 每一步都写日志到 logs/, 断了可以从下一步接着跑(本脚本会跳过已完成的产物)。
# 用法: ./run_all.sh
# ============================================================================
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

ROOT=/home/seeed/rk_deploy
PY="$ROOT/_bench/venv/bin/python"
BENCH="$ROOT/_bench"
LOGDIR="$HERE/logs"
mkdir -p "$LOGDIR" out

export LD_LIBRARY_PATH="$ROOT/rk3588_zero_copy/third_party/lib:${LD_LIBRARY_PATH:-}"

LPR_MODEL="$ROOT/rk3588_zero_copy/model/lprnet_finetuned_int8.rknn"
LPR_CHARS="0123456789ABCDEFGHJKLMNPQRSTUVWXYZ"
DICT="$ROOT/rk3588_zero_copy/model/ppocr_keys_v1.txt"
FRAMES="data/manifests/frames_list.txt"
PLATES="data/manifests/plates_serial.csv"

step() { echo "[$(date '+%H:%M:%S')] ===== $* ====="; }

# ---------------------------------------------------------------- 0) 校准集
step "0) 造校准集"
$PY build_calib.py --outdir calib 2>&1 | tee "$LOGDIR/00_calib.log"

# ---------------------------------------------------------------- 1) 转换
step "1) 转换模型(PP-OCR fp16/int8×2 + yolov8n int8 新校准集)"
$PY convert_models.py 2>&1 | tee "$LOGDIR/01_convert.log"

# ---------------------------------------------------------------- 2) 推理
step "2a) YOLO 批量推理(1724 帧 × 4 个模型)"
run_yolo() {  # $1=模型 $2=标签
    if [ ! -f "$1" ]; then echo "  跳过(模型不存在): $1"; return 0; fi
    if [ -s "out/boxes_$2.csv" ]; then echo "  跳过(已存在): out/boxes_$2.csv"; return 0; fi
    echo "  -> $2"
    ./acc_batch yolo --model "$1" --list "$FRAMES" --out "out/boxes_$2.csv" \
        --conf 0.25 --nms 0.45 --size 640x640 2>&1 | tail -3
}
run_yolo "$BENCH/yolov8n_rk3588_fp16.rknn"        yolov8n_fp16
run_yolo "$BENCH/yolov8n_rk3588_int8.rknn"        yolov8n_int8_calib24
run_yolo "out/yolov8n_int8_calib200.rknn"          yolov8n_int8_calib200
run_yolo "$ROOT/rk3588_zero_copy/model/ebike_int8_rk2332.rknn" ebike_int8

step "2b) 识别批量推理(217 张车牌 × 4 个模型)"
run_rec() {  # $1=模型 $2=标签 $3=额外的模型参数
    if [ ! -f "$1" ]; then echo "  跳过(模型不存在): $1"; return 0; fi
    if [ -s "out/rec_$2.csv" ]; then echo "  跳过(已存在): out/rec_$2.csv"; return 0; fi
    echo "  -> $2"
    ./acc_batch rec --list "$PLATES" --out "out/rec_$2.csv" $3 2>&1 | tail -2
}
run_rec "$LPR_MODEL" lprnet_int8 "--lpr $LPR_MODEL --charset $LPR_CHARS"
# PP-OCR 的三个模型都是"归一化已烤进模型"(转模型时 mean/std=127.5)
# -> 板端只喂原始 0~255 像素(--ppocr-baked)
# ★ --ppocr-all: 不做字符白名单过滤。
#   ★★ 千万别加白名单: 车牌裁剪图上还带着上面一行"苏N", 模型在那里会输出一个
#      汉字; 白名单把这个汉字换成了一个字母, 整串就废了(实测输出 "TLA390580C10")。
#      不加白名单时 FP16 读出来就是干净的 "A339058"。
run_rec "out/ppocr_fp16.rknn"           ppocr_fp16          "--ppocr out/ppocr_fp16.rknn --dict $DICT --ppocr-baked --ppocr-all"
run_rec "out/ppocr_int8_calib24.rknn"   ppocr_int8_calib24  "--ppocr out/ppocr_int8_calib24.rknn --dict $DICT --ppocr-baked --ppocr-all"
run_rec "out/ppocr_int8_calib200.rknn"  ppocr_int8_calib200 "--ppocr out/ppocr_int8_calib200.rknn --dict $DICT --ppocr-baked --ppocr-all"

# ---------------------------------------------------------------- 3) 汇总
step "3) 汇总"
$PY aggregate.py 2>&1 | tee "$LOGDIR/03_aggregate.log"

step "完成"
