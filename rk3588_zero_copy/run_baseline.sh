#!/bin/bash
# ============================================================================
# run_baseline.sh —— 固化的「软解 + CPU 转换」基线配置
#
# 这是 2026-11-10 在 recomputer-rk3588-devkit 上实测跑通的配置:
#     fps=26.4   npu=12ms   objs 正常检出   零崩溃
#
# 为什么是这三个开关:
#   RK_FORCE_CPU_RGA=1  彻底绕开 RGA。板子上 librga 1.9.1 的 rga_init() 会自己
#                       段错误(打开 /dev/rga 之后, 打印版本号那一行之后崩),
#                       强制走 OpenCV 的 CPU 转换(NV12->RGB) 就不碰它。
#   --decoder ffmpeg    系统 ffmpeg 没有 h264_rkmpp, 只能软解。
#                       MPP 硬解目前会段错误(旧版 20190626 用户态 vs 新内核)。
#   --no-show           演示/长跑时不弹窗口。
#
# 用法:
#   ./run_baseline.sh                    # 基线(旧 head/helmet 模型)
#   ./run_baseline.sh --ebike            # 换双输出人/车模型
#   ./run_baseline.sh --frames 600       # 多跑几帧
#   ./run_baseline.sh --show             # 带显示窗口
#   FRAMES=600 ./run_baseline.sh --ebike
#
# 其他可用环境变量: RTSP / MODEL / LABELS / BIN
# ============================================================================
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

# 工程内的 librga.so / librknnrt.so: 二进制已用 $ORIGIN 相对 RUNPATH,
# 这里再兜一层, 保证从任何位置调用都能找到库。
export LD_LIBRARY_PATH="${HERE}/third_party/lib:${HERE}/lib:/usr/lib:${LD_LIBRARY_PATH:-}"

FRAMES="${FRAMES:-300}"
RTSP="${RTSP:-rtsp://192.168.31.200:8557/h264}"
BIN="${BIN:-./build/rk_zero_copy}"

# 置信度: 0.10 —— 人/车模型在监控画面里目标偏小, 0.25 会漏检(实测 objs=0)。
# 需要更保守时用 CONF=0.25 ./run_baseline.sh 覆盖。
CONF="${CONF:-0.10}"

MODEL="${MODEL:-./model/best.rknn}"
LABELS="${LABELS:-./model/labels_list.txt}"

USE_EBIKE=0
SHOW=0
EXTRA=()

while [ $# -gt 0 ]; do
    case "$1" in
        --ebike)  USE_EBIKE=1; shift ;;
        --frames) FRAMES="$2"; shift 2 ;;
        --conf)   CONF="$2"; shift 2 ;;
        --show)   SHOW=1; shift ;;
        *)        EXTRA+=("$1"); shift ;;
    esac
done

if [ "$USE_EBIKE" = "1" ]; then
    MODEL="${MODEL_EBIKE:-./model/ebike_int8_rk2332.rknn}"
    LABELS="${LABELS_EBIKE:-./model/labels_ebike.txt}"
    # 人/车模型: 只跟 ebike(否则地面取点落在骑手脚上, 速度会系统性偏高)
    # NMS 0.45: 与算法组参考实现 / Ultralytics 训练默认一致
    EXTRA+=(--track-cls 1 --nms "${NMS:-0.45}")
else
    # 老模型保持原来的 NMS 0.60
    EXTRA+=(--nms "${NMS:-0.60}")
fi
# 预处理用【拉伸缩放】(实测: 监控画面里的小目标, 拉伸比 letterbox 检出好得多;
#  见 src/main.cc 里 Options::letterbox 的注释)
EXTRA+=(--stretch)

if [ ! -x "$BIN" ]; then
    echo "找不到 $BIN, 先编译: mkdir -p build && cd build && cmake .. && make -j\$(nproc)" >&2
    exit 1
fi
if [ ! -f "$MODEL" ]; then
    echo "找不到模型 $MODEL" >&2
    exit 1
fi

export RK_FORCE_CPU_RGA=1

SHOW_ARG="--no-show"
[ "$SHOW" = "1" ] && SHOW_ARG=""

echo "=========================================================="
echo " 基线配置 (软解 + CPU 转换)"
echo "   模型     : $MODEL"
echo "   标签     : $LABELS"
echo "   流       : $RTSP"
echo "   帧数     : $FRAMES"
echo "   置信度   : $CONF"
echo "   RGA      : 已禁用 (RK_FORCE_CPU_RGA=1, 走 OpenCV CPU 转换)"
echo "   解码     : FFmpeg 软解"
echo "=========================================================="
echo ""

exec "$BIN" "$RTSP" "$MODEL" --labels "$LABELS" $SHOW_ARG \
     --frames "$FRAMES" --decoder ffmpeg --conf "$CONF" ${EXTRA[@]+"${EXTRA[@]}"}
