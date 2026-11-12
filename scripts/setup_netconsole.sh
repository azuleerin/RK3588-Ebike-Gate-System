#!/usr/bin/env bash
# setup_netconsole.sh —— 在 RK3588 板端配置 netconsole，把内核日志实时发到局域网工作站
#
# 用途: 硬挂死时 SSH 已经断了，但网络往往还通。netconsole 是唯一能拿到
#       "死机前最后几秒内核输出"的实时通道（ramoops 只能事后取，且只有 96KB）。
#
# 用法: sudo ./setup_netconsole.sh <接收端IP> [端口]
#   例: sudo ./setup_netconsole.sh 192.168.31.214 4444
#
# 接收端需先运行:  python3 scripts/netconsole_receiver.py 4444
#
# 注意:
#   - netconsole 走 UDP，内核日志量很大时可能丢包，这是"尽力而为"的通道，
#     不能替代 ramoops。两者同时开着才完整。
#   - 内核日志级别已在 /boot/armbianEnv.txt 里改为 verbosity=7（下次重启生效）。

set -euo pipefail

TARGET_IP="${1:-192.168.31.214}"
TARGET_PORT="${2:-4444}"

echo "== 目标接收端: ${TARGET_IP}:${TARGET_PORT} =="

# 1) 先确认目标可达（不通就别装了，免得白等）
if ! ping -c 1 -W 2 "${TARGET_IP}" >/dev/null 2>&1; then
    echo "[WARN] ping 不通 ${TARGET_IP}，netconsole 可能发不出去，但仍继续安装"
fi

# 2) 立即生效（本次开机）
if lsmod | grep -q '^netconsole'; then
    echo "netconsole 已加载，尝试重设目标"
else
    modprobe netconsole || { echo "[FATAL] modprobe netconsole 失败"; exit 1; }
fi
# 内核 netconsole 的目标通过 sysfs 设置
if [ -w /proc/sys/kernel/printk_devkmsg ]; then
    : # 某些内核需要写这个，忽略
fi

# 3) 持久化（重启后自动生效）
mkdir -p /etc/modules-load.d /etc/modprobe.d
echo "netconsole" > /etc/modules-load.d/netconsole.conf
cat > /etc/modprobe.d/netconsole.conf <<EOF
# 由 scripts/setup_netconsole.sh 生成
# 格式: netconsole=<本机端口>@<接收端IP>:<接收端端口>
options netconsole netconsole=4444@${TARGET_IP}:${TARGET_PORT}
EOF

# 4) 装载带参数的模块（重载一次让参数生效）
modprobe -r netconsole 2>/dev/null || true
modprobe netconsole netconsole=4444@${TARGET_IP}:${TARGET_PORT}

echo
echo "== 状态 =="
lsmod | grep netconsole
echo "--- 模块参数 ---"
cat /sys/module/netconsole/parameters/netconsole 2>/dev/null || echo "(无)"
echo
echo "下一步:"
echo "  1) 在工作站 ${TARGET_IP} 上运行:  python3 scripts/netconsole_receiver.py ${TARGET_PORT}"
echo "  2) 验证:  dmesg -w | head -20   应能看到日志流到工作站"
