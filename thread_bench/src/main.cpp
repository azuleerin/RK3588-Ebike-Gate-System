// thread_bench —— RK3588 三核 NPU 极限并发测试台
//
// ★ 它【不接入主工程】: 不共用 rk3588_zero_copy 的任何代码, 不改它的
//   CMakeLists, 只用板上的 librknnrt.so + rknn_api.h。
//   目的是在隔离环境里单独回答:
//     "多路并发时, 最佳的工作线程数 T 是多少? 核该怎么绑?"
//     "T > 3 到底会不会把 CPU 抢占 / DDR 带宽打爆?"
//
// ★ 四条红线(见 README):
//   零拷贝    : rknn_create_mem + rknn_set_io_mem, 热循环里 0 次 memcpy
//   内存平线  : 启动时一次性分配完, 测量窗口内 0 次 malloc/free
//   纯 C++    : 只用 std::thread / mutex / condition_variable
//   丢帧防雪崩: 队列满丢最旧, 生产者永不阻塞
//
// 用法:
//   ./thread_bench --model ../models/ebike_int8_rk2332.rknn
//       --threads 4,6,8,12 --strategy auto,round,all3 --queue-size 64
//       --channels 4 --fps 25 --duration 30 --warmup 5
//
//   --model2 ../models/ppocrv4_rec_int8.rknn   # 加上第二个模型(串联)
//   --producer-mode unlimited                   # Producer 狂推, 压防雪崩
//   --queue-size 1,64,128                       # 队列容量边界
//
// 每组配置在 stdout 打一行 JSON, 方便脚本汇总。
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "tb/bench_types.h"
#include "tb/frame_queue.h"
#include "tb/latency_rec.h"
#include "tb/npu_worker.h"
#include "tb/sys_probe.h"

namespace tb {
namespace {

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
inline int64_t now_ns() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

void sleep_until_ns(int64_t target_ns) {
    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(target_ns / 1000000000LL);
    ts.tv_nsec = static_cast<long>(target_ns % 1000000000LL);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
        // 被信号打断 -> 重睡到同一个绝对时刻
    }
}

bool parse_int_list(const std::string& s, std::vector<int>* out) {
    out->clear();
    size_t pos = 0;
    while (pos <= s.size()) {
        const size_t comma = s.find(',', pos);
        const std::string tok = s.substr(pos, comma == std::string::npos
                                                 ? std::string::npos
                                                 : comma - pos);
        if (!tok.empty()) {
            char* end = nullptr;
            const long v = strtol(tok.c_str(), &end, 10);
            if (!end || *end != '\0' || v <= 0) return false;
            out->push_back(static_cast<int>(v));
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return !out->empty();
}

bool parse_size_list(const std::string& s, std::vector<size_t>* out) {
    out->clear();
    size_t pos = 0;
    while (pos <= s.size()) {
        const size_t comma = s.find(',', pos);
        const std::string tok = s.substr(pos, comma == std::string::npos
                                                 ? std::string::npos
                                                 : comma - pos);
        if (!tok.empty()) {
            char* end = nullptr;
            const long long v = strtoll(tok.c_str(), &end, 10);
            if (!end || *end != '\0' || v <= 0) return false;
            out->push_back(static_cast<size_t>(v));
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return !out->empty();
}

bool load_model(const std::string& path, ModelBlob* out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        fprintf(stderr, "[bench] 打不开模型文件: %s\n", path.c_str());
        return false;
    }
    const std::streamsize sz = f.tellg();
    if (sz <= 0) {
        fprintf(stderr, "[bench] 模型文件是空的: %s\n", path.c_str());
        return false;
    }
    f.seekg(0, std::ios::beg);
    out->path = path;
    out->bytes.resize(static_cast<size_t>(sz));
    if (!f.read(reinterpret_cast<char*>(out->bytes.data()), sz)) {
        fprintf(stderr, "[bench] 读模型文件失败: %s\n", path.c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// ★ 主线程 = Producer
//
//   normal   : 每路按 --fps 错相推流(合计 fps*channels)
//   unlimited: 死循环狂推, 无限速 —— 专门压"队列满 -> 丢最旧"这条路径
// ---------------------------------------------------------------------------
void producer_loop(const Config& cfg, FrameQueue* q, int64_t start_ns, int64_t end_ns) {
    const uint32_t ch_cnt = static_cast<uint32_t>(cfg.channels > 0 ? cfg.channels : 1);

    if (cfg.producer_mode == ProducerMode::kUnlimited) {
        uint64_t seq = 0;
        uint32_t ch = 0;
        uint64_t since_yield = 0;
        while (now_ns() < end_ns) {
            FrameQueue::Item it;
            it.channel = ch;
            it.seq = seq++;
            it.t_enqueue_ns = now_ns();
            q->push(it);
            ch = (ch + 1) % ch_cnt;
            // 每 N 帧让出一次 CPU: 不限速, 但不让生产者把 worker 饿死
            // (glibc 的 mutex 不保证公平, 不让的话可能出现真的饿死)。
            if (cfg.unlimited_yield_every && ++since_yield >= cfg.unlimited_yield_every) {
                since_yield = 0;
                std::this_thread::yield();
            }
        }
        return;
    }

    // 每 fps*channels 一拍, 轮流推各路 -> 每路正好 fps
    int64_t tick_ns = static_cast<int64_t>(1e9 / (cfg.fps * static_cast<double>(ch_cnt)));
    if (tick_ns < 1) tick_ns = 1;
    int64_t next = start_ns;
    uint64_t seq = 0;
    uint32_t ch = 0;
    for (;;) {
        next += tick_ns;
        if (next >= end_ns) break;
        sleep_until_ns(next);
        FrameQueue::Item it;
        it.channel = ch;
        it.seq = seq++;
        it.t_enqueue_ns = now_ns();
        q->push(it);
        ch = (ch + 1) % ch_cnt;
    }
}

// ---------------------------------------------------------------------------
// 跑一组配置
// ---------------------------------------------------------------------------
bool run_one_config(const Config& cfg, const ModelBlob& m1, const ModelBlob* m2,
                    int threads, Strategy strategy, size_t queue_size,
                    const std::vector<int>& big_cpus, const std::vector<int>& all_cpus,
                    RunResult* out) {
    const bool dual = (m2 != nullptr);

    // ---- 启动期一次性分配完(测量窗口内不再有任何分配) ----
    FrameQueue queue(queue_size);
    LatencyRecorder rec;
    SysProbe probe;
    probe.set_npu_load_path(cfg.npu_load_path);
    probe.open();

    std::vector<std::unique_ptr<NpuWorker>> workers;
    workers.reserve(static_cast<size_t>(threads));
    for (int i = 0; i < threads; ++i) {
        const int mask1 = strategy_mask(strategy, i);
        const int mask2 = (dual && cfg.model2_core_mask >= 0) ? cfg.model2_core_mask : mask1;
        auto w = std::make_unique<NpuWorker>(i, mask1, mask2, &m1, m2, &rec, &cfg, &queue);
        if (cfg.cpu_affinity == CpuAffinity::kBig) {
            w->set_cpus(big_cpus);
        } else if (cfg.cpu_affinity == CpuAffinity::kAll) {
            w->set_cpus(all_cpus);
        }
        w->start();
        workers.push_back(std::move(w));
    }

    // ---- 等所有 worker 把上下文建好(否则启动开销会污染统计) ----
    const int64_t ready_deadline = now_ns() + 120ll * 1000000000ll;
    for (auto& w : workers) {
        while (!w->ready() && !w->failed() && now_ns() < ready_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    bool bad = false;
    for (auto& w : workers) {
        if (w->failed()) {
            fprintf(stderr, "[bench] worker %d 初始化失败: %s\n", w->index(), w->error().c_str());
            bad = true;
        } else if (!w->ready()) {
            fprintf(stderr, "[bench] worker %d 初始化超时\n", w->index());
            bad = true;
        }
    }
    if (bad) {
        queue.stop();
        for (auto& w : workers) w->stop();
        for (auto& w : workers) w->join();
        return false;
    }

    // ---- 预热: 让 NPU 频率爬上来, 让 worker 进入稳定态 ----
    if (cfg.warmup_sec > 0.0) {
        const int64_t ws = now_ns();
        producer_loop(cfg, &queue, ws, ws + static_cast<int64_t>(cfg.warmup_sec * 1e9));
    }

    // ---- 测量窗口 ----
    const int64_t win_start = now_ns();
    const int64_t win_end = win_start + static_cast<int64_t>(cfg.duration_sec * 1e9);
    rec.reset(win_start);
    queue.reset_stats();
    for (auto& w : workers) w->reset_counters();

    ProbeSample ps0;
    probe.sample(&ps0);

    const long clk = SysProbe::clk_tck();
    const long ncpu = sysconf(_SC_NPROCESSORS_ONLN);

    // ---- 资源监控线程: 每秒打印一次 ----
    std::atomic<bool> mon_stop{false};
    double npu_load_max[3] = {-1.0, -1.0, -1.0};
    std::thread mon([&] {
        ProbeSample prev = ps0;
        uint64_t prev_frames = 0;
        while (!mon_stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            if (mon_stop.load(std::memory_order_relaxed)) break;
            ProbeSample s;
            if (!probe.sample(&s)) break;
            for (int c = 0; c < 3; ++c) {
                if (s.npu_load[c] >= 0.0 && s.npu_load[c] > npu_load_max[c]) {
                    npu_load_max[c] = s.npu_load[c];
                }
            }
            if (!cfg.quiet) {
                const double dt = static_cast<double>(s.ts_ns - prev.ts_ns) / 1e9;
                const double cpu = (dt > 0.0)
                    ? ((static_cast<double>(s.cpu_ticks - prev.cpu_ticks) / clk) / dt * 100.0 /
                       static_cast<double>(ncpu))
                    : 0.0;
                uint64_t frames = 0;
                for (auto& w : workers) frames += w->frames();
                // 区间吞吐(不是累计值)
                const double fps = (dt > 0.0)
                    ? static_cast<double>(frames - prev_frames) / dt
                    : 0.0;
                prev_frames = frames;
                char load[64] = "n/a";
                if (s.npu_load[0] >= 0.0) {
                    snprintf(load, sizeof(load), "%.0f/%.0f/%.0f%%",
                             s.npu_load[0], s.npu_load[1], s.npu_load[2]);
                }
                fprintf(stderr,
                        "[bench] T=%-2d %-5s q=%-3zu | %5.1fs 吞吐=%7.2f fps 队列=%3zu "
                        "丢帧=%-10llu CPU=%5.1f%% RSS=%4lldMB 堆=%6lldKB NPU=%s\n",
                        threads, strategy_name(strategy), queue_size,
                        static_cast<double>(s.ts_ns - win_start) / 1e9,
                        fps, queue.size(),
                        static_cast<unsigned long long>(queue.dropped()),
                        cpu, static_cast<long long>(s.rss_kb / 1024),
                        static_cast<long long>(s.heap_kb), load);
                fflush(stderr);
            }
            prev = s;
        }
    });

    // ---- ★ 主线程当 Producer 推流 ----
    producer_loop(cfg, &queue, win_start, win_end);
    const int64_t win_end_actual = now_ns();

    mon_stop.store(true, std::memory_order_relaxed);
    mon.join();

    // ---- 收尾: 停队列 -> 停 worker(各自线程内销毁自己的 ctx) ----
    const uint64_t leftover = queue.size();
    queue.stop();
    for (auto& w : workers) w->stop();
    for (auto& w : workers) w->join();

    ProbeSample ps1;
    probe.sample(&ps1);

    // ================= 汇总 =================
    double win_sec =
        static_cast<double>(win_end_actual - win_start) / 1e9;
    if (win_sec <= 0.0) win_sec = 1e-9;

    out->threads = threads;
    out->strategy = strategy;
    out->queue_size = queue_size;
    out->producer_mode = cfg.producer_mode;
    out->channels = cfg.channels;
    out->duration_sec = win_sec;
    out->dual_model = dual;

    for (auto& w : workers) {
        out->frames += w->frames();
        out->errors += w->errors();
        for (int c = 0; c < 16; ++c) out->chan_frames[c] += w->chan_frames(c);
    }
    out->pushed = queue.pushed();
    out->dropped = queue.dropped();
    out->leftover = leftover;
    out->throughput_fps = static_cast<double>(out->frames) / win_sec;
    out->arrival_fps = static_cast<double>(out->pushed) / win_sec;
    out->drop_rate = out->pushed ? static_cast<double>(out->dropped) / static_cast<double>(out->pushed)
                                 : 0.0;

    for (int s = 0; s < kLatSeriesCount; ++s) out->lat[s] = rec.stat(s);

    // CPU
    const double cpu_sec = static_cast<double>(ps1.cpu_ticks - ps0.cpu_ticks) / clk;
    out->cpu_pct = cpu_sec / win_sec * 100.0 / static_cast<double>(ncpu);

    // 内存
    out->rss_peak_kb = std::max(ps1.hwm_kb, ps1.rss_kb);
    out->rss_end_kb = ps1.rss_kb;
    out->heap_slope_kb_min = static_cast<double>(ps1.heap_kb - ps0.heap_kb) / win_sec * 60.0;

    // NPU 分核占用
    //
    //   ★ 为什么要按"该核上的 worker 数"归一化
    //     同一颗核上挂 N 个上下文时, 它们【串行】跑。驱动给每次推理报的
    //     run_duration 含"等前面那个上下文让出核"的时间, 于是把 N 个 worker
    //     的时长加起来 = N × 窗口(核饱和时), 直接除以窗口会得到 N00%,
    //     看起来像超载 —— 其实那正是"核被抢"的表现, 不是利用率。
    //     所以: 核占用 = Σ时长 / (该核 worker 数 × 窗口), 上限 100%。
    //     原始的和另外报在 core_m1_sum_pct 里, 不藏。
    double core1[3] = {0.0, 0.0, 0.0};
    double core2[3] = {0.0, 0.0, 0.0};
    int cnt1[3] = {0, 0, 0};
    int cnt2[3] = {0, 0, 0};
    double agg_busy = 0.0;
    double agg_compute = 0.0;
    for (auto& w : workers) {
        const double b1 = w->busy1_ms();
        const double b2 = w->busy2_ms();
        const double n1 = w->npu1_ms();
        const double n2 = w->npu2_ms();
        agg_busy += (b1 + b2) / (win_sec * 1000.0) * 100.0;
        agg_compute += (n1 + n2) / (win_sec * 1000.0) * 100.0;
        for (int c = 0; c < 3; ++c) {
            if (w->core_mask1() & (1 << c)) { core1[c] += n1; ++cnt1[c]; }
            if (w->core_mask2() & (1 << c)) { core2[c] += n2; ++cnt2[c]; }
        }
    }
    out->npu_busy_agg_pct = agg_busy;
    out->npu_compute_agg_pct = agg_compute;

    // 每核的【总】占用: 一个 worker 的两个模型在同一个核上是串行的, 必须先加总
    // 再按"该核上的 worker 数"归一化, 否则双模型场景会低估一半。
    int workers_on_core[3] = {0, 0, 0};
    double core_total[3] = {0.0, 0.0, 0.0};
    for (auto& w : workers) {
        const double n1 = w->npu1_ms();
        const double n2 = w->npu2_ms();
        const int m1 = w->core_mask1();
        const int m2 = w->core_mask2();
        for (int c = 0; c < 3; ++c) {
            double t = 0.0;
            if (m1 & (1 << c)) t += n1;
            if (m2 & (1 << c)) t += n2;
            if (t > 0.0) {
                core_total[c] += t;
                ++workers_on_core[c];
            }
        }
    }
    for (int c = 0; c < 3; ++c) {
        if (workers_on_core[c] > 0) {
            const double raw = core_total[c] / (win_sec * 1000.0) * 100.0;
            out->core_total_sum_pct[c] = raw;
            const double norm = raw / workers_on_core[c];
            out->core_total_pct[c] = norm > 100.0 ? 100.0 : norm;
        }
        out->workers_per_core1[c] = cnt1[c];
        out->workers_per_core2[c] = cnt2[c];
        if (cnt1[c] > 0) {
            const double raw = core1[c] / (win_sec * 1000.0) * 100.0;
            out->core_m1_sum_pct[c] = raw;
            const double norm = raw / cnt1[c];
            out->core_m1_pct[c] = norm > 100.0 ? 100.0 : norm;
        }
        if (cnt2[c] > 0) {
            const double raw = core2[c] / (win_sec * 1000.0) * 100.0;
            out->core_m2_sum_pct[c] = raw;
            const double norm = raw / cnt2[c];
            out->core_m2_pct[c] = norm > 100.0 ? 100.0 : norm;
        }
    }

    // debugfs NPU load
    for (int c = 0; c < 3; ++c) {
        out->npu_load[c] = ps1.npu_load[c];
        out->npu_load_max[c] = npu_load_max[c];
    }
    out->npu_load_ok = probe.npu_load_available();

    // NPU 侧内存(单 worker 的两个 ctx 合计)
    if (!workers.empty()) {
        out->npu_weight_bytes = workers.front()->npu_weight_bytes();
        out->npu_internal_bytes = workers.front()->npu_internal_bytes();
        out->npu_dma_bytes = workers.front()->npu_dma_bytes();
    }
    return true;
}

// ---------------------------------------------------------------------------
// JSON 输出
// ---------------------------------------------------------------------------
void print_series(FILE* f, const char* name, const SeriesStat& s) {
    fprintf(f,
            "\"%s\":{\"count\":%llu,\"avg_ms\":%.3f,\"p50_ms\":%.3f,\"p90_ms\":%.3f,"
            "\"p99_ms\":%.3f,\"max_ms\":%.3f,\"min_ms\":%.3f}",
            name, static_cast<unsigned long long>(s.count), s.avg_ms, s.p50_ms, s.p90_ms,
            s.p99_ms, s.max_ms, s.min_ms);
}

void print_json_line(FILE* f, const RunResult& r) {
    fprintf(f, "{\"threads\":%d,\"strategy\":\"%s\",\"queue_size\":%zu,\"producer\":\"%s\",",
            r.threads, strategy_name(r.strategy), r.queue_size,
            producer_mode_name(r.producer_mode));
    fprintf(f, "\"channels\":%d,\"duration_s\":%.3f,\"dual_model\":%s,",
            r.channels, r.duration_sec, r.dual_model ? "true" : "false");
    fprintf(f, "\"frames\":%llu,\"pushed\":%llu,\"dropped\":%llu,\"leftover\":%llu,",
            static_cast<unsigned long long>(r.frames),
            static_cast<unsigned long long>(r.pushed),
            static_cast<unsigned long long>(r.dropped),
            static_cast<unsigned long long>(r.leftover));
    fprintf(f, "\"throughput_fps\":%.3f,\"arrival_fps\":%.3f,\"drop_rate\":%.6f,",
            r.throughput_fps, r.arrival_fps, r.drop_rate);

    print_series(f, "lat_e2e", r.lat[kLatE2E]);
    fputc(',', f);
    print_series(f, "lat_m1", r.lat[kLatModel1]);
    fputc(',', f);
    print_series(f, "lat_m2", r.lat[kLatModel2]);
    fputc(',', f);

    fprintf(f, "\"cpu_pct\":%.2f,\"rss_peak_kb\":%lld,\"rss_end_kb\":%lld,"
               "\"heap_slope_kb_min\":%.3f,\"npu_busy_agg_pct\":%.2f,"
               "\"npu_compute_agg_pct\":%.2f,",
            r.cpu_pct, static_cast<long long>(r.rss_peak_kb),
            static_cast<long long>(r.rss_end_kb), r.heap_slope_kb_min, r.npu_busy_agg_pct,
            r.npu_compute_agg_pct);
    fprintf(f, "\"core_total_pct\":[%.2f,%.2f,%.2f],\"core_total_sum_pct\":[%.2f,%.2f,%.2f],",
            r.core_total_pct[0], r.core_total_pct[1], r.core_total_pct[2],
            r.core_total_sum_pct[0], r.core_total_sum_pct[1], r.core_total_sum_pct[2]);
    fprintf(f, "\"core_m1_pct\":[%.2f,%.2f,%.2f],\"core_m2_pct\":[%.2f,%.2f,%.2f],",
            r.core_m1_pct[0], r.core_m1_pct[1], r.core_m1_pct[2],
            r.core_m2_pct[0], r.core_m2_pct[1], r.core_m2_pct[2]);
    fprintf(f, "\"core_m1_sum_pct\":[%.2f,%.2f,%.2f],\"core_m2_sum_pct\":[%.2f,%.2f,%.2f],",
            r.core_m1_sum_pct[0], r.core_m1_sum_pct[1], r.core_m1_sum_pct[2],
            r.core_m2_sum_pct[0], r.core_m2_sum_pct[1], r.core_m2_sum_pct[2]);
    fprintf(f, "\"workers_per_core1\":[%d,%d,%d],\"workers_per_core2\":[%d,%d,%d],",
            r.workers_per_core1[0], r.workers_per_core1[1], r.workers_per_core1[2],
            r.workers_per_core2[0], r.workers_per_core2[1], r.workers_per_core2[2]);
    fprintf(f, "\"npu_load\":[%.1f,%.1f,%.1f],\"npu_load_max\":[%.1f,%.1f,%.1f],"
               "\"npu_load_ok\":%s,",
            r.npu_load[0], r.npu_load[1], r.npu_load[2],
            r.npu_load_max[0], r.npu_load_max[1], r.npu_load_max[2],
            r.npu_load_ok ? "true" : "false");
    fprintf(f, "\"npu_weight_bytes\":%llu,\"npu_internal_bytes\":%llu,\"npu_dma_bytes\":%llu,",
            static_cast<unsigned long long>(r.npu_weight_bytes),
            static_cast<unsigned long long>(r.npu_internal_bytes),
            static_cast<unsigned long long>(r.npu_dma_bytes));
    fprintf(f, "\"errors\":%llu}\n", static_cast<unsigned long long>(r.errors));
}

// ---------------------------------------------------------------------------
// 参数解析
// ---------------------------------------------------------------------------
void print_usage() {
    fprintf(stderr,
        "thread_bench —— RK3588 三核 NPU 极限并发测试台\n"
        "\n"
        "用法: thread_bench --model <model.rknn> [选项]\n"
        "\n"
        "  --model <path>            主模型(检测)             [必填]\n"
        "  --model2 <path>           第二模型(识别), 串联在同一 worker 里\n"
        "  --model2-core-mask <n>    第二模型的核掩码 0-7; 省略 = 与主模型同掩码\n"
        "  --threads <list>          工作线程数, 逗号分隔   [默认 4,6,8,12]\n"
        "  --strategy <list>         绑定策略 auto,round,all3 [默认 三个都跑]\n"
        "  --queue-size <list>       队列容量, 逗号分隔       [默认 64]\n"
        "  --channels <n>            模拟几路摄像头             [默认 4]\n"
        "  --fps <f>                 每路帧率                   [默认 25]\n"
        "  --producer-mode <m>       normal | unlimited         [默认 normal]\n"
        "  --warmup <sec>            预热秒数(不计入统计)      [默认 5]\n"
        "  --duration <sec>          测量窗口秒数               [默认 30]\n"
        "  --cpu-affinity <m>        none | big | all           [默认 none]\n"
        "  --per-channel-buffers     每 worker 每路各一块输入缓冲(默认关)\n"
        "  --npu-load-path <path>    debugfs NPU load 节点      [默认自动探测]\n"
        "  --out <path>              额外把 JSONL 写到文件\n"
        "  --quiet                   不打印每秒监控\n"
        "  -h, --help                本帮助\n");
}

bool parse_args(int argc, char** argv, Config* cfg) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                fprintf(stderr, "[bench] %s 缺少参数\n", name);
                return nullptr;
            }
            return argv[++i];
        };

        if (a == "--model") {
            const char* v = need("--model");
            if (!v) return false;
            cfg->model = v;
        } else if (a == "--model2") {
            const char* v = need("--model2");
            if (!v) return false;
            cfg->model2 = v;
        } else if (a == "--model2-core-mask") {
            const char* v = need("--model2-core-mask");
            if (!v) return false;
            const int m = atoi(v);
            if (m < 0 || m > 7) {
                fprintf(stderr, "[bench] --model2-core-mask 必须是 0-7\n");
                return false;
            }
            cfg->model2_core_mask = m;
        } else if (a == "--threads") {
            const char* v = need("--threads");
            if (!v || !parse_int_list(v, &cfg->threads)) return false;
        } else if (a == "--strategy") {
            const char* v = need("--strategy");
            if (!v) return false;
            cfg->strategies.clear();
            std::string s = v;
            size_t pos = 0;
            while (pos <= s.size()) {
                const size_t comma = s.find(',', pos);
                const std::string tok = s.substr(pos, comma == std::string::npos
                                                         ? std::string::npos
                                                         : comma - pos);
                if (!tok.empty()) {
                    if (tok == "auto") cfg->strategies.push_back(Strategy::kAuto);
                    else if (tok == "round") cfg->strategies.push_back(Strategy::kRound);
                    else if (tok == "all3") cfg->strategies.push_back(Strategy::kAll3);
                    else {
                        fprintf(stderr, "[bench] 未知策略: %s\n", tok.c_str());
                        return false;
                    }
                }
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            if (cfg->strategies.empty()) return false;
        } else if (a == "--queue-size") {
            const char* v = need("--queue-size");
            if (!v || !parse_size_list(v, &cfg->queue_sizes)) return false;
        } else if (a == "--channels") {
            const char* v = need("--channels");
            if (!v) return false;
            cfg->channels = atoi(v);
            if (cfg->channels <= 0 || cfg->channels > 16) {
                fprintf(stderr, "[bench] --channels 必须在 1-16\n");
                return false;
            }
        } else if (a == "--fps") {
            const char* v = need("--fps");
            if (!v) return false;
            cfg->fps = atof(v);
            if (cfg->fps <= 0.0) return false;
        } else if (a == "--producer-mode") {
            const char* v = need("--producer-mode");
            if (!v) return false;
            if (std::strcmp(v, "normal") == 0) cfg->producer_mode = ProducerMode::kNormal;
            else if (std::strcmp(v, "unlimited") == 0) cfg->producer_mode = ProducerMode::kUnlimited;
            else {
                fprintf(stderr, "[bench] --producer-mode 只能是 normal|unlimited\n");
                return false;
            }
        } else if (a == "--warmup") {
            const char* v = need("--warmup");
            if (!v) return false;
            cfg->warmup_sec = atof(v);
        } else if (a == "--duration") {
            const char* v = need("--duration");
            if (!v) return false;
            cfg->duration_sec = atof(v);
            if (cfg->duration_sec <= 0.0) return false;
        } else if (a == "--cpu-affinity") {
            const char* v = need("--cpu-affinity");
            if (!v) return false;
            if (std::strcmp(v, "none") == 0) cfg->cpu_affinity = CpuAffinity::kNone;
            else if (std::strcmp(v, "big") == 0) cfg->cpu_affinity = CpuAffinity::kBig;
            else if (std::strcmp(v, "all") == 0) cfg->cpu_affinity = CpuAffinity::kAll;
            else {
                fprintf(stderr, "[bench] --cpu-affinity 只能是 none|big|all\n");
                return false;
            }
        } else if (a == "--per-channel-buffers") {
            cfg->per_channel_buffers = true;
        } else if (a == "--npu-load-path") {
            const char* v = need("--npu-load-path");
            if (!v) return false;
            cfg->npu_load_path = v;
        } else if (a == "--out") {
            const char* v = need("--out");
            if (!v) return false;
            cfg->out_json = v;
        } else if (a == "--quiet") {
            cfg->quiet = true;
        } else if (a == "-h" || a == "--help") {
            print_usage();
            exit(0);
        } else {
            fprintf(stderr, "[bench] 未知参数: %s\n", a.c_str());
            print_usage();
            return false;
        }
    }
    if (cfg->model.empty()) {
        fprintf(stderr, "[bench] 必须指定 --model\n");
        return false;
    }
    return true;
}

}  // namespace
}  // namespace tb

int main(int argc, char** argv) {
    using namespace tb;

    Config cfg;
    if (!parse_args(argc, argv, &cfg)) {
        print_usage();
        return 2;
    }

    // ---- 模型(启动期一次性读入) ----
    ModelBlob m1;
    if (!load_model(cfg.model, &m1)) return 1;
    ModelBlob m2;
    const bool dual = !cfg.model2.empty();
    if (dual && !load_model(cfg.model2, &m2)) return 1;

    // ---- CPU 亲和用的核列表 ----
    const std::vector<int> big_cpus = SysProbe::detect_big_cores();
    std::vector<int> all_cpus;
    const long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    for (long i = 0; i < ncpu; ++i) all_cpus.push_back(static_cast<int>(i));

    // ---- 双模型的内存预检 ----
    if (dual) {
        int max_threads = 0;
        for (int t : cfg.threads) max_threads = std::max(max_threads, t);
        const int64_t avail = SysProbe::mem_available_kb();
        const int64_t need_mb = static_cast<int64_t>(max_threads) *
                                static_cast<int64_t>((m1.bytes.size() + m2.bytes.size()) / (1024 * 1024) + 1);
        fprintf(stderr,
                "[bench] 双模型模式: 最多 %d 个 worker x 2 个上下文, 权重常驻约 %lld MB "
                "(系统可用 %lld MB)\n",
                max_threads, static_cast<long long>(need_mb),
                static_cast<long long>(avail / 1024));
        if (avail > 0 && need_mb * 1024 > avail * 0.8) {
            fprintf(stderr, "[bench] 警告: 可用内存可能不够, 建议减少 --threads\n");
        }
    }

    fprintf(stderr,
            "[bench] 模型1=%s%s  线程矩阵=%zu 组  策略=%zu 种  队列=%zu 档  "
            "路数=%d fps=%.1f  预热=%.0fs 窗口=%.0fs  producer=%s  cpu亲和=%s\n",
            cfg.model.c_str(), dual ? " (+model2)" : "", cfg.threads.size(),
            cfg.strategies.size(), cfg.queue_sizes.size(), cfg.channels, cfg.fps,
            cfg.warmup_sec, cfg.duration_sec, producer_mode_name(cfg.producer_mode),
            cpu_affinity_name(cfg.cpu_affinity));
    if (!big_cpus.empty()) {
        fprintf(stderr, "[bench] 大核(cpu_capacity 最大)=%d,%d,%d,%d\n", big_cpus[0],
                big_cpus.size() > 1 ? big_cpus[1] : 0, big_cpus.size() > 2 ? big_cpus[2] : 0,
                big_cpus.size() > 3 ? big_cpus[3] : 0);
    }
    fflush(stderr);

    FILE* fout = nullptr;
    if (!cfg.out_json.empty()) {
        fout = fopen(cfg.out_json.c_str(), "a");
        if (!fout) fprintf(stderr, "[bench] 打不开输出文件: %s\n", cfg.out_json.c_str());
    }

    int rc = 0;
    for (int t : cfg.threads) {
        for (Strategy st : cfg.strategies) {
            for (size_t q : cfg.queue_sizes) {
                RunResult r;
                if (!run_one_config(cfg, m1, dual ? &m2 : nullptr, t, st, q, big_cpus,
                                    all_cpus, &r)) {
                    fprintf(stderr, "[bench] 配置失败(T=%d 策略=%s q=%zu), 中止\n", t,
                            strategy_name(st), q);
                    rc = 1;
                    break;
                }
                print_json_line(stdout, r);
                fflush(stdout);
                if (fout) {
                    print_json_line(fout, r);
                    fflush(fout);
                }
            }
            if (rc) break;
        }
        if (rc) break;
    }

    if (fout) fclose(fout);
    return rc;
}
