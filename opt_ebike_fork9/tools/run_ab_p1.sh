#!/usr/bin/env bash
# ============================================================================
# run_ab_p1.sh —— P1 fork9 4 路生产二进制交错 A/B(帧计数器差分口径)
#
# ★ 口径(严格按任务书):
#   · 吞吐 = 4 路【帧计数器差分】(frames_end-frames_start)/(t_end-t_start)
#     —— 不用 rk_gate 自报的 deliv(累计平均, 虚高 2~4%)
#   · base / new 交错跑(base,new,base,new) —— 抵消温度漂移
#   · 每轮起跑前记录 /sys/kernel/debug/rknpu/load 三核占用(逐核, 不是 devfreq load)
#   · 统计窗口: 口径① 前 15 s / 后 5 s; 口径② 前 20 s / 后 5 s
#   · 其余参数与生产完全一致(4 路 MP4 / 3 det + 3 ocr workers / conf 0.15 / nms 0.45 /
#     GATE_ROI_* 全套), 只换 --model
#
# ★ 其它: NPU 已锁 1 GHz, 板子已 quiesce(无其它 NPU 消费者)
# ============================================================================
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
REPO=/home/seeed/rk_deploy
BIN=$REPO/rk3588_zero_copy/build/rk_gate
BASE=$REPO/models/ebike_int8_rk2332.rknn
NEW=$ROOT/models/ebike_real_fork9_int8.rknn
OUT=${OUT:-$ROOT/out/ab}
DUR20=${DUR20:-100}      # 口径① 生产限帧: 每轮 100 s
DUR0=${DUR0:-180}        # 口径② 解除限帧: 每轮 180 s
WARM20=${WARM20:-15}     # 口径① 剔除前 15 s
WARM0=${WARM0:-20}       # 口径② 剔除前 20 s
ROUNDS=${ROUNDS:-2}      # base,new,base,new —— 至少 2 轮

mkdir -p "$OUT"
cd "$ROOT"

rknpu_load() {
    sudo cat /sys/kernel/debug/rknpu/load 2>/dev/null \
        | grep -oE 'Core[0-9]+: *[0-9]+%' | grep -oE '[0-9]+%' | tr -d '%' | tr '\n' ' '
}

run_arm() {  # $1=arm(base|new) $2=max_fps $3=duration $4=warmup $5=tag
    local arm="$1" fps="$2" dur="$3" warm="$4" tag="$5"
    local model=$BASE; [ "$arm" = new ] && model=$NEW
    local idle
    idle="$(rknpu_load)"
    echo ">>> [$(date '+%F %T')] arm=$arm max_fps=$fps dur=${dur}s 起跑前逐核 NPU: ${idle:-读不到}"
    # 纪律: 任一核 > 8% 则该轮作废
    local bad=0 v
    for v in $idle; do [ "$v" -gt 8 ] && bad=1; done
    if [ "$bad" -eq 1 ]; then
        echo "!!! 起跑前 NPU 不干净(>8%), 该轮作废"
        return 2
    fi
    "$REPO/_bench/venv/bin/python" -I "$HERE/ab_harness.py" \
        --bin "$BIN" --label "$tag" --model "$model" \
        --duration "$dur" --warmup "$warm" --max-fps "$fps" \
        --out "$OUT/${tag}.json" 2>&1 | grep -vE "RuntimeWarning|self\.stderr"
}

for r in $(seq 1 "$ROUNDS"); do
    echo "################ 口径① 生产限帧(max-fps=20) 第 $r 轮 ################"
    run_arm base 20 "$DUR20" "$WARM20" "base_r${r}_f20"
    sleep 12
    run_arm new  20 "$DUR20" "$WARM20" "new_r${r}_f20"
    sleep 12
done

for r in $(seq 1 "$ROUNDS"); do
    echo "################ 口径② 解除限帧(max-fps=0) 第 $r 轮 ################"
    run_arm base 0 "$DUR0" "$WARM0" "base_r${r}_f0"
    sleep 12
    run_arm new  0 "$DUR0" "$WARM0" "new_r${r}_f0"
    sleep 12
done

echo
echo "=== 全部轮次完成, 结果在 $OUT ==="
ls -la "$OUT"
