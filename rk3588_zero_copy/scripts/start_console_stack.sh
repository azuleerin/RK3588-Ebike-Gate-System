#!/usr/bin/env bash
# ============================================================================
# start_console_stack.sh —— 一键起"网页控制台三件套"
#
# 三个进程:
#   1) 网页控制台   scripts/web_console.py     监听 :8080
#      订阅 device/<sn>/message/up/status 和 .../up/load, 下发 .../down/control
#   2) 板端主程序   build/rk_zero_copy         发 status + ivs_result
#   3) 负载采样器   scripts/hw_load_mqtt.py    发 .../up/load
#
# ★ 关键: 三个进程必须用【同一个 SN】, 否则控制台订阅不到数据, 页面一直空。
#   这个脚本把 SN 收在一个地方, 三个进程共用, 不会再对不上。
#
# ★ 关于 root:
#   NPU 三核占用和 RGA 占用在 /sys/kernel/debug/ 下, 权限是 drwx------ root,
#   非 root 读不到 -> 控制台这两根条会是空的(其余 VPU/CPU/DMC/GPU 是真值)。
#   所以想看完整负载, 就【用 sudo 跑这个脚本】, 负载采样器会继承 root:
#       sudo ./scripts/start_console_stack.sh
#   (口令是 see 用户的登录口令)
#
# 用法:
#   ./scripts/start_console_stack.sh                  # 起(默认)
#   ./scripts/start_console_stack.sh --stop           # 停掉本脚本起的三个进程
#   ./scripts/start_console_stack.sh --status         # 看状态
#   ./scripts/start_console_stack.sh --sn my-device   # 换设备号
#   ./scripts/start_console_stack.sh --source file --file /path/clip.mp4
#   ./scripts/start_console_stack.sh --broker 192.168.31.30
# ============================================================================
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(dirname "$HERE")"
REPO="$(dirname "$PROJ")"
RUN="$REPO/logs/console_stack"

BROKER="${BROKER:-192.168.31.214}"
BROKER_PORT="${BROKER_PORT:-1883}"
SN="${SN:-rk3588-01}"
HTTP_PORT="${HTTP_PORT:-8080}"
SOURCE="rtsp"
SRC_FILE=""
STATS_INTERVAL="${STATS_INTERVAL:-1000}"
BIN="$PROJ/build/rk_zero_copy"
MODEL="$PROJ/model/ebike_int8_rk2332.rknn"
LABELS="$PROJ/model/labels_ebike.txt"
POCR="${POCR:-$PROJ/model/ppocrv4_rec_fp16.rknn}"
RTSP="${RTSP:-rtsp://192.168.31.200:8557/h264}"
ACTION="start"

while [ $# -gt 0 ]; do
    case "$1" in
        --stop)     ACTION="stop"; shift ;;
        --status)   ACTION="status"; shift ;;
        --sn)       SN="$2"; shift 2 ;;
        --broker)   BROKER="$2"; shift 2 ;;
        --broker-port) BROKER_PORT="$2"; shift 2 ;;
        --http-port) HTTP_PORT="$2"; shift 2 ;;
        --source)   SOURCE="$2"; shift 2 ;;
        --file)     SRC_FILE="$2"; shift 2 ;;
        --rtsp)     RTSP="$2"; shift 2 ;;
        --pocr)     POCR="$2"; shift 2 ;;
        -h|--help)  sed -n '2,32p' "$0"; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

mkdir -p "$RUN"

# ---------------------------------------------------------------- PID 管理
pid_file() { echo "$RUN/$1.pid"; }

# 只按【本脚本记下的 PID】停, 不用 pkill 扫命令行 ——
# pkill -f 的模式串会匹配到"正在执行这条命令的 shell 自己", 把自己也杀掉
# (这个坑踩过好几次: 脚本被自己 pkill 掉)。
stop_one() {
    local name="$1" f
    f="$(pid_file "$name")"
    [ -f "$f" ] || return 0
    local p
    p="$(cat "$f" 2>/dev/null)"
    if [ -n "$p" ] && kill -0 "$p" 2>/dev/null; then
        kill "$p" 2>/dev/null
        sleep 1
        kill -0 "$p" 2>/dev/null && kill -9 "$p" 2>/dev/null
        echo "  已停 $name (PID $p)"
    fi
    rm -f "$f"
}

stop_all() {
    echo "停掉本脚本起的进程:"
    stop_one app
    stop_one load
    stop_one console
}

if [ "$ACTION" = "stop" ]; then
    stop_all
    echo "完成。"
    exit 0
fi

if [ "$ACTION" = "status" ]; then
    echo "=== 控制台三件套状态 ==="
    for n in console app load; do
        f="$(pid_file "$n")"
        if [ -f "$f" ] && kill -0 "$(cat "$f")" 2>/dev/null; then
            echo "  $n : 运行中 (PID $(cat "$f"))"
        else
            echo "  $n : 未运行"
        fi
    done
    if command -v curl >/dev/null 2>&1; then
        echo "  --- /api/state ---"
        curl -s --max-time 3 "http://127.0.0.1:$HTTP_PORT/api/state" | head -c 400; echo
    fi
    exit 0
fi

# ---------------------------------------------------------------- 前置检查
echo "================================================================"
echo " RK3588 控制台三件套"
echo "================================================================"
echo " broker : $BROKER:$BROKER_PORT"
echo " SN     : $SN"
echo " 网页   : http://$(hostname -I 2>/dev/null | awk '{print $1}'):$HTTP_PORT/"
echo "================================================================"

if ! timeout 3 bash -c "cat < /dev/null > /dev/tcp/$BROKER/$BROKER_PORT" 2>/dev/null; then
    echo "❌ broker $BROKER:$BROKER_PORT 连不上, 先确认它开着(比如 PC 上的 mosquitto)"
    exit 1
fi
[ -x "$BIN" ] || { echo "❌ 找不到 $BIN(先编译)"; exit 1; }
[ -f "$POCR" ] || { echo "❌ 找不到车牌模型 $POCR"; exit 1; }

if [ "$SOURCE" = "file" ]; then
    [ -n "$SRC_FILE" ] && [ -f "$SRC_FILE" ] || { echo "❌ --source file 需要 --file <路径>"; exit 1; }
    SRC="$SRC_FILE"
else
    SRC="$RTSP"
fi

# 先把上一轮停掉 —— 不停的话会有两个负载发布者往同一个 topic 发,
# 控制台的负载条会在"真值"和"-1 未知"之间来回跳。
stop_all

# ---------------------------------------------------------------- root 判定
SUDO=""
if [ "$(id -u)" = "0" ]; then
    SUDO=""                                   # 本身就是 root
elif sudo -n true 2>/dev/null; then
    SUDO="sudo -n"                            # 配了免密
fi
if [ -n "$SUDO" ] || [ "$(id -u)" = "0" ]; then
    LOAD_NOTE="负载采样器以 root 运行 -> NPU 三核 / RGA 内核真值可读"
else
    LOAD_NOTE="负载采样器非 root -> NPU 三核 / RGA 显示为空(VPU/CPU/DMC/GPU 仍是真值);"
    LOAD_NOTE="$LOAD_NOTE 要完整负载请用 sudo 重跑本脚本"
fi

# ---------------------------------------------------------------- 1) 网页控制台
setsid nohup python3 "$HERE/web_console.py" \
    --host "$BROKER" --port "$BROKER_PORT" --sn "$SN" --http-port "$HTTP_PORT" \
    > "$RUN/console.log" 2>&1 < /dev/null &
echo $! > "$(pid_file console)"
sleep 2

# ---------------------------------------------------------------- 2) 板端主程序
setsid nohup "$BIN" \
    "$SRC" \
    "$MODEL" \
    --labels "$LABELS" \
    --decoder mpp \
    --conf 0.15 --nms 0.45 \
    --no-show \
    --ppocr "$POCR" \
    --plate-cls 1 \
    --track-cls 1 \
    --sn "$SN" \
    --mqtt-host "$BROKER" --mqtt-port "$BROKER_PORT" --mqtt-qos 1 \
    --status-interval-ms "$STATS_INTERVAL" \
    --log-file "$RUN/app.log" \
    > "$RUN/app.stdout" 2>&1 < /dev/null &
echo $! > "$(pid_file app)"

# ---------------------------------------------------------------- 3) 负载采样器
# ★ 先看有没有人已经在发同一个 SN 的负载:
#   两个发布者会【交替】往同一个 topic 发, 控制台的负载条就在
#   "真值(rknpu)" 和 "假值(devfreq 恒 100)" 之间来回跳 —— 很难看也很误导。
#   所以已经有实例在跑就直接沿用它, 不再起第二个。
EXISTING_LOAD=""
for p in $(pgrep -f "hw_load_mqtt.py" 2>/dev/null); do
    if tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | grep -q -- "--sn $SN"; then
        EXISTING_LOAD="$p"; break
    fi
done

if [ -n "$EXISTING_LOAD" ]; then
    echo "  已有负载采样器在跑 (PID $EXISTING_LOAD), 沿用它, 不再起第二个"
    echo "$EXISTING_LOAD" > "$(pid_file load)"
else
setsid nohup $SUDO python3 "$HERE/hw_load_mqtt.py" \
    --host "$BROKER" --port "$BROKER_PORT" --sn "$SN" --interval 1.0 \
    > "$RUN/load.log" 2>&1 < /dev/null &
echo $! > "$(pid_file load)"
fi

# ---------------------------------------------------------------- 自检
sleep 6
echo
echo "--- 进程 ---"
for n in console app load; do
    f="$(pid_file "$n")"
    p="$(cat "$f" 2>/dev/null)"
    if [ -n "$p" ] && kill -0 "$p" 2>/dev/null; then
        echo "  ✅ $n (PID $p)"
    else
        echo "  ❌ $n 没起来, 看 $RUN/$n.log"
    fi
done
echo
echo "--- $LOAD_NOTE"
echo
if command -v curl >/dev/null 2>&1; then
    echo "--- 控制台 /api/state ---"
    curl -s --max-time 4 "http://127.0.0.1:$HTTP_PORT/api/state" | head -c 420
    echo
    echo
fi
echo "打开   : http://$(hostname -I 2>/dev/null | awk '{print $1}'):$HTTP_PORT/"
echo "看状态 : $0 --status"
echo "停止   : $0 --stop"
echo "日志   : $RUN/{console,app,load}.log"
