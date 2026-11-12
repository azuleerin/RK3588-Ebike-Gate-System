#!/usr/bin/env bash
# memtest_sampler.sh —— 采样 RSS + 活堆(uordblks), 写 CSV
#
# 活堆从主程序日志的 [frame N] 行里取(heap=NNNKB), 它才是"有没有真泄漏"的判据;
# RSS 会被 malloc_trim 的锯齿干扰。
set -u
LOG=${1:-/home/seeed/rk_deploy/logs/memtest.log}
CSV=${2:-/home/seeed/rk_deploy/logs/memtest.csv}
INTERVAL=${3:-30}

echo "time,rss_kb,heap_kb,fd_count,threads,frame,fps" > "$CSV"
while true; do
    # ★ 必须按【命令行】挑进程: 板上可能有多个 rk_zero_copy(别的 systemd 实例),
    #   pgrep -x 取第一个会抓到别人的, 采样就全错了。
    PID=$(pgrep -x rk_zero_copy | while read -r p; do
              tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | grep -q "frames 200000" && echo "$p"
          done | head -1)
    if [ -z "$PID" ]; then
        sleep "$INTERVAL"
        continue
    fi
    RSS=$(awk '/VmRSS/{print $2}' "/proc/$PID/status" 2>/dev/null)
    FDS=$(ls "/proc/$PID/fd" 2>/dev/null | wc -l)
    THR=$(ls "/proc/$PID/task" 2>/dev/null | wc -l)
    LAST=$(grep -E "^\[frame [0-9]+\]" "$LOG" 2>/dev/null | tail -1)
    HEAP=$(echo "$LAST" | sed -nE 's/.*heap=([0-9]+)KB.*/\1/p')
    FR=$(echo "$LAST" | sed -nE 's/^\[frame ([0-9]+)\].*/\1/p')
    FPS=$(echo "$LAST" | sed -nE 's/.*fps=([0-9.]+).*/\1/p')
    echo "$(date +%s),${RSS:-0},${HEAP:-0},${FDS},${THR},${FR:-0},${FPS:-0}" >> "$CSV"
    sleep "$INTERVAL"
done
