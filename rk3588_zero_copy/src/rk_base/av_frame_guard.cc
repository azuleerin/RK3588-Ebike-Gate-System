// rk_base/av_frame_guard.cc —— AVFrame RAII 守卫实现
//
// 日志策略: 构造/析构各打一条 INFO(带 tag 与 pts), 便于长跑时人工核对
// "构造次数 == 析构次数"; 另外每 1000 次打一条汇总, 免得刷屏时看不出平衡。
//
// 注意: av_frame_unref 只是把引用计数减到 0 时才真正释放, 所以拷贝语义
// 必须走 av_frame_ref(标准引用计数), 绝不能浅拷贝指针。

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
    const int64_t p = pts();     // ★ 必须在 unref 之前取: unref 会把 pts 清成 AV_NOPTS_VALUE
    av_frame_unref(frame_);
    frame_ = nullptr;
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
        // 标准引用计数: 拷贝必须 av_frame_ref, 否则两个守卫会重复 unref
        if (av_frame_ref(other.frame_, frame_) == 0) {
            frame_ = other.frame_;
            g_copied.fetch_add(1);
            const uint64_t live = g_live.fetch_add(1) + 1;
            RK_LOG_INFO("guard", "av-frame",
                        "拷贝 tag=%s pts=%" PRId64 " 存活守卫=%" PRIu64 " (引用计数 +1)",
                        tag_.c_str(), pts(), live);
        } else {
            frame_ = nullptr;
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
        av_frame_unref(frame_);
        frame_ = nullptr;
        const uint64_t live = g_live.fetch_sub(1) - 1;
        g_destroyed.fetch_add(1);
        RK_LOG_INFO("guard", "av-frame", "析构(tag变更) tag=%s pts=%" PRId64
                    " 存活守卫=%" PRIu64, tag_.c_str(), pts(), live);
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
