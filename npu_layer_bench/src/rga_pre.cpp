// src/rga_pre.cpp —— 第 3 层的 RGA 预处理(NV12 -> RGB888 + 缩放)
//
// ★ 三个必须记住的坑(全部来自主工程实测):
//
// 1) 【wstride 单位】RgaImage 里是字节, librga 的 rga_buffer_t 里是像素。
//    NV12 是 1 字节/像素 -> 数值相同; RGB888 是 3 字节/像素 -> 要除以 3。
//    搞错会让 RGA 把图像横向压扁。
//
// 2) 【fd 有效时不要填 vir_addr】
//    两者都填, librga 会走"软件 MMU"那条路(用户态 VA 自己映射成物理地址),
//    那条路会缓存 VA->PA 映射, 分配复用之后 PA 对不上 —— 堆被随机破坏。
//
// 3) 【/dev/rga 打不开时 librga 会 SIGSEGV, 而不是返回错误码】
//    librga 1.9.1 的 rga_init() 是第一次调 imcheck/improcess 时才触发的,
//    它内部 open("/dev/rga"); open 失败时它只打印一行然后继续解引用
//    没初始化的全局句柄 -> 段错误。所以必须【自己先 open 一次】,
//    把原因查清楚再决定要不要碰 librga。
#include "nlb/rga_pre.h"

#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "im2d.h"
#include "im2d_buffer.h"

#include "nlb/dma_buf.h"

namespace nlb {
namespace {

// ★ librga 在【一个进程里】是串行的:
//   每次调用都要 importbuffer_fd 注册缓冲, 而那张全局 handle 表是
//   进程内共享 + 加锁的。多线程同时转就在这把锁上排队。
//   生产链路(gate_main.cc 的 g_rga_m)也是自己加锁的, 口径一致。
std::mutex g_rga_mutex;

// librga 里有两个"成功"值: imcheck 返回 IM_STATUS_NOERROR(2),
// 而 improcess/imcvt 返回 IM_STATUS_SUCCESS(1)。两个都当作成功。
inline bool rga_ok(IM_STATUS s) {
    return s == IM_STATUS_NOERROR || s == IM_STATUS_SUCCESS;
}

// ---- 一个极简的崩溃探针(只在【单线程】阶段用) ------------------------
// librga 的 rga_init() 崩溃时不会给错误码, 只能靠信号兜住。
// 注意: sigaction + sigsetjmp 是【进程级】的, 所以只在初始化阶段(单线程)用。
sigjmp_buf g_probe_jmp;
volatile sig_atomic_t g_probe_armed = 0;

void probe_sigsegv(int sig) {
    if (g_probe_armed) {
        g_probe_armed = 0;
        siglongjmp(g_probe_jmp, sig);
    }
    _exit(128 + sig);
}

bool arm_probe() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = probe_sigsegv;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER;
    if (sigaction(SIGSEGV, &sa, nullptr) != 0) return false;
    if (sigaction(SIGBUS, &sa, nullptr) != 0) return false;
    return true;
}

void disarm_probe() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    g_probe_armed = 0;
}

const char* rga_device_path() {
    const char* e = getenv("RGA_DEVICE");
    if (e && e[0]) return e;
    return "/dev/rga";
}

int align_up(int v, int a) { return ((v + a - 1) / a) * a; }

// RGA 的 format 枚举在 rga.h 里, 这里只需要这两个。
constexpr int kFmtNv12 = RK_FORMAT_YCbCr_420_SP;
constexpr int kFmtRgb888 = RK_FORMAT_RGB_888;

// 目标缓冲的字节数(RGB888 + 16 行余量)。
std::size_t dst_bytes(int w, int h) {
    return static_cast<std::size_t>(w) * 3 * (h + kRgaDstPadRows);
}

}  // namespace

bool RgaConverter::available(std::string* reason) {
    // ---- ① 设备节点预检(完全不碰 librga, 绝对不会崩) ----
    const char* path = rga_device_path();
    int fd = ::open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0 && errno == EINVAL) fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (reason) {
            *reason = std::string("打不开 RGA 设备节点 ") + path + ": " +
                      strerror(errno) + "(errno=" + std::to_string(errno) + ")";
        }
        return false;
    }
    ::close(fd);

    // ---- ② librga 初始化探测(用信号兜住可能的 SIGSEGV) ----
    if (!arm_probe()) {
        if (reason) *reason = "sigaction 失败";
        return false;
    }
    volatile bool survived = true;
    if (sigsetjmp(g_probe_jmp, 1) == 0) {
        g_probe_armed = 1;
        rga_buffer_t s, d;
        memset(&s, 0, sizeof(s));
        memset(&d, 0, sizeof(d));
        im_rect r;
        memset(&r, 0, sizeof(r));
        r.width = 16;
        r.height = 16;
        imcheck(s, d, r, r);   // 目的只是触发 rga_init()
        g_probe_armed = 0;
    } else {
        survived = false;
    }
    disarm_probe();

    if (!survived) {
        if (reason) {
            *reason = std::string("rga_init() 内部崩溃(设备节点能打开, 但 librga 初始化时 SIGSEGV) —— "
                                  "多半是 librga 和内核驱动版本对不上");
        }
        return false;
    }
    return true;
}

bool RgaConverter::init_src(int src_w, int src_h, std::string* err) {
    destroy();

    std::string reason;
    if (!available(&reason)) {
        if (err) *err = "RGA 不可用: " + reason;
        return false;
    }
    rga_ready_ = true;

    src_w_ = src_w;
    src_h_ = src_h;
    // NV12 的行/列按 16 对齐(和 MPP 的输出一致)。
    src_wstride_ = align_up(src_w, 16);
    src_hstride_ = align_up(src_h, 16);
    src_size_ = static_cast<std::size_t>(src_wstride_) * src_hstride_ * 3 / 2;

    src_fd_ = dma_buf_alloc(src_size_, /*uncached=*/true);
    if (src_fd_ < 0) {
        if (err) *err = std::string("dma-buf 分配失败: ") + dma_buf_backend();
        return false;
    }
    src_ptr_ = dma_buf_map(src_fd_, src_size_);
    if (!src_ptr_) {
        if (err) *err = "dma-buf mmap 失败";
        destroy();
        return false;
    }
    return true;
}

void RgaConverter::fill_src_pattern() {
    if (!src_ptr_) return;
    uint8_t* p = static_cast<uint8_t*>(src_ptr_);
    // Y 平面: 一个斜向渐变(取值温和, 不产生极端值)。
    for (int y = 0; y < src_hstride_; ++y) {
        for (int x = 0; x < src_wstride_; ++x) {
            p[static_cast<size_t>(y) * src_wstride_ + x] =
                static_cast<uint8_t>((x + y) & 0xFF);
        }
    }
    // UV 平面: 中性灰(128) —— 不引入彩色噪声, 让缩放路径的行为稳定。
    uint8_t* uv = p + static_cast<size_t>(src_wstride_) * src_hstride_;
    memset(uv, 128, src_size_ - static_cast<size_t>(src_wstride_) * src_hstride_);
}

int RgaConverter::convert(int dst_fd, void* dst_ptr, std::size_t dst_size,
                          int dst_w, int dst_h) {
    // librga 进程内串行 -> 注册 + 转换 + 释放整段都在锁里
    std::lock_guard<std::mutex> lk(g_rga_mutex);

    if (!rga_ready_ || src_fd_ < 0 || dst_fd < 0) return -1;

    // 目标必须放得下(含 16 行余量)。
    if (dst_size < dst_bytes(dst_w, dst_h)) return -2;

    // ---- 注册源 / 目标 ----
    // importbuffer_fd 传的是【字节数】, 用 dma-buf 的真实大小最保险。
    const std::size_t src_bytes = dma_buf_size(src_fd_);
    const std::size_t dst_real = dma_buf_size(dst_fd);

    rga_buffer_handle_t src_handle =
        importbuffer_fd(src_fd_, static_cast<int>(src_bytes > 0 ? src_bytes : src_size_));
    if (!src_handle) return -3;
    rga_buffer_handle_t dst_handle =
        importbuffer_fd(dst_fd, static_cast<int>(dst_real > 0 ? dst_real : dst_size));
    if (!dst_handle) {
        releasebuffer_handle(src_handle);
        return -4;
    }

    // ★ wstride: NV12 = 字节 = 像素; RGB888 = 字节 / 3 = 像素。
    rga_buffer_t sb = wrapbuffer_handle_t(src_handle, src_w_, src_h_,
                                          src_wstride_, src_hstride_, kFmtNv12);
    sb.color_space_mode = IM_COLOR_SPACE_DEFAULT;   // BT.601 limited(本板 librga 不支持 BT.709)

    rga_buffer_t db = wrapbuffer_handle_t(dst_handle, dst_w, dst_h,
                                          dst_w, dst_h, kFmtRgb888);
    db.color_space_mode = IM_COLOR_SPACE_DEFAULT;

    im_rect srect;
    memset(&srect, 0, sizeof(srect));
    srect.x = 0; srect.y = 0; srect.width = src_w_; srect.height = src_h_;

    im_rect drect;
    memset(&drect, 0, sizeof(drect));
    drect.x = 0; drect.y = 0; drect.width = dst_w; drect.height = dst_h;

    rga_buffer_t pat;
    memset(&pat, 0, sizeof(pat));

    const IM_STATUS st = improcess(sb, db, pat, srect, drect, drect,
                                   -1, nullptr, nullptr, 0);

    releasebuffer_handle(dst_handle);
    releasebuffer_handle(src_handle);

    (void)dst_ptr;
    return rga_ok(st) ? 0 : -5;
}

void RgaConverter::destroy() {
    if (src_ptr_) dma_buf_unmap(src_ptr_, src_size_);
    if (src_fd_ >= 0) ::close(src_fd_);
    src_ptr_ = nullptr;
    src_fd_ = -1;
    src_size_ = 0;
    rga_ready_ = false;
}

}  // namespace nlb
