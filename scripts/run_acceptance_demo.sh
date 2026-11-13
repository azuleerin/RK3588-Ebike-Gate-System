#!/usr/bin/env bash
# ============================================================================
# run_acceptance_demo.sh —— 一键自动验收演示(客户来了敲这一条命令就能看)
#
# 用法:
#   ./run_acceptance_demo.sh                 # 用 DJI 航拍 MP4 跑, 自动判定
#   ./run_acceptance_demo.sh --frames 600    # 跑更久一点
#   ./run_acceptance_demo.sh --source rtsp    # 改用现场相机(RTSP)
#   MQTT_HOST=192.168.31.30 ./run_acceptance_demo.sh   # 指定 MQTT broker
#
# 它做三件事:
#   1) 拉起板端主程序(生产严格配置: MPP硬解 + LPRNet车牌 + IPM测速 + MQTT上报)
#   2) 跑完自动分析日志, 逐条核对"系统是不是活的"
#   3) 全部符合预期就在终端打印绿色的 "✅ DEMO PASSED"
#
# ★ 为什么要写这个脚本:
#   交付给客户时, 现场不一定有真实业务场景视频(路口电动车)。
#   这个脚本把"演示"变成【可重复、可自证】的一键动作:
#   客户敲完命令, 看到绿色的 PASSED, 就知道整条链路(硬解/推理/测速/上报)是通的。
#
# 环境变量:
#   MQTT_HOST   MQTT broker 地址(不设就自动探测: 先试默认网关里的常用地址,
#               再扫一遍 192.168.31.0/24 找 1883 端口开着的)
#   MQTT_PORT   MQTT 端口(默认 1883)
#   DEMO_SOURCE 默认 demo(用 DJI MP4); 可选 rtsp(用现场相机)
# ============================================================================
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
cd "$REPO/rk3588_zero_copy" || exit 1

# ---------------------------------------------------------------- 参数
FRAMES="${FRAMES:-300}"
SOURCE="demo"                 # demo = 用 DJI MP4; rtsp = 用现场相机
MQTT_HOST="${MQTT_HOST:-}"
MQTT_PORT="${MQTT_PORT:-1883}"
LOG="$REPO/logs/demo_run.log"

while [ $# -gt 0 ]; do
    case "$1" in
        --frames) FRAMES="$2"; shift 2 ;;
        --source) SOURCE="$2"; shift 2 ;;
        --mqtt-host) MQTT_HOST="$2"; shift 2 ;;
        -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
        *) echo "未知参数: $1"; exit 2 ;;
    esac
done

# ---------------------------------------------------------------- 素材
# DJI 航拍 MP4: 工程里自带(H.265 1920x1080@59.94)。
# ★ 注意: 本地 MP4 是 AVCC 封装, 主程序会自动过 mp4toannexb 转成 Annex-B 再喂 MPP,
#   并且会按源帧率限速, 所以画面是"看着在跑", 不会瞬间跑完。
DEMO_MP4="$REPO/rk3588_zero_copy/DJI_20261102155401_0126_D.MP4"
RTSP_URL="rtsp://192.168.31.200:8557/h264"

if [ "$SOURCE" = "rtsp" ]; then
    SRC="$RTSP_URL"
    SRC_DESC="现场相机 RTSP"
elif [ -f "$DEMO_MP4" ]; then
    SRC="$DEMO_MP4"
    SRC_DESC="DJI 航拍 MP4(内置素材)"
else
    SRC="$RTSP_URL"
    SRC_DESC="现场相机 RTSP(内置 MP4 不存在, 自动回退)"
fi

# ---------------------------------------------------------------- MQTT broker 探测
# ★ 为什么自动探测:
#   broker 一般跑在客户的 PC 上, PC 重启后 DHCP 可能换 IP。
#   写死 IP 会让"客户敲命令看到 FAIL"变成常态, 那就不是演示了。
probe_mqtt() {
    local host="$1"
    timeout 1 bash -c "cat < /dev/null > /dev/tcp/$host/$MQTT_PORT" 2>/dev/null
}

if [ -z "$MQTT_HOST" ]; then
    for cand in 192.168.31.30 192.168.31.214; do
        if probe_mqtt "$cand"; then MQTT_HOST="$cand"; break; fi
    done
fi
if [ -z "$MQTT_HOST" ]; then
    # 扫一遍本网段(只探 1883, 每个 0.2s 超时)
    for i in $(seq 2 250); do
        cand="192.168.31.$i"
        if probe_mqtt "$cand"; then MQTT_HOST="$cand"; break; fi
    done
fi
if [ -z "$MQTT_HOST" ]; then
    echo "❌ 找不到可用的 MQTT broker(1883)。"
    echo "   请指定:  MQTT_HOST=192.168.31.30 ./run_acceptance_demo.sh"
    echo "   (broker 可以在客户 PC 上起一个:  mosquitto -p 1883)"
    exit 1
fi

# ---------------------------------------------------------------- 运行环境
export LD_LIBRARY_PATH="$REPO/rk3588_zero_copy/third_party/lib:$REPO/rk3588_zero_copy/lib:/usr/lib:${LD_LIBRARY_PATH:-}"
BIN="$REPO/rk3588_zero_copy/build/rk_zero_copy"
MODEL="model/ebike_int8_rk2332.rknn"
LABELS="model/labels_ebike.txt"
LPRNET="$REPO/models/lprnet_finetuned_int8.rknn"

[ -x "$BIN" ]   || { echo "❌ 找不到可执行文件 $BIN(先编译: cmake -B build && cmake --build build -j)"; exit 1; }
[ -f "$LPRNET" ] || { echo "❌ 找不到车牌模型 $LPRNET"; exit 1; }
mkdir -p "$REPO/logs"

echo "================================================================"
echo " RK3588 边缘 AI 系统 —— 一键验收演示"
echo "================================================================"
echo " 视频源    : $SRC_DESC"
echo " MQTT      : $MQTT_HOST:$MQTT_PORT"
echo " 帧数      : $FRAMES"
echo " 日志      : $LOG"
echo " 时间      : $(date '+%F %T')"
echo "================================================================"
echo

# 每次演示都从干净日志开始(方便事后核对)
rm -f "$LOG"

# ★ 关于 2>>:
#   主程序启动后会【接管 stderr】写进 --log-file, 但接管之前那几行
#   (比如 "[解码] 后端 = Rockchip MPP 硬解")已经打到 stderr 了。
#   所以这里用 2>>(追加模式)把 stderr 也追加进同一个日志文件,
#   这样一份日志里既有启动信息, 又有运行期信息, grep 一次就能查全。
#   (追加模式不会和主程序自己的写入打架: 两边都是 O_APPEND)
#   同时用 tail -f 把日志实时显示到终端, 让客户看得见画面在跑。
"$BIN" \
  "$SRC" \
  "$MODEL" \
  --labels "$LABELS" \
  --no-show \
  --decoder mpp \
  --track-cls 1 --nms 0.45 \
  --lprnet "$LPRNET" \
  --plate-cls 1 \
  --roi-ratio 0.72 --roi-y 0.70 --roi-height 0.16 \
  --ipm-points "300,622;1500,643;1450,877;550,819" \
  --ipm-width-m 3.5 \
  --mqtt-host "$MQTT_HOST" --mqtt-port "$MQTT_PORT" --mqtt-qos 1 \
  --sn RK3588-12CH-01 \
  --mqtt-topic 'device/RK3588-12CH-01/message/up/ivs_result' \
  --mem-trim-frames 500 \
  --frames "$FRAMES" \
  --log-file "$LOG" \
  2>>"$LOG" &
APP_PID=$!

# 实时把日志显示到终端(客户能看到 FPS/耗时在跳)
tail -n +1 -f "$LOG" &
TAIL_PID=$!

wait "$APP_PID"
RC=$?
kill "$TAIL_PID" 2>/dev/null
wait "$TAIL_PID" 2>/dev/null

echo
echo "================================================================"
echo " 自动判定 (分析 $LOG)"
echo "================================================================"

# ---------------------------------------------------------------- 判定
PASS=0
FAIL=0
declare -a RESULTS=()

check() {   # check <名称> <是否通过 0/1> <证据>
    local name="$1" ok="$2" ev="$3"
    if [ "$ok" = "0" ]; then
        printf '  ✅ %-28s %s\n' "$name" "$ev"
        PASS=$((PASS+1))
    else
        printf '  ❌ %-28s %s\n' "$name" "$ev"
        FAIL=$((FAIL+1))
    fi
}

# 1) 硬解后端
if grep -aq "\[解码\] 后端 = Rockchip MPP 硬解" "$LOG"; then
    check "MPP 硬解已启用" 0 "$(grep -ao "\[解码\] 后端 = Rockchip MPP 硬解[^)]*)" "$LOG" | head -1)"
else
    check "MPP 硬解已启用" 1 "日志里没有 [解码] 后端 = Rockchip MPP 硬解"
fi

# 2) 真的硬解出了帧
NFRAME_OUT="$(grep -ao "\[MPP\] 硬解出帧: [0-9]*x[0-9]*" "$LOG" | head -1)"
if [ -n "$NFRAME_OUT" ]; then
    check "MPP 硬解出帧" 0 "$NFRAME_OUT"
else
    check "MPP 硬解出帧" 1 "没有任何 [MPP] 硬解出帧 记录"
fi

# 3) MQTT 报文发送成功
N_MQTT="$(grep -ao "\[MQTT\] 报文发送成功" "$LOG" | wc -l)"
if [ "$N_MQTT" -gt 0 ]; then
    check "MQTT 报文发送成功" 0 "$N_MQTT 次 (broker $MQTT_HOST:$MQTT_PORT)"
else
    check "MQTT 报文发送成功" 1 "0 次 (broker $MQTT_HOST:$MQTT_PORT 不通?)"
fi

# 4) 推理真的在跑(帧号在推进)
LAST_FRAME="$(grep -ao "^\[frame [0-9]*\]" "$LOG" | tail -1 | grep -o "[0-9]*")"
if [ -n "$LAST_FRAME" ] && [ "$LAST_FRAME" -gt 0 ]; then
    check "推理流水线在推进" 0 "处理到第 $LAST_FRAME 帧"
else
    check "推理流水线在推进" 1 "帧号没有推进"
fi

# 5) 测速模块(IPM 标定已加载)
if grep -aq "\[测速\] IPM 模式" "$LOG"; then
    check "IPM 测速已标定" 0 "$(grep -ao "\[测速\] IPM 模式: .*" "$LOG" | head -1)"
else
    check "IPM 测速已标定" 1 "没有 [测速] IPM 模式 记录"
fi

# 6) 车牌识别模块已加载
if grep -aq "\[LPRNet\] 已启用" "$LOG"; then
    check "LPRNet 车牌模块" 0 "$(grep -ao "\[LPRNet\] 已启用.*" "$LOG" | head -1 | cut -c1-60)"
else
    check "LPRNet 车牌模块" 1 "没有 [LPRNet] 已启用 记录"
fi

# 7) 内存守卫 1:1(没有泄漏/没有帧悬垂)
# ★ 要看【守卫日志行】的最后一条, 不能看 [frame N] 统计行:
#   统计行是在那一帧还没释放时打的快照, 末帧必然是 构300/析299/存1,
#   看着像泄漏, 其实下一行守卫日志就会补上析构(构造=300 析构=300)。
G="$(grep -ao "\[guard\]\[mpp-frame\] 析构.*构造=[0-9]* 析构=[0-9]*" "$LOG" | tail -1)"
if [ -z "$G" ]; then
    G="$(grep -ao "guard\[mpp 构[0-9]*/析[0-9]*" "$LOG" | tail -1)"
fi
if [ -n "$G" ]; then
    BUILT=$(echo "$G" | sed -E 's/.*构造=([0-9]+).*/\1/')
    DESTROYED=$(echo "$G" | sed -E 's/.*析构=([0-9]+).*/\1/')
    if [ "$BUILT" = "$DESTROYED" ]; then
        check "帧守卫 构造/析构 1:1" 0 "构造=$BUILT 析构=$DESTROYED"
    else
        check "帧守卫 构造/析构 1:1" 1 "构造=$BUILT 析构=$DESTROYED (不等!)"
    fi
else
    check "帧守卫 构造/析构 1:1" 1 "日志里没有守卫计数"
fi

# 8) 没有崩溃
if [ "$RC" = "0" ] && ! grep -aqE "Segmentation fault|Assertion|Aborted|core dumped" "$LOG"; then
    check "进程正常退出" 0 "退出码 0, 无崩溃"
else
    check "进程正常退出" 1 "退出码 $RC$(grep -aoE 'Segmentation fault|Assertion' "$LOG" | head -1)"
fi

# 9) 内存没有失控(活堆 < 200MB)
HEAP_KB="$(grep -ao "heap=[0-9]*KB" "$LOG" | tail -1 | grep -o "[0-9]*")"
if [ -n "$HEAP_KB" ] && [ "$HEAP_KB" -lt 204800 ]; then
    check "活堆未失控" 0 "$((HEAP_KB / 1024)) MB"
else
    check "活堆未失控" 1 "${HEAP_KB:-未知} KB"
fi

echo
echo "  通过 $PASS 项, 失败 $FAIL 项"
echo "================================================================"
echo

if [ "$FAIL" -eq 0 ]; then
    printf '\033[32m'
    echo "✅ DEMO PASSED"
    printf '\033[0m'
    echo
    echo "   完整日志: $LOG"
    echo "   客户可以自己核对:  grep -c '硬解出帧' $LOG"
    exit 0
else
    printf '\033[31m'
    echo "❌ DEMO FAILED  ($FAIL 项不通过)"
    printf '\033[0m'
    echo
    echo "   日志: $LOG"
    echo "   排查建议:"
    echo "     1) 看 [解码] 后端 / [MPP] 硬解出帧 有没有出现 —— 没出现就是解码后端的问题"
    echo "     2) 看 [MQTT] 连接成功 有没有出现 —— 没出现就是 broker 不通"
    echo "     3) 看 [测速] / [LPRNet] 有没有出现 —— 没出现就是模型或参数问题"
    exit 1
fi
