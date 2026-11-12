// stats.h —— 各阶段耗时统计
#ifndef RK_STATS_H_
#define RK_STATS_H_

#include <chrono>
#include <cstddef>
#include <deque>

namespace rk {

// 作用域计时器：析构时把耗时(毫秒)写回目标变量
class ScopedTimer {
public:
    explicit ScopedTimer(double* out_ms)
        : out_(out_ms), t0_(std::chrono::steady_clock::now()) {}
    ~ScopedTimer() {
        *out_ = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0_).count();
    }

private:
    double* out_;
    std::chrono::steady_clock::time_point t0_;
};

// 滑动窗口平均
class RollingMean {
public:
    explicit RollingMean(std::size_t n = 60) : cap_(n) {}
    void push(double v) {
        sum_ += v;
        buf_.push_back(v);
        if (buf_.size() > cap_) { sum_ -= buf_.front(); buf_.pop_front(); }
    }
    double mean() const {
        return buf_.empty() ? 0.0 : sum_ / static_cast<double>(buf_.size());
    }
    std::size_t count() const { return buf_.size(); }

private:
    std::size_t cap_;
    double sum_ = 0.0;
    std::deque<double> buf_;
};

struct PipelineStats {
    RollingMean decode{60};
    RollingMean rga{60};
    RollingMean infer{60};
    RollingMean draw{60};
    RollingMean total{60};
    RollingMean fps_hist{60};      // 瞬时 FPS 的滑动窗口（不用 EMA）
    int    frames = 0;
    std::chrono::steady_clock::time_point last{};

    void on_frame(double total_ms) {
        total.push(total_ms);
        auto now = std::chrono::steady_clock::now();
        if (last.time_since_epoch().count() == 0) { last = now; frames++; return; }
        double sec = std::chrono::duration<double>(now - last).count();
        last = now;
        // ⚠️ 不要用 EMA: EMA 对相邻帧高度相关的样本做平滑, 只是把噪声延后, 没有削掉。
        //    这里用滑动窗口内的瞬时 FPS 求平均。
        if (sec > 0.0) fps_hist.push(1.0 / sec);
        frames++;
    }

    double fps() const { return fps_hist.mean(); }

    void dump() const {
        fprintf(stderr,
                "\n===== 性能统计 (最近 %zu 帧) =====\n"
                "  流水线 FPS : %.2f\n"
                "  单帧总耗时 : %.2f ms\n"
                "  解码       : %.2f ms\n"
                "  RGA 转换   : %.2f ms\n"
                "  NPU 推理   : %.2f ms\n"
                "  画框+显示  : %.2f ms\n",
                total.count(), fps(), total.mean(), decode.mean(),
                rga.mean(), infer.mean(), draw.mean());
    }
};

}  // namespace rk

#endif  // RK_STATS_H_
