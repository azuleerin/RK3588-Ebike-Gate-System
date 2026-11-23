// nlb/stat.h —— 预分配的分位数采样器
//
// ★ 红线: 测量窗口内【零动态内存申请】。
//   所以采样数组在构造时一次性分配好容量, 热循环里只做
//   "写一个 uint32 + 加一个计数", 没有任何 malloc。
//   分位数排序在【窗口结束之后】做, 而且用的是另一块同样预分配好的缓冲,
//   所以整个进程生命周期内的内存占用是恒定的。
#ifndef NLB_STAT_H_
#define NLB_STAT_H_

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

namespace nlb {

class LatencySeries {
public:
    LatencySeries() = default;

    // cap = 该序列最多采样多少个点。0 = 不采样。
    void reserve(size_t cap) {
        buf_.assign(cap, 0);
        sort_buf_.assign(cap, 0);
        cap_ = cap;
        n_ = 0;
    }

    // 热循环里唯一要调的函数: 无分支、无分配。
    inline void add(uint32_t us) {
        if (n_ < cap_) buf_[n_++] = us;
    }

    size_t count() const { return n_; }
    bool empty() const { return n_ == 0; }

    // 把另一条序列的样本并进来(多 worker 汇总时用)。
    // ★ dst 的容量必须事先 reserve 够(见 SeriesSet 的用法)。
    void merge_from(const LatencySeries& src) {
        for (size_t i = 0; i < src.n_; ++i) {
            if (n_ < cap_) buf_[n_++] = src.buf_[i];
        }
    }

    double mean() const {
        if (n_ == 0) return 0.0;
        uint64_t s = 0;
        for (size_t i = 0; i < n_; ++i) s += buf_[i];
        return static_cast<double>(s) / static_cast<double>(n_) / 1000.0;  // us -> ms
    }

    // 排序后的分位数(会改动内部缓冲, 因此只能在窗口结束后调)。
    // p 取 0..100。
    double pct(double p) {
        if (n_ == 0) return 0.0;
        std::copy(buf_.begin(), buf_.begin() + n_, sort_buf_.begin());
        std::sort(sort_buf_.begin(), sort_buf_.begin() + n_);
        size_t idx = static_cast<size_t>(p / 100.0 * static_cast<double>(n_));
        if (idx >= n_) idx = n_ - 1;
        return static_cast<double>(sort_buf_[idx]) / 1000.0;  // us -> ms
    }

    double max() const {
        if (n_ == 0) return 0.0;
        uint32_t m = buf_[0];
        for (size_t i = 1; i < n_; ++i) {
            if (buf_[i] > m) m = buf_[i];
        }
        return static_cast<double>(m) / 1000.0;
    }

    double min() const {
        if (n_ == 0) return 0.0;
        uint32_t m = buf_[0];
        for (size_t i = 1; i < n_; ++i) {
            if (buf_[i] < m) m = buf_[i];
        }
        return static_cast<double>(m) / 1000.0;
    }

private:
    std::vector<uint32_t> buf_;
    std::vector<uint32_t> sort_buf_;
    size_t cap_ = 0;
    size_t n_ = 0;
};

// ---------------------------------------------------------------------------
// 简单计数 / 累加器(热循环里用原子)
// ---------------------------------------------------------------------------
struct Counter {
    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<uint64_t> busy_us{0};     // Σ run+get 墙钟
    std::atomic<uint64_t> npu_us{0};      // Σ 驱动自报推理耗时
    std::atomic<uint64_t> rga_us{0};     // Σ RGA 墙钟

    void reset() {
        frames.store(0, std::memory_order_relaxed);
        errors.store(0, std::memory_order_relaxed);
        busy_us.store(0, std::memory_order_relaxed);
        npu_us.store(0, std::memory_order_relaxed);
        rga_us.store(0, std::memory_order_relaxed);
    }
};

}  // namespace nlb

#endif  // NLB_STAT_H_
