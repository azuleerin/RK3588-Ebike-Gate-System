#!/usr/bin/env bash
# collect_crash.sh —— 硬挂死/崩溃后收集现场证据
#
# 规则要求: "确保即使 SSH 断连，重启后也能查到断连前最后几秒的日志"
# 本脚本把散落在各处的证据打包成一个 tar.gz:
#   1. /sys/fs/pstore/dmesg-*   ← ramoops，内核 oops 现场（跨重启保留，96KB）
#   2. dmesg 尾段                ← 当前启动的内核日志
#   3. logs/*.log 尾段           ← 应用日志（含最后几秒）
#   4. 系统状态快照              ← 内存/cgroup/fd 数/频率/温度
#
# 用法: sudo ./collect_crash.sh [输出目录]
#   例: sudo ./collect_crash.sh /home/seeed/rk_deploy/logs

set -uo pipefail

OUT_DIR="${1:-/home/seeed/rk_deploy/logs}"
STAMP="$(date +%Y%m%d_%H%M%S)"
WORK="$(mktemp -d /tmp/crashcollect.XXXXXX)"
TARBALL="${OUT_DIR}/crash_${STAMP}.tar.gz"

mkdir -p "$OUT_DIR"
echo "== 收集现场到 ${WORK} =="

# --- 1. ramoops（最重要：内核 oops 现场）---
if [ -d /sys/fs/pstore ]; then
    for f in /sys/fs/pstore/*; do
        [ -f "$f" ] || continue
        cp "$f" "${WORK}/pstore_$(basename "$f")" 2>/dev/null \
            && echo "  [+] pstore/$(basename "$f") ($(stat -c%s "$f" 2>/dev/null) bytes)"
    done
    if ! ls "${WORK}"/pstore_* >/dev/null 2>&1; then
        echo "  [-] pstore 为空（上次启动没有记录到内核 oops）"
    fi
else
    echo "  [-] /sys/fs/pstore 未挂载"
fi

# --- 2. dmesg 尾段 ---
dmesg -T 2>/dev/null | tail -500 > "${WORK}/dmesg_tail.txt" && echo "  [+] dmesg_tail.txt ($(wc -l < "${WORK}/dmesg_tail.txt") 行)"
dmesg 2>/dev/null | tail -500 > "${WORK}/dmesg_tail_raw.txt"

# --- 3. 应用日志尾段 ---
if [ -d "${OUT_DIR}" ]; then
    for f in "${OUT_DIR}"/*.log; do
        [ -f "$f" ] || continue
        tail -300 "$f" > "${WORK}/log_$(basename "$f").tail.txt" 2>/dev/null \
            && echo "  [+] log_$(basename "$f").tail.txt"
    done
fi

# --- 4. 系统状态快照 ---
{
    echo "### 时间: $(date -Is)"
    echo "### uptime: $(uptime)"
    echo
    echo "### 频率"
    echo "NPU gov=$(cat /sys/class/devfreq/fdab0000.npu/governor 2>/dev/null) cur=$(cat /sys/class/devfreq/fdab0000.npu/cur_freq 2>/dev/null)"
    echo "DDR gov=$(cat /sys/class/devfreq/dmc/governor 2>/dev/null) cur=$(cat /sys/class/devfreq/dmc/cur_freq 2>/dev/null)"
    echo "CPU gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)"
    echo
    echo "### NPU 电源域"
    echo "runtime_status=$(cat /sys/devices/platform/fdab0000.npu/power/runtime_status 2>/dev/null)"
    echo "control=$(cat /sys/devices/platform/fdab0000.npu/power/control 2>/dev/null)"
    echo
    echo "### 温度"
    for z in /sys/class/thermal/thermal_zone*/; do
        echo "$(cat $z/type 2>/dev/null): $(cat $z/temp 2>/dev/null)"
    done
    echo
    echo "### 内存"
    free -h
    echo
    echo "### 内存 cgroup (v2)"
    cat /proc/self/cgroup
    echo "--- memory.current ---"
    cat /sys/fs/cgroup/user.slice/memory.current 2>/dev/null
    echo "--- memory.peak ---"
    cat /sys/fs/cgroup/user.slice/memory.peak 2>/dev/null
    echo "--- memory.events ---"
    cat /sys/fs/cgroup/user.slice/memory.events 2>/dev/null
    echo
    echo "### fd 总数"
    ls /proc/self/fd 2>/dev/null | wc -l
    echo "### dma-buf 节点占用"
    ls -l /dev/dma_heap/ 2>/dev/null
    echo
    echo "### 最近的内核 oops / abort"
    dmesg 2>/dev/null | grep -iE "oops|internal error|external abort|hard lockup|watchdog|BUG:" | tail -50
} > "${WORK}/sys_snapshot.txt" 2>&1
echo "  [+] sys_snapshot.txt"

# --- 5. 打包 ---
tar czf "$TARBALL" -C "$WORK" . 2>/dev/null
rm -rf "$WORK"
echo
echo "== 现场已打包: ${TARBALL} ($(du -h "$TARBALL" | cut -f1)) =="
echo "解包: tar xzf ${TARBALL}"
