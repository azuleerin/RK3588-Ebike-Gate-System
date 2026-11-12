#!/usr/bin/env bash
# rss_ab_monitor.sh —— 双进程 RSS 同窗对比采样器
#
# 用途: 在同一台机器、同一路 RTSP 流、同一时刻, 跑两个 rk_zero_copy 实例
#       (一个带 malloc_trim 的新二进制, 一个不带的老二进制), 把两者的 RSS
#       采到同一个 CSV 里, 得到一条【同窗对比】曲线。
#
# 这比"先后跑两次"更硬: 两个进程共享同一个 NPU/内存带宽, 负载完全对等。
#
# 用法:
#   ./scripts/rss_ab_monitor.sh <PID_A> <PID_B> <CSV> [间隔秒]
#   ./scripts/rss_ab_monitor.sh <PID_A> <CSV> [间隔秒]     # 只跟一个
set -u

PID_A="${1:?用法: rss_ab_monitor.sh PID_A [PID_B] CSV [间隔]}"
PID_B=""
CSV=""
INTERVAL=30

if [ "$#" -ge 3 ] && [ "${3:0:1}" = "/" ]; then
    PID_B="${2}"
    CSV="${3}"
    INTERVAL="${4:-30}"
else
    CSV="${2}"
    INTERVAL="${3:-30}"
fi

rss_of() {
    awk '/^VmRSS:/{print $2}' "/proc/$1/status" 2>/dev/null
}
vsz_of() {
    awk '/^VmSize:/{print $2}' "/proc/$1/status" 2>/dev/null
}

echo "time,rss_a_kb,vsz_a_kb,rss_b_kb,vsz_b_kb" > "$CSV"
echo "采样: A=$PID_A B=${PID_B:-无} -> $CSV (每 ${INTERVAL}s)"

while true; do
    TS=$(date +%s)
    RA=$(rss_of "$PID_A"); VA=$(vsz_of "$PID_A")
    if [ -n "${PID_B}" ]; then
        RB=$(rss_of "$PID_B"); VB=$(vsz_of "$PID_B")
    else
        RB=""; VB=""
    fi
    echo "${TS},${RA:-0},${VA:-0},${RB:-0},${VB:-0}" >> "$CSV"
    sleep "$INTERVAL"
done
