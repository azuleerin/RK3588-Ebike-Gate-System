// rk/stack_guard.h —— 线程栈水位监控 + 主循环心跳看门狗
//
// 为什么需要（AGENT_RULES_FOR_CRASH_DEBUGGING.md 第二、四节）:
//   - "堆栈监控": 栈溢出是野指针/递归失控的典型后果, 越早发现越好
//   - "关键路径打点": 需要能区分"程序卡死"与"程序正常但慢"
//
// 机制:
//   1) 线程注册时记录自己的栈区间 [stack_low, stack_high)(向下生长)
//   2) 线程在主循环里调 mark_stack(), 记录本次踩到的最深位置(栈水位)
//   3) 看门狗线程每 interval_ms 检查:
//        - 心跳计数是否在推进(没推进 = 主循环卡死)
//        - 各线程栈余量是否低于阈值
//      并把结果写进日志文件(WARN/ERROR 立即 fdatasync 落盘)
//
// 跨线程:
//   - 看门狗线程只读其他线程登记的信息, 不去动它们的内存
//   - 每条线程记录自带 mutex, mark_stack() 只写自己那条
//
// 生命周期:
//   - start_watchdog() 在 main() 早期调用一次, 内部起一个 detached 线程
//   - stop_watchdog() 在退出前调用; 进程退出也会自动结束
#ifndef RK_STACK_GUARD_H_
#define RK_STACK_GUARD_H_

#include <cstddef>
#include <cstdint>

namespace rk {

class StackGuard {
public:
    // 在线程入口处调用一次, 登记线程名并捕获栈区间。
    static void register_thread(const char* name);

    // 在线程主循环里调用, 记录当前栈深度(取历史最大值)。
    // 线程如果从不调用, 看门狗就只会报初始水位。
    static void mark_stack();

    // 主循环心跳: 每次完成一轮就调一次。
    static void beat();

    static uint64_t heartbeat_count();

    // 当前线程栈余量(字节); 未注册返回 0
    static size_t free_bytes();

    // 启动看门狗线程。
    //   min_free_bytes : 栈余量低于此值记 ERROR
    //   interval_ms    : 检查间隔
    static void start_watchdog(size_t min_free_bytes = 256 * 1024,
                               uint32_t interval_ms = 500);

    static void stop_watchdog();

    // 打印所有线程的栈水位与心跳
    static void dump();

private:
    StackGuard() = delete;
};

}  // namespace rk

#endif  // RK_STACK_GUARD_H_
