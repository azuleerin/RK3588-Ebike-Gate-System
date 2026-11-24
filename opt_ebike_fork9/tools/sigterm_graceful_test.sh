#!/usr/bin/env bash
# ============================================================================
# sigterm_graceful_test.sh —— P2-A2 优雅关闭实测(沙盒, 不碰生产 rk-gate)
#
# ★ 门槛: 收到 SIGTERM 后【<= 3 秒】完全退出, 退出码 0(不是 137/SIGKILL)
#
# 用法: bash sigterm_graceful_test.sh [标签]
# ============================================================================
set -uo pipefail

TAG=${1:-graceful}
REPO=/home/seeed/rk_deploy
PROJ=$REPO/rk3588_zero_copy
BIN=$PROJ/build/rk_gate
OUT=/tmp/p2a
mkdir -p "$OUT"
LOG=$OUT/sigterm_${TAG}.log

mapfile -t LINES < <(bash "$REPO/opt_ebike_fork9/tools/render_gate_args.sh" --lines)
BIN=${LINES[0]}
ARGS=("${LINES[@]:1}")

# 换成 4 路生产配置(与生产完全一致), duration 放开(由 SIGTERM 终止)
NEWARGS=()
i=0
while [ $i -lt ${#ARGS[@]} ]; do
    case "${ARGS[$i]}" in
        --duration) NEWARGS+=("--duration" "0"); i=$((i + 2)); continue ;;
        *) NEWARGS+=("${ARGS[$i]}"); i=$((i + 1)) ;;
    esac
done

export LD_LIBRARY_PATH=$PROJ/third_party/lib:$PROJ/lib:/usr/lib
export RK_LOG_LEVEL="${RK_LOG_LEVEL:-warn}"

echo ">>> 启动 4 路 rk_gate (生产同参), 等待出帧后发 SIGTERM"
"$BIN" "${NEWARGS[@]}" > "$LOG" 2>&1 &
PID=$!
echo ">>> PID=$PID"

# 等它真的开始出帧(最多 25s)
for i in $(seq 1 50); do
    if grep -q "^\[ch1\]\[frame" "$LOG" 2>/dev/null; then
        echo ">>> 已出帧(第 $((i/2)) 秒), 统计行:"
        grep -E "^\[ch[0-9]\]\[frame" "$LOG" | tail -4 | sed 's/^/      /'
        break
    fi
    sleep 0.5
done

if ! grep -q "^\[ch1\]\[frame" "$LOG" 2>/dev/null; then
    echo "!!! 25 秒内没有出帧, 放弃"
    kill -9 "$PID" 2>/dev/null
    exit 1
fi

T0=$(date +%s.%N)
kill -TERM "$PID"
wait "$PID"
RC=$?
T1=$(date +%s.%N)
ELAPSED=$(awk -v a="$T0" -v b="$T1" 'BEGIN{printf "%.2f", b-a}')

echo
echo "=== 优雅关闭实测 ==="
echo "  从发 SIGTERM 到进程完全退出: ${ELAPSED} 秒"
echo "  退出码: $RC  $([ "$RC" -eq 0 ] && echo '(干净退出 ✓)' || echo '(★ 不是 0)')"
if [ "$RC" -eq 137 ]; then echo "  ★ 137 = 被 SIGKILL 强杀(说明没优雅退干净)"; fi
if [ "$RC" -gt 128 ]; then echo "  ★ 128+N = 被信号 $((RC-128)) 杀死"; fi
echo
echo "=== 退出前的日志尾部 ==="
tail -n 12 "$LOG" | sed 's/^/  /'
echo
echo "=== 通道/NpuPool 回收痕迹 ==="
grep -iE "退出|回收|destroy|释放|release|join|NpuPool|worker" "$LOG" | tail -8 | sed 's/^/  /'

echo
if [ "$RC" -eq 0 ] && awk -v e="$ELAPSED" 'BEGIN{exit !(e<=3.0)}'; then
    echo "==> PASS: ${ELAPSED}s 退出, 退出码 0"
    exit 0
else
    echo "==> FAIL: ${ELAPSED}s 退出码 $RC (门槛: <=3s 且 退出码 0)"
    exit 1
fi
