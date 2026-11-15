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
    # 用 MATCH 指定的特征串挑进程(默认挑命令行里带 "--log-file <日志>" 的那个),
    # 避免板上同时跑着别的 rk_zero_copy 实例时采错进程。
    MATCH="${MATCH:---log-file}"
    PID=$(pgrep -x rk_zero_copy | while read -r p; do
              tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | grep -q -- "$MATCH" && echo "$p"
          done | head -1)
    if [ -z "$PID" ]; then
        sleep "$INTERVAL"
        continue
    fi
    RSS=$(awk '/VmRSS/{print $2}' "/proc/$PID/status" 2>/dev/null)
    FDS=$(ls "/proc/$PID/fd" 2>/dev/null | wc -l)
    THR=$(ls "/proc/$PID/task" 2>/dev/null | wc -l)
    # ★ 日志可能被 logrotate 切走(rotate 后当前文件里是后半段),
    #   所以要把当前文件 + 最近的 .1 一起看, 取【帧号最大】的那一行。
    #   注意不能直接 tail -1: .1 可能是上一轮跑剩的旧日志, 帧号比当前文件还大,
    #   直接拼接会被旧数据带偏(实测踩过: 采样一直显示 2220 帧不动)。
    LAST=$(cat "$LOG" "$LOG.1" 2>/dev/null | grep -E "^\[frame [0-9]+\]" | \
           awk '{gsub(/\]/,"",$2); f=$2+0; if (f>m) {m=f; l=$0}} END{print l}')
    HEAP=$(echo "$LAST" | sed -nE 's/.*heap=([0-9]+)KB.*/\1/p')
    # 兼容两种统计行格式: "[frame 300] fps=..." 和 "[frame 300 fps=..."
    FR=$(echo "$LAST" | sed -nE 's/^\[frame ([0-9]+)\]? .*/\1/p')
    FPS=$(echo "$LAST" | sed -nE 's/.*fps=([0-9.]+).*/\1/p')
    echo "$(date +%s),${RSS:-0},${HEAP:-0},${FDS},${THR},${FR:-0},${FPS:-0}" >> "$CSV"
    sleep "$INTERVAL"
done
