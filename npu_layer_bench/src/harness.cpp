// src/harness.cpp —— 多 worker 测量骨架的实现
#include "nlb/harness.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

#include "nlb/sys_probe.h"

namespace nlb {
namespace {

using clk = std::chrono::steady_clock;

double to_sec(const clk::time_point& t) {
    return std::chrono::duration<double>(t.time_since_epoch()).count();
}

// 两段式闸门:
//
//   阶段 1  worker: setup() 完成后 arrive(); 主线程等齐。
//   阶段 2  主线程: 算出 t0 / warmup_end / meas_end 并发布, 然后 open();
//            worker: wait_open() 返回后读时间戳, 开始跑。
//
// ★ 为什么分两段:
//   时间戳必须在【所有 worker 都建好上下文之后】才算, 否则第一个 worker
//   会比别的 worker 多等一段"建上下文"的时间, 它的第一个帧会被算成慢帧。
//   而时间戳又必须在 worker 开跑【之前】发布 —— 所以需要一个发布点。
class StartGate {
public:
    explicit StartGate(int n) : need_(n) {}

    // worker 侧: 报到
    void arrive() {
        {
            std::lock_guard<std::mutex> lk(m_);
            ++arrived_;
        }
        arrived_cv_.notify_all();
    }

    // worker 侧: 等放行
    void wait_open() {
        std::unique_lock<std::mutex> lk(m_);
        open_cv_.wait(lk, [this] { return open_; });
    }

    // 主线程侧: 等所有 worker 报到
    void wait_all_arrived() {
        std::unique_lock<std::mutex> lk(m_);
        arrived_cv_.wait(lk, [this] { return arrived_ >= need_; });
    }

    // 主线程侧: 发布时间戳后放行
    void open() {
        {
            std::lock_guard<std::mutex> lk(m_);
            open_ = true;
        }
        open_cv_.notify_all();
    }

private:
    std::mutex m_;
    std::condition_variable arrived_cv_;
    std::condition_variable open_cv_;
    int need_;
    int arrived_ = 0;
    bool open_ = false;
};

}  // namespace

RunResult run_harness(const std::string& case_name, Layer layer,
                       const std::vector<int>& cores,
                       std::vector<WorkerTask*>& tasks,
                       const HarnessOpts& opts, const Config& cfg,
                       int repeat) {
    (void)cfg;
    const int n = static_cast<int>(cores.size());

    RunResult res;
    res.layer = layer;
    res.case_name = case_name;
    res.repeat = repeat;
    res.warmup_s = opts.warmup_sec;

    // ---- 采样缓冲预分配(测量窗口内零分配) ----
    std::vector<WorkerOut> outs(n);
    LatencySeries merged_det, merged_ocr, merged_rga, merged_frame;
    merged_det.reserve(opts.samples_per_worker * static_cast<size_t>(n));
    merged_ocr.reserve(opts.samples_per_worker * static_cast<size_t>(n));
    merged_rga.reserve(opts.samples_per_worker * static_cast<size_t>(n));
    merged_frame.reserve(opts.samples_per_worker * static_cast<size_t>(n));

    StartGate gate(n);
    std::vector<std::string> errs(static_cast<size_t>(n));

    // 测量窗口边界(所有 worker 共用)
    double warmup_end = 0.0;
    double meas_end = 0.0;

    for (int i = 0; i < n; ++i) {
        outs[i].ser_det.reserve(opts.samples_per_worker);
        outs[i].ser_ocr.reserve(opts.samples_per_worker);
        outs[i].ser_rga.reserve(opts.samples_per_worker);
        outs[i].ser_frame.reserve(opts.samples_per_worker);
        outs[i].core = cores[static_cast<size_t>(i)];
    }

    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(n));

    for (int i = 0; i < n; ++i) {
        threads.emplace_back([&, i]() {
            WorkerOut& o = outs[static_cast<size_t>(i)];

            // ★★★ setup() 必须在本线程里调用:
            //   RKNN 上下文在 A 线程建、拿 B 线程用, 运行时内部会死锁。
            //   所以"建上下文"和"跑推理"必须发生在同一个线程。
            std::string err;
            if (!tasks[static_cast<size_t>(i)]->setup(i, cores[static_cast<size_t>(i)], &err)) {
                errs[static_cast<size_t>(i)] = err;
                gate.arrive();
                return;
            }

            gate.arrive();
            gate.wait_open();

            const double t_warm = warmup_end;
            const double t_end = meas_end;

            while (true) {
                FrameTiming ft;
                const bool ok = tasks[static_cast<size_t>(i)]->run_frame(i, &ft);
                const double t_now = to_sec(clk::now());
                if (t_now >= t_end) break;

                if (!ok) {
                    o.errors++;
                    continue;
                }
                // 只统计"跑完落在测量窗口内"的帧
                if (t_now < t_warm) continue;

                o.frames++;
                o.det_busy_us += ft.det_busy_us;
                o.det_npu_us += ft.det_npu_us;
                o.ocr_busy_us += ft.ocr_busy_us;
                o.ocr_npu_us += ft.ocr_npu_us;
                o.rga_us += ft.rga_us;
                o.frame_us += ft.frame_us;

                if (ft.det_busy_us) o.ser_det.add(static_cast<uint32_t>(ft.det_busy_us));
                if (ft.ocr_busy_us) o.ser_ocr.add(static_cast<uint32_t>(ft.ocr_busy_us));
                if (ft.rga_us) o.ser_rga.add(static_cast<uint32_t>(ft.rga_us));
                if (ft.frame_us) o.ser_frame.add(static_cast<uint32_t>(ft.frame_us));
            }
            tasks[static_cast<size_t>(i)]->teardown();
        });
    }

    // 主线程: 等所有 worker 就绪 -> 算时间戳 -> 放行 -> 采样资源
    gate.wait_all_arrived();
    const double t_start = to_sec(clk::now());
    warmup_end = t_start + opts.warmup_sec;
    meas_end = warmup_end + opts.duration_sec;
    gate.open();

    SysProbe probe;
    probe.sample_cpu();   // 建立基线
    std::vector<double> npu_load_sum(3, 0.0);
    int npu_load_n = 0;

    const double sample_period = 0.5;
    double next_sample = t_start + sample_period;
    double freq_min = -1.0, freq_max = -1.0;
    while (true) {
        const double now = to_sec(clk::now());
        if (now >= meas_end) break;
        if (now >= next_sample) {
            probe.sample_cpu();
            if (probe.sample_npu_load()) {
                for (int c = 0; c < 3; ++c) npu_load_sum[static_cast<size_t>(c)] += probe.npu_load()[c];
                npu_load_n++;
            }
            if (probe.sample_npu_freq()) {
                const double f = probe.npu_freq_mhz();
                if (freq_min < 0 || f < freq_min) freq_min = f;
                if (f > freq_max) freq_max = f;
            }
            next_sample += sample_period;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    for (auto& t : threads) t.join();

    // ---- 汇总 ----
    uint64_t frames = 0, errors = 0;
    uint64_t det_busy = 0, det_npu = 0, ocr_busy = 0, ocr_npu = 0, rga_us = 0, frame_us = 0;
    double core_app[3] = {0, 0, 0};
    double core_drv[3] = {0, 0, 0};
    int ctx_per_core[3] = {0, 0, 0};

    for (int i = 0; i < n; ++i) {
        const WorkerOut& o = outs[static_cast<size_t>(i)];
        frames += o.frames;
        errors += o.errors;
        det_busy += o.det_busy_us;
        det_npu += o.det_npu_us;
        ocr_busy += o.ocr_busy_us;
        ocr_npu += o.ocr_npu_us;
        rga_us += o.rga_us;
        frame_us += o.frame_us;

        const int c = o.core;
        if (c >= 0 && c < 3) {
            ctx_per_core[c]++;
            core_app[c] += static_cast<double>(o.det_busy_us + o.ocr_busy_us);
            core_drv[c] += static_cast<double>(o.det_npu_us + o.ocr_npu_us);
        }

        merged_det.merge_from(o.ser_det);
        merged_ocr.merge_from(o.ser_ocr);
        merged_rga.merge_from(o.ser_rga);
        merged_frame.merge_from(o.ser_frame);
    }

    const double window = meas_end - warmup_end;
    res.duration_s = window;
    res.frames = frames;
    res.errors = errors;
    res.fps = window > 0 ? static_cast<double>(frames) / window : 0.0;

    if (frames > 0) {
        res.det_wall_ms = static_cast<double>(det_busy) / static_cast<double>(frames) / 1000.0;
        res.det_npu_ms = static_cast<double>(det_npu) / static_cast<double>(frames) / 1000.0;
        res.ocr_wall_ms = static_cast<double>(ocr_busy) / static_cast<double>(frames) / 1000.0;
        res.ocr_npu_ms = static_cast<double>(ocr_npu) / static_cast<double>(frames) / 1000.0;
        res.rga_ms = static_cast<double>(rga_us) / static_cast<double>(frames) / 1000.0;
        res.frame_ms = static_cast<double>(frame_us) / static_cast<double>(frames) / 1000.0;
    }

    res.det_p50 = merged_det.pct(50);
    res.det_p99 = merged_det.pct(99);
    res.det_max = merged_det.max();
    res.ocr_p50 = merged_ocr.pct(50);
    res.ocr_p99 = merged_ocr.pct(99);
    res.ocr_max = merged_ocr.max();
    res.frame_p50 = merged_frame.pct(50);
    res.frame_p99 = merged_frame.pct(99);
    res.frame_max = merged_frame.max();

    // 单帧总墙钟里, 扣掉"真正在算"的部分, 剩下的就是排队时间。
    res.queue_ms = res.frame_ms - res.det_wall_ms - res.ocr_wall_ms - res.rga_ms;
    if (res.queue_ms < 0) res.queue_ms = 0;

    // 分核归一化: 同核上挂 N 个上下文时, 它们的等待会互相重叠,
    // 所以要除以"该核上的上下文数", 上限才是 100%。
    for (int c = 0; c < 3; ++c) {
        res.ctx_per_core[c] = ctx_per_core[c];
        if (ctx_per_core[c] > 0) {
            res.core_app_pct[c] = 100.0 * core_app[c] / (window * 1e6 * ctx_per_core[c]);
            res.core_drv_pct[c] = 100.0 * core_drv[c] / (window * 1e6 * ctx_per_core[c]);
        }
    }

    res.cpu_pct = probe.cpu_pct();
    res.cpu_cores = probe.cpu_cores();
    res.rss_mb = probe.rss_mb();
    res.rss_peak_mb = probe.rss_peak_mb();
    res.npu_load_ok = npu_load_n > 0;
    if (npu_load_n > 0) {
        for (int c = 0; c < 3; ++c) {
            res.npu_load[c] = npu_load_sum[static_cast<size_t>(c)] / npu_load_n;
        }
    }
    res.npu_freq_min_mhz = freq_min;
    res.npu_freq_max_mhz = freq_max;

    for (int i = 0; i < n; ++i) {
        if (!errs[static_cast<size_t>(i)].empty()) {
            res.note += (res.note.empty() ? "" : "; ");
            res.note += "worker " + std::to_string(i) + ": " + errs[static_cast<size_t>(i)];
        }
    }

    return res;
}

}  // namespace nlb
