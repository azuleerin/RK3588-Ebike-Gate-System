#!/usr/bin/env bash
# ============================================================================
# rtsp_fanout_relay.sh —— 真实相机的【1 拉 4 播】本地零转码中继(管理脚本)
#
# ★ 为什么需要它(2026-11-26 Phase 1 实测):
#   rk_gate 的 4 路通道【直连远端相机】时, 4 路里只有最先启动的那一路能持续
#   出帧, 其余 3 路的帧号永不推进 -> 看门狗 L2 判卡死 -> 每 ~90 秒重启一次
#   服务。逐项排除后确认: 相机协议层/libav/4xMPP/MP4 源都没问题, 只有
#   "FFmpeg RTSP 客户端直连远端相机"这条路有问题, 而那几个
#   rtsp_transport/stimeout/max_delay/nobuffer/direct 选项写死在 gate_main.cc
#   里, 本阶段 C++ 冻结不能改。
#
#   => 唯一不改 C++ 的解: 板端起一个本地中继, 只从相机拉【1 路】,
#      再在本地零转码地分发给 4 个通道。
#      实测: 4 路全通, 每路 deliv=20.0 满速, "no frame" 从 17~28 条降到 0。
#
# ★ 零转码: H.264 不解码不重编码, 码流 payload 一个字节都不改。
#   中继只在 RTP 层做两件事:
#     1) 在每个 IDR 前面补插 SPS/PPS(相机只在关键帧里带参数集,
#        客户端错过就永远解不出来);
#     2) 重写 seq/ts/ssrc。
#
# 用法:
#   bash scripts/rtsp_fanout_relay.sh start     # 起中继(后台)
#   bash scripts/rtsp_fanout_relay.sh status    # 看状态(不泄密)
#   bash scripts/rtsp_fanout_relay.sh stop      # 停中继
#   bash scripts/rtsp_fanout_relay.sh restart
#
# 产物:
#   PID 文件: /tmp/rtsp_fanout.pid
#   日志:     /tmp/rtsp_fanout.log
#   本地地址: rtsp://127.0.0.1:8555/live   (rk_gate 的 4 路都连这个)
#
# ★ 凭据: 相机的 RTSP 地址从【仓库外】的私密文件读
#   /home/seeed/.config/rk-gate/real_rtsp.url (chmod 600),
#   所以进程命令行里【没有】账号密码, 日志里也做了脱敏。
# ============================================================================
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
SERVER="$HERE/rtsp_fanout_server.py"

PORT="${FANOUT_PORT:-8555}"
PATH_URI="${FANOUT_PATH:-/live}"
BIND="${FANOUT_BIND:-127.0.0.1}"
SRC_FILE="${FANOUT_SOURCE_FILE:-/home/seeed/.config/rk-gate/real_rtsp.url}"

PID_FILE=/tmp/rtsp_fanout.pid
LOG_FILE=/tmp/rtsp_fanout.log

usage() {
    sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}

alive() {
    [ -f "$PID_FILE" ] || return 1
    local p; p="$(cat "$PID_FILE" 2>/dev/null)"
    [ -n "$p" ] || return 1
    kill -0 "$p" 2>/dev/null
}

case "${1:-}" in
start)
    if alive; then
        echo "[relay] 已经在跑 (PID $(cat "$PID_FILE"))"
        exit 0
    fi
    if [ ! -r "$SRC_FILE" ]; then
        echo "[relay] 读不到相机地址文件: $SRC_FILE" >&2
        echo "        里面应该是一行 rtsp://用户名:密码@相机IP:8557/ (权限 600)" >&2
        exit 1
    fi
    if [ ! -r "$SERVER" ]; then
        echo "[relay] 找不到中继服务端: $SERVER" >&2
        exit 1
    fi
    : > "$LOG_FILE"
    nohup python3 "$SERVER" --bind "$BIND" --port "$PORT" --path "$PATH_URI" \
        --source-file "$SRC_FILE" >> "$LOG_FILE" 2>&1 &
    echo $! > "$PID_FILE"
    sleep 2
    if ! alive; then
        echo "[relay] 启动失败, 日志尾部:" >&2
        tail -5 "$LOG_FILE" >&2
        rm -f "$PID_FILE"
        exit 1
    fi
    echo "[relay] 已启动 PID=$(cat "$PID_FILE")  本地地址: rtsp://$BIND:$PORT$PATH_URI"
    echo "[relay] 日志: $LOG_FILE"
    ;;

stop)
    if ! alive; then
        echo "[relay] 没在跑"
        rm -f "$PID_FILE"
        exit 0
    fi
    p="$(cat "$PID_FILE")"
    kill "$p" 2>/dev/null
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        alive || break
        sleep 0.3
    done
    if alive; then
        kill -9 "$p" 2>/dev/null
        sleep 0.5
    fi
    rm -f "$PID_FILE"
    echo "[relay] 已停止 (PID $p)"
    ;;

restart)
    "$0" stop
    sleep 1
    "$0" start
    ;;

status)
    if ! alive; then
        echo "[relay] 状态: 未运行"
        exit 1
    fi
    echo "[relay] 状态: 运行中  PID=$(cat "$PID_FILE")"
    echo "[relay] 本地地址: rtsp://$BIND:$PORT$PATH_URI"
    if command -v ss >/dev/null 2>&1; then
        ss -tn 2>/dev/null | grep -c ":$PORT " | xargs echo "[relay] 本地 $PORT 上的连接数:"
    fi
    echo "[relay] 日志尾部:"
    tail -5 "$LOG_FILE" 2>/dev/null | sed 's/^/    /'
    ;;

*)
    usage
    ;;
esac
