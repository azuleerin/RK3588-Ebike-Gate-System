#!/usr/bin/env bash
# ============================================================================
# smoke_plates_compare.sh —— 300 帧冒烟 + 【车牌识别对比】硬门槛
#
# ★ 为什么要对比而不是只看"零崩溃":
#   2026-11-23 实测: 新模型(fork9)的检测框尺寸口径与生产模型不同
#   (ROI 裁剪宽度中位 144 px → 64 px), 车牌裁剪是直接从检测框推出来的,
#   所以换模型会改变车牌识别结果 —— 必须逐项比, 不能只看进程没崩。
#
# ★ 口径:
#   · 两轮完全相同的参数(4 路 / --max-fps 20 / 同一批视频, 从头播), 只换 --model
#   · 每轮 15 s ≈ 300 帧/路
#   · 对比项: 退出码 / 总帧数 / 各路 objs / [车牌] 行数 / 识别文本 / 框尺寸 / 裁剪尺寸
#
# 用法: smoke_plates_compare.sh <旧模型> <新模型> [每轮秒数]
# ============================================================================
set -uo pipefail

REPO=/home/seeed/rk_deploy
PROJ=$REPO/rk3588_zero_copy
BIN=$PROJ/build/rk_gate
OUT=${OUT:-$REPO/opt_ebike_fork9/out/smoke}
DUR=${3:-15}
export LD_LIBRARY_PATH=$PROJ/third_party/lib:$PROJ/lib:/usr/lib

mkdir -p "$OUT"

run_one() {  # $1=model $2=tag
    local model="$1" tag="$2"
    echo ">>> [$tag] 模型=$model  时长=${DUR}s"
    "$BIN" \
        --model "$model" \
        --labels $REPO/models/labels_ebike.txt \
        --ppocr $REPO/models/ppocrv4_rec_fp16.rknn \
        --ppocr-dict $REPO/models/ppocr_keys_v1.txt \
        --det-core 0,1,2 --ocr-core 0,1,2 --det-workers 3 --ocr-workers 3 --det-slots 2 \
        --conf 0.15 --nms 0.45 --max-fps 20 --stats-interval 50 \
        --roi-ratio 0.164 --roi-y 1.0 --roi-height 0.40 --roi-shift 0.126 --roi-below 0 \
        --ipm-width-m 3.5 --sn RK3588-GATE-01 --device-name 0001 --ipaddr 192.168.31.77 \
        --duration "$DUR" \
        --ch "ch1:/home/seeed/testvideo3.mp4:near" \
        --ch "ch2:/home/seeed/testvideo2.mp4:high:200,415;1000,429;967,585;367,546" \
        --ch "ch3:/home/seeed/Test2_fixed.mp4:near" \
        --ch "ch4:/home/seeed/电动车素材.mp4:high:600,1244;3000,1286;2900,1754;1100,1638" \
        > "$OUT/$tag.log" 2>&1
    echo "    退出码=$?  日志 $OUT/$tag.log"
}

run_one "$1" old
sleep 5
run_one "$2" new

/home/seeed/rk_deploy/_bench/venv/bin/python -I "$REPO/opt_ebike_fork9/tools/smoke_compare.py" \
    "$OUT/old.log" "$OUT/new.log"
