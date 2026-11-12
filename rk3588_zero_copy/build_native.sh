#!/bin/bash
# 板端原生编译 (不需要 cmake, 只要板子上有 g++/gcc)
#
# 在 RK3588 板子上执行:
#   cd /root/rk3588_zero_copy && ./build_native.sh
#
# 所有依赖路径都可以用环境变量覆盖, 例如:
#   RKNN_INC=/usr/include RKNN_LIB=/usr/lib/librknnrt.so \
#   RGA_INC=/usr/include  RGA_LIB=/usr/lib/librga.so \
#   ./build_native.sh
set -e

ROOT=$(cd "$(dirname "$0")" && pwd)
cd "$ROOT"

# ---- 自动探测依赖 ---------------------------------------------------------
find_file() {   # $1=文件名  其余=候选目录
  local f="$1"; shift
  for d in "$@"; do
    if [ -f "$d/$f" ]; then echo "$d/$f"; return 0; fi
  done
  return 1
}

# ★ 也要搜工程自带的 third_party/(工程目录里就有 librknnrt.so / librga.so,
#   不装到系统目录也能编译)
: ${RKNN_INC:=$(dirname "$(find_file rknn_api.h "$ROOT/third_party/include" /usr/include /usr/local/include /root/include 2>/dev/null || echo /usr/include)")}
: ${RKNN_LIB:=$(find_file librknnrt.so "$ROOT/third_party/lib" /usr/lib /usr/local/lib /root/lib 2>/dev/null || echo "")}
: ${RGA_INC:=$(dirname "$(find_file im2d.h "$ROOT/third_party/include/rga" /usr/include/rga /usr/include /usr/local/include 2>/dev/null || echo /usr/include)")}
: ${RGA_LIB:=$(find_file librga.so "$ROOT/third_party/lib" /usr/lib /usr/local/lib /root/lib 2>/dev/null || echo "")}
: ${MPP_INC:=$(dirname "$(find_file rk_mpp.h /usr/include/rk_mpp /usr/include /usr/local/include 2>/dev/null || echo /usr/include)")}
: ${MPP_LIB:=$(find_file librockchip_mpp.so /usr/lib /usr/local/lib 2>/dev/null || echo "")}
: ${FFMPEG_INC:=$(dirname "$(find_file avformat.h /usr/include/libavformat /usr/include /usr/local/include 2>/dev/null || echo /usr/include)")}

# OpenCV: pkg-config 优先, 否则手工
if pkg-config --exists opencv4 2>/dev/null; then
  CV_CFLAGS=$(pkg-config --cflags opencv4)
  CV_LIBS=$(pkg-config --libs opencv4)
elif pkg-config --exists opencv 2>/dev/null; then
  CV_CFLAGS=$(pkg-config --cflags opencv)
  CV_LIBS=$(pkg-config --libs opencv)
else
  CV_INC=""
  for d in /usr/include/opencv4 /usr/local/include/opencv4 /usr/include /usr/local/include; do
    if [ -f "$d/opencv2/core.hpp" ]; then CV_INC="$d"; break; fi
  done
  CV_CFLAGS="-I${CV_INC}"
  CV_LIBS="-lopencv_core -lopencv_imgproc -lopencv_highgui -lopencv_imgcodecs"
fi

echo "== 依赖探测结果 =="
echo "  RKNN_INC    = $RKNN_INC"
echo "  RKNN_LIB    = $RKNN_LIB"
echo "  RGA_INC     = $RGA_INC"
echo "  RGA_LIB     = $RGA_LIB"
echo "  MPP_INC     = $MPP_INC"
echo "  MPP_LIB     = $MPP_LIB"
echo "  FFMPEG_INC  = $FFMPEG_INC"
echo "  OpenCV      = $CV_CFLAGS $CV_LIBS"

for f in "$RKNN_LIB" "$RGA_LIB"; do
  if [ ! -f "$f" ]; then
    echo "!! 缺少 $f —— 请从 PC 上把 librknnrt.so / librga.so 一起 scp 到板子," >&2
    echo "   然后: RKNN_LIB=/root/lib/librknnrt.so RGA_LIB=/root/lib/librga.so ./build_native.sh" >&2
    exit 1
  fi
done

# ---- 编译 -----------------------------------------------------------------
MPP_DEF="-DRK_HAVE_MPP=1"
[ -n "$MPP_LIB" ] && [ -f "$MPP_LIB" ] || MPP_DEF="-DRK_HAVE_MPP=0"

# 独立 libiconv 才需要显式链接(glibc 自带时不需要)。
# 注意: 必须在这里赋值 —— 之前这行在 g++ 命令之后, 而且写成了 \$ICONV_FLAG,
# 在双引号 heredoc 里会变成字面量 "$ICONV_FLAG", 链接必然失败。
if ldconfig -p 2>/dev/null | grep -q libiconv; then
    ICONV_FLAG="-liconv"
else
    ICONV_FLAG=""
fi

mkdir -p build_native
g++ -O2 -std=c++17 -Wall -Wno-unused-variable \
    -o build_native/rk_zero_copy \
    src/main.cc src/dmabuf.cc src/ipm.cc src/led_serial.cc src/lprnet.cc \
    src/mpp_decoder.cc src/mqtt_pub.cc src/rga_convert.cc src/result_sink.cc \
    src/safe_probe.cc src/speed_track.cc src/yolo_post.cc \
    -Iinclude \
    -I"$RKNN_INC" -I"$RGA_INC" -I"$MPP_INC" -I"$FFMPEG_INC" \
    $CV_CFLAGS $MPP_DEF \
    "$RKNN_LIB" "$RGA_LIB" \
    ${MPP_LIB:+$MPP_LIB} \
    -lavformat -lavcodec -lavutil $ICONV_FLAG \
    $CV_LIBS \
    -ldl -lpthread

echo ""
echo "编译完成: $(pwd)/build_native/rk_zero_copy"
file build_native/rk_zero_copy
