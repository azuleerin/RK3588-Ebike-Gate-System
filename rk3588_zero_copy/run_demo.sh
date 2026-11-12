#!/bin/bash
# ============================================================================
# run_demo.sh —— 在板子上跑零拷贝推理 demo
#
#   ./run_demo.sh                                   # 默认参数跑起来
#   ./run_demo.sh --no-show --frames 300            # 不显示窗口, 只看性能
#   ./run_demo.sh --ipm-points "300,622;1500,643;1450,877;550,819" \
#                 --ipm-width-m 7.0 --ipm-fov 84     # 打开测速
#
# 可用环境变量覆盖:
#   RTSP=rtsp://...  MODEL=./model/best.rknn  LABELS=./model/labels_list.txt
#
# 换人/车模型(双输出形态C):
#   ★ 必须用 ebike_int8_rk2332.rknn —— 板端运行时是 toolkit 2.3.2, 编译器版本必须匹配;
#     算法组原始的 ebike_int8.rknn 是 2.5.0 编的, 板端加载会失败。
#   MODEL=./model/ebike_int8_rk2332.rknn LABELS=./model/labels_ebike.txt \
#     ./run_demo.sh --no-show --frames 300 --track-cls 1 --nms 0.45
# ============================================================================
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

RTSP="${RTSP:-rtsp://192.168.31.200:8557/h264}"
MODEL="${MODEL:-./model/ebike_int8_rk2332.rknn}"
LABELS="${LABELS:-./model/labels_ebike.txt}"
BIN="${BIN:-./build/rk_zero_copy}"

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
echo "参数   : $*"
echo "------------------------------------------------------------"

exec "$BIN" "$RTSP" "$MODEL" --labels "$LABELS" "$@"
