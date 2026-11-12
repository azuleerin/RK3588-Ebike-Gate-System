#!/usr/bin/env bash
# pipeline_watchdog.sh —— 业务流水线的进度看门狗
#
# 判据: pipeline.log 的 mtime 在 STALL_SEC 秒内没有推进 => 程序卡死 => 重启。
#
# 为什么不用 systemd 的 WatchdogSec:
#   业务二进制不会 sd_notify, systemd 会无条件 SIGABRT 它(看着像崩溃)。
#   改用"日志进度"判断更真实: 程序在跑, stderr 就一直在写。
#
# 重要: 本看门狗只能救"进程卡死但系统还活着"的情况。
#   真正的硬挂死(整机失去响应)时, 本脚本自己也跑不了 ——
#   那时靠 ramoops + kern_log 两个通道取证。

set -u

LOG="${PIPELINE_LOG:-/home/seeed/rk_deploy/logs/pipeline.log}"
SERVICE="${PIPELINE_SERVICE:-rk-pipeline.service}"
STALL_SEC="${STALL_SEC:-60}"
CHECK_SEC="${CHECK_SEC:-20}"

echo "[watchdog] 监控 $LOG (停滞 ${STALL_SEC}s 就重启 $SERVICE)"

while true; do
    sleep "$CHECK_SEC"
    if [ ! -f "$LOG" ]; then
        continue
    fi
    now=$(date +%s)
    mtime=$(stat -c %Y "$LOG" 2>/dev/null || echo 0)
    age=$((now - mtime))
    if [ "$age" -gt "$STALL_SEC" ]; then
        echo "[watchdog] 日志已 $age 秒无进展, 认为卡死 -> 重启 $SERVICE"
        systemctl restart "$SERVICE" 2>&1 | head -3
        # 重启后给点时间让日志重新增长
        sleep 15
    fi
done
