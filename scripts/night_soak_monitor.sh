#!/usr/bin/env bash
# night_soak_monitor.sh —— 夜间长跑的看护进程(每 30 秒采一次 + 红灯告警)
#
# 采集(写 logs/night_soak_metrics.csv):
#   time, frame, rss_kb, heap_kb, npu_temp_c, soc_temp_c, bigcpu_temp_c, little_temp_c,
#   cpu_freq_khz, npu_pct, restarts, pid
#
# 红灯规则(写 logs/night_soak_alerts.log, 同时打屏):
#   R1 进程重启次数 > 0            -> 发生过崩溃/死机重启
#   R2 活堆比第一条样本涨了 > 100MB -> 持续上涨(泄漏)
#   R3 帧号 3 分钟没动              -> 卡死(进程还在但不干活) -> 主动拉起
#
# 每 10 分钟重新生成一次 NIGHT_SOAK_REPORT.md(早上直接看报告就行)
set -u

REPO=/home/seeed/rk_deploy
LOG=$REPO/logs/night_soak.log
CSV=$REPO/logs/night_soak_metrics.csv
ALERT=$REPO/logs/night_soak_alerts.log
CNT=$REPO/logs/night_soak_restarts.txt
REPORT=$REPO/rk3588_zero_copy/docs/NIGHT_SOAK_REPORT.md
INTERVAL=30
STALL_SECONDS=180          # 帧号多久不动算卡死
HEAP_ALERT_KB=$((100 * 1024))
UNIT=rk-night-soak

mkdir -p "$REPO/logs"
# ★ 追加而不是覆盖: 监控进程自己重启(或者我手动重启它)也不能把历史采样弄丢
if [ ! -s "$CSV" ]; then
    echo "time,frame,rss_kb,heap_kb,npu_temp_c,soc_temp_c,bigcpu_temp_c,little_temp_c,cpu_freq_khz,npu_pct,restarts,pid" > "$CSV"
fi

# 温度分区
read_zones() {
    for z in /sys/class/thermal/thermal_zone*; do
        t=$(cat "$z/type" 2>/dev/null)
        case "$t" in
            npu-thermal)      NPU_ZONE=$z ;;
            soc-thermal)      SOC_ZONE=$z ;;
            bigcore0-thermal) BIG_ZONE=$z ;;
            littlecore-thermal) LITTLE_ZONE=$z ;;
        esac
    done
}
read_zones

temp_of() {  # $1=分区路径 -> 摄氏度(取不到就 -1)
    [ -n "${1:-}" ] && [ -r "$1/temp" ] && awk '{printf "%.1f", $1/1000}' "$1/temp" 2>/dev/null || echo -1
}

red() {  # 红灯告警
    local msg="$1"
    printf '[%s] 🔴 %s\n' "$(date '+%F %T')" "$msg" >> "$ALERT"
    printf '\n[%s] 🔴 [night-soak 告警] %s\n' "$(date '+%F %T')" "$msg" >> "$LOG"
    printf '[%s] 🔴 %s\n' "$(date '+%F %T')" "$msg" >&2
}

first_heap=""
last_frame=-1
last_advance=$(date +%s)
restart_seen=0
last_report=$(date +%s)

while true; do
    now=$(date +%s)
    ts=$(date '+%F %T')

    # ---- 找主进程 ----
    # ★ 要的是【应用本身】的 PID, 不是 systemd 的 MainPID(那是包装脚本 bash,
    #   它的 RSS 只有 3MB, 读出来全是错的)。用二进制路径精确匹配。
    # ★ 精确找应用进程: pgrep -x 匹配不了 >15 字符的进程名(内核只留 15 个字符),
    #   pgrep -f 又会把"命令行里含这个串的 shell"一起捞进来(读出来的 RSS 是 shell 的)。
    #   可靠办法: 挨个查 /proc/<pid>/exe 是不是本二进制。
    pid=""
    for p in $(pgrep -f rk_zero_copy_night 2>/dev/null); do
        exe=$(readlink "/proc/$p/exe" 2>/dev/null)
        case "$exe" in
            *rk_zero_copy_night) pid="$p"; break ;;
        esac
    done
    if [ -z "$pid" ]; then
        pid=$(systemctl show -p MainPID --value "$UNIT" 2>/dev/null)
        [ "$pid" = "0" ] && pid=""
    fi

    # ---- 帧号 / 活堆: 从主程序日志里取 ----
    line=$(grep -E "^\[frame [0-9]+\]" "$LOG" 2>/dev/null | tail -1)
    frame=$(echo "$line" | sed -nE 's/^\[frame ([0-9]+)\].*/\1/p')
    heap=$(echo "$line" | sed -nE 's/.*heap=([0-9]+)KB.*/\1/p')
    [ -z "$frame" ] && frame=-1
    [ -z "$heap" ] && heap=0

    # ---- RSS: 从 /proc 取 ----
    rss=0
    if [ -n "$pid" ] && [ -r "/proc/$pid/status" ]; then
        rss=$(awk '/VmRSS/{print $2}' "/proc/$pid/status" 2>/dev/null)
        [ -z "$rss" ] && rss=0
    fi

    npu_t=$(temp_of "$NPU_ZONE")
    soc_t=$(temp_of "$SOC_ZONE")
    big_t=$(temp_of "$BIG_ZONE")
    little_t=$(temp_of "$LITTLE_ZONE")

    # ---- CPU 频率(降频判据) + NPU 负载 ----
    freq=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null || echo 0)
    npu_pct=""
    if [ -r /sys/kernel/debug/rknpu/load ]; then
        npu_pct=$(cat /sys/kernel/debug/rknpu/load 2>/dev/null | sed -nE 's/.*Core0: *([0-9]+)%.*/\1/p')
    fi
    [ -z "$npu_pct" ] && npu_pct=-1

    restarts=$(cat "$CNT" 2>/dev/null || echo 0)

    echo "${now},${frame},${rss},${heap},${npu_t},${soc_t},${big_t},${little_t},${freq},${npu_pct},${restarts},${pid}" >> "$CSV"

    # ---- 红灯规则 ----
    if [ -z "$first_heap" ] && [ "$heap" -gt 0 ]; then first_heap=$heap; fi
    if [ "$heap" -gt 0 ] && [ -n "$first_heap" ]; then
        d=$((heap - first_heap))
        if [ "$d" -gt "$HEAP_ALERT_KB" ]; then
            red "活堆持续上涨: $first_heap KB -> $heap KB (+$((d/1024))MB, 超过 100MB 阈值)"
        fi
    fi
    # ★ 计数是"第几次启动": 1 = 正常首启, >1 才说明发生过崩溃重启
    if [ "$restarts" -gt 1 ] && [ "$restart_seen" -eq 0 ]; then
        restart_seen=1
        red "进程发生过重启: 当前是第 $restarts 次启动(详见 $LOG 里的分隔行)"
    fi

    # ---- 卡死看门狗: 帧号 3 分钟不动 -> 主动拉起 ----
    if [ "$frame" != "-1" ] && [ "$frame" -gt "$last_frame" ]; then
        last_frame=$frame
        last_advance=$now
    elif [ "$frame" -eq "-1" ] && [ $((now - last_advance)) -gt $((STALL_SECONDS * 2)) ]; then
        red "连续 ${STALL_SECONDS} 秒没有任何帧输出(连 [frame] 日志都没有) -> 主动重启"
        systemctl restart "$UNIT" 2>/dev/null
        last_advance=$now
    elif [ "$frame" != "-1" ] && [ $((now - last_advance)) -gt "$STALL_SECONDS" ]; then
        red "卡死: 帧号停在 $frame 超过 ${STALL_SECONDS} 秒 -> 主动重启"
        systemctl restart "$UNIT" 2>/dev/null
        last_advance=$now
    fi

    # ---- 每 10 分钟刷新一次报告 ----
    if [ $((now - last_report)) -ge 600 ]; then
        python3 "$REPO/scripts/night_soak_report.py" >/dev/null 2>&1
        last_report=$now
    fi

    sleep "$INTERVAL"
done
