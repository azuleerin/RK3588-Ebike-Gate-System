// mpp_compat.h —— MPP_BUFFER -> dma-buf fd 的兼容层
//
// 背景：FFmpeg 的 h264_rkmpp 解码器内部用的是 librockchip_mpp，输出帧是一块
// MPP_BUFFER（底层是 dma-buf）。要拿到这块 dma-buf 的 fd。
//
// ⚠️ 两个容易踩的坑（已踩过，务必注意）:
//  1) 头文件路径: MPP 的头【不是】 rk_mpp/rk_mpp.h，而是【扁平】装在
//     /usr/include/rockchip/ 下(rk_mpi.h / mpp_buffer.h / mpp_frame.h ...)。
//     写 #include <rk_mpp/rk_mpp.h> 永远找不到, CMake 的 find_path 也永远
//     找不到 -> MPP 支持会被静默关掉。
//  2) mpp_buffer_get_fd 在新版 MPP 里是【宏】, 展开成
//         mpp_buffer_get_fd_with_caller(buffer, __FUNCTION__)
//     所以真正导出的动态符号是 "mpp_buffer_get_fd_with_caller",
//     签名 int (*)(MPP_BUFFER, const char*)。
//     直接 dlsym("mpp_buffer_get_fd") 永远返回 NULL。
#ifndef RK_MPP_COMPAT_H_
#define RK_MPP_COMPAT_H_

#include <dlfcn.h>
#include <cstdio>

#if RK_HAVE_MPP
#if defined(__has_include)
#if __has_include(<rockchip/rk_mpi.h>)
#include <rockchip/rk_mpi.h>
#elif __has_include(<rk_mpp/rk_mpi.h>)
#include <rk_mpp/rk_mpi.h>
#elif __has_include(<rk_mpi.h>)
#include <rk_mpi.h>
#else
#error "找不到 MPP 头文件(rk_mpi.h)。请安装 librockchip-mpp-dev, " \
      "或用 -DRK_HAVE_MPP=0 关掉 MPP 支持(会自动走 CPU 映射地址路径)"
#endif
#else
#include <rockchip/rk_mpi.h>
#endif
#endif  // RK_HAVE_MPP

namespace rk {

#if RK_HAVE_MPP
// MPP 的缓冲类型名在不同版本里不一样:
//   Rockchip MPP 1.0.x / 1.3.x (SDK 2019+)  -> MppBuffer  (typedef void*)
//   更老的 vendor 版本                       -> MPP_BUFFER (宏别名)
#if defined(MPP_BUFFER)
typedef MPP_BUFFER rk_mpp_buffer_t;
#else
typedef MppBuffer rk_mpp_buffer_t;
#endif

// 取 MPP 缓冲对应的 dma-buf fd; 取不到返回 -1。
//
// 新版 MPP: mpp_buffer_get_fd 是宏, 实际符号是 mpp_buffer_get_fd_with_caller
//           签名 int (*)(MppBuffer, const char*)
// 旧版 MPP: 直接导出 mpp_buffer_get_fd, 签名 int (*)(MppBuffer)
inline int mpp_buffer_get_fd_compat(rk_mpp_buffer_t buf) {
    typedef int (*fn_caller_t)(rk_mpp_buffer_t, const char*);
    typedef int (*fn_plain_t)(rk_mpp_buffer_t);

    static fn_caller_t fn_caller = nullptr;
    static fn_plain_t  fn_plain = nullptr;
    static bool tried = false;

    if (!tried) {
        tried = true;
        fn_caller = reinterpret_cast<fn_caller_t>(
            dlsym(RTLD_DEFAULT, "mpp_buffer_get_fd_with_caller"));
        if (!fn_caller) {
            fn_plain = reinterpret_cast<fn_plain_t>(
                dlsym(RTLD_DEFAULT, "mpp_buffer_get_fd"));
            if (!fn_plain)
                fprintf(stderr,
                        "[MPP] 进程里找不到 mpp_buffer_get_fd(_with_caller), "
                        "将只走 /proc 反查或 CPU 映射地址路径\n");
        }
    }

    if (fn_caller) {
        int fd = fn_caller(buf, __FUNCTION__);
        if (fd >= 0) return fd;
    }
    if (fn_plain) {
        int fd = fn_plain(buf);
        if (fd >= 0) return fd;
    }
    return -1;
}
#endif  // RK_HAVE_MPP

}  // namespace rk

#endif  // RK_MPP_COMPAT_H_
