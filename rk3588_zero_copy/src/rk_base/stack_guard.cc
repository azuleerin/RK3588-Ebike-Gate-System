// rk_base/stack_guard.cc —— 栈水位 + 心跳看门狗实现
//
// 栈区间的获取:
//   pthread_getattr_np(pthread_self(), &attr) + pthread_attr_getstack()
//   拿到 [stackaddr, stackaddr+stacksize), ARM64 上栈向低地址增长。
//   线程踩到的最深位置 = 观测到的最小地址, 用掉的栈 = stack_high - 最深地址。
//
// 看门狗为什么必须是独立线程:
//   主循环卡死时, 如果心跳检查也在主循环里做, 就永远检查不到。
//   看门狗线程独立运行, 主循环卡住时它还活着, 才能把"卡住了"写进日志。
//
// 注意: 看门狗自己也会被硬挂死带走(整机挂死时所有 CPU 都不动了),
//       所以它只能给出"挂死前的最后信息"; 真正的内核现场靠 ramoops/dmesg。
//       这一点在取证时要分清楚: 日志停在哪一行, 说明挂死发生在那之后。

#include "rk/stack_guard.h"

#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "rk/log.h"

namespace rk {
namespace {

struct ThreadInfo {
    std::string name;
    uintptr_t stack_low = 0;   // 栈区最低地址
    uintptr_t stack_high = 0;  // 栈区最高地址(= low + size)
    uintptr_t deepest = 0;     // 观测到的最深(最小)地址
    uint32_t tid = 0;
    bool has_depth = false;
};

struct GuardState {
    std::mutex mu;
    std::map<uint32_t, ThreadInfo> threads;
    std::atomic<uint64_t> heartbeat{0};
    std::atomic<bool> watchdog_on{false};
    std::atomic<uint64_t> stall_reported{0};
    size_t min_free = 256 * 1024;
    uint32_t interval_ms = 500;
};

GuardState& state() {
    static GuardState s;
    return s;
}

uint32_t tid() {
    return static_cast<uint32_t>(syscall(SYS_gettid));
}

}  // namespace

void StackGuard::register_thread(const char* name) {
    GuardState& st = state();

    pthread_attr_t attr;
    ThreadInfo info;
    info.name = name ? name : "unnamed";
    info.tid = tid();

    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        void* addr = nullptr;
        size_t size = 0;
        if (pthread_attr_getstack(&attr, &addr, &size) == 0) {
            info.stack_low = reinterpret_cast<uintptr_t>(addr);
            info.stack_high = info.stack_low + size;
        }
        pthread_attr_destroy(&attr);
    }

    {
        std::lock_guard<std::mutex> lk(st.mu);
        st.threads[info.tid] = info;
    }
    RK_LOG_INFO("stack", "register", "线程 tid=%u name=%s 栈区间 [0x%lx, 0x%lx) 大小 %zu B",
                info.tid, info.name.c_str(),
                static_cast<unsigned long>(info.stack_low),
                static_cast<unsigned long>(info.stack_high),
                info.stack_high - info.stack_low);
}

void StackGuard::mark_stack() {
    GuardState& st = state();
    char probe = 0;
    const uintptr_t here = reinterpret_cast<uintptr_t>(&probe);

    std::lock_guard<std::mutex> lk(st.mu);
    auto it = st.threads.find(tid());
    if (it == st.threads.end()) return;
    // 栈向下增长, 越小越深
    if (!it->second.has_depth || here < it->second.deepest) {
        it->second.deepest = here;
        it->second.has_depth = true;
    }
}

void StackGuard::beat() {
    state().heartbeat.fetch_add(1, std::memory_order_relaxed);
}

uint64_t StackGuard::heartbeat_count() {
    return state().heartbeat.load(std::memory_order_relaxed);
}

size_t StackGuard::free_bytes() {
    GuardState& st = state();
    std::lock_guard<std::mutex> lk(st.mu);
    auto it = st.threads.find(tid());
    if (it == st.threads.end() || !it->second.has_depth) return 0;
    if (it->second.deepest <= it->second.stack_low) return 0;
    return it->second.deepest - it->second.stack_low;
}

void StackGuard::start_watchdog(size_t min_free_bytes, uint32_t interval_ms) {
    GuardState& st = state();
    st.min_free = min_free_bytes;
    st.interval_ms = interval_ms;
    if (st.watchdog_on.exchange(true)) return;

    std::thread([]() {
        GuardState& s = state();
        uint64_t last_beat = 0;
        uint64_t stalls = 0;

        while (s.watchdog_on.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(s.interval_ms));

            const uint64_t beat = s.heartbeat.load(std::memory_order_relaxed);
            if (beat == last_beat) {
                ++stalls;
                if (stalls == 1 || stalls % 10 == 0) {
                    RK_LOG_WARN("stack", "watchdog",
                                "主循环心跳停滞 %llu 次 (每 %ums 未推进, beat=%llu)",
                                static_cast<unsigned long long>(stalls),
                                s.interval_ms, static_cast<unsigned long long>(beat));
                }
            } else {
                if (stalls > 0) {
                    RK_LOG_INFO("stack", "watchdog", "心跳恢复, 停滞 %llu 次后继续",
                                static_cast<unsigned long long>(stalls));
                }
                stalls = 0;
            }
            last_beat = beat;

            // 栈余量检查
            std::lock_guard<std::mutex> lk(s.mu);
            for (const auto& kv : s.threads) {
                if (!kv.second.has_depth) continue;
                if (kv.second.deepest <= kv.second.stack_low) continue;
                const size_t used = kv.second.stack_high - kv.second.deepest;
                const size_t freeb = kv.second.deepest - kv.second.stack_low;
                if (freeb < s.min_free) {
                    RK_LOG_ERROR("stack", "watchdog",
                                 "栈余量不足! tid=%u name=%s 已用 %zu B 剩余 %zu B (阈值 %zu B)",
                                 kv.second.tid, kv.second.name.c_str(), used, freeb,
                                 s.min_free);
                }
            }
        }
        RK_LOG_INFO("stack", "watchdog", "看门狗退出");
    }).detach();

    RK_LOG_INFO("stack", "watchdog", "看门狗启动: 间隔 %ums 栈余量阈值 %zu B",
                interval_ms, min_free_bytes);
}

void StackGuard::stop_watchdog() {
    state().watchdog_on.store(false, std::memory_order_relaxed);
}

void StackGuard::dump() {
    GuardState& st = state();
    std::lock_guard<std::mutex> lk(st.mu);
    RK_LOG_INFO("stack", "dump", "心跳计数 %llu  线程数 %zu",
                static_cast<unsigned long long>(
                    st.heartbeat.load(std::memory_order_relaxed)),
                st.threads.size());
    for (const auto& kv : st.threads) {
        size_t used = 0;
        size_t freeb = 0;
        if (kv.second.has_depth) {
            used = kv.second.stack_high - kv.second.deepest;
            freeb = kv.second.deepest - kv.second.stack_low;
        }
        RK_LOG_INFO("stack", "dump", "  tid=%-6u name=%-14s 已用 %8zu B 剩余 %8zu B",
                    kv.second.tid, kv.second.name.c_str(), used, freeb);
    }
}

}  // namespace rk
