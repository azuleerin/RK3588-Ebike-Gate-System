#!/bin/bash
# build_x86_stub.sh —— 生成 x86 版本的 stub 库, 用于在非 RK3588 机器上
# 完成链接并跑启动流程(CI / 本机验证用)。
#
# ⚠️ 生成的库名必须以 "lib" 开头: CMake 的链接项分类规则是
#    "basename 以 lib 开头的 .so 才当文件路径", 否则会被转成 -l<名字>。
#
# 用法:
#   ./tests/build_x86_stub.sh
#   cmake -B build -DRKNN_RT_LIB=$PWD/build_stub/librknnrt.so \
#                     -DRGA_LIB=$PWD/build_stub/librga.so
#   cmake --build build -j4
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "${HERE}/.." && pwd)
OUT="${ROOT}/build_stub"
mkdir -p "${OUT}"

g++ -shared -fPIC -o "${OUT}/librknnrt.so" "${HERE}/stubs/native/rknn_stub.cpp"
g++ -shared -fPIC -o "${OUT}/librga.so" "${HERE}/stubs/native/rga_stub.cpp" \
    -I"${ROOT}/third_party/include/rga"

echo "stub 库已生成:"
ls -la "${OUT}/librknnrt.so" "${OUT}/librga.so"
echo
echo "接着:"
echo "  cmake -B build -DCMAKE_BUILD_TYPE=Release \\"
echo "        -DRKNN_RT_LIB=${OUT}/librknnrt.so -DRGA_LIB=${OUT}/librga.so"
echo "  cmake --build build -j\$(nproc)"
