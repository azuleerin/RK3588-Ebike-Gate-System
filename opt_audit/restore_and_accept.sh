#!/usr/bin/env bash
# restore_and_accept.sh —— 测后恢复生产环境 + 验收
#
# 恢复顺序(与测前对称):
#   1. 还原 CPU / devfreq 的 min/max/governor(lock_max_freq.sh --restore)
#   2. 删掉 /run/rk-gate/paused 标记
#   3. 拉起测前就在跑的服务(rk-gate-watchdog / rk-pipeline-watchdog /
#      rk-night-soak-monitor), 并把测前就在自动重启循环里的三个服务
#      (rk-gate / rk-night-soak / rk-pipeline) 恢复成"enabled + 自动重启"
#   4. 验收: 用生产 CLI 直接跑一遍 rk_gate, 确认 4 路出帧 + npu 回到基线
set -uo pipefail

# ---- 需要 root: 以 root 身份运行, 或先执行 `sudo -v` 缓存凭据(凭据不落盘) ----
if [ "$(id -u)" -ne 0 ] && ! sudo -n true 2>/dev/null; then
    echo "需要 root 权限，请使用 sudo 运行或先执行 sudo -v" >&2
    exit 1
fi
PROJ=/home/seeed/rk_deploy/rk3588_zero_copy
SUDO="sudo"

echo "=============================================================="
echo " ① 还原频率 (lock_max_freq.sh --restore)"
echo "=============================================================="
cd "$PROJ"
# lock_max_freq.sh 从环境变量取凭据, 这里只做透传(明文不入库)
SUDO_PASSWORD="${SUDO_PASSWORD:-}" ./scripts/lock_max_freq.sh --restore 2>&1 | tail -20

echo ""
echo "=============================================================="
echo " ② 清除 pause 标记"
echo "=============================================================="
$SUDO rm -f /run/rk-gate/paused && echo "  /run/rk-gate/paused 已删除"
ls -la /run/rk-gate/ 2>&1 | head -5

echo ""
echo "=============================================================="
echo " ③ 恢复服务(测前状态: 三个服务在自动重启循环, 三个看门狗在跑)"
echo "=============================================================="
$SUDO bash -c '
systemctl unmask rk-gate rk-night-soak rk-pipeline 2>/dev/null
systemctl start  rk-gate-watchdog rk-pipeline-watchdog rk-night-soak-monitor 2>&1
systemctl start  rk-gate rk-night-soak rk-pipeline 2>&1
'
sleep 5
systemctl list-units --type=service --all 2>/dev/null | grep -E "rk-" | sed 's/^/  /'

echo ""
echo "=============================================================="
echo " ④ 验收: 生产 CLI 直接跑 rk_gate(60s), 确认 4 路出帧 + npu 基线"
echo "=============================================================="
export LD_LIBRARY_PATH="$PROJ/third_party/lib:$PROJ/lib:/usr/lib"
export RK_LOG_LEVEL=warn
timeout 70 "$PROJ/build/rk_gate" \
    --model /home/seeed/rk_deploy/models/ebike_int8_rk2332.rknn \
    --labels /home/seeed/rk_deploy/models/labels_ebike.txt \
    --ppocr /home/seeed/rk_deploy/models/ppocrv4_rec_fp16.rknn \
    --ppocr-dict /home/seeed/rk_deploy/models/ppocr_keys_v1.txt \
    --det-core 0,1,2 --ocr-core 0,1,2 --det-workers 3 --ocr-workers 3 --det-slots 2 \
    --conf 0.15 --nms 0.45 --max-fps 20 --stats-interval 100 \
    --roi-ratio 0.164 --roi-y 1.0 --roi-height 0.40 --roi-shift 0.126 --roi-below 0 \
    --ipm-width-m 3.5 --sn RK3588-GATE-01 --device-name 0001 --ipaddr 192.168.31.77 \
    --duration 60 \
    --ch "ch1:/home/seeed/testvideo3.mp4:near" \
    --ch "ch2:/home/seeed/testvideo2.mp4:high:200,415;1000,429;967,585;367,546" \
    --ch "ch3:/home/seeed/Test2_fixed.mp4:near" \
    --ch "ch4:/home/seeed/电动车素材.mp4:high:600,1244;3000,1286;2900,1754;1100,1638" \
    > /tmp/accept.log 2>&1
echo "  --- 每路最后一帧统计 ---"
grep -E "\[ch[0-9]\]\[frame" /tmp/accept.log | tail -4 | sed 's/^/  /'
echo "  --- 出帧路数 ---"
echo "  有帧号输出的通道数: $(grep -oE '^\[ch[0-9]\]\[frame' /tmp/accept.log | sort -u | wc -l)"
echo "  --- 每核 NPU 占用 ---"
grep -A 8 "每核 NPU 占用" /tmp/accept.log | sed 's/^/  /'
