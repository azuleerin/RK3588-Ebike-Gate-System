#!/usr/bin/env bash
# run_ab.sh —— rk_gate 改造前/改造后 板端 A/B(3 轮交错, 取中位数)
#
# ★ 口径:
#   · 吞吐 = 4 路帧计数器差分 (frames_end-frames_start)/(t_end-t_start)
#   · base / new 两组【交错】跑(base,new,base,new,...) —— 抵消温度漂移
#   · 每轮前查 /sys/kernel/debug/rknpu/load 确认 NPU 空闲
#   · 频率已由 scripts/lock_max_freq.sh 锁死(测完必须 --restore)
set -uo pipefail

BASE=/home/seeed/rk_deploy/opt_audit/main_wt/rk3588_zero_copy/build/rk_gate
NEW=/home/seeed/rk_deploy/rk3588_zero_copy/build/rk_gate
OUT=/home/seeed/rk_deploy/opt_audit/ab
DURATION=${DURATION:-100}
WARMUP=${WARMUP:-15}
ROUNDS=${ROUNDS:-3}
MAXFPS=${MAXFPS:-0}          # 0 = 不限帧(测真实吞吐上限)

mkdir -p "$OUT"
cd /home/seeed/rk_deploy/opt_audit

for r in $(seq 1 "$ROUNDS"); do
    for arm in base new; do
        BIN=$BASE; [ "$arm" = new ] && BIN=$NEW
        echo "=== 轮次 $r / $arm (max-fps=$MAXFPS, ${DURATION}s) ==="
        # 上一轮退场后等 NPU 负载落下来, 保证每轮起跑时 NPU 是空的
        sleep 12
        python3 ab_harness.py --bin "$BIN" --label "${arm}_r${r}_f${MAXFPS}" \
            --duration "$DURATION" --warmup "$WARMUP" --max-fps "$MAXFPS" \
            --out "$OUT/${arm}_r${r}_f${MAXFPS}.json" 2>&1 | grep -v RuntimeWarning | grep -v "self.stderr"
    done
done
echo "=== 全部轮次完成, 结果在 $OUT ==="
