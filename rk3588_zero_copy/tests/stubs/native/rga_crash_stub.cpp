// rga_crash_stub.cpp —— 故意在 imcheck 里段错误的 librga stub
//
// 用来复现板子上的真实故障: librga 的 rga_init() 打不开 /dev/rga 时,
// 打印完错误并没有干净返回, 而是继续解引用未初始化的句柄 -> SIGSEGV。
// 崩在【第一次触发 rga_init 的调用】上, 也就是 imcheck。
//
// 链接这个 stub 就能在 PC/容器里复现, 用来验证:
//   1) 不加保护时进程直接 core dump
//   2) 加了 SafeProbe 初始化探针后, 干净降级到 CPU 转换
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "im2d.h"

namespace {
[[noreturn]] void boom() {
    volatile int* p = nullptr;
    *p = 42;      // SIGSEGV
    __builtin_unreachable();
}
}  // namespace

extern "C" {
IM_C_API IM_STATUS imcheck_t(rga_buffer_t, rga_buffer_t, rga_buffer_t, im_rect, im_rect,
                             im_rect, int) {
    printf("rga_api version 1.9.1_[4]\n");   // 模拟 librga 初始化时打印的版本号
    fflush(stdout);
    boom();
}

IM_C_API const char* imStrError_t(IM_STATUS status) {
    (void)status;
    return "crash-stub";
}
}

IM_API IM_STATUS improcess(rga_buffer_t, rga_buffer_t, rga_buffer_t, im_rect, im_rect, im_rect,
                            int, int*, im_opt_t*, int) {
    boom();
}

IM_API IM_STATUS improcess(rga_buffer_t, rga_buffer_t, rga_buffer_t, im_rect, im_rect, im_rect,
                            int) {
    boom();
}

// ---------------------------------------------------------------------------
// importbuffer_fd / wrapbuffer_handle_t / releasebuffer_handle
//
// 零拷贝路径新增的三个符号。stub 里给个"永远失败"的实现即可:
// 这个 stub 的用途是验证"降级判断", 不需要真的做 RGA 转换。
// (importbuffer_fd 返回 0 -> rga_convert.cc 会自动退回老路 fd 方式)
// ---------------------------------------------------------------------------
IM_API rga_buffer_handle_t importbuffer_fd(int fd, int size) {
    (void)fd; (void)size;
    return 0;
}

IM_API rga_buffer_handle_t importbuffer_fd(int fd, int width, int height, int format) {
    (void)fd; (void)width; (void)height; (void)format;
    return 0;
}

IM_C_API rga_buffer_t wrapbuffer_handle_t(rga_buffer_handle_t handle, int width, int height,
                                           int wstride, int hstride, int format) {
    rga_buffer_t b;
    memset(&b, 0, sizeof(b));
    b.handle = handle;
    b.width = width;
    b.height = height;
    b.wstride = wstride;
    b.hstride = hstride;
    b.format = format;
    return b;
}

IM_EXPORT_API IM_STATUS releasebuffer_handle(rga_buffer_handle_t handle) {
    (void)handle;
    return IM_STATUS_SUCCESS;
}
