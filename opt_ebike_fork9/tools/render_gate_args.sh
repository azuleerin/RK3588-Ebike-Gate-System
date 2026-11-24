#!/usr/bin/env bash
# ============================================================================
# render_gate_args.sh —— 从 config/gate.env 渲染【与生产完全一致】的 rk_gate 命令行
#
# ★ 为什么自己渲染而不是复制粘贴:
#   冒烟测试必须和 systemd 起来的那一个【逐参数一致】, 否则测的不是同一个东西。
#   这里照抄 scripts/run_gate_service.sh 的 ARGS 组装与 4 路通道渲染逻辑。
#   (两份逻辑若不同步, 以 run_gate_service.sh 为准 —— 它才是生产真相。)
#
# 用法: bash render_gate_args.sh            # 打印命令行
#       bash render_gate_args.sh --run 20   # 打印并直接跑(--duration 20)
# ============================================================================
set -uo pipefail

PROJ=/home/seeed/rk_deploy/rk3588_zero_copy
BIN=$PROJ/build/rk_gate
GATE_ENV=${GATE_ENV:-$PROJ/config/gate.env}

# shellcheck disable=SC1090
set -a; . "$GATE_ENV"; set +a

GATE_BIN=${GATE_BIN:-$BIN}
GATE_OCR=${GATE_OCR:-/home/seeed/rk_deploy/models/ppocrv4_rec_fp16.rknn}
GATE_OCR_DICT=${GATE_OCR_DICT:-/home/seeed/rk_deploy/models/ppocr_keys_v1.txt}
GATE_DET_CORES=${GATE_DET_CORES:-0,1,2}
GATE_OCR_CORES=${GATE_OCR_CORES:-0,1,2}
GATE_DET_WORKERS=${GATE_DET_WORKERS:-3}
GATE_OCR_WORKERS=${GATE_OCR_WORKERS:-3}
GATE_DET_SLOTS=${GATE_DET_SLOTS:-2}
GATE_STATS_INTERVAL=${GATE_STATS_INTERVAL:-100}
GATE_IPM_WIDTH_M=${GATE_IPM_WIDTH_M:-3.5}
GATE_SN=${GATE_SN:-RK3588-GATE-01}
GATE_DEVICE_NAME=${GATE_DEVICE_NAME:-0001}
GATE_IPADDR=${GATE_IPADDR:-192.168.31.77}
GATE_DURATION=${GATE_DURATION:-0}

ARGS=(
    --model   "$GATE_MODEL"
    --labels  "$GATE_LABELS"
    --ppocr   "$GATE_OCR"
    --ppocr-dict "$GATE_OCR_DICT"
    --det-core     "$GATE_DET_CORES"
    --ocr-core     "$GATE_OCR_CORES"
    --det-workers  "$GATE_DET_WORKERS"
    --ocr-workers  "$GATE_OCR_WORKERS"
    --det-slots    "$GATE_DET_SLOTS"
    --conf    "$GATE_CONF"
    --nms     "$GATE_NMS"
    --max-fps "$GATE_MAX_FPS"
    --stats-interval "$GATE_STATS_INTERVAL"
    --roi-ratio  "$GATE_ROI_RATIO"
    --roi-y      "$GATE_ROI_Y"
    --roi-height "$GATE_ROI_HEIGHT"
    --roi-shift  "$GATE_ROI_SHIFT"
    --roi-below  "$GATE_ROI_BELOW"
    --ipm-width-m "$GATE_IPM_WIDTH_M"
    --sn       "$GATE_SN"
    --device-name "$GATE_DEVICE_NAME"
    --ipaddr   "$GATE_IPADDR"
    --duration "$GATE_DURATION"
)

for i in 1 2 3 4; do
    var="GATE_CH$i";      url="${!var:-}"
    kind_var="GATE_CH${i}_KIND"; kind="${!kind_var:-}"
    ipm_var="GATE_CH${i}_IPM";  ipm="${!ipm_var:-}"
    [ -z "$url" ] && continue
    spec="ch${i}:${url}:${kind}"
    if [ "$kind" = "high" ] && [ -n "$ipm" ]; then
        spec="${spec}:${ipm}"
    fi
    ARGS+=(--ch "$spec")
done

if [ -n "${GATE_MQTT_HOST:-}" ]; then
    ARGS+=(--mqtt-host "$GATE_MQTT_HOST" --mqtt-port "${GATE_MQTT_PORT:-1883}"
           --mqtt-qos "${GATE_MQTT_QOS:-1}")
fi
for i in 1 2 3 4; do
    var="GATE_TRIPWIRE_CH$i"; line="${!var:-}"
    [ -z "$line" ] && continue
    ARGS+=(--tripwire-line "ch${i}:${line}")
done
if [ -n "${GATE_TRIPWIRE_INTERVAL_MS:-}" ]; then
    ARGS+=(--tripwire-interval-ms "$GATE_TRIPWIRE_INTERVAL_MS")
fi

if [ "${1:-}" = "--run" ]; then
    DUR="${2:-20}"
    ARGS=("${ARGS[@]/--duration $GATE_DURATION/}")   # 去掉原 duration
    ARGS+=(--duration "$DUR")
    export LD_LIBRARY_PATH=$PROJ/third_party/lib:$PROJ/lib:/usr/lib
    export RK_LOG_LEVEL="${RK_LOG_LEVEL:-warn}"
    exec "$GATE_BIN" "${ARGS[@]}"
fi

if [ "${1:-}" = "--lines" ]; then
    # ★ --lines 模式: 每行一个参数, 【不加任何 shell 转义】。
    #   默认的 printf '%q ' 是给人复制粘贴看的, 但 IPM 标定点里带 ';' 和 ',',
    #   %q 会把它们转义成 \; \, —— 直接拿去 exec 会把参数改坏。
    #   所以脚本消费方一律用 --lines。
    printf '%s\n' "$GATE_BIN" "${ARGS[@]}"
    exit 0
fi

printf '%q ' "$GATE_BIN" "${ARGS[@]}"
echo
