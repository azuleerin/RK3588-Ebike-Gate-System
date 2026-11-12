#!/bin/bash
# 交叉编译脚本 (RK3588 / aarch64)
#
# 用法:
#   TOOLCHAIN=/path/to/aarch64-rockchip-linux-gcc ./build.sh
#   RK_DEMO_ROOT=/path/to/yolov8-helmet-rk3588-multithread ./build.sh
set -e

ROOT_PWD=$(cd "$(dirname "$0")" && pwd)
: ${TOOLCHAIN:=/opt/rk-linux/toolchain/aarch64-rockchip-linux-gcc}
: ${RK_DEMO_ROOT:=${ROOT_PWD}/../yolov8-helmet-rk3588-multithread}

BUILD_DIR=${ROOT_PWD}/build/aarch64
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

cmake "${ROOT_PWD}" \
  -DCMAKE_TOOLCHAIN_FILE="${ROOT_PWD}/cmake/toolchain-rockchip.cmake" \
  -DTOOLCHAIN_ROOT="${TOOLCHAIN}" \
  -DRK_DEMO_ROOT="${RK_DEMO_ROOT}" \
  -DCMAKE_BUILD_TYPE=Release \
  "$@"

make -j"$(nproc 2>/dev/null || echo 4)"
make install

echo ""
echo "完成。可执行文件: ${BUILD_DIR}/rk_zero_copy"
echo "上传板子:"
echo "  scp ${BUILD_DIR}/rk_zero_copy root@192.168.31.77:/usr/local/bin/"
