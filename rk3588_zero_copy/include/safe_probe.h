// safe_probe.h —— "受控崩溃"探测
//
// 为什么需要它:
//   FFmpeg 的 h264_rkmpp 解码器把 MPP_BUFFER 挂在 frame->buf[0] 的 opaque 上,
//   但这个约定在各个 FFmpeg 分支/版本里并不统一 —— 有的版本 opaque 存的是
//   MPP_BUFFER, 有的存别的、甚至存 0。强行把它当 MPP_BUFFER 解引用
//   (mpp_buffer_get_ptr) 会直接段错误, 整个进程挂掉。
//
// 做法: 临时装一个 SIGSEGV/SIGBUS handler, 探测函数里如果崩了, handler 用
//       siglongjmp 跳回, 调用方拿到 false, 走兜底路径而不是让进程死掉。
//
// 约束: 只能在单线程调用, 且被探测的函数不能持有锁 / 不能有副作用。
#ifndef RK_SAFE_PROBE_H_
#define RK_SAFE_PROBE_H_

#include <setjmp.h>
#include <signal.h>

#include <cstdio>
#include <cstring>

namespace rk {

class SafeProbe {
public:
    // fn 返回 true 表示"探测通过"
    typedef bool (*Fn)(void*);

    static bool call(Fn fn, void* arg) {
        struct sigaction sa, old_segv, old_bus;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = &on_signal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_NODEFER | SA_RESTART;

        if (sigaction(SIGSEGV, &sa, &old_segv) != 0) return false;
        if (sigaction(SIGBUS, &sa, &old_bus) != 0) {
            sigaction(SIGSEGV, &old_segv, nullptr);
            return false;
        }

        bool ok = false;
        if (sigsetjmp(jump_, 1) == 0) {
            in_probe_ = true;
            ok = fn(arg);
            in_probe_ = false;
        } else {
            // 被信号打断 -> 探测失败, 走兜底
            in_probe_ = false;
            ok = false;
        }

        sigaction(SIGSEGV, &old_segv, nullptr);
        sigaction(SIGBUS, &old_bus, nullptr);
        return ok;
    }

    // 指针是否"长得像"一个合法的用户态指针 (过滤掉 0 / 小整数 / 未对齐的野值)
    static bool plausible_ptr(const void* p) {
        unsigned long v = reinterpret_cast<unsigned long>(p);
        if (v < 0x1000UL) return false;                       // 空指针 / 小整数
        if (v & 0x7UL) return false;                          // 未对齐
        if (v > 0x00007fffffffffffUL) return false;            // 非用户态地址
        return true;
    }

private:
    static void on_signal(int sig) {
        if (in_probe_) {
            in_probe_ = false;
            siglongjmp(jump_, 1);
        }
        // 不在探测中 -> 保留默认行为(终止进程), 避免掩盖真实的崩溃
        _exit(128 + sig);
    }

    static sigjmp_buf jump_;
    static volatile sig_atomic_t in_probe_;
};

}  // namespace rk

#endif  // RK_SAFE_PROBE_H_
