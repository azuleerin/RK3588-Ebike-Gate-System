#!/usr/bin/env bash
# crash_archive.sh —— 开机自动抢收硬挂死现场
#
# 为什么需要:
#   硬挂死(整机失去响应)时, SSH 连不上, 只能断电重启。
#   断电重启后内核的环形缓冲区已经没了(ramoops 只在真 panic 时才有内容,
#   "CPU 死循环"这种硬挂死根本不走 panic 路径)。
#   所以最后一次 fsync 落盘的应用日志和内核日志就是唯一的证据 ——
#   必须在【下次开机时】第一时间把它们抢出来, 不能等我人工连上去。
#
# 触发条件: 存在标记文件 /run/rk-ignition.active
#   (引爆测试启动前创建, 正常退出会删掉; 异常重启说明板子挂过)
set -u

MARK=/run/rk-ignition.active
LOGS=/home/seeed/rk_deploy/logs

[ -f "$MARK" ] || exit 0

TS="$(date +%Y%m%d_%H%M%S)"
DEST="$LOGS/crash_$TS"
mkdir -p "$DEST"

echo "[archive] 检测到异常重启, 抢收现场到 $DEST"

# 1. ramoops (若有真 panic/oops)
if [ -d /sys/fs/pstore ]; then
    for f in /sys/fs/pstore/*; do
        [ -f "$f" ] || continue
        cp "$f" "$DEST/pstore_$(basename "$f")" 2>/dev/null \
            && echo "[archive]   + pstore/$(basename "$f") ($(stat -c%s "$f" 2>/dev/null) 字节)"
    done
fi

# 2. 内核日志(每条都 fsync 过的那份)
[ -f "$LOGS/kernel_raw.log" ] && cp "$LOGS/kernel_raw.log" "$DEST/" && echo "[archive]   + kernel_raw.log"
[ -f "$LOGS/kernel.log" ] && cp "$LOGS/kernel.log" "$DEST/" 2>/dev/null

# 3. 应用日志尾部
for f in "$LOGS"/pipeline.log "$LOGS"/minimal_*.log; do
    [ -f "$f" ] || continue
    tail -3000 "$f" > "$DEST/tail_$(basename "$f")" 2>/dev/null
    echo "[archive]   + tail_$(basename "$f")"
done

# 4. 当前内核日志与状态
dmesg -T > "$DEST/dmesg_now.txt" 2>/dev/null
{
    echo "### archive 时间: $(date -Is)"
    echo "### uptime: $(uptime)"
    echo "### 标记文件内容:"; cat "$MARK" 2>/dev/null
    echo "### 频率:"
    echo "NPU gov=$(cat /sys/class/devfreq/fdab0000.npu/governor 2>/dev/null) cur=$(cat /sys/class/devfreq/fdab0000.npu/cur_freq 2>/dev/null)"
    echo "DDR gov=$(cat /sys/class/devfreq/dmc/governor 2>/dev/null) cur=$(cat /sys/class/devfreq/dmc/cur_freq 2>/dev/null)"
    echo "### 内存:"; free -h
    echo "### 内核错误:"
    dmesg -T 2>/dev/null | grep -iE "oops|abort|fault|internal error|watchdog|rga|iommu" | tail -80
} > "$DEST/sys_state.txt" 2>&1

rm -f "$MARK"
echo "[archive] 完成: $DEST"
ls -la "$DEST"
