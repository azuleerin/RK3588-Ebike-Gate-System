#!/usr/bin/env bash
# night_soak_run.sh —— 夜间长跑的启动包装器(被 systemd 反复拉起)
#
# 职责:
#   1) 累加"重启次数"计数(用户要看的核心指标之一)
#   2) 每次启动都在日志里打一条醒目的分隔行, 顺便记下【上一次是怎么退的】
#      (退出码 >128 就是被信号杀的, 139=SIGSEGV/段错误, 137=SIGKILL/OOM)
#   3) 用【固定下来的二进制】跑, 不受别的会话重编影响
#
# 注意: 这里【不】用 exec —— 要先把退出码记下来再退出, systemd 才能拿到正确的
#       退出状态并决定重启。
set -u

REPO=/home/seeed/rk_deploy
BIN=$REPO/rk3588_zero_copy/build/rk_zero_copy_night
LOG=$REPO/logs/night_soak.log
CNT=$REPO/logs/night_soak_restarts.txt
LAST=$REPO/logs/night_soak_last_exit.txt
LOGDIR=$REPO/logs

mkdir -p "$LOGDIR"

# ---- 重启计数 ----
n=$(cat "$CNT" 2>/dev/null || echo 0)
n=$((n + 1))
echo "$n" > "$CNT"

prev=$(cat "$LAST" 2>/dev/null || echo "无(首次启动)")

{
  printf '\n'
  printf '################################################################\n'
  printf '# [night-soak] 第 %d 次启动   %s\n' "$n" "$(date '+%F %T')"
  printf '# 上一次退出: %s\n' "$prev"
  printf '# 二进制: %s (%s)\n' "$BIN" "$(stat -c %y "$BIN" 2>/dev/null)"
  printf '################################################################\n'
} >> "$LOG"

export LD_LIBRARY_PATH=$REPO/rk3588_zero_copy/third_party/lib:$REPO/rk3588_zero_copy/lib:/usr/lib

cd "$REPO/rk3588_zero_copy" || exit 90

# ★ systemd 停服务时会给【包装脚本】发 SIGTERM(它是主进程), 应用是它的子进程。
#   不装这个 trap 的话, 退出原因就记不下来(日志里只会显示"无(首次启动)")。
terminated=0
on_term() { terminated=1; }
trap on_term TERM INT

"$BIN" \
  rtsp://192.168.31.200:8557/h264 \
  model/ebike_int8_rk2332.rknn \
  --labels model/labels_ebike.txt \
  --no-show \
  --decoder mpp \
  --track-cls 1 --nms 0.45 \
  --lprnet /home/seeed/rk_deploy/models/lprnet_finetuned_int8.rknn \
  --plate-cls 1 \
  --roi-ratio 0.72 --roi-y 0.70 --roi-height 0.16 \
  --ipm-points "300,622;1500,643;1450,877;550,819" \
  --ipm-width-m 3.5 \
  --mqtt-host 192.168.31.30 --mqtt-port 1883 --mqtt-qos 1 \
  --sn RK3588-12CH-01 \
  --mqtt-topic 'device/RK3588-12CH-01/message/up/ivs_result' \
  --mem-trim-frames 500 \
  --log-file "$LOG"

rc=$?
now=$(date '+%F %T')
if [ "$terminated" = "1" ]; then
    desc="被 systemd 停止(SIGTERM, 人工/定时器操作, 非崩溃) / 退出码 $rc"
elif [ "$rc" -gt 128 ]; then
  sig=$((rc - 128))
  case "$sig" in
    11) desc="SIGSEGV(段错误)";;
    6)  desc="SIGABRT(abort)";;
    9)  desc="SIGKILL(被 OOM 或人工杀掉)";;
    15) desc="SIGTERM(正常停止)";;
    *)  desc="信号 $sig";;
  esac
  desc="$desc / 退出码 $rc"
else
  desc="退出码 $rc"
fi
echo "$desc ($now)" > "$LAST"
{
  printf '\n'
  printf '################################################################\n'
  printf '# [night-soak] 第 %d 次运行结束   %s\n' "$n" "$now"
  printf '# 退出: %s\n' "$desc"
  printf '################################################################\n'
} >> "$LOG"

exit "$rc"
