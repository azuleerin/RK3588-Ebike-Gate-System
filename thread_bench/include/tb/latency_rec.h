// tb/latency_rec.h —— 多序列延迟采样器
//
// ★ 三条红线同时满足:
//   1) 热循环里【零分配】: 数组在启动时一次性分配好, 之后只往里写。
//   2) 多 worker 【无锁】写: 用 atomic fetch_add 抢一个全局唯一下标,
//      各自写自己那格, 不会互相覆盖。
//   3) 分位数在【测量窗口结束后】算(排序会分配临时数组, 那是循环外的事)。
//
// ★ 为什么要三条序列
//   E2E    : 入队 -> 结果就绪(含排队)。业务最关心的延迟。
//   MODEL1 : 模型1 的 run+get。真正占核的时间。
//   MODEL2 : 模型2 的 run+get。
//   双模型场景下, 如果 T 增大时 MODEL1 稳定而 MODEL2 劣化, 说明第二个模型
//   的权重/激活把 DDR 带宽挤爆了 —— 这是 MODEL1/MODEL2 分开记的唯一理由。
#ifndef TB_LATENCY_REC_H_
#define TB_LATENCY_REC_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "tb/bench_types.h"

namespace tb {

class LatencyRecorder {
public:
    // per_series: 每条序列最多记多少个样本(超出后只计数不存储)
    explicit LatencyRecorder(size_t per_series = (1u << 20));
    ~LatencyRecorder();

    // 复位统计并设置"时间闸门":
    //   只有 t_enqueue >= gate_ns 的帧才计入 —— 用来切掉预热期。
    void reset(int64_t gate_ns = INT64_MIN);

    int64_t gate() const { return gate_ns_.load(std::memory_order_relaxed); }

    // 记一个样本(单位: 微秒)
    void add(int series, uint32_t us);

    // 统计(会分配临时数组并排序 —— 只能在测量窗口结束后调用)
    SeriesStat stat(int series) const;

    uint64_t count(int series) const;
    uint64_t overflow(int series) const;

private:
    std::vector<std::vector<uint32_t>> buf_;
    std::unique_ptr<std::atomic<uint64_t>[]> cnt_;
    std::unique_ptr<std::atomic<uint64_t>[]> over_;
    std::atomic<int64_t> gate_ns_;
    size_t cap_ = 0;
};

}  // namespace tb

#endif  // TB_LATENCY_REC_H_
