#!/usr/bin/env bash
# stream_n_test.sh —— 单板跑 N 路流的实测(跨进程, 每路一个 NPU 核掩码)
#
# ★ 为什么用【跨进程】而不是 --npu-pipe N
#   --npu-pipe 最多 3 个实例(一个 NPU 核一个), 再多会被静默截断成 3。
#   而真实的 12 路部署是"一块板 4 路流", 每路流是【一个独立的 RTSP 源】,
#   用独立进程才装得下 —— 也正好回答"第 4 路挤进来会怎样"。
#
# ★ 核掩码分配
#   --npu-core 1=core0  2=core1  4=core2
#   N 路时按 1,2,4,1,2,4... 轮转: 第 4 路会和第 1 路抢 core0。
#
# ★ 关键指标: 【每核占用率】
#   occupancy = rknn_run 耗时(ms) × 实际帧率 / 1000
#   单核跑满 = 100%。>100% 说明这一核已经欠了账(排队), 帧率必然掉。
#
# 用法:
#   ./scripts/stream_n_test.sh --n 4 --frames 600
#   ./scripts/stream_n_test.sh --n 6 --frames 600
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=/tmp/stream_n
SRC=/tmp/npu_bench/clip1080_60.mp4
MODEL=$ROOT/model/ebike_int8_rk2332.rknn
LABELS=$ROOT/model/labels_ebike.txt
BIN=$ROOT/build/rk_zero_copy
N=4
FRAMES=600
MAXFPS=0
EXTRA=""

while [ $# -gt 0 ]; do
    case "$1" in
        --n)      N="$2"; shift 2;;
        --frames) FRAMES="$2"; shift 2;;
        --src)    SRC="$2"; shift 2;;
        --max-fps) MAXFPS="$2"; shift 2;;
        --extra)  EXTRA="$2"; shift 2;;
        -h|--help) sed -n '2,25p' "$0"; exit 0;;
        *) echo "未知参数: $1" >&2; exit 2;;
    esac
done

[ -f "$SRC" ] || { echo "找不到源 $SRC" >&2; exit 1; }
[ -x "$BIN" ] || { echo "找不到 $BIN" >&2; exit 1; }
mkdir -p "$OUT"
export LD_LIBRARY_PATH="$ROOT/third_party/lib:$ROOT/lib:/usr/lib:${LD_LIBRARY_PATH:-}"

MASKS=(1 2 4)

echo "=================================================================="
echo " 单板 $N 路并发  frames=$FRAMES  源=$(basename "$SRC")"
echo "=================================================================="

# 板上还有谁在用 NPU —— 不记下来, 数字没法解释
others="$(ps -eo pid,args | grep rk_zero_copy | grep -v grep | grep -v "$$" || true)"
if [ -n "$others" ]; then
    echo " ⚠ 板上还有别的实例在跑(会抢 NPU, 结果偏悲观):"
    echo "$others" | sed 's/^/     /'
fi

pids=()
for i in $(seq 1 "$N"); do
    m=${MASKS[$(( (i-1) % 3 ))]}
    "$BIN" "$SRC" "$MODEL" \
        --labels "$LABELS" \
        --decoder mpp --no-show --no-speed --no-sync-src \
        --frames "$FRAMES" --loop-src \
        --stats-interval 100 \
        --npu-pipe 1 --npu-core "$m" \
        --conf 0.15 --nms 0.45 \
        ${MAXFPS:+--max-fps $MAXFPS} $EXTRA \
        > "$OUT/stream$i.log" 2>&1 &
    pids+=($!)
    echo "  启动 第 $i 路: mask=$m  pid=${pids[-1]}"
    sleep 0.3          # 错峰启动: 不要让所有流在同一瞬间抢核
done

for p in "${pids[@]}"; do wait "$p"; done

echo
echo "-- 每路结果 --"
printf "%-6s %-6s %-10s %-12s %-12s %-12s %-12s\n" "路" "mask" "交付FPS" "rknn_run" "NPU总计" "占用率/核" "占用率合计"
total_occ=0
for i in $(seq 1 "$N"); do
    log="$OUT/stream$i.log"
    [ -f "$log" ] || continue
    m=${MASKS[$(( (i-1) % 3 ))]}
    fps="$(grep -E '交付 FPS' "$log" | tail -1 | grep -oE '[0-9]+\.[0-9]+' | head -1)"
    run="$(grep -E 'NPU 推理' "$log" | tail -1 | grep -oE 'rknn_run [0-9.]+' | grep -oE '[0-9.]+' | head -1)"
    # NPU 总耗时 = 推理 + 车牌识别(PP-OCR) + 取回输出, 用它算占用率才准
    inf="$(grep -E 'NPU 推理' "$log" | tail -1 | grep -oE 'NPU 推理 +: [0-9.]+' | grep -oE '[0-9.]+' | head -1)"
    [ -n "$fps" ] || fps=0; [ -n "$run" ] || run=0; [ -n "$inf" ] || inf=0
    occ="$(awk -v f="$fps" -v r="$inf" 'BEGIN{printf "%.1f", f*r/10}')"
    tot="$(awk -v f="$fps" -v r="$inf" 'BEGIN{printf "%.1f", f*r/30}')"
    total_occ="$(awk -v a="$total_occ" -v b="$tot" 'BEGIN{printf "%.1f", a+b}')"
    printf "%-6s %-6s %-10s %-12s %-12s %-12s %-12s\n" "$i" "$m" "$fps" "$run" "$inf" "$occ%" "$tot%"
done
echo
echo " 3 个 NPU 核的总占用率 = $total_occ%   (>100% = 已经欠账, 加流只会降帧率)"
echo
echo " 日志: $OUT/stream*.log"
