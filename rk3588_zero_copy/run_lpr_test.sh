#!/bin/bash
# ============================================================================
# run_lpr_test.sh —— 板端单图车牌识别测试
#
# 拿一张车牌照片, 直接送进 rknn 模型, 打印识别结果。
#
#   ./run_lpr_test.sh                        # 用默认模型 + 默认图片
#   ./run_lpr_test.sh test_plate.jpg         # 换图片
#   ./run_lpr_test.sh 照片.jpg --prefix 粤B  # 加省份前缀
#   ./run_lpr_test.sh 照片.jpg --model xxx.rknn --charset-file dict.txt
#
# ★ 换模型只要改参数, 不用改代码:
#   --model        rknn 模型路径(输入宽高/布局/量化参数都自动从模型里查)
#   --charset      字符集(默认 LPRNet 的 34 个数字字母)
#   --charset-file 从文件读字符集(PP-OCR 那种几千字的字典用这个)
#   --prefix       车牌前缀(LPRNet 只输出数字字母, 前端要带省份就加这个)
#   --resize       stretch(拉伸) / letterbox(留边) / center(居中裁)
#   --crop x,y,w,h 先裁一块再缩放
#   --feed         int8 / float / uint8(默认按模型属性自动选)
#   --json         输出 JSON, 方便脚本消费
#
# 其它:
#   --help          看全部参数
#   --save-preprocessed P   把预处理后的图存出来(肉眼核对有没有裁歪)
# ============================================================================
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

# 模型和图片都可以用环境变量覆盖
MODEL="${MODEL:-./model/lprnet_finetuned_int8.rknn}"
IMAGE="${IMAGE:-./test_plate.jpg}"
BIN="${BIN:-./build/lpr_test}"

# 第一个【不带横杠】的参数当成图片路径, 方便直接:
#   ./run_lpr_test.sh 我的车牌.jpg --prefix 粤B
if [ $# -gt 0 ]; then
  case "$1" in
    -*) : ;;
    *)  IMAGE="$1"; shift ;;
  esac
fi

if [ ! -x "$BIN" ]; then
  echo "找不到 $BIN, 先编译:  cd build && cmake .. && make lpr_test" >&2
  exit 1
fi
if [ ! -f "$MODEL" ]; then
  echo "找不到模型 $MODEL (可用 MODEL=xxx.rknn 覆盖)" >&2
  exit 1
fi

# 图片没给 / 给错了, 而且同目录有 test_plate.jpg 就自动用它
if [ ! -f "$IMAGE" ]; then
  if [ -f ./test_plate.jpg ] && [ "$IMAGE" = "./test_plate.jpg" ]; then
    echo "[提示] 找不到 $IMAGE, 先用 tests/gen_test_plate.py 合成一张:" >&2
    echo "       python3 tests/gen_test_plate.py -o test_plate.jpg -t B12345" >&2
    exit 1
  fi
  echo "找不到图片 $IMAGE" >&2
  exit 1
fi

# third_party/lib 里是 librknnrt.so
export LD_LIBRARY_PATH="${HERE}/third_party/lib:${HERE}/lib:/usr/lib:${LD_LIBRARY_PATH:-}"

exec "$BIN" --model "$MODEL" --image "$IMAGE" "$@"
