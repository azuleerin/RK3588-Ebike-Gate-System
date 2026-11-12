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
# 自检（不依赖外部机器，验证"内核 -> UDP -> 接收器"整条链路通）:
#   sudo ./setup_netconsole.sh --selftest
#
# ------------------------------------------------------------------
# 配置串格式（踩坑记录，2026-11-12 实测）:
#   netconsole=[src-port]@[src-ip]/[src-dev],[tgt-port]@<tgt-ip>/[<tgt-macaddr>]
#   例: netconsole=6666@192.168.31.77/end0,4444@192.168.31.214/14:ac:60:ad:8a:bb
#
#   踩过的坑:
#   1. modprobe 的参数值**不能**再带 "netconsole=" 前缀，否则内核解析到
#      "netconsole=4444" 而报 couldn't parse config at 'netconsole=4444'
#   2. **源端必须带本机设备名**（/end0），只写 "4444@<ip>" 一定失败
#   3. **目标端必须带 MAC**（/<mac>），只写 "4444@<ip>" 或 "4444@<ip>:port" 都失败
#   4. 端口写在 @ 左边，@ 右边是 IP，再斜杠后面是设备名或 MAC
# ------------------------------------------------------------------

set -uo pipefail

PORT=4444
SRC_PORT=6666
SELFTEST=0
TARGET_IP=""
TARGET_MAC=""
SRC_DEV=""
SRC_IP=""

if [ "${1:-}" = "--selftest" ]; then
    SELFTEST=1
else
    TARGET_IP="${1:-192.168.31.214}"
    PORT="${2:-4444}"
fi

# ---- 自动探测本机源 IP / 源设备 ----
SRC_IP="$(ip -4 route get "${TARGET_IP:-8.8.8.8}" 2>/dev/null | awk '/src/ {for(i=1;i<=NF;i++) if($i=="src") print $(i+1); exit}')"
SRC_IP="${SRC_IP:-$(ip -4 addr show end0 2>/dev/null | awk '/inet /{print $2}' | cut -d/ -f1)}"
SRC_DEV="end0"

if [ "$SELFTEST" = "1" ]; then
    TARGET_IP="127.0.0.1"
    TARGET_MAC="00:00:00:00:00:00"
    echo "== 自检模式: 目标 127.0.0.1:${PORT} =="
else
    TARGET_MAC="$(ip neigh show "$TARGET_IP" 2>/dev/null | awk '{print $5}' | head -1)"
    if [ -z "$TARGET_MAC" ]; then
        echo "[WARN] 邻居表里没有 ${TARGET_IP} 的 MAC（ping 一下让它 ARP 解析），"
        echo "       目标端 MAC 必填，否则内核解析失败。"
        exit 1
    fi
fi

CONFIG="${SRC_PORT}@${SRC_IP}/${SRC_DEV},${PORT}@${TARGET_IP}/${TARGET_MAC}"
echo "== 源: ${SRC_PORT}@${SRC_IP}/${SRC_DEV}   目标: ${PORT}@${TARGET_IP}/${TARGET_MAC} =="
echo "   modprobe netconsole netconsole=\"${CONFIG}\""

load_netconsole() {
    modprobe -r netconsole 2>/dev/null
    modprobe netconsole netconsole="$1" 2>/dev/null
    if lsmod | grep -q '^netconsole'; then
        echo "   ✅ netconsole 已加载"
        return 0
    fi
    echo "   ❌ 加载失败，最后一条内核日志:"
    dmesg | grep -iE "netconsole|netpoll" | tail -3 | sed 's/^/      /'
    return 1
}

load_netconsole "$CONFIG" || exit 2

# ---- 持久化（重启后自动生效）----
mkdir -p /etc/modules-load.d /etc/modprobe.d
echo "netconsole" > /etc/modules-load.d/netconsole.conf
cat > /etc/modprobe.d/netconsole.conf <<EOF
# 由 scripts/setup_netconsole.sh 生成
# 格式: [src-port]@[src-ip]/[src-dev],[tgt-port]@<tgt-ip>/[<tgt-macaddr>]
options netconsole netconsole=${CONFIG}
EOF
echo "   [+] 已写入 /etc/modules-load.d/netconsole.conf 与 /etc/modprobe.d/netconsole.conf"

echo
echo "== 当前状态 =="
lsmod | grep netconsole | sed 's/^/   /'
echo "   参数: $(cat /sys/module/netconsole/parameters/netconsole 2>/dev/null)"
echo
if [ "$SELFTEST" = "1" ]; then
    echo "== 自检完成 =="
    echo "  板端已把内核日志发到本机 ${TARGET_IP}:${PORT}。"
    echo "  真实部署时请把目标改成工作站的 IP+MAC。"
else
    echo "== 部署完成 =="
    echo "  1) 在工作站 ${TARGET_IP} 运行:  python3 scripts/netconsole_receiver.py ${PORT}"
    echo "  2) 验证: 在板端触发一条内核消息（如 dmesg -w 看输出），工作站应同步出现"
fi
