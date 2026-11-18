// tb/latency_rec.cpp —— 多序列延迟采样器实现
#include "tb/latency_rec.h"

#include <algorithm>
#include <cstdio>

namespace tb {

LatencyRecorder::LatencyRecorder(size_t per_series)
    : gate_ns_(INT64_MIN) {
    cap_ = per_series ? per_series : 1;
    buf_.resize(kLatSeriesCount);
    for (int s = 0; s < kLatSeriesCount; ++s) buf_[s].resize(cap_);
    cnt_.reset(new std::atomic<uint64_t>[kLatSeriesCount]);
    over_.reset(new std::atomic<uint64_t>[kLatSeriesCount]);
    for (int s = 0; s < kLatSeriesCount; ++s) {
        cnt_[s].store(0, std::memory_order_relaxed);
        over_[s].store(0, std::memory_order_relaxed);
    }
}

LatencyRecorder::~LatencyRecorder() = default;

void LatencyRecorder::reset(int64_t gate_ns) {
    for (int s = 0; s < kLatSeriesCount; ++s) {
        cnt_[s].store(0, std::memory_order_relaxed);
        over_[s].store(0, std::memory_order_relaxed);
    }
    gate_ns_.store(gate_ns, std::memory_order_relaxed);
}

void LatencyRecorder::add(int series, uint32_t us) {
    if (series < 0 || series >= kLatSeriesCount) return;
    // fetch_add 抢一个全局唯一下标 —— 多 worker 无锁写入, 各写各的格子。
    const uint64_t i = cnt_[series].fetch_add(1, std::memory_order_relaxed);
    if (i < cap_) {
        buf_[series][static_cast<size_t>(i)] = us;
    } else {
        over_[series].fetch_add(1, std::memory_order_relaxed);
    }
}

uint64_t LatencyRecorder::count(int series) const {
    if (series < 0 || series >= kLatSeriesCount) return 0;
    return cnt_[series].load(std::memory_order_relaxed);
}

uint64_t LatencyRecorder::overflow(int series) const {
    if (series < 0 || series >= kLatSeriesCount) return 0;
    return over_[series].load(std::memory_order_relaxed);
}

SeriesStat LatencyRecorder::stat(int series) const {
    SeriesStat s;
    if (series < 0 || series >= kLatSeriesCount) return s;

    const uint64_t total = cnt_[series].load(std::memory_order_relaxed);
    const uint64_t n = std::min<uint64_t>(total, cap_);
    if (n == 0) return s;

    // ↓↓↓ 这里的临时数组分配是【测量窗口结束之后】的事, 不在热循环里 ↓↓↓
    std::vector<uint32_t> tmp(buf_[series].begin(),
                              buf_[series].begin() + static_cast<long>(n));
    std::sort(tmp.begin(), tmp.end());

    double sum = 0.0;
    for (uint32_t v : tmp) sum += v;

    const size_t last = tmp.size() - 1;
    auto pick = [&tmp, last](double q) -> double {
        size_t idx = static_cast<size_t>(static_cast<double>(tmp.size()) * q);
        if (idx > last) idx = last;
        return tmp[idx] / 1000.0;
    };

    s.count = total;
    s.avg_ms = sum / static_cast<double>(n) / 1000.0;
    s.min_ms = tmp.front() / 1000.0;
    s.max_ms = tmp.back() / 1000.0;
    s.p50_ms = pick(0.50);
    s.p90_ms = pick(0.90);
    s.p99_ms = pick(0.99);
    return s;
}

}  // namespace tb
