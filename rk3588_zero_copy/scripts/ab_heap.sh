#!/bin/bash
# A/B 测活堆斜率: 同一台板子、同一条流、同样的参数, 跑 N 帧, 看 heap 涨不涨。
#
# 用法:
#   scripts/ab_heap.sh <tag> [frames]
#
# 产出:
#   logs/heap_<tag>.log   —— 主程序日志(含 [frame] 行的 heap= 字段)
#   终端直接打印斜率判定
#
# ⚠️ 固定用 --decoder ffmpeg(软解): MPP 硬解路径不经过 FFmpeg 解码器,
#    跟"包泄漏"这条线无关。要测硬解请加 --decoder mpp。
set -e

ROOT_PWD=$(cd "$(dirname "$0")/.." && pwd)
TAG=${1:-run}
FRAMES=${2:-600}
LOGDIR=${LOGDIR:-/home/seeed/rk_deploy/logs}
LOG="${LOGDIR}/heap_${TAG}.log"

export LD_LIBRARY_PATH="$ROOT_PWD/third_party/lib:$ROOT_PWD/lib:/usr/lib:${LD_LIBRARY_PATH:-}"

cd "$ROOT_PWD"
echo "=== A/B: tag=${TAG} frames=${FRAMES} decoder=ffmpeg ==="
./build/rk_zero_copy \
  rtsp://192.168.8.200:8557/h264 \
  model/ebike_int8_rk2332.rknn \
  --labels model/labels_ebike.txt \
  --no-show \
  --frames "${FRAMES}" \
  --decoder ffmpeg \
  --track-cls 1 --nms 0.45 \
  --lprnet /home/seeed/rk_deploy/models/lprnet_finetuned_int8.rknn \
  --plate-cls 1 \
  --roi-ratio 0.72 --roi-y 0.70 --roi-height 0.16 \
  --mem-trim-frames 500 \
  --log-file "${LOG}" > /dev/null 2>&1 || true

python3 scripts/heap_slope.py "${LOG}"
