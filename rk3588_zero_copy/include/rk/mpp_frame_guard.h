// rk/mpp_frame_guard.h —— MPP 帧的 RAII 守卫
//
// ★ 为什么除了 AVFrameGuard 还要这个:
//   生产的 MPP 路径里, av_read_frame 拿到的 AVFrame 其实是【空的】
//   (extract_nv12 只在软解路径调用), 真正持有解码像素的是 MPP 的 MppFrame。
//   而 MppDecoder::decode() 在【函数内部】就调用了 mpp_frame_deinit():
//       out->fd  = mpp_frame_get_fd_compat(frame);   // 取出 dma-buf fd
//       out->ptr = mpp_buffer_get_ptr(buf);          // 取出 CPU 映射地址
//       ...
//       mpp_frame_deinit(&frame);                    // ← 引用在这里就没了
//   于是调用方拿着一个"已经被归还给 MPP buffer group"的 fd/ptr 继续用
//   (LPR 裁剪在 av_frame_unref 之后才用) —— 这就是硬挂死的前兆。
//
//   语义同 AVFrameGuard:
//       构造/reset : 接管 MppFrame 的所有权
//       析构        : mpp_frame_deinit(&frame_)
//       移动        : 转移所有权
//       拷贝        : 禁止(MPP 帧没有干净的引用计数接口)
//
// 为什么用 dlsym 而不是 <rockchip/rk_mpi.h>:
//   rk_base 不该绑死任何 MPP 版本。工程里 src/mpp_decoder.cc 本来就用 dlsym
//   探测 MPP 符号(兼容 20190626 旧版与 BSP 新版), 这里沿用同一手法。
#ifndef RK_MPP_FRAME_GUARD_H_
#define RK_MPP_FRAME_GUARD_H_

#include <cstdint>
#include <string>

namespace rk {

class MppFrameGuard {
public:
    MppFrameGuard() = default;
    ~MppFrameGuard();

    MppFrameGuard(const MppFrameGuard&) = delete;
    MppFrameGuard& operator=(const MppFrameGuard&) = delete;
    MppFrameGuard(MppFrameGuard&& other) noexcept;
    MppFrameGuard& operator=(MppFrameGuard&& other) noexcept;

    // 接管一个 MppFrame 的所有权(MppFrame 就是 void*, 与 MPP API 一致)。
    void reset(void* mpp_frame, std::string tag = "mpp-frame");

    void* get() const { return frame_; }
    explicit operator bool() const { return frame_ != nullptr; }

    // 放弃所有权, 由调用方自己 deinit。
    void* release();

    int64_t pts() const;

    static uint64_t live_count();
    static uint64_t total_constructed();
    static uint64_t total_destroyed();
    static void reset_counters();

private:
    void* frame_ = nullptr;
    std::string tag_;
};

}  // namespace rk

#endif  // RK_MPP_FRAME_GUARD_H_
