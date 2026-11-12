#!/usr/bin/env bash
# memtest_supervisor.sh —— 内存 Zero-Growth 验收长跑(等效 systemd 的 simple+Restart)
#
# 用法: nohup ./scripts/memtest_supervisor.sh > /tmp/memtest_sup.out 2>&1 &
#
# 为什么不用 systemd: 本机 sudo 需要密码, 装不了 unit。
#   等价 unit 已备好: /tmp/rk-memtest.service (sudo cp 到 /etc/systemd/system/ 即可)
#
# 采样输出: logs/memtest.csv  (time, rss_kb, heap_kb, fd, threads, frame, fps)
set -u
REPO=/home/seeed/rk_deploy
BIN=$REPO/rk3588_zero_copy/build/rk_zero_copy
LOG=$REPO/logs/memtest.log
CSV=$REPO/logs/memtest.csv
DURATION=${DURATION:-9000}          # 2.5 小时
export LD_LIBRARY_PATH=$REPO/rk3588_zero_copy/third_party/lib:$REPO/rk3588_zero_copy/lib:/usr/lib

echo "time,rss_kb,heap_kb,fd_count,threads,frame,fps" > "$CSV"
: > "$LOG"

END=$(( $(date +%s) + DURATION ))
while [ "$(date +%s)" -lt "$END" ]; do
    echo "=== supervisor: 启动一轮 $(date '+%F %T') ===" >> "$LOG"
    "$BIN" \
        rtsp://192.168.31.200:8557/h264 \
        "$REPO/rk3588_zero_copy/model/ebike_int8_rk2332.rknn" \
        --labels "$REPO/rk3588_zero_copy/model/labels_ebike.txt" \
        --no-show --frames 200000 --decoder ffmpeg --track-cls 1 --nms 0.45 \
        --lprnet "$REPO/models/lprnet_finetuned_int8.rknn" --plate-cls 1 \
        --roi-ratio 0.72 --roi-y 0.70 --roi-height 0.16 \
        --mem-trim-frames 500 \
        --log-file "$LOG" >> "$LOG" 2>&1
    echo "=== supervisor: 进程退出(rc=$?), 5 秒后重启 ===" >> "$LOG"
    sleep 5
done
echo "=== supervisor: 到时退出 ===" >> "$LOG"
