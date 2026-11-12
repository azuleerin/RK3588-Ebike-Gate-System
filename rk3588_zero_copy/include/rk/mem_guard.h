// rk/mem_guard.h —— 受控分配器 + canary 越界哨兵
//
// 为什么需要（AGENT_RULES_FOR_CRASH_DEBUGGING.md 第二节）:
//   "严禁全局 new / malloc 乱飞" + "任何越界写都会直接导致总线挂死"
//
// 机制:
//   每个受控分配的块前后加哨兵(canary), 释放时校验。
//   一旦发生越界写, 释放或巡检时立刻报出"哪个 tag 的块被踩了",
//   而不是等到几小时后的随机崩���。
//
// 内存布局:
//   +--------------------------+  <- 返回给调用方的指针 (Header+64)
//   | Header (64B, alignas64) |
//   +--------------------------+
//   | payload (用户数据)       |
//   +--------------------------+
//   | tail canary (32B)        |
//   +--------------------------+
//
// 物理属性: 普通堆内存(CPU 内存), 不是 DMA-BUF。
//   DMA-BUF 不能用这里的机制保护 —— DMA 绕过 CPU 缓存与 canary 校验,
//   它的保护靠 FdLedger/DmaGuard(校验 fd 与物理尺寸), 不是 canary。
//
// 申请者: 调用方(各最小化单元)
// 释放者: 同一个调用方, 必须用 MemGuard::free 释放
// 跨线程: MemGuard 自身线程安全(内部 mutex);
//         但**块本身不是线程安全的**, 谁分配谁用, 跨线程传递必须自行同步。
#ifndef RK_MEM_GUARD_H_
#define RK_MEM_GUARD_H_

#include <cstddef>
#include <cstdint>

namespace rk {

class MemGuard {
public:
    // 受控分配。tag 用于按用途统计与定位越界(如 "npu-input" / "rga-dst")。
    // 返回的指针已按 64 字节对齐; 失败返回 nullptr。
    static void* alloc(size_t bytes, const char* tag);

    // 释放。传入的不是受控分配的指针时, 记 ERROR 并拒绝释放(防止二次释放野指针)。
    static void free(void* p);

    // 模板版: 释放后把调用方的指针置空(规则要求"释放内存后必须将指针置空")
    template <typename T>
    static void free_ref(T*& p) {
        if (p) {
            free(static_cast<void*>(p));
            p = nullptr;
        }
    }

    // 巡检所有存活块的头尾哨兵。
    // 返回被破坏的块数量(0 = 全部完好)。发现破坏时逐个记 ERROR。
    static size_t check_canaries();

    // 当前受控内存总量 / 历史峰值(字节) —— 用于确定 cgroup MemoryMax
    static size_t current_bytes();
    static size_t peak_bytes();

    // 存活块数量
    static size_t live_blocks();

    // 按 tag 打印 当前/峰值/块数
    static void dump_by_tag();

    // 打印每块明细(发生越界后用来定位是哪个变量被踩)
    static void dump_blocks();

private:
    MemGuard() = delete;
};

}  // namespace rk

#endif  // RK_MEM_GUARD_H_
