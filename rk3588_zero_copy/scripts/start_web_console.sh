#!/usr/bin/env bash
# start_web_console.sh —— 一键启动网页控制台(PC 端)
#
# 用法:
#   ./scripts/start_web_console.sh                      # 用下面的默认值
#   ./scripts/start_web_console.sh --host 192.168.31.30 --sn rk3588-01 --port 8080
#
# 启动后浏览器打开:  http://<本机IP>:<http-port>/
#
# 零依赖: 只用 Python 标准库, 不需要 pip install。
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"

BROKER="${BROKER:-192.168.31.214}"
BROKER_PORT="${BROKER_PORT:-1883}"
SN="${SN:-rk3588-01}"
HTTP_PORT="${HTTP_PORT:-8080}"

while [ $# -gt 0 ]; do
    case "$1" in
        --host)  BROKER="$2"; shift 2 ;;
        --port)  HTTP_PORT="$2"; shift 2 ;;
        --sn)    SN="$2"; shift 2 ;;
        --broker-port) BROKER_PORT="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 1 ;;
    esac
done

# 顺手把本机 IP 找出来, 省得用户自己猜
IPS=$(hostname -I 2>/dev/null || ip -4 -o addr show 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | tr '\n' ' ')

echo "=============================================="
echo " RK3588 网页控制台"
echo "   broker : $BROKER:$BROKER_PORT"
echo "   设备 SN: $SN"
echo "   本机 IP : $IPS"
echo "   打开    : http://${IPS%% *}:$HTTP_PORT/"
echo "=============================================="

exec python3 "$HERE/web_console.py" \
    --host "$BROKER" --port "$BROKER_PORT" --sn "$SN" --http-port "$HTTP_PORT"
