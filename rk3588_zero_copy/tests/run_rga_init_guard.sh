#!/bin/bash
# ============================================================================
# run_rga_init_guard.sh —— 验证「RGA 初始化」的降级判断
#
# 覆盖两种链接方式:
#
#   A) 真 librga (third_party/lib/librga.so)
#      · missing  : RGA_DEVICE 指向不存在的节点 -> 必须报 "设备节点不存在",
#                   而且【根本不该调用 librga】(日志里不能出现 rga_api version)
#      · default  : 默认 /dev/rga -> 报什么取决于板子状态, 但一定要有
#                   "RGA 不可用，原因：xxx" 或者 "设备节点 ... 可以打开"
#
#   B) 崩溃版 librga stub (tests/stubs/native/rga_crash_stub.cpp)
#      它的 imcheck 会故意段错误, 模拟板子上 rga_init() 崩溃:
#      · missing  : 设备节点打不开 -> 【预检就该拦住】, 进程不能死,
#                   也不该看到 stub 的 "rga_api version" (说明压根没碰 librga)
#      · openable : 设备节点能打开 -> 才轮到 librga 初始化, 崩了要报
#                   "rga_init() 内部崩溃", 然后干净降级到 CPU
#
# 用法: ./tests/run_rga_init_guard.sh
# ============================================================================
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
cd "$ROOT"

BUILD="${TMPDIR:-/tmp}/rga_guard"
mkdir -p "$BUILD"

CV_CFLAGS="$(pkg-config --cflags opencv4 2>/dev/null || echo '')"
CV_LIBS="$(pkg-config --libs opencv4 2>/dev/null || echo '-lopencv_core -lopencv_imgproc')"

FAILED=0
run_case() {
    local bin="$1" mode="$2" expect="$3" label="$4"
    echo ""
    echo "===== $label (mode=$mode, 期望关键字: $expect) ====="
    if ! "$bin" "$mode" "$expect"; then
        echo "!!!! 失败: $label"
        FAILED=1
    fi
}

# ---- A) 真 librga ----
g++ -O2 -std=c++17 -Wall -Wextra -o "${BUILD}/guard_real" \
    tests/test_rga_init_guard.cpp src/rga_convert.cc src/safe_probe.cc \
    -Iinclude -Ithird_party/include/rga third_party/lib/librga.so \
    ${CV_CFLAGS} ${CV_LIBS} || { echo "编译失败(真 librga)"; exit 1; }

# ---- B) 崩溃版 stub ----
g++ -O2 -std=c++17 -Wall -Wextra -o "${BUILD}/guard_crash" \
    tests/test_rga_init_guard.cpp src/rga_convert.cc src/safe_probe.cc \
    tests/stubs/native/rga_crash_stub.cpp \
    -Iinclude -Ithird_party/include/rga \
    ${CV_CFLAGS} ${CV_LIBS} || { echo "编译失败(crash stub)"; exit 1; }

export LD_LIBRARY_PATH="${ROOT}/third_party/lib:${LD_LIBRARY_PATH:-}"

run_case "${BUILD}/guard_real"   missing   "设备节点不存在"  "真 librga: 设备节点不存在"
run_case "${BUILD}/guard_real"   noperm   "权限不足"        "真 librga: 设备节点权限不足(EACCES)"
run_case "${BUILD}/guard_real"   default  "RGA 不可用"      "真 librga: 默认 /dev/rga"
run_case "${BUILD}/guard_crash"  missing   "设备节点不存在"  "崩溃 stub: 预检拦住(不该碰 librga)"
run_case "${BUILD}/guard_crash"  noperm   "权限不足"        "崩溃 stub: 权限预检拦住"
run_case "${BUILD}/guard_crash"  openable  "rga_init() 内部崩溃" "崩溃 stub: init 内部崩溃"

echo ""
if [ "$FAILED" = "0" ]; then
    echo "==> 全部通过"
else
    echo "==> 有失败项"
fi
exit "$FAILED"
