// npu_baseline_bench.cpp —— 纯推理基线测试程序(项4)
//
// 目的: 复测【单帧 NPU 纯推理耗时】, 与主工程 26.83ms 基线对表。
//
// ★ 计时口径与主工程严格对齐 (src/rk_base/npu_pool.cc:390-395):
//       t0 = now_ms();
//       rknn_run(ctx, nullptr);
//       rknn_outputs_get(ctx, n, outs, nullptr);
//       dt_npu = now_ms() - t0;
//   —— 只含 rknn_run + rknn_outputs_get, 不含任何前后处理 / RGA / 排队 / 跟踪。
//
// ★ 输入走 rknn_create_mem + rknn_set_io_mem (零拷贝), 与主工程检测路径
//   (npu_pool.cc:309/330) 同构; 也可用 --io memcpy 走 rknn_inputs_set,
//   用来量化【隐式 memcpy 的代价】。
//
// ★ 核绑定: 每线程一个独立 rknn_context, 绑到【单核】掩码(1/2/4),
//   与主工程 NpuPool 一致 (npu_pool.cc:134 core_mask = 1 << core)。
//   注意: 0x7(三核合一)已被驱动层禁用, 本程序同样不使用。
//
// ★ RKNN 上下文必须【在同一个线程里创建和销毁】, 跨线程会死锁。
//
// 用法:
//   npu_baseline_bench <model.rknn> [--iters N] [--warmup N] [--reps N]
//                      [--mode single|multi|both] [--cores 1,2,4]
//                      [--io zerocopy|memcpy] [--perfdetail] [--json]
//
//   --iters     每轮测量迭代数   (默认 100)
//   --warmup    每轮预热迭代数   (默认 20)
//   --reps      测量轮数         (默认 5, 取稳态平均值)
//   --mode      single=单核 / multi=多核并发 / both=都跑(默认 both)
//   --cores     multi 模式每线程的核掩码, 逗号分隔 (默认 1,2,4)
//   --io        zerocopy=rknn_create_mem+set_io_mem / memcpy=rknn_inputs_set
//   --perfdetail 打印驱动逐算子耗时表
//   --json      末尾输出机器可读结果

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "rknn_api.h"

namespace {

double NowMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Stat {
    std::vector<double> v;
    void push(double x) { v.push_back(x); }
    double mean() const {
        if (v.empty()) return 0;
        double s = 0;
        for (double x : v) s += x;
        return s / v.size();
    }
    // 最近秩百分位(第 p 百分位的秩 = ceil(p/100 * N), 下标 -1)
    double pct(double p) const {
        if (v.empty()) return 0;
        std::vector<double> t = v;
        std::sort(t.begin(), t.end());
        double rank = p / 100.0 * t.size();
        size_t i = static_cast<size_t>(std::ceil(rank));
        if (i == 0) i = 1;
        if (i > t.size()) i = t.size();
        return t[i - 1];
    }
    double min() const { return v.empty() ? 0 : *std::min_element(v.begin(), v.end()); }
    double max() const { return v.empty() ? 0 : *std::max_element(v.begin(), v.end()); }
};

void PrintStat(const char* name, const Stat& s) {
    if (s.v.empty()) return;
    printf("    %-12s mean %8.3f ms | min %8.3f | p50 %8.3f | p90 %8.3f | p95 %8.3f | "
           "p99 %8.3f | max %8.3f\n",
           name, s.mean(), s.min(), s.pct(50), s.pct(90), s.pct(95), s.pct(99), s.max());
}

// 单线程一轮的测量结果
struct RoundResult {
    Stat lat;        // rknn_run + rknn_outputs_get  (== 主工程 dt_npu)
    Stat run;        // 只 rknn_run
    Stat get;        // 只 rknn_outputs_get
    Stat setup;      // 只输入绑定(memcpy 模式下 = rknn_inputs_set)
    int64_t drv_us = 0;   // 驱动自报 run_duration
};

// 一个 worker 上下文: 自建、自用、自毁(同线程)
class Ctx {
public:
    Ctx(const char* model, int core_mask, bool zerocopy) {
        uint32_t flags = RKNN_FLAG_PRIOR_HIGH;
        int ret = rknn_init(&ctx_, const_cast<char*>(model), 0, flags, nullptr);
        if (ret != RKNN_SUCC) {
            fprintf(stderr, "[FATAL] rknn_init(%s) = %d\n", model, ret);
            std::exit(1);
        }
        rknn_set_core_mask(ctx_, static_cast<rknn_core_mask>(core_mask));

        rknn_input_output_num io{};
        if (rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)) != RKNN_SUCC) {
            fprintf(stderr, "[FATAL] rknn_query(IN_OUT_NUM) 失败\n");
            std::exit(1);
        }
        n_out_ = io.n_output;

        rknn_tensor_attr in{};
        memset(&in, 0, sizeof(in));
        if (rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, &in, sizeof(in)) != RKNN_SUCC) {
            fprintf(stderr, "[FATAL] rknn_query(INPUT_ATTR) 失败\n");
            std::exit(1);
        }
        in_size_ = in.size ? in.size : 640 * 640 * 3;

        // 输出缓冲预分配 (is_prealloc=1, 与主工程 npu_pool.cc:351-365 同构)
        outs_.resize(n_out_);
        out_bufs_.resize(n_out_);
        for (uint32_t i = 0; i < n_out_; i++) {
            rknn_tensor_attr oa{};
            memset(&oa, 0, sizeof(oa));
            oa.index = i;
            if (rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &oa, sizeof(oa)) != RKNN_SUCC) continue;
            size_t bytes = (size_t)oa.n_elems * sizeof(float);
            if (bytes == 0) bytes = 1024;
            out_bufs_[i].assign(bytes, 0);
            outs_[i] = {};
            outs_[i].index = i;
            outs_[i].want_float = 1;
            outs_[i].is_prealloc = 1;
            outs_[i].buf = out_bufs_[i].data();
            outs_[i].size = (uint32_t)out_bufs_[i].size();
        }

        zerocopy_ = zerocopy;
        if (zerocopy_) {
            // rknn_create_mem 分配输入张量内存 + rknn_set_io_mem 绑定 -> 零 CPU 拷贝
            mem_ = rknn_create_mem(ctx_, in_size_);
            if (!mem_) {
                fprintf(stderr, "[FATAL] rknn_create_mem 失败\n");
                std::exit(1);
            }
            rknn_tensor_attr bind{};
            memset(&bind, 0, sizeof(bind));
            bind = in;                       // ★ 必须整体拷贝 INPUT_ATTR (同 npu_pool.cc:320)
            bind.index = 0;
            bind.pass_through = 0;
            bind.type = RKNN_TENSOR_UINT8;
            bind.fmt = RKNN_TENSOR_NHWC;
            bind.n_dims = 4;
            bind.dims[0] = 1; bind.dims[1] = 640; bind.dims[2] = 640; bind.dims[3] = 3;
            if (rknn_set_io_mem(ctx_, mem_, &bind) != RKNN_SUCC) {
                fprintf(stderr, "[FATAL] rknn_set_io_mem 失败\n");
                std::exit(1);
            }
            FillPattern(mem_->virt_addr, in_size_);
        } else {
            in_buf_ = aligned_alloc(64, (in_size_ + 63) / 64 * 64);
            if (!in_buf_) { fprintf(stderr, "[FATAL] 输入缓冲分配失败\n"); std::exit(1); }
            FillPattern(in_buf_, in_size_);
            in_.index = 0;
            in_.buf = in_buf_;
            in_.size = (uint32_t)in_size_;
            in_.pass_through = 0;
            in_.type = RKNN_TENSOR_UINT8;
            in_.fmt = RKNN_TENSOR_NHWC;
        }
    }

    ~Ctx() {
        if (mem_) rknn_destroy_mem(ctx_, mem_);
        if (in_buf_) free(in_buf_);
        rknn_destroy(ctx_);
    }

    void RunOnce() {
        if (!zerocopy_) {
            rknn_inputs_set(ctx_, 1, &in_);
        }
        rknn_run(ctx_, nullptr);
        rknn_outputs_get(ctx_, n_out_, outs_.data(), nullptr);
    }

    void Warmup(int n) {
        for (int i = 0; i < n; i++) RunOnce();
    }

    void Measure(int n, RoundResult* r) {
        for (int i = 0; i < n; i++) {
            double a = NowMs();
            if (!zerocopy_) {
                rknn_inputs_set(ctx_, 1, &in_);
                double b = NowMs();
                rknn_run(ctx_, nullptr);
                double c = NowMs();
                rknn_outputs_get(ctx_, n_out_, outs_.data(), nullptr);
                double d = NowMs();
                r->setup.push(b - a);
                r->run.push(c - b);
                r->get.push(d - c);
                r->lat.push(d - a);
            } else {
                rknn_run(ctx_, nullptr);
                double c = NowMs();
                rknn_outputs_get(ctx_, n_out_, outs_.data(), nullptr);
                double d = NowMs();
                r->run.push(c - a);
                r->get.push(d - c);
                r->lat.push(d - a);
            }
        }
        rknn_perf_run prun{};
        memset(&prun, 0, sizeof(prun));
        if (rknn_query(ctx_, RKNN_QUERY_PERF_RUN, &prun, sizeof(prun)) == RKNN_SUCC)
            r->drv_us = prun.run_duration;
    }

    rknn_context handle() const { return ctx_; }

private:
    static void FillPattern(void* p, size_t n) {
        unsigned char* b = static_cast<unsigned char*>(p);
        for (size_t i = 0; i < n; i++) b[i] = (unsigned char)(i * 31 + 17);
    }

    rknn_context ctx_ = 0;
    uint32_t n_out_ = 0;
    uint32_t in_size_ = 0;
    bool zerocopy_ = true;
    rknn_tensor_mem* mem_ = nullptr;
    void* in_buf_ = nullptr;
    rknn_input in_{};
    std::vector<rknn_output> outs_;
    std::vector<std::vector<float>> out_bufs_;
};

struct ThreadOut {
    RoundResult rr;
    double wall_ms = 0;
};

void RunOneThread(const char* model, int core_mask, bool zerocopy, int iters, int warmup,
                  ThreadOut* out) {
    double t0 = NowMs();
    Ctx c(model, core_mask, zerocopy);
    c.Warmup(warmup);
    c.Measure(iters, &out->rr);
    out->wall_ms = NowMs() - t0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
                "用法: %s <model.rknn> [--iters N] [--warmup N] [--reps N] "
                "[--mode single|multi|both] [--cores 1,2,4] [--io zerocopy|memcpy] "
                "[--perfdetail] [--json]\n",
                argv[0]);
        return 2;
    }
    const char* model = argv[1];
    int iters = 100, warmup = 20, reps = 5;
    std::string mode = "both", iomode = "zerocopy";
    std::vector<int> cores = {1, 2, 4};
    bool perf_detail = false, json = false;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--iters") && i + 1 < argc) iters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) warmup = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
        else if (!strcmp(argv[i], "--io") && i + 1 < argc) iomode = argv[++i];
        else if (!strcmp(argv[i], "--cores") && i + 1 < argc) {
            cores.clear();
            char* s = argv[++i];
            for (char* tok = strtok(s, ","); tok; tok = strtok(NULL, ","))
                cores.push_back(atoi(tok));
        } else if (!strcmp(argv[i], "--perfdetail")) perf_detail = true;
        else if (!strcmp(argv[i], "--json")) json = true;
    }
    bool zerocopy = (iomode != "memcpy");

    printf("模型        : %s\n", model);
    printf("口径        : rknn_run + rknn_outputs_get (== 主工程 npu_pool.cc dt_npu)\n");
    printf("输入通路    : %s\n", zerocopy ? "rknn_create_mem + rknn_set_io_mem (零拷贝)"
                                        : "rknn_inputs_set (隐式 memcpy)");
    printf("每轮迭代    : %d  (预热 %d)   测量轮数: %d\n", iters, warmup, reps);

    // SDK / 驱动版本
    {
        rknn_context c0 = 0;
        if (rknn_init(&c0, const_cast<char*>(model), 0, RKNN_FLAG_PRIOR_HIGH, nullptr) == RKNN_SUCC) {
            rknn_sdk_version ver{};
            if (rknn_query(c0, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver)) == RKNN_SUCC)
                printf("Runtime     : API %s | driver %s\n", ver.api_version, ver.drv_version);
            rknn_tensor_attr in{};
            memset(&in, 0, sizeof(in));
            if (rknn_query(c0, RKNN_QUERY_INPUT_ATTR, &in, sizeof(in)) == RKNN_SUCC) {
                printf("输入张量    : type=%d fmt=%d dims=[", in.type, in.fmt);
                for (uint32_t k = 0; k < in.n_dims; k++)
                    printf("%u%s", in.dims[k], k + 1 < in.n_dims ? "," : "");
                printf("] size=%u B\n", in.size);
            }
            rknn_destroy(c0);
        }
    }

    double single_mean_of_means = 0, single_p95_of_means = 0;
    double multi_fps_mean = 0, multi_lat_mean = 0, multi_steady_fps = 0;

    // ---------------- 单核 ----------------
    if (mode == "single" || mode == "both") {
        printf("\n================ 单核 (mask=%d) 纯推理 ================\n", cores[0]);
        std::vector<double> means, p95s;
        for (int rep = 0; rep < reps; rep++) {
            ThreadOut o;
            RunOneThread(model, cores[0], zerocopy, iters, warmup, &o);
            printf("  [轮 %d] ", rep + 1);
            PrintStat("lat", o.rr.lat);
            printf("         run mean %8.3f | get mean %8.3f | drv %lld us\n",
                   o.rr.run.mean(), o.rr.get.mean(), (long long)o.rr.drv_us);
            means.push_back(o.rr.lat.mean());
            p95s.push_back(o.rr.lat.pct(95));
        }
        double sum = 0;
        for (double x : means) sum += x;
        single_mean_of_means = sum / means.size();
        double s2 = 0;
        for (double x : means) s2 += (x - single_mean_of_means) * (x - single_mean_of_means);
        double stddev = means.size() > 1 ? std::sqrt(s2 / (means.size() - 1)) : 0;
        double ps = 0;
        for (double x : p95s) ps += x;
        single_p95_of_means = ps / p95s.size();
        printf("\n  >>> 单核稳态: 平均 %0.3f ms (±%0.3f, %zu 轮) | P95 平均 %0.3f ms\n",
               single_mean_of_means, stddev, means.size(), single_p95_of_means);
        printf("  >>> 单核 FPS: %0.2f\n", 1000.0 / single_mean_of_means);
    }

    // ---------------- 多核并发 ----------------
    if (mode == "multi" || mode == "both") {
        printf("\n================ 多核并发 (masks=");
        for (size_t k = 0; k < cores.size(); k++) printf("%d%s", cores[k], k + 1 < cores.size() ? "," : "");
        printf(") 纯推理 ================\n");
        std::vector<double> fps_list, lat_list;
        for (int rep = 0; rep < reps; rep++) {
            size_t T = cores.size();
            std::vector<ThreadOut> outs(T);
            std::vector<std::thread> ths;
            double w0 = NowMs();
            for (size_t k = 0; k < T; k++) {
                ths.emplace_back(RunOneThread, model, cores[k], zerocopy, iters, warmup,
                                 &outs[k]);
            }
            for (auto& t : ths) t.join();
            double wall = NowMs() - w0;
            double total_infer = (double)T * iters;
            double fps = total_infer / (wall / 1000.0);
            double lat_sum = 0;
            for (auto& o : outs) lat_sum += o.rr.lat.mean();
            lat_sum /= T;
            printf("  [轮 %d] 墙钟 %8.1f ms | 总推理 %zu 次 | 总 FPS %7.2f | 单线程平均延迟 %7.3f ms\n",
                   rep + 1, wall, (size_t)total_infer, fps, lat_sum);
            for (size_t k = 0; k < T; k++) {
                printf("         thread%zu(mask=%d): ", k, cores[k]);
                PrintStat("lat", outs[k].rr.lat);
            }
            fps_list.push_back(fps);
            lat_list.push_back(lat_sum);
        }
        double s = 0;
        for (double x : fps_list) s += x;
        multi_fps_mean = s / fps_list.size();
        double sd = 0;
        for (double x : fps_list) sd += (x - multi_fps_mean) * (x - multi_fps_mean);
        double fps_std = fps_list.size() > 1 ? std::sqrt(sd / (fps_list.size() - 1)) : 0;
        double s2 = 0;
        for (double x : lat_list) s2 += x;
        multi_lat_mean = s2 / lat_list.size();
        printf("\n  >>> 多核稳态: 总 FPS %0.2f (±%0.2f, %zu 轮) | 单线程延迟 %0.3f ms\n",
               multi_fps_mean, fps_std, fps_list.size(), multi_lat_mean);
        // ★ 墙钟 FPS 里含【每轮预热】和【每线程各建一次 RKNN 上下文】, 会系统性低估稳态吞吐。
        //   稳态吞吐 = 线程数 / 单线程稳态延迟 (只算正式测量段)。
        double steady_fps = cores.size() / (multi_lat_mean / 1000.0);
        multi_steady_fps = steady_fps;
        printf("  >>> 稳态吞吐(不含预热/建上下文): %0.2f FPS  <-- 与单核对比用这个\n",
               steady_fps);
        printf("  >>> 三核相对单核加速比: 墙钟 %0.2fx | 稳态 %0.2fx\n",
               multi_fps_mean / (1000.0 / single_mean_of_means),
               steady_fps / (1000.0 / single_mean_of_means));
    }

    if (perf_detail) {
        rknn_context c0 = 0;
        if (rknn_init(&c0, const_cast<char*>(model), 0,
                       RKNN_FLAG_PRIOR_HIGH | RKNN_FLAG_COLLECT_PERF_MASK, nullptr) == RKNN_SUCC) {
            rknn_input_output_num io{};
            rknn_query(c0, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
            rknn_tensor_attr in{};
            memset(&in, 0, sizeof(in));
            rknn_query(c0, RKNN_QUERY_INPUT_ATTR, &in, sizeof(in));
            void* b = aligned_alloc(64, (in.size + 63) / 64 * 64);
            memset(b, 114, in.size);
            rknn_input inp{};
            inp.index = 0; inp.buf = b; inp.size = in.size;
            inp.pass_through = 0; inp.type = RKNN_TENSOR_UINT8; inp.fmt = RKNN_TENSOR_NHWC;
            for (int i = 0; i < 5; i++) {
                rknn_inputs_set(c0, 1, &inp);
                rknn_run(c0, nullptr);
                rknn_output o{};
                o.index = 0;
                rknn_outputs_get(c0, 1, &o, nullptr);
                rknn_outputs_release(c0, 1, &o);
            }
            rknn_perf_detail pd{};
            memset(&pd, 0, sizeof(pd));
            if (rknn_query(c0, RKNN_QUERY_PERF_DETAIL, &pd, sizeof(pd)) == RKNN_SUCC && pd.perf_data)
                printf("\n===== 驱动逐算子耗时表 =====\n%s\n", pd.perf_data);
            free(b);
            rknn_destroy(c0);
        }
    }

    if (json) {
        printf("\n---JSON---\n");
        printf("{\"model\":\"%s\",\"io\":\"%s\",\"iters\":%d,\"reps\":%d,"
               "\"single_mean_ms\":%.4f,\"single_p95_ms\":%.4f,\"single_fps\":%.4f,"
               "\"multi_total_fps\":%.4f,\"multi_lat_ms\":%.4f,"
               "\"multi_steady_fps\":%.4f}\n",
               model, iomode.c_str(), iters, reps, single_mean_of_means, single_p95_of_means,
               single_mean_of_means > 0 ? 1000.0 / single_mean_of_means : 0.0, multi_fps_mean,
               multi_lat_mean,
               multi_steady_fps);
    }
    return 0;
}
