// rk/av_frame_guard.h —— AVFrame 的 RAII 守卫
//
// 为什么需要(AGENT_RULES_FOR_CRASH_DEBUGGING.md 第二节 + 人类批复"方案B 标准引用计数"):
//   手动调用 av_frame_unref 极易在异常分支(continue / return / 抛异常)被遗漏,
//   一旦漏掉就是内存泄漏; 漏在错误分支上还会让帧的生命周期与使用它的硬件
//   (RGA / NPU)脱节 —— 那正是 P0-1 的形态。
//
// 语义:
//   构造: 接管调用方交出来的 AVFrame* 所有权(不额外引用)
//   拷贝: av_frame_ref, 引用计数 +1(标准引用计数)
//   析构: av_frame_free —— 释放帧引用的【缓冲区】+ AVFrame【结构体本身】
//   移动: 转移所有权, 不动引用计数
//
// ★ 所有权契约(重要):
//   交进来的 AVFrame* 必须是 av_frame_alloc() 分配出来的, 守卫会用
//   av_frame_free() 把它【连结构体一起】释放。
//   如果只写 av_frame_unref(), 结构体(416 字节)不会被释放 -> 每帧泄漏。
//
// 生命周期: 栈对象。离开作用域(含异常展开)时自动释放。
//   典型用法:
//       rk::AVFrameGuard g(av_frame_alloc());
//       if (avcodec_receive_frame(vctx, g.get()) < 0) continue;   // continue 也会释放
//       use(g->data[0], g->pts);
//       // 作用域结束 -> 自动 av_frame_free
//
// 观测: 构造/析构都打 INFO 日志(带 tag 与 pts), 并且维护全局计数,
//   长跑时可以直接核对 "构造次数 == 析构次数"。
#ifndef RK_AV_FRAME_GUARD_H_
#define RK_AV_FRAME_GUARD_H_

// ⚠️ FFmpeg 7.x 的 libavutil/frame.h 【自身没有】 extern "C" 包裹 ——
//   它假定你通过 <libavcodec/avcodec.h> 引入(那个头有 extern "C")。
//   如果直接包含 frame.h, 声明会变成 C++ 符号, 链接时报
//   "undefined reference to av_frame_ref(AVFrame*, AVFrame const*)"。
//   所以这里必须自己包一层。
extern "C" {
#include <libavutil/frame.h>
}

#include <cstdint>
#include <string>

namespace rk {

class AVFrameGuard {
public:
    AVFrameGuard() = default;

    // 接管所有权: 调用方把 frame 的所有权交给本对象, 不增加引用计数。
    explicit AVFrameGuard(AVFrame* frame, std::string tag = "av-frame");
    ~AVFrameGuard();

    // 拷贝 = 标准引用计数(av_frame_ref)。拷贝出来的守卫与原守卫指向同一帧。
    AVFrameGuard(const AVFrameGuard& other);
    AVFrameGuard& operator=(const AVFrameGuard& other);

    // 移动 = 转移所有权, 不动引用计数。
    AVFrameGuard(AVFrameGuard&& other) noexcept;
    AVFrameGuard& operator=(AVFrameGuard&& other) noexcept;

    AVFrame* get() const { return frame_; }
    AVFrame* operator->() const { return frame_; }
    explicit operator bool() const { return frame_ != nullptr; }

    // 释放当前持有的帧(如果有), 然后接管新的。
    void reset(AVFrame* frame = nullptr, std::string tag = "av-frame");

    // 放弃所有权并返回指针(调用方自己负责 av_frame_free)。极少用。
    AVFrame* release();

    int64_t pts() const;

    // ---- 全局计数: 长跑时核对 ref/unref 是否 1:1 ----
    static uint64_t live_count();        // 当前存活的守卫数
    static uint64_t total_constructed();
    static uint64_t total_destroyed();
    static uint64_t total_copied();     // 拷贝(av_frame_ref)次数
    static void reset_counters();

private:
    AVFrame* frame_ = nullptr;
    std::string tag_;
};

}  // namespace rk

#endif  // RK_AV_FRAME_GUARD_H_
