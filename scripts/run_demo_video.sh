#!/usr/bin/env bash
# ============================================================================
# run_demo_video.sh —— 真实场景视频验收(硬解 + 检测 + 车牌 + MQTT)
#
# 用法:
#   ./scripts/run_demo_video.sh                          # 用默认素材跑全流程
#   ./scripts/run_demo_video.sh --src ./某视频.mp4       # 指定视频
#   ./scripts/run_demo_video.sh --frames 300             # 只跑前 300 帧
#   ./scripts/run_demo_video.sh --no-speed               # 关测速(没标定时用)
#   ./scripts/run_demo_video.sh --ipm-points "x,y;x,y;x,y;x,y" --ipm-width-m 3.5
#   ./scripts/run_demo_video.sh --mqtt-host 192.168.31.214
#
# 它做四件事:
#   1) 起一个 MQTT 订阅端, 把板端推上来的报文【原文】抓成 jsonl
#      (日志里只有"报文发送成功", 看不到报文里的真实 plate_no/speed, 必须抓包)
#   2) 跑板端主程序(生产严格配置)
#   3) 跑完调 demo_video_analyze.py 生成 DEMO_VIDEO_REPORT.md
#   4) 打印关键结论
#
# ★ 两个容易踩的坑(脚本里已经处理掉了):
#   - --log-file 的路径: rk3588_zero_copy/logs/ 这个目录【不存在】,
#     fopen 失败会一条日志都不留。统一写 /home/seeed/rk_deploy/logs/。
#   - 主程序启动后会接管 stderr 写进 --log-file, 但接管之前那几行
#     (比如 "[解码] 后端 = Rockchip MPP 硬解")已经打到 stderr 了。
#     所以这里用 2>> 追加进同一个日志, 一份日志里既有启动信息也有运行期信息。
# ============================================================================
set -u

REPO=/home/seeed/rk_deploy
PROJ=$REPO/rk3588_zero_copy
BIN=$PROJ/build/rk_zero_copy
LOG=$REPO/logs/demo_video_run.log
CAP=$REPO/logs/demo_video_mqtt.jsonl
REPORT=$REPO/DEMO_VIDEO_REPORT.md

SRC=""
FRAMES=0
NO_SPEED=0
IPM_POINTS=""
IPM_WIDTH=""
MQTT_HOST="${MQTT_HOST:-}"
MQTT_PORT="${MQTT_PORT:-1883}"
STATS_INTERVAL="${STATS_INTERVAL:-1}"     # 1 = 逐帧统计(默认给全量数据)
SN="RK3588-DEMO-01"
CONF="0.15"
DUMP_FRAMES=0
DUMP_DIR=""

while [ $# -gt 0 ]; do
    case "$1" in
        --src)          SRC="$2"; shift 2 ;;
        --frames)       FRAMES="$2"; shift 2 ;;
        --no-speed)     NO_SPEED=1; shift ;;
        --ipm-points)   IPM_POINTS="$2"; shift 2 ;;
        --ipm-width-m)  IPM_WIDTH="$2"; shift 2 ;;
        --mqtt-host)    MQTT_HOST="$2"; shift 2 ;;
        --stats-interval) STATS_INTERVAL="$2"; shift 2 ;;
        --sn)           SN="$2"; shift 2 ;;
        --conf)         CONF="$2"; shift 2 ;;
        --dump-frames)  DUMP_FRAMES="$2"; shift 2 ;;
        --dump-dir)     DUMP_DIR="$2"; shift 2 ;;
        -h|--help)      sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "未知参数: $1"; exit 2 ;;
    esac
done

# ---- 默认素材 ----
if [ -z "$SRC" ]; then
    for cand in "$PROJ/电动车素材.mp4" "$PROJ/ebike_street.mp4" "$PROJ/DJI_20261102155401_0126_D.MP4"; do
        [ -f "$cand" ] && { SRC="$cand"; break; }
    done
fi
[ -n "$SRC" ] && [ -f "$SRC" ] || { echo "❌ 找不到视频文件(用 --src 指定)"; exit 1; }
[ -x "$BIN" ] || { echo "❌ 找不到 $BIN"; exit 1; }

# ---- MQTT broker 自动探测 ----
probe() { timeout 1 bash -c "cat < /dev/null > /dev/tcp/$1/$MQTT_PORT" 2>/dev/null; }
if [ -z "$MQTT_HOST" ]; then
    for c in 192.168.31.30 192.168.31.214; do
        if probe "$c"; then MQTT_HOST="$c"; break; fi
    done
fi
[ -n "$MQTT_HOST" ] || { echo "❌ 找不到可用 MQTT broker(--mqtt-host 指定)"; exit 1; }

export LD_LIBRARY_PATH=$PROJ/third_party/lib:$PROJ/lib:/usr/lib:${LD_LIBRARY_PATH:-}
mkdir -p "$REPO/logs"
rm -f "$LOG" "$CAP"

echo "================================================================"
echo " 真实场景视频验收"
echo "================================================================"
echo " 视频    : $SRC"
echo " MQTT    : $MQTT_HOST:$MQTT_PORT"
echo " 日志    : $LOG"
echo " 抓包    : $CAP"
echo " 报告    : $REPORT"
echo " 统计间隔: 每 $STATS_INTERVAL 帧一行 [frame N]"
echo " 测速    : $([ "$NO_SPEED" = 1 ] && echo '关闭(--no-speed)' || echo "开启 ${IPM_POINTS:-默认}")"
echo "================================================================"

# ---- 1) 先起 MQTT 订阅端(抓真实报文) ----
timeout 86400 python3 "$PROJ/scripts/mqtt_sub.py" \
    --host "$MQTT_HOST" --port "$MQTT_PORT" \
    --topic 'device/#' --out "$CAP" --seconds 86400 --quiet \
    >/dev/null 2>&1 &
SUB_PID=$!
sleep 2

# ---- 2) 跑主程序 ----
ARGS=(
  "$SRC"
  ./model/ebike_int8_rk2332.rknn
  --decoder mpp
  --labels ./model/labels_ebike.txt
  --lprnet ./model/lprnet_finetuned_int8.rknn
  --plate-cls 1
  --track-cls 1
  --conf "$CONF" --nms 0.45
  --mqtt-host "$MQTT_HOST" --mqtt-port "$MQTT_PORT" --mqtt-qos 1
  --sn "$SN"
  --no-show
  --sink-stdout
  --mem-trim-frames 500
  --stats-interval "$STATS_INTERVAL"
  --log-file "$LOG"
)
[ "$FRAMES" -gt 0 ] 2>/dev/null && ARGS+=(--frames "$FRAMES")
[ "$DUMP_FRAMES" -gt 0 ] 2>/dev/null && ARGS+=(--dump-frames "$DUMP_FRAMES")
[ -n "$DUMP_DIR" ] && ARGS+=(--dump-dir "$DUMP_DIR")
[ "$NO_SPEED" = 1 ] && ARGS+=(--no-speed)
[ -n "$IPM_POINTS" ] && ARGS+=(--ipm-points "$IPM_POINTS")
[ -n "$IPM_WIDTH" ] && ARGS+=(--ipm-width-m "$IPM_WIDTH")

cd "$PROJ" || exit 1
START=$(date +%s)
"$BIN" "${ARGS[@]}" 2>>"$LOG"
RC=$?
END=$(date +%s)

kill "$SUB_PID" 2>/dev/null
wait "$SUB_PID" 2>/dev/null

echo
echo "主程序退出码 $RC, 用时 $((END - START)) 秒"
echo

# ---- 3) 分析并出报告 ----
python3 "$REPO/scripts/demo_video_analyze.py" "$LOG" "$CAP" "$REPORT"

echo
echo "================================================================"
echo " 关键结论"
echo "================================================================"
printf "  解码后端   : %s\n" "$(grep -ao '\[解码\] 后端 = .*' "$LOG" | head -1 || echo '未记录')"
printf "  硬解出帧   : %s 次\n" "$(grep -ac '硬解出帧' "$LOG")"
printf "  帧数       : %s\n" "$(grep -ao '^\[frame [0-9]*\]' "$LOG" | tail -1)"
printf "  检出目标   : 平均 %s 个\n" \
  "$(grep -ao 'objs=[0-9]*' "$LOG" | cut -d= -f2 | awk '{s+=$1;n++} END{if(n)printf "%.1f", s/n; else print "-"}')"
printf "  车牌记录   : %s 条\n" "$(grep -ac '^\[车牌\]' "$LOG")"
printf "  MQTT 发送  : %s 次\n" "$(grep -ao '\[MQTT\] 报文发送成功' "$LOG" | wc -l)"
printf "  抓到报文   : %s 条\n" "$(wc -l < "$CAP" 2>/dev/null || echo 0)"
echo
echo "  报告: $REPORT"
echo "================================================================"
