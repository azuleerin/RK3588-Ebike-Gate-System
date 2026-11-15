#!/bin/bash
# ============================================================================
# run_demo.sh —— 在板子上跑零拷贝推理 demo
#
#   ./run_demo.sh                                   # 默认参数跑起来(推荐配置)
#   ./run_demo.sh --no-show --frames 300            # 不显示窗口, 只看性能
#   ./run_demo.sh --ipm-points "300,622;1500,643;1450,877;550,819" \
#                 --ipm-width-m 7.0 --ipm-fov 84     # 打开测速
#
# 可用环境变量覆盖:
#   RTSP=rtsp://...  MODEL=./model/best.rknn  LABELS=./model/labels_list.txt
#   NPU_PIPE=1  关掉双实例(单实例, 老行为)
#   LPR_CORE=0  识别模型不绑核(默认绑 Core2, 避免和检测抢核)
#   PPOCR_TTL=2000  车牌结果缓存时长(ms)
#
# 换人/车模型(双输出形态C):
#   ★ 必须用 ebike_int8_rk2332.rknn —— 板端运行时是 toolkit 2.3.2, 编译器版本必须匹配;
#     算法组原始的 ebike_int8.rknn 是 2.5.0 编的, 板端加载会失败。
#   MODEL=./model/ebike_int8_rk2332.rknn LABELS=./model/labels_ebike.txt \
#     ./run_demo.sh --no-show --frames 300 --track-cls 1 --nms 0.45
#
# ★ 默认推荐配置(2026-11-15 实测定, 素材: 真实路口骑行视频 1280x720@30fps):
#     --npu-pipe 2   检测双实例, 分别跑在 Core0 / Core1
#     --lpr-core 4   车牌识别(PP-OCR)绑到 Core2 —— 三个核各干各的, 不再抢核
#     --ppocr-ttl 5000  车牌结果缓存 5 秒(默认 2 秒)
#   实测(3000 帧, 同一段素材):
#     基线(TTL 2s + 单实例)      : 交付 28.40 fps, 最忙 NPU 核 77.1%(峰 83%), 真识别 694 次
#     推荐配置(TTL 5s + 双实例)    : 交付 29.98 fps(打满源), 最忙核 64.2%(峰 70%), 真识别 382 次
#   也就是: 帧率跟上了源(原来在丢帧), 最忙核降 12.8 个百分点,
#   而【精度零损失】(只是同一条轨迹少重复识别几次)。
#   想关掉: NPU_PIPE=1 ./run_demo.sh
# ============================================================================
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

RTSP="${RTSP:-rtsp://192.168.31.200:8557/h264}"
MODEL="${MODEL:-./model/ebike_int8_rk2332.rknn}"
LABELS="${LABELS:-./model/labels_ebike.txt}"
BIN="${BIN:-./build/rk_zero_copy}"

# ---- 推荐配置(可用环境变量覆盖) ----
NPU_PIPE="${NPU_PIPE:-2}"        # 检测双实例(Core0 + Core1)
LPR_CORE="${LPR_CORE:-4}"        # 识别绑 Core2(掩码 4), 不跟检测抢核
PPOCR_TTL="${PPOCR_TTL:-5000}"   # 车牌结果缓存 5 秒

if [ ! -x "$BIN" ]; then
  echo "找不到可执行文件 $BIN, 先跑 ./board_setup.sh" >&2
  exit 1
fi
if [ ! -f "$MODEL" ]; then
  echo "找不到模型 $MODEL" >&2
  exit 1
fi

# third_party/lib 里是 librknnrt.so / librga.so
export LD_LIBRARY_PATH="${HERE}/third_party/lib:${HERE}/lib:/usr/lib:${LD_LIBRARY_PATH}"

echo "模型   : $MODEL"
echo "标签   : $LABELS"
echo "视频流 : $RTSP"
echo "推荐配置: --npu-pipe $NPU_PIPE --lpr-core $LPR_CORE --ppocr-ttl $PPOCR_TTL"
echo "参数   : $*"
echo "------------------------------------------------------------"

exec "$BIN" "$RTSP" "$MODEL" --labels "$LABELS" \
    --npu-pipe "$NPU_PIPE" --lpr-core "$LPR_CORE" --ppocr-ttl "$PPOCR_TTL" "$@"
