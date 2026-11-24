#!/usr/bin/env bash
# ============================================================================
# sigterm_blocking_test.sh —— P2-A2 优雅关闭【阻塞读】场景实测
#
# ★ 为什么本地文件测不出来:
#   本地 MP4 的 read() 立刻返回, 所以线程很少真的卡在 read() 里。
#   真正会卡住的是【网络源】(RTSP)或假死的源 —— 握手成功后服务器不说话,
#   客户端就永远阻塞在 av_read_frame() 的 read() 上。
#   这个脚本用仓库自带的 scripts/rtsp_replay_server.py --stall-only 起一个
#   "握手后一直不说话"的 RTSP 服务, 复现【卡在阻塞读】的真实场景。
#
# ★ 门槛: 收到 SIGTERM 后 <= 3 秒完全退出, 退出码 0
#
# 用法: bash sigterm_blocking_test.sh <标签> [二进制路径]
# ============================================================================
set -uo pipefail

TAG=${1:-blocking}
BIN=${2:-/home/seeed/rk_deploy/rk3588_zero_copy/build/rk_gate}
REPO=/home/seeed/rk_deploy
PROJ=$REPO/rk3588_zero_copy
OUT=/tmp/p2a
mkdir -p "$OUT"
LOG=$OUT/sigterm_${TAG}_rtsp.log
PORT=8556

echo ">>> 启动 RTSP 假死服务 (握手后一直不说话): rtsp://127.0.0.1:${PORT}/camstall"
python3 "$PROJ/scripts/rtsp_replay_server.py" --port "$PORT" --path /camstall --stall-only \
    > "$OUT/rtsp_stall.log" 2>&1 &
SRV=$!
sleep 2
if ! kill -0 "$SRV" 2>/dev/null; then
    echo "!!! RTSP 服务没起来:"; cat "$OUT/rtsp_stall.log"; exit 1
fi
echo ">>> RTSP 服务 PID=$SRV"

export LD_LIBRARY_PATH=$PROJ/third_party/lib:$PROJ/lib:/usr/lib
export RK_LOG_LEVEL="${RK_LOG_LEVEL:-warn}"

"$BIN" \
    --model "$REPO/models/ebike_real_fork9_int8_rk2332.rknn" \
    --labels "$REPO/models/labels_ebike.txt" \
    --ppocr "$REPO/models/ppocrv4_rec_fp16.rknn" \
    --ppocr-dict "$REPO/models/ppocr_keys_v1.txt" \
    --det-core 0,1,2 --ocr-core 0,1,2 --det-workers 3 --ocr-workers 3 --det-slots 2 \
    --conf 0.15 --nms 0.45 --max-fps 20 --stats-interval 100 \
    --roi-ratio 0.30 --roi-y 0.38 --roi-height 0.22 --roi-shift 0.126 --roi-below 0 \
    --ipm-width-m 3.5 --sn RK3588-GATE-01 --device-name 0001 --ipaddr 192.168.8.77 \
    --duration 0 \
    --ch "ch1:rtsp://127.0.0.1:${PORT}/camstall:near" \
    > "$LOG" 2>&1 &
PID=$!
echo ">>> rk_gate PID=$PID"

# 等它进入"连上了但收不到数据"的状态(握手成功, 之后阻塞在 read)
sleep 8
if ! kill -0 "$PID" 2>/dev/null; then
    echo "!!! 进程已经自己退出了, 看日志:"; tail -n 20 "$LOG"; kill "$SRV" 2>/dev/null; exit 1
fi
echo ">>> 进程仍在运行(此刻应阻塞在 av_read_frame 的 read 上)"

T0=$(date +%s.%N)
kill -TERM "$PID"
# 最多等 40 秒(模拟 systemd 的 30s 超时 + 余量)
for i in $(seq 1 400); do
    kill -0 "$PID" 2>/dev/null || break
    sleep 0.1
done
if kill -0 "$PID" 2>/dev/null; then
    echo ">>> 40 秒还没退出 —— 强制 SIGKILL(等价于 systemd 的 TimeoutStopSec 到点)"
    kill -9 "$PID" 2>/dev/null
    wait "$PID" 2>/dev/null
    RC=137
else
    wait "$PID"
    RC=$?
fi
T1=$(date +%s.%N)
ELAPSED=$(awk -v a="$T0" -v b="$T1" 'BEGIN{printf "%.2f", b-a}')
kill "$SRV" 2>/dev/null

echo
echo "=== 阻塞读场景下的优雅关闭实测 [$TAG] ==="
echo "  从发 SIGTERM 到进程完全退出: ${ELAPSED} 秒"
echo "  退出码: $RC  $([ "$RC" -eq 0 ] && echo '(干净退出 ✓)' || echo '(★ 不干净)')"
[ "$RC" -eq 137 ] && echo "  ★ 137 = 被 SIGKILL 强杀"
echo
echo "=== 日志尾部 ==="
tail -n 8 "$LOG" | sed 's/^/  /'
echo
if [ "$RC" -eq 0 ] && awk -v e="$ELAPSED" 'BEGIN{exit !(e<=3.0)}'; then
    echo "==> PASS: ${ELAPSED}s 退出, 退出码 0"
    exit 0
else
    echo "==> FAIL: ${ELAPSED}s 退出码 $RC (门槛: <=3s 且 退出码 0)"
    exit 1
fi
