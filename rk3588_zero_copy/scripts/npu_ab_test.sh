#!/usr/bin/env bash
# ============================================================================
# npu_ab_test.sh —— NPU 负载 A/B/C 实测(检测/识别 核分离 的效果)
#
# 目的: 回答"识别比检测还贵且串行抢 Core0"这个瓶颈, 到底能省多少。
#
# 三组配置(同一段真实车流视频, 同样帧数):
#   A 基线   --ppocr-ttl 2000 --npu-pipe 1                     (改造前)
#   B 组合   --ppocr-ttl 5000 --npu-pipe 2 --lpr-core 4       (检测双核 + 识别挪到 Core2)
#   C 单项   --ppocr-ttl 5000 --npu-pipe 1                     (只改 TTL, 用于归因)
#
# 每组采集:
#   1) 主程序日志   -> FPS / NPU 耗时 / rknn_run / 车牌识别条数
#   2) MQTT 负载流 -> rknnp 三核真实占用(需要板子上有 root 的负载采样器在发)
#
# 用法:
#   ./scripts/npu_ab_test.sh [视频] [帧数]
#   ./scripts/npu_ab_test.sh /home/seeed/testvideo3.mp4 3000
# ============================================================================
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(dirname "$HERE")"
REPO="$(dirname "$PROJ")"

SRC="${1:-/home/seeed/testvideo3.mp4}"
FRAMES="${2:-3000}"
BROKER="${BROKER:-192.168.8.21}"
BROKER_PORT="${BROKER_PORT:-1883}"
SN="${SN:-rk3588-01}"          # 用 root 负载采样器正在发的那个 SN, 才能抓到真值
OUT="$REPO/logs/npu_ab"
MODEL="$PROJ/model/ebike_int8_rk2332.rknn"
LABELS="$PROJ/model/labels_ebike.txt"
POCR="${POCR:-$PROJ/model/ppocrv4_rec_fp16.rknn}"
BIN="$PROJ/build/rk_zero_copy"

mkdir -p "$OUT"
[ -f "$SRC" ] || { echo "❌ 视频不存在: $SRC"; exit 1; }
[ -x "$BIN" ] || { echo "❌ 找不到 $BIN"; exit 1; }

export LD_LIBRARY_PATH="$PROJ/third_party/lib:$PROJ/lib:/usr/lib:${LD_LIBRARY_PATH:-}"

run_group() {
    local name="$1"; shift
    local desc="$1"; shift
    echo
    echo "================================================================"
    echo " $name  $desc"
    echo "   参数: $*"
    echo "================================================================"
    rm -f "$OUT/${name}.log" "$OUT/${name}_load.jsonl"

    # 抓负载流(root 采样器在发 device/<sn>/message/up/load)
    timeout 900 python3 "$PROJ/scripts/mqtt_sub.py" \
        --host "$BROKER" --port "$BROKER_PORT" \
        --topic "device/$SN/message/up/load" \
        --out "$OUT/${name}_load.jsonl" --seconds 900 --quiet \
        >/dev/null 2>&1 &
    local sub_pid=$!
    sleep 2

    local t0 t1
    t0=$(date +%s)
    ( cd "$PROJ" && "$BIN" "$SRC" "$MODEL" \
        --labels "$LABELS" --decoder mpp \
        --conf 0.15 --nms 0.45 --no-show \
        --ppocr "$POCR" --plate-cls 1 --track-cls 1 \
        --sn "$SN" --mqtt-host "$BROKER" --mqtt-port "$BROKER_PORT" \
        --stats-interval 60 \
        --frames "$FRAMES" \
        --log-file "$OUT/${name}.log" "$@" ) >/dev/null 2>&1
    t1=$(date +%s)
    sleep 1
    kill $sub_pid 2>/dev/null

    echo "  用时 $((t1 - t0)) 秒"
    grep -a -A12 "性能统计" "$OUT/${name}.log" | tail -12
}

run_group A "基线(TTL 2s, 单实例)" --ppocr-ttl 2000 --npu-pipe 1
run_group B "组合(TTL 5s, 双实例, 识别绑 Core2)" --ppocr-ttl 5000 --npu-pipe 2 --lpr-core 4
run_group C "单项(TTL 5s, 单实例)" --ppocr-ttl 5000 --npu-pipe 1
# D 组: B 的配置 + INT8 识别模型(第二步的对照: 精度换算力)
POCR_INT8="${POCR_INT8:-$PROJ/model/ppocrv4_rec_int8.rknn}"
if [ -f "$POCR_INT8" ]; then
    run_group D "组合 + INT8 识别(精度-6点换算力)" --ppocr-ttl 5000 --npu-pipe 2 --lpr-core 4 \
        --ppocr "$POCR_INT8"
else
    echo "  (跳过 D 组: 没有 $POCR_INT8)"
fi

echo
echo "================================================================"
echo " 汇总"
echo "================================================================"
python3 "$HERE/npu_ab_report.py" "$OUT"
