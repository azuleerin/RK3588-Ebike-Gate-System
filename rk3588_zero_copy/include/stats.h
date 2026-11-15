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
    // 各阶段耗时(毫秒, 滑动窗口平均)
    RollingMean wait{60};      // 解码等待: 阻塞在网络/取帧上的时间(★ 判断源帧率的关键)
    RollingMean decode{60};    // 真正的解码时间(软解: h264 CPU 解码; 硬解: VPU)
    RollingMean rga{60};       // 预处理(RGA 或 CPU 兜底)
    RollingMean infer{60};     // NPU 推理(检测 + LPRNet)
    RollingMean npu_run{60};   // 其中: rknn_run 真正在 NPU 上跑的时间
    RollingMean npu_out{60};   // 其中: rknn_outputs_get 把输出搬回 CPU 的时间
                                //      (9 个输出的反量化+拷贝, 有时比推理还贵!)
    RollingMean post{60};      // 后处理(DFL 解码 + NMS + 几何过滤 + 跟踪 + 业务组装)
    RollingMean draw{60};      // 画框 + 显示
    RollingMean total{60};     // 整帧墙钟耗时
    RollingMean fps_hist{60};      // 瞬时 FPS 的滑动窗口（不用 EMA）
    RollingMean src_gap{60};       // 相邻两帧【解码出来】的间隔(ms) -> 相机实际推送间隔
    int    frames = 0;
    std::chrono::steady_clock::time_point last{};
    // 第一帧的时刻 —— 用来算【真实交付帧率】
    std::chrono::steady_clock::time_point first{};

    void on_frame(double total_ms) {
        total.push(total_ms);
        auto now = std::chrono::steady_clock::now();
        if (first.time_since_epoch().count() == 0) first = now;
        if (last.time_since_epoch().count() == 0) { last = now; frames++; return; }
        double sec = std::chrono::duration<double>(now - last).count();
        last = now;
        // ⚠️ 不要用 EMA: EMA 对相邻帧高度相关的样本做平滑, 只是把噪声延后, 没有削掉。
        //    这里用滑动窗口内的瞬时 FPS 求平均。
        if (sec > 0.0) fps_hist.push(1.0 / sec);
        frames++;
    }

    // ★ 真实交付帧率 = 处理到的帧数 / 从第一帧到现在的墙钟。
    //   和 fps() 的区别:
    //     fps()          = 1000/单帧处理耗时 -> 是【处理速率】
    //     delivered_fps() = 帧数/墙钟       -> 是【真的每秒交出去多少帧】
    //   源被限速时(本地文件按源帧率播)两者差很多: 前者可以到几百,
    //   后者被源卡在 60 —— 评估"这台板子能带几路"必须用后者。
    double delivered_fps() const {
        if (first.time_since_epoch().count() == 0) return 0.0;
        const double sec = std::chrono::duration<double>(last - first).count();
        if (sec <= 0.0) return 0.0;
        return static_cast<double>(frames - 1) / sec;
    }

    // 相邻两帧解码出来的间隔(毫秒) —— 相机/流的【实际】推送节奏
    void on_source_gap(double ms) {
        if (ms > 0.0 && ms < 5000.0) src_gap.push(ms);
    }

    // ★ 用【帧间隔均值】反推 FPS, 不要用 mean(1/秒):
    //   均值函数的均值(mean(1/x))会系统性大于 1/mean(x)(Jensen 不等式),
    //   帧间隔抖动越大偏得越多 —— 实测同一段 68.7ms/帧的运行,
    //   mean(1/sec) 报 17.97 fps, 而真实吞吐只有 14.55 fps, 差了 23%。
    double fps() const {
        const double m = total.mean();
        return (m > 0.01) ? (1000.0 / m) : 0.0;
    }

    // 瞬时 FPS 的滑动窗口(看抖动用, 不作为吞吐指标)
    double fps_instant_mean() const { return fps_hist.mean(); }

    // 相机实际推送帧率(由解码间隔反推); 样本不足时返回 0
    double source_fps() const {
        const double m = src_gap.mean();
        return (m > 0.01) ? (1000.0 / m) : 0.0;
    }

    // 一帧里【我们自己】花掉的总时间(不含等流的时间)
    double work_ms() const {
        return decode.mean() + rga.mean() + infer.mean() + post.mean() + draw.mean();
    }

    void dump() const {
        const double fps_v = fps();
        const double interval = (fps_v > 0.01) ? (1000.0 / fps_v) : 0.0;
        const double work = work_ms();
        fprintf(stderr,
                "\n===== 性能统计 (最近 %zu 帧) =====\n"
                "  流水线 FPS   : %.2f   (帧间隔 %.2f ms)\n"
                "  交付 FPS     : %.2f   (墙钟, 真正每秒交出去多少帧)\n"
                "  相机推送 FPS : %.2f   (解码间隔 %.2f ms)\n"
                "  ── 阶段耗时 ──\n"
                "  解码等待     : %.2f ms   ★ 阻塞在取流上(不算我们的计算)\n"
                "  解码         : %.2f ms\n"
                "  RGA 转换     : %.2f ms\n"
                "  NPU 推理     : %.2f ms  (其中 rknn_run %.2f / 取回输出 %.2f)\n"
                "  后处理       : %.2f ms\n"
                "  画框+显示    : %.2f ms\n"
                "  单帧总耗时   : %.2f ms\n"
                "  我们的活儿   : %.2f ms (占帧间隔 %.0f%%)\n",
                total.count(), fps_v, interval,
                delivered_fps(),
                source_fps(), src_gap.mean(),
                wait.mean(), decode.mean(), rga.mean(), infer.mean(),
                npu_run.mean(), npu_out.mean(),
                post.mean(), draw.mean(), total.mean(),
                work, interval > 0.0 ? (work * 100.0 / interval) : 0.0);

        // ---- 瓶颈判定 ----
        const char* verdict = "数据不足";
        if (interval > 0.0) {
            if (wait.mean() > interval * 0.5 && work < interval * 0.9) {
                verdict = "瓶颈=【源帧率/网络】: 等流时间占大头, 我们自己的活儿只占帧间隔"
                          "的一部分 —— 加算力也不会更快";
            } else if (post.mean() > work * 0.4) {
                verdict = "瓶颈=【CPU 后处理】: 后处理(DFL+NMS+跟踪)占比最高, 优化它";
            } else if (infer.mean() > work * 0.5) {
                verdict = "瓶颈=【NPU 算力】: 推理占比最高, 考虑量化/批处理/换小模型";
            } else if (decode.mean() > work * 0.5) {
                verdict = "瓶颈=【解码】: 软解 CPU 时间占比最高, 换硬解或降分辨率";
            } else {
                verdict = "瓶颈=分散: 各阶段耗时接近, 没有单点热点";
            }
        }
        fprintf(stderr, "  判定          : %s\n", verdict);
    }
};

}  // namespace rk

#endif  // RK_STATS_H_
