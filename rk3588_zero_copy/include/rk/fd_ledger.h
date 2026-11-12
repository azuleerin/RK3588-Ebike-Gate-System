// rk/fd_ledger.h —— 文件描述符台账（登记 + 与 /proc/self/fd 对账）
//
// 为什么需要（AGENT_RULES_FOR_CRASH_DEBUGGING.md 第二节）:
//   "涉及 DMA-BUF、mmap、物理地址指针传递的地方...必须校验文件描述符(fd)的有效性和大小边界。
//    任何越界写都会直接导致总线挂死。"
//
// 背景（本工程审计结论）:
//   P0-1 的机理是 —— 一帧用完后 av_frame_unref 释放了它, 但后面的代码仍在用
//   该帧的 fd。此时 fd 号可能已被内核回收并复用给别的对象(socket/串口/另一个
//   dma-buf), RGA 再拿这个 fd 去 importbuffer_fd, 就会把别人的内存映射进来,
//   然后往里 DMA 写 -> IOMMU fault -> 内核 oops -> 整机挂死。
//
//   台账能做两件事:
//     1) 登记每个 fd 的来源/大小/线程, 定期与 /proc/self/fd 对账
//        - 有 fd 用了没登记 -> 有代码绕过了台账(隐藏泄漏)
//        - 台账里有 fd 但 /proc/self/fd 里没了 -> 有人提前 close 了它(上面那种坑)
//     2) validate() 在把 fd 交给 RGA/NPU **之前**校验有效性与尺寸,
//        不合格就拒绝并记 ERROR
//
// 生命周期:
//   - 进程级单例, 内部 mutex
//   - 每个 fd 登记一次, 释放时注销一次
//   - 跨线程: 台账本身线程安全; fd 的**使用**仍由调用方保证不并发
#ifndef RK_FD_LEDGER_H_
#define RK_FD_LEDGER_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace rk {

class FdLedger {
public:
    // 打开并登记。
    static int open_tracked(const char* path, int flags, const char* tag);

    // 登记一个由别处创建的 fd(如 dma_buf_alloc 返回的)。
    // size 传 0 表示稍后用 fstat 补齐。
    static void track(int fd, const char* tag, size_t size);

    // dup 并登记(登记的是新的 fd 号)。
    static int dup_tracked(int fd, const char* tag);

    // 注销并关闭。
    static void close_tracked(int fd, const char* tag);

    // ---- 把 fd 交给硬件之前必须过的关 ----
    // 校验: fd 有效 + fstat 成功 + 尺寸足够。
    // 返回 false 时**不要**继续用这个 fd —— 继续用就是 P0-1 的现场。
    // 注意: 本函数只校验不修改行为, 调用方是否继续由调用方决定(带病复现需要)。
    static bool validate(int fd, size_t need_bytes, const char* tag);

    // 查询 fd 的实际字节数; 失败返回 0
    static size_t fstat_size(int fd);

    // 采集基线: 把当前打开的 fd 全部记为"已知基线"。
    //
    // 为什么需要: stdio(0/1/2)、日志文件、以及 RKNN runtime 自己打开的
    // /dev/dri/cardN 与 /dmabuf: 都不在台账里, 每次对账都会报一遍,
    // 24h 长跑时这种噪音会淹没真正的信号。
    // 在单元初始化完成后调用一次, 之后只报"基线之后新出现的未登记 fd"。
    static void capture_baseline();

    // 打印台账并与 /proc/self/fd 对账
    static void dump_open();

    static size_t tracked_count();
    // 对账结果: 未登记的 fd 数量
    static size_t untracked_count();
    // 对账结果: 台账登记了但系统里已经不在的 fd 数量
    static size_t vanished_count();

private:
    FdLedger() = delete;
};

}  // namespace rk

#endif  // RK_FD_LEDGER_H_
