#!/usr/bin/env bash
# soak_monitor.sh —— 挂机压力测试的监控采样器
#
# 每 30 秒采一次, 写 CSV:
#   时间, RSS(KB), 虚拟内存(KB), fd 数, 线程数, 守卫构造, 守卫析构, 存活守卫, 帧数, FPS
# 用途(Step 07.4 验收):
#   - 内存占用必须是一条直线(无泄漏)
#   - fd 数量必须恒定
#   - 守卫构造/析构必须 1:1
#   - 帧号必须持续推进(没有卡死)
set -u

LOG="${1:-/home/seeed/rk_deploy/logs/minimal_npu-only.log}"
PIPE_LOG="${2:-/home/seeed/rk_deploy/logs/pipeline.log}"
CSV="${3:-/home/seeed/rk_deploy/logs/soak.csv}"
INTERVAL="${4:-30}"

echo "time,rss_kb,vsz_kb,fd_count,threads,guard_built,guard_destroyed,guard_live,frame,fps" > "$CSV"

while true; do
    PID=$(pgrep -x rk_zero_copy | head -1)
    if [ -z "$PID" ]; then
        echo "$(date +%s),,,,,,,,,," >> "$CSV"
        sleep "$INTERVAL"
        continue
    fi
    RSS=$(awk '/VmRSS/{print $2}' "/proc/$PID/status" 2>/dev/null)
    VSZ=$(awk '/VmSize/{print $2}' "/proc/$PID/status" 2>/dev/null)
    FDS=$(ls "/proc/$PID/fd" 2>/dev/null | wc -l)
    THR=$(ls "/proc/$PID/task" 2>/dev/null | wc -l)
    # 从统计行里取守卫计数与帧号(最新一行)
    LAST=$(grep -E "^\[frame [0-9]+\]" "$PIPE_LOG" 2>/dev/null | tail -1)
    GB=$(echo "$LAST" | sed -nE 's/.*guard\[mpp 构([0-9]+)\/析([0-9]+)\/存([0-9]+)\].*/\1 \2 \3/p')
    FR=$(echo "$LAST" | sed -nE 's/^\[frame ([0-9]+)\].*/\1/p')
    FPS=$(echo "$LAST" | sed -nE 's/.*fps=([0-9.]+).*/\1/p')
    set -- $GB
    B=${1:-0}; D=${2:-0}; L=${3:-0}
    echo "$(date +%s),${RSS:-0},${VSZ:-0},${FDS},${THR},${B},${D},${L},${FR:-0},${FPS:-0}" >> "$CSV"
    sleep "$INTERVAL"
done
