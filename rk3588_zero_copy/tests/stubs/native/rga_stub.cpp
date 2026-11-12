// x86 版本的 librga stub —— 只为在非 RK3588 机器上完成链接并跑启动流程。
// 板端构建不使用它（板端用 third_party/lib 里真正的 aarch64 librga.so）。
#include <cstddef>
#include <cstdio>
#include <cstring>
#include "im2d.h"

extern "C" {
IM_C_API IM_STATUS imcheck_t(rga_buffer_t, rga_buffer_t, rga_buffer_t, im_rect, im_rect,
                             im_rect, int) {
    return IM_STATUS_SUCCESS;
}

IM_C_API const char* imStrError_t(IM_STATUS status) {
    (void)status;
    return "stub";
}
}

IM_API IM_STATUS improcess(rga_buffer_t, rga_buffer_t, rga_buffer_t, im_rect, im_rect, im_rect,
                            int, int*, im_opt_t*, int) {
    return IM_STATUS_SUCCESS;
}

IM_C_API IM_STATUS improcess(rga_buffer_t, rga_buffer_t, rga_buffer_t, im_rect, im_rect, im_rect,
                             int) {
    return IM_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// importbuffer_fd / wrapbuffer_handle_t / releasebuffer_handle
// (零拷贝路径新增的符号; x86 stub 里返回 0 表示"import 失败",
//  rga_convert.cc 会自动退回老路 fd 方式)
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
