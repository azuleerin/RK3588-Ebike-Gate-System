// rk_base/mpp_frame_guard.cc —— MPP 帧 RAII 守卫实现
//
// MPP 符号用 dlsym 取(与 src/mpp_decoder.cc 同一手法), rk_base 因此
// 不需要 rockchip/rk_mpi.h, 也不会被 MPP 的版本差异绑死。
//
// 关键: mpp_frame_deinit 会把 MppBuffer 的引用计数减到 0, 缓冲随即归还给
// MPP 的 buffer group —— 下一次解码就可能把它分配给别的帧。
// 所以凡是"取出 fd/ptr 之后还要继续用"的地方, 都必须让守卫活到用完为止。

#include "rk/mpp_frame_guard.h"

#include <dlfcn.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <utility>

#include "rk/log.h"

namespace rk {
namespace {

using MppFrameDeinitFn = int (*)(void** frame);
using MppFrameGetPtsFn = int64_t (*)(void* frame);

MppFrameDeinitFn resolve_deinit() {
    static MppFrameDeinitFn fn = []() -> MppFrameDeinitFn {
        return reinterpret_cast<MppFrameDeinitFn>(
            dlsym(RTLD_DEFAULT, "mpp_frame_deinit"));
    }();
    return fn;
}

MppFrameGetPtsFn resolve_pts() {
    static MppFrameGetPtsFn fn = []() -> MppFrameGetPtsFn {
        return reinterpret_cast<MppFrameGetPtsFn>(
            dlsym(RTLD_DEFAULT, "mpp_frame_get_pts"));
    }();
    return fn;
}

std::atomic<uint64_t> g_live{0};
std::atomic<uint64_t> g_constructed{0};
std::atomic<uint64_t> g_destroyed{0};

constexpr uint64_t kSummaryEvery = 1000;

}  // namespace

MppFrameGuard::~MppFrameGuard() {
    if (!frame_) return;
    const int64_t p = pts();     // ★ deinit 之后帧就没了, pts 要先取
    if (auto fn = resolve_deinit()) {
        fn(&frame_);
    } else {
        RK_LOG_ERROR("guard", "mpp-frame",
                     "找不到 mpp_frame_deinit 符号, 帧无法释放 —— 可能有泄漏");
    }
    frame_ = nullptr;
    const uint64_t live = g_live.fetch_sub(1) - 1;
    g_destroyed.fetch_add(1);
    RK_LOG_INFO("guard", "mpp-frame",
                "析构 tag=%s pts=%" PRId64 " 存活守卫=%" PRIu64 " (构造=%" PRIu64
                " 析构=%" PRIu64 ")",
                tag_.c_str(), p, live, g_constructed.load(), g_destroyed.load());
}

MppFrameGuard::MppFrameGuard(MppFrameGuard&& other) noexcept
    : frame_(other.frame_), tag_(std::move(other.tag_)) {
    other.frame_ = nullptr;
}

MppFrameGuard& MppFrameGuard::operator=(MppFrameGuard&& other) noexcept {
    if (this == &other) return *this;
    reset(nullptr, tag_);
    frame_ = other.frame_;
    tag_ = std::move(other.tag_);
    other.frame_ = nullptr;
    return *this;
}

void MppFrameGuard::reset(void* mpp_frame, std::string tag) {
    if (frame_ == mpp_frame && frame_) {
        tag_ = std::move(tag);
        return;
    }
    if (frame_) {
        if (auto fn = resolve_deinit()) {
            fn(&frame_);
        }
        frame_ = nullptr;
        const uint64_t live = g_live.fetch_sub(1) - 1;
        g_destroyed.fetch_add(1);
        RK_LOG_INFO("guard", "mpp-frame", "析构(tag变更) tag=%s pts=%" PRId64
                    " 存活守卫=%" PRIu64, tag_.c_str(), pts(), live);
    }
    tag_ = std::move(tag);
    frame_ = mpp_frame;
    if (frame_) {
        const uint64_t live = g_live.fetch_add(1) + 1;
        const uint64_t c = g_constructed.fetch_add(1) + 1;
        RK_LOG_INFO("guard", "mpp-frame",
                    "构造 tag=%s pts=%" PRId64 " frame=%p 存活守卫=%" PRIu64,
                    tag_.c_str(), pts(), frame_, live);
        if (c % kSummaryEvery == 0) {
            RK_LOG_INFO("guard", "mpp-frame",
                        "统计 构造=%" PRIu64 " 析构=%" PRIu64 " 存活=%" PRIu64,
                        g_constructed.load(), g_destroyed.load(), g_live.load());
        }
    }
}

void* MppFrameGuard::release() {
    void* f = frame_;
    frame_ = nullptr;
    if (f) {
        g_live.fetch_sub(1);
        g_destroyed.fetch_add(1);
    }
    return f;
}

int64_t MppFrameGuard::pts() const {
    if (!frame_) return 0;
    if (auto fn = resolve_pts()) return fn(frame_);
    return 0;
}

uint64_t MppFrameGuard::live_count() { return g_live.load(); }
uint64_t MppFrameGuard::total_constructed() { return g_constructed.load(); }
uint64_t MppFrameGuard::total_destroyed() { return g_destroyed.load(); }

void MppFrameGuard::reset_counters() {
    g_live.store(0);
    g_constructed.store(0);
    g_destroyed.store(0);
}

}  // namespace rk
