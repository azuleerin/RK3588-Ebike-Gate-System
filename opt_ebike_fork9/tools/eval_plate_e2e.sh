#!/usr/bin/env bash
# ============================================================================
# eval_plate_e2e.sh —— 真实车牌端到端评测(生产二进制单路跑 DJI 真牌视频)
#
# ★ 为什么用生产二进制而不是离线脚本:
#   两行牌的切分(3 个上行切分点 + 1 下行)、候选择优(含汉字 > 字符多 > 分数高)、
#   ROI 的 pad/偶对齐/最小尺寸 —— 这些都在 gate_main.cc 里。离线复刻必然有偏差,
#   所以直接用生产二进制跑, 读它自己打印的 [车牌] 行(含识别文本与裁剪尺寸)。
#
# ★ 只测精度与裁剪尺寸, 不测帧率 => 不需要锁频、不需要停生产服务
#   (NPU 上与生产服务有争用, 但只影响速度, 不影响识别结果)
#
# ★ 日志: stderr 落到各自的文件, 不写 /var/log.hdd, 不污染生产日志与看门狗的帧号解析
#
# 用法: eval_plate_e2e.sh <标签> <模型路径> <ratio> <roi_y> <height> <shift> <below> [时长秒]
# ============================================================================
set -uo pipefail

TAG="$1"; MODEL="$2"; RATIO="$3"; ROIY="$4"; HEIGHT="$5"; SHIFT="$6"; BELOW="$7"; DUR="${8:-30}"

REPO=/home/seeed/rk_deploy
PROJ=$REPO/rk3588_zero_copy
BIN=$PROJ/build/rk_gate
VIDEO=$PROJ/DJI_20261102155401_0126_D.MP4
OUT=$REPO/opt_ebike_fork9/out/plate_e2e/$TAG
mkdir -p "$OUT"
export LD_LIBRARY_PATH=$PROJ/third_party/lib:$PROJ/lib:/usr/lib
export RK_LOG_LEVEL="${RK_LOG_LEVEL:-warn}"

"$BIN" \
  --model "$MODEL" \
  --labels $REPO/models/labels_ebike.txt \
  --ppocr $REPO/models/ppocrv4_rec_fp16.rknn \
  --ppocr-dict $REPO/models/ppocr_keys_v1.txt \
  --det-core 0,1,2 --ocr-core 0,1,2 --det-workers 3 --ocr-workers 3 --det-slots 2 \
  --conf 0.15 --nms 0.45 --max-fps 20 --stats-interval 100 \
  --roi-ratio "$RATIO" --roi-y "$ROIY" --roi-height "$HEIGHT" \
  --roi-shift "$SHIFT" --roi-below "$BELOW" \
  --ipm-width-m 3.5 --sn RK3588-GATE-01 --device-name 0001 --ipaddr 192.168.31.77 \
  --duration "$DUR" \
  --save-roi --save-roi-dir "$OUT/roi" \
  --ch "ch1:$VIDEO:near" \
  > "$OUT/stderr.log" 2>&1
rc=$?
echo "[$TAG] 退出码=$rc  日志=$OUT/stderr.log  ROI图=$(ls "$OUT/roi" 2>/dev/null | wc -l) 张"
grep -a "\[车牌\]" "$OUT/stderr.log" | tail -3
exit $rc
