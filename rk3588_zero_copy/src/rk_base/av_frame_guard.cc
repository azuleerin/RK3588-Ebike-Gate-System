// rk_base/av_frame_guard.cc —— AVFrame RAII 守卫实现
//
// 日志策略: 构造/析构各打一条 INFO(带 tag 与 pts), 便于长跑时人工核对
// "构造次数 == 析构次数"; 另外每 1000 次打一条汇总, 免得刷屏时看不出平衡。
//
// ★★ 所有权契约(2026-11-12 定位活堆泄漏时定下)★★
//   本守卫【接管】一个由 av_frame_alloc() 分配出来的 AVFrame*。
//   av_frame_alloc() 分配的是【结构体本身】(sizeof(AVFrame) = 416 字节),
//   它必须由 av_frame_free() 释放:
//       av_frame_unref()  只释放帧【引用的缓冲区】, 不释放结构体本身;
//       av_frame_free()   = av_frame_unref() + av_free(结构体)。
//   之前析构里写的是 av_frame_unref() -> 每帧泄漏一个 416 字节的 AVFrame
//   结构体, 表现为"活堆 +0.42 KB/帧"。软解路径每帧一个 AVFrame, 所以必漏;
//   硬解路径走 MppFrame(用 mpp_frame_deinit, 结构和缓冲一起释放)所以看不出来。
//
// 另外: 拷贝语义必须走 av_frame_ref(标准引用计数), 绝不能浅拷贝指针;
//   而且【必须先 av_frame_alloc() 再 av_frame_ref()】—— av_frame_ref() 要往
//   dst 里写, dst 指向空指针会直接段错误(旧代码正是这么写的)。

#include "rk/av_frame_guard.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <utility>

#include "rk/log.h"

namespace rk {
namespace {

std::atomic<uint64_t> g_live{0};
std::atomic<uint64_t> g_constructed{0};
std::atomic<uint64_t> g_destroyed{0};
std::atomic<uint64_t> g_copied{0};

constexpr uint64_t kSummaryEvery = 1000;

}  // namespace

AVFrameGuard::AVFrameGuard(AVFrame* frame, std::string tag) : tag_(std::move(tag)) {
    reset(frame, tag_);
}

AVFrameGuard::~AVFrameGuard() {
    if (!frame_) return;
    const int64_t p = pts();     // ★ 必须在释放之前取: 释放会把 pts 清成 AV_NOPTS_VALUE
    av_frame_free(&frame_);      // ★ av_frame_free = unref(缓冲区) + free(结构体)
    const uint64_t live = g_live.fetch_sub(1) - 1;
    g_destroyed.fetch_add(1);
    RK_LOG_INFO("guard", "av-frame",
                "析构 tag=%s pts=%" PRId64 " 存活守卫=%" PRIu64 " (构造=%" PRIu64
                " 析构=%" PRIu64 " 拷贝=%" PRIu64 ")",
                tag_.c_str(), p, live, g_constructed.load(), g_destroyed.load(),
                g_copied.load());
}

AVFrameGuard::AVFrameGuard(const AVFrameGuard& other) : tag_(other.tag_) {
    if (other.frame_) {
        // 标准引用计数: 拷贝必须 av_frame_ref, 否则两个守卫会重复 unref。
        // ★ 必须先分配一块新的 AVFrame —— av_frame_ref() 要往 dst 里写,
        //   dst 是空指针会直接段错误(旧代码就是往 nullptr 上写)。
        frame_ = av_frame_alloc();
        if (frame_ && av_frame_ref(other.frame_, frame_) == 0) {
            g_copied.fetch_add(1);
            const uint64_t live = g_live.fetch_add(1) + 1;
            RK_LOG_INFO("guard", "av-frame",
                        "拷贝 tag=%s pts=%" PRId64 " 存活守卫=%" PRIu64 " (引用计数 +1)",
                        tag_.c_str(), pts(), live);
        } else {
            if (frame_) { av_frame_free(&frame_); frame_ = nullptr; }
            RK_LOG_ERROR("guard", "av-frame", "拷贝失败 av_frame_ref 返回错误 tag=%s",
                         tag_.c_str());
        }
    }
}

AVFrameGuard& AVFrameGuard::operator=(const AVFrameGuard& other) {
    if (this == &other) return *this;
    AVFrameGuard tmp(other);          // 先构造新的(av_frame_ref), 成功后再交换
    std::swap(frame_, tmp.frame_);
    std::swap(tag_, tmp.tag_);
    return *this;                     // tmp 析构 -> 释放旧帧
}

AVFrameGuard::AVFrameGuard(AVFrameGuard&& other) noexcept
    : frame_(other.frame_), tag_(std::move(other.tag_)) {
    other.frame_ = nullptr;
}

AVFrameGuard& AVFrameGuard::operator=(AVFrameGuard&& other) noexcept {
    if (this == &other) return *this;
    reset(nullptr, tag_);
    frame_ = other.frame_;
    tag_ = std::move(other.tag_);
    other.frame_ = nullptr;
    return *this;
}

void AVFrameGuard::reset(AVFrame* frame, std::string tag) {
    if (frame_ == frame && frame) {
        tag_ = std::move(tag);
        return;
    }
    if (frame_) {
        const int64_t p = pts();     // ★ 先取 pts: 释放后会被清成 AV_NOPTS_VALUE
        av_frame_free(&frame_);      // ★ 同上: 结构体也要 free
        frame_ = nullptr;
        const uint64_t live = g_live.fetch_sub(1) - 1;
        g_destroyed.fetch_add(1);
        RK_LOG_INFO("guard", "av-frame", "析构(tag变更) tag=%s pts=%" PRId64
                    " 存活守卫=%" PRIu64, tag_.c_str(), p, live);
    }
    tag_ = std::move(tag);
    frame_ = frame;
    if (frame_) {
        const uint64_t live = g_live.fetch_add(1) + 1;
        const uint64_t c = g_constructed.fetch_add(1) + 1;
        RK_LOG_INFO("guard", "av-frame",
                    "构造 tag=%s pts=%" PRId64 " data=%p %dx%d 存活守卫=%" PRIu64,
                    tag_.c_str(), pts(), frame_->data[0], frame_->width, frame_->height, live);
        if (c % kSummaryEvery == 0) {
            RK_LOG_INFO("guard", "av-frame",
                        "统计 构造=%" PRIu64 " 析构=%" PRIu64 " 拷贝=%" PRIu64
                        " 存活=%" PRIu64,
                        g_constructed.load(), g_destroyed.load(), g_copied.load(),
                        g_live.load());
        }
    }
}

AVFrame* AVFrameGuard::release() {
    AVFrame* f = frame_;
    frame_ = nullptr;
    if (f) {
        g_live.fetch_sub(1);
        g_destroyed.fetch_add(1);
    }
    return f;
}

int64_t AVFrameGuard::pts() const {
    return frame_ ? frame_->pts : AV_NOPTS_VALUE;
}

uint64_t AVFrameGuard::live_count() { return g_live.load(); }
uint64_t AVFrameGuard::total_constructed() { return g_constructed.load(); }
uint64_t AVFrameGuard::total_destroyed() { return g_destroyed.load(); }
uint64_t AVFrameGuard::total_copied() { return g_copied.load(); }

void AVFrameGuard::reset_counters() {
    g_live.store(0);
    g_constructed.store(0);
    g_destroyed.store(0);
    g_copied.store(0);
}

}  // namespace rk
