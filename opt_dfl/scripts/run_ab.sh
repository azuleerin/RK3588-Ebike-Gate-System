#!/usr/bin/env bash
# ============================================================================
# run_ab.sh —— fork9 (DFL 移出 NPU) 板端 4 路 A/B
#
# 口径(与 opt_audit/run_ab.sh 同源):
#   · 吞吐 = 4 路【帧计数器差分】(frames_end-frames_start)/(t_end-t_start)
#     —— 不用 rk_gate 自报的 deliv (累计平均, 会虚高 2~4%)
#   · base / new 两组【交错】跑 (base,new,base,new,...) 抵消温度漂移
#   · 每轮前查 /sys/kernel/debug/rknpu/load 逐核, >8% 作废重跑
#   · 统计窗口: 进程启动 +15s 之后到结束前 5s (避开起播爬坡)
#   · 板子已由 board_quiesce.sh quiesce (服务全停 + 频率锁死)
# ============================================================================
set -uo pipefail

ROOT=/home/seeed/rk_deploy
OUT=$ROOT/opt_dfl/out/ab
DURATION=${DURATION:-100}
WARMUP=${WARMUP:-15}
ROUNDS=${ROUNDS:-3}
MAXFPS=${MAXFPS:-0}
LABELS=$ROOT/models/labels_ebike.txt
ARMA=$ROOT/opt_dfl/models/armA_dfl_in_npu_int8.rknn
ARMB=$ROOT/opt_dfl/models/armB_fork9_dfl_on_cpu_int8.rknn
# 第三个臂: 生产模型本身(形态C, nc=2) —— 作为"今天的基线"参照
ARMP=$ROOT/models/ebike_int8_rk2332.rknn

mkdir -p "$OUT"
cd "$ROOT/opt_dfl"

npu_idle() {
    echo 123456 | sudo -S -p "" cat /sys/kernel/debug/rknpu/load 2>/dev/null \
        | grep -oE 'Core[0-9]+: *[0-9]+%' | grep -oE '[0-9]+' | tr '\n' ' '
}

wait_idle() {
    local i ok=0 loads v bad
    for i in $(seq 1 15); do
        loads="$(npu_idle)"
        bad=0
        for v in $loads; do [ "$v" -gt 8 ] && bad=1; done
        if [ "$bad" -eq 0 ] && [ -n "$loads" ]; then
            ok=$((ok+1)); echo "    [$i] 每核占用: $loads (干净 $ok/3)"
            [ "$ok" -ge 3 ] && return 0
        else
            ok=0; echo "    [$i] 每核占用: $loads (有污染, 等)"
        fi
        sleep 3
    done
    return 1
}

for r in $(seq 1 "$ROUNDS"); do
    for arm in prod armA armB; do
        case "$arm" in
            prod) M=$ARMP ;;
            armA) M=$ARMA ;;
            armB) M=$ARMB ;;
        esac
        echo "=== 轮次 $r / $arm  (max-fps=$MAXFPS, ${DURATION}s) ==="
        sleep 10
        if ! wait_idle; then
            echo "    [!] 板子不干净, 本轮作废, 继续下一轮"
            continue
        fi
        python3 scripts/ab_harness.py \
            --bin "$ROOT/rk3588_zero_copy/build/rk_gate" \
            --model "$M" --labels "$LABELS" \
            --label "${arm}_r${r}_f${MAXFPS}" \
            --duration "$DURATION" --warmup "$WARMUP" --max-fps "$MAXFPS" \
            --out "$OUT/${arm}_r${r}_f${MAXFPS}.json" 2>&1 \
            | grep -vE "RuntimeWarning|self.stderr|onnxruntime"
    done
done
echo "=== 完成, 结果在 $OUT ==="
