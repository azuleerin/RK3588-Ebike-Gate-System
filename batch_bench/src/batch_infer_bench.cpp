// batch_infer_bench.cpp —— RK3588 多 Batch 推理吞吐/延迟基准
//
// ======================================================================
// 【这个程序要同时给出两组数, 缺一不可】
// ======================================================================
//   1) 吞吐口径: 总吞吐 FPS、单帧等效耗时(= 单 Batch 耗时 / N)
//      —— Batch 推理优化的是这个: 一次 rknn_run 把 N 张图送进 NPU,
//         datapath 利用率更高, 单位时间的帧数更多。
//   2) 延迟口径: 单帧推理延迟(= 单 Batch 耗时)
//      —— 主工程是 4 路独立通道, 每路拿到自己的结果就要往下走。
//         拼成 Batch 之后, 某一路的帧要【等另外 N-1 路都算完】才拿到结果,
//         所以单帧延迟 = 整个 Batch 的耗时, 而不是 batch_ms/N。
//      ★ Batch 推理【不会】降低单帧延迟, 只提升总吞吐。
//
// ======================================================================
// 【实现要点】
// ======================================================================
//   · 零拷贝输入: rknn_create_mem + rknn_set_io_mem(不碰 rknn_inputs_set)
//   · 全部内存预分配: 输入/输出缓冲在测量前一次性分配好, 测量循环里
//     【零 malloc / 零 free / 零 memset】
//   · 输出缓冲 is_prealloc=1, 且【不调用 rknn_outputs_release】
//     (release 会让 runtime free 我们的缓冲)
//   · 后处理只做"结果拆分"(把 batch 输出按帧切开看), 不计入推理耗时
//   · 稳态测量: 预热 --warmup 秒 -> 测 --duration 秒
//   · DDR 占用: 后台线程采样 /sys/class/devfreq/dmc/load (dmc busy%)
//
// ======================================================================
// 【三种运行模式】
// ======================================================================
//   single   : 1 个 context, rknn_set_batch_core_num(1) —— 整个 Batch 跑 1 颗核
//   multi3   : 1 个 context, rknn_set_batch_core_num(3) —— Batch 拆到 3 颗核
//   workers3 : 3 个独立 context, 每个独占一颗核(0/1/2), 各跑整个 Batch
//              ★ 这是主工程现在的做法(NpuPool 3 worker 分核),
//                作为"batch 到底值不值"的对照组
//
//   注: batch>1 的模型【不能】用 rknn_set_core_mask 绑核
//       (驱动报 "core mask = N is invalid for batch size: M, fall back to
//        CORE_AUTO mode"), 所以 batch>1 时只能靠 rknn_set_batch_core_num
//       控制用几颗核。
//
// ======================================================================
// 【用法】
// ======================================================================
//   batch_infer_bench <model.rknn> [--duration 30] [--warmup 5]
//                      [--mode single|multi3|workers3|auto]
//                      [--ddr-stress N] [--label NAME] [--json FILE]
//
//   --duration    正式测量时长(秒, 默认 30)
//   --warmup      预热时长(秒, 默认 5)
//   --mode        运行模式(默认 single)
//   --ddr-stress  后台 DDR 压力线程数(0=空载; 4 = 模拟 4 路 MPP+RGA)
//   --label       结果标签(写进 JSON)
//   --json        结果写 JSON

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

#include "rknn_api.h"

namespace {

constexpr int MAX_OUT = 16;

std::string StringPrintf(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return std::string(buf);
}

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

// ---------------------------------------------------------------- DDR 采样
std::atomic<bool> g_ddr_stop{false};
std::atomic<double> g_dmc_mean{0.0}, g_dmc_max{0.0};
std::atomic<long> g_dmc_n{0};

int ReadDmcLoad() {
    // /sys/class/devfreq/dmc/load 形如 "45@2400000000Hz"
    static int fd = -1;
    if (fd < 0) {
        fd = open("/sys/class/devfreq/dmc/load", O_RDONLY);
        if (fd < 0) return -1;
    }
    char buf[64] = {0};
    ssize_t n = pread(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return -1;
    buf[n] = 0;
    return atoi(buf);          // "45@..." -> 45
}

void DmcSamplerThread() {
    double sum = 0;
    long n = 0;
    double mx = 0;
    while (!g_ddr_stop.load()) {
        int v = ReadDmcLoad();
        if (v >= 0) {
            sum += v;
            n++;
            if (v > mx) mx = v;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    g_dmc_mean.store(n ? sum / n : -1);
    g_dmc_max.store(mx);
    g_dmc_n.store(n);
}

// ---------------------------------------------------------------- DDR 压力
// 模拟 4 路 MPP 解码 + RGA 预处理:
//   每路一个 4K NV12 帧缓冲(3840x2160x1.5 = 11.9 MB) + 一个 RGB 缓冲
//   循环做: 读 NV12 -> 写 RGB(模拟 RGA 的读+写) -> 写回 NV12(模拟 MPP 写帧)
// 全部缓冲预分配, 循环里不申请内存。
struct StressCtx {
    std::vector<uint8_t> nv12;      // 4K NV12
    std::vector<uint8_t> rgb;       // 4K RGB888
    std::atomic<bool> stop{false};
};

void DdrStressThread(StressCtx* c) {
    const size_t n = c->nv12.size();
    const size_t m = c->rgb.size();
    uint64_t acc = 0;
    while (!c->stop.load()) {
        // 读 NV12 + 写 RGB
        for (size_t i = 0; i < m; i += 64) {
            uint8_t v = c->nv12[i % n];
            c->rgb[i] = v;
            acc += v;
        }
        // 写回 NV12(模拟解码器写帧)
        for (size_t i = 0; i < n; i += 64) {
            c->nv12[i] = static_cast<uint8_t>(acc + i);
        }
    }
}

// ---------------------------------------------------------------- 一个推理上下文
struct InferCtx {
    rknn_context ctx = 0;
    rknn_tensor_mem* in_mem = nullptr;
    uint8_t* host_in = nullptr;      // memcpy 模式用的预分配输入缓冲
    bool zerocopy = true;
    std::vector<rknn_output> outs;
    std::vector<std::vector<float>> out_bufs;
    uint32_t in_size = 0;
    uint32_t batch = 1;
    std::vector<uint32_t> in_dims;
    std::vector<std::vector<uint32_t>> out_dims;
};

bool SetupCtx(InferCtx* c, const char* model, int core_mask, int batch_core_num,
              bool pin_core, bool zerocopy = true) {
    c->zerocopy = zerocopy;
    int ret = rknn_init(&c->ctx, const_cast<char*>(model), 0, RKNN_FLAG_PRIOR_HIGH, nullptr);
    if (ret != RKNN_SUCC) {
        fprintf(stderr, "[FATAL] rknn_init(%s) = %d\n", model, ret);
        return false;
    }
    if (pin_core) {
        // ★ batch>1 时驱动会拒绝绑核并退回 CORE_AUTO, 所以只对 batch==1 显式绑
        if (c->batch > 1) {
            fprintf(stderr, "[warn] batch>1: 不设 core_mask(驱动会退回 CORE_AUTO)\n");
        } else {
            rknn_set_core_mask(c->ctx, static_cast<rknn_core_mask>(core_mask));
        }
    }
    if (batch_core_num > 0) {
        int bc = rknn_set_batch_core_num(c->ctx, batch_core_num);
        if (bc != RKNN_SUCC)
            fprintf(stderr, "[warn] rknn_set_batch_core_num(%d) = %d\n", batch_core_num, bc);
    }

    rknn_input_output_num io;
    memset(&io, 0, sizeof(io));
    rknn_query(c->ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));

    rknn_tensor_attr in;
    memset(&in, 0, sizeof(in));
    in.index = 0;
    rknn_query(c->ctx, RKNN_QUERY_INPUT_ATTR, &in, sizeof(in));
    c->in_dims.assign(in.dims, in.dims + in.n_dims);
    c->batch = c->in_dims.empty() ? 1 : c->in_dims[0];
    c->in_size = in.size ? in.size : 640 * 640 * 3;

    c->outs.resize(io.n_output);
    c->out_bufs.resize(io.n_output);
    c->out_dims.resize(io.n_output);
    for (uint32_t i = 0; i < io.n_output; i++) {
        rknn_tensor_attr oa;
        memset(&oa, 0, sizeof(oa));
        oa.index = i;
        rknn_query(c->ctx, RKNN_QUERY_OUTPUT_ATTR, &oa, sizeof(oa));
        c->out_dims[i].assign(oa.dims, oa.dims + oa.n_dims);
        size_t bytes = (size_t)oa.n_elems * sizeof(float);
        if (bytes == 0) bytes = 4096;
        c->out_bufs[i].assign(bytes, 0);
        c->outs[i] = {};
        c->outs[i].index = i;
        c->outs[i].want_float = 1;
        c->outs[i].is_prealloc = 1;
        c->outs[i].buf = c->out_bufs[i].data();
        c->outs[i].size = (uint32_t)c->out_bufs[i].size();
    }

    if (zerocopy) {
        // 零拷贝输入缓冲
        c->in_mem = rknn_create_mem(c->ctx, c->in_size);
        if (!c->in_mem) {
            fprintf(stderr, "[FATAL] rknn_create_mem 失败\n");
            return false;
        }
        rknn_tensor_attr bind;
        memset(&bind, 0, sizeof(bind));
        bind = in;
        bind.index = 0;
        bind.pass_through = 0;
        bind.type = RKNN_TENSOR_UINT8;
        bind.fmt = RKNN_TENSOR_NHWC;
        if (rknn_set_io_mem(c->ctx, c->in_mem, &bind) != RKNN_SUCC) {
            fprintf(stderr, "[FATAL] rknn_set_io_mem 失败\n");
            return false;
        }
        uint8_t* p = static_cast<uint8_t*>(c->in_mem->virt_addr);
        for (uint32_t i = 0; i < c->in_size; i++) p[i] = static_cast<uint8_t>(i * 31 + 17);
    } else {
        // memcpy 模式: 预分配 host 缓冲, 测量循环里零动态申请
        c->host_in = (uint8_t*)aligned_alloc(64, ((size_t)c->in_size + 63) / 64 * 64);
        if (!c->host_in) {
            fprintf(stderr, "[FATAL] host 输入缓冲分配失败\n");
            return false;
        }
        for (uint32_t i = 0; i < c->in_size; i++) c->host_in[i] = (uint8_t)(i * 31 + 17);
    }
    return true;
}

void DestroyCtx(InferCtx* c) {
    if (c->in_mem) rknn_destroy_mem(c->ctx, c->in_mem);
    if (c->host_in) free(c->host_in);
    if (c->ctx) rknn_destroy(c->ctx);
}

// 一次推理(含输入上传), 用于探测
void RunOnce(InferCtx* c, double* infer_ms, double* upload_ms) {
    if (!c->zerocopy) {
        rknn_input ri;
        memset(&ri, 0, sizeof(ri));
        ri.index = 0;
        ri.buf = c->host_in;
        ri.size = c->in_size;
        ri.pass_through = 0;
        ri.type = RKNN_TENSOR_UINT8;
        ri.fmt = RKNN_TENSOR_NHWC;
        double u0 = NowMs();
        rknn_inputs_set(c->ctx, 1, &ri);
        double u1 = NowMs();
        rknn_run(c->ctx, nullptr);
        rknn_outputs_get(c->ctx, c->outs.size(), c->outs.data(), nullptr);
        double u2 = NowMs();
        if (upload_ms) *upload_ms = u1 - u0;
        if (infer_ms) *infer_ms = u2 - u1;
        return;
    }
    double u0 = NowMs();
    rknn_run(c->ctx, nullptr);
    rknn_outputs_get(c->ctx, c->outs.size(), c->outs.data(), nullptr);
    double u1 = NowMs();
    if (upload_ms) *upload_ms = 0.0;
    if (infer_ms) *infer_ms = u1 - u0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
                "用法: %s <model.rknn> [--duration 30] [--warmup 5] "
                "[--mode single|multi3|workers3|auto] [--ddr-stress N] "
                "[--label NAME] [--json FILE]\n",
                argv[0]);
        return 2;
    }
    const char* model = argv[1];
    double duration = 30.0, warmup = 5.0;
    std::string mode = "single", label = "", json_out = "", io_mode = "zerocopy";
    int ddr_stress = 0;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--duration") && i + 1 < argc) duration = atof(argv[++i]);
        else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) warmup = atof(argv[++i]);
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
        else if (!strcmp(argv[i], "--ddr-stress") && i + 1 < argc) ddr_stress = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--io") && i + 1 < argc) io_mode = argv[++i];
        else if (!strcmp(argv[i], "--label") && i + 1 < argc) label = argv[++i];
        else if (!strcmp(argv[i], "--json") && i + 1 < argc) json_out = argv[++i];
    }

    printf("================ batch_infer_bench ================\n");
    printf("模型      : %s\n", model);
    printf("label     : %s\n", label.empty() ? "(未指定)" : label.c_str());
    printf("模式      : %s   DDR压力线程: %d\n", mode.c_str(), ddr_stress);
    printf("输入通路  : %s\n", io_mode.c_str());
    printf("预热/测量 : %.1fs / %.1fs\n", warmup, duration);

    // ---- DDR 压力线程 ----
    std::vector<StressCtx*> stresses;
    for (int i = 0; i < ddr_stress; i++) {
        StressCtx* s = new StressCtx();
        s->nv12.resize(3840 * 2160 * 3 / 2);
        s->rgb.resize(3840 * 2160 * 3);
        stresses.push_back(s);
    }
    std::vector<std::thread> stress_threads;
    for (auto* s : stresses)
        stress_threads.emplace_back(DdrStressThread, s);
    if (ddr_stress) {
        printf("DDR 压力  : %d 个线程 x (4K NV12 11.9MB + RGB 24.9MB) 循环读写\n", ddr_stress);
        std::this_thread::sleep_for(std::chrono::seconds(2));   // 让压力先起来
    }

    // ----------------------------------------------------------------
    // 零拷贝 + batch>1 的可用性探测
    //
    // 实测: librknnrt 2.3.2 在 RK3588 上, batch>1 的模型用 rknn_set_io_mem
    //       绑定输入后, rknn_outputs_get 会段错误(内部 memset 越界)。
    //       同样的模型走 rknn_inputs_set(memcpy) 则完全正常。
    // 这里用 fork 跑一次来固化这个证据(子进程崩了不影响主进程),
    // 崩了就自动退回 memcpy 通路继续测, 并在 JSON 里记录。
    // ----------------------------------------------------------------
    std::string zc_probe = "n/a";
    bool io_zerocopy = (io_mode == "zerocopy");
    if (io_zerocopy) {
        pid_t pid = fork();
        if (pid == 0) {
            InferCtx probe_ctx;
            if (SetupCtx(&probe_ctx, model, 1, 0, false, true)) {
                RunOnce(&probe_ctx, nullptr, nullptr);
            }
            _exit(0);
        } else if (pid > 0) {
            int st = 0;
            waitpid(pid, &st, 0);
            if (WIFEXITED(st)) {
                zc_probe = "ok";
            } else if (WIFSIGNALED(st)) {
                zc_probe = StringPrintf("crash:signal%d", WTERMSIG(st));
            } else {
                zc_probe = "unknown";
            }
        }
        if (zc_probe.rfind("crash", 0) == 0) {
            fprintf(stderr,
                    "[已知限制] 零拷贝 + batch>1 在本运行库上不可用(%s): "
                    "rknn_outputs_get 段错误\n"
                    "           -> 自动退回 memcpy 通路(rknn_inputs_set)继续测量\n",
                    zc_probe.c_str());
            io_zerocopy = false;
        }
    }

    g_ddr_stop.store(false);
    std::thread dmc_thread(DmcSamplerThread);

    double batch_ms_mean = 0, batch_ms_p50 = 0, batch_ms_p95 = 0, batch_ms_max = 0;
    double upload_ms_mean = 0;
    double per_frame_ms = 0, fps = 0, frame_latency_ms = 0;
    long long total_frames = 0;
    int batch = 1;
    double elapsed = 0;
    int n_workers = 1;

    if (mode == "workers3") {
        // 3 个独立 context, 每个独占一颗核, 各跑整个 Batch
        n_workers = 3;
        std::vector<InferCtx> cs(3);
        for (int i = 0; i < 3; i++) {
            if (!SetupCtx(&cs[i], model, 1 << i, 0, true, io_zerocopy)) return 1;
            batch = cs[i].batch;
        }
        printf("Batch     : %d   workers: 3 (core0/1/2)\n", batch);
        for (int i = 0; i < 3; i++)
            for (int w = 0; w < 10; w++) RunOnce(&cs[i], nullptr, nullptr);
        std::vector<Stat> per(3);
        std::vector<std::thread> ths;
        std::atomic<bool> stop{false};
        std::vector<long long> cnt(3, 0);
        double t0 = NowMs();
        for (int i = 0; i < 3; i++) {
            ths.emplace_back([&, i]() {
                while (!stop.load()) {
                    double inf = 0, upl = 0;
                    RunOnce(&cs[i], &inf, &upl);
                    per[i].push(inf);
                    cnt[i]++;
                }
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds((int)(warmup * 1000)));
        for (auto& t : per) t.v.clear();
        for (int i = 0; i < 3; i++) cnt[i] = 0;
        double tm = NowMs();
        std::this_thread::sleep_for(std::chrono::milliseconds((int)(duration * 1000)));
        elapsed = NowMs() - tm;
        stop.store(true);
        for (auto& t : ths) t.join();
        for (int i = 0; i < 3; i++) total_frames += cnt[i];
        double sum = 0;
        for (int i = 0; i < 3; i++) sum += per[i].mean();
        batch_ms_mean = sum / 3;
        batch_ms_p50 = batch_ms_mean;
        batch_ms_p95 = batch_ms_mean;
        for (int i = 0; i < 3; i++) {
            batch_ms_p50 = std::max(batch_ms_p50, per[i].pct(50));
            batch_ms_p95 = std::max(batch_ms_p95, per[i].pct(95));
            batch_ms_max = std::max(batch_ms_max, per[i].max());
        }
        for (int i = 0; i < 3; i++) DestroyCtx(&cs[i]);
    } else {
        int bc = (mode == "multi3") ? 3 : (mode == "single" ? 1 : 0);
        InferCtx c;
        if (!SetupCtx(&c, model, 1, bc, mode == "single", io_zerocopy)) return 1;
        batch = c.batch;
        printf("Batch     : %d   batch_core_num: %d\n", batch, bc);
        for (int w = 0; w < 10; w++) RunOnce(&c, nullptr, nullptr);
        // 预热
        double tw = NowMs();
        while (NowMs() - tw < warmup * 1000.0) RunOnce(&c, nullptr, nullptr);
        Stat lat, upl;
        double t0 = NowMs();
        long long iters = 0;
        while (NowMs() - t0 < duration * 1000.0) {
            double inf = 0, upl_ms = 0;
            RunOnce(&c, &inf, &upl_ms);
            lat.push(inf);
            upl.push(upl_ms);
            iters++;
        }
        upload_ms_mean = upl.mean();
        elapsed = NowMs() - t0;
        total_frames = iters * batch;
        batch_ms_mean = lat.mean();
        batch_ms_p50 = lat.pct(50);
        batch_ms_p95 = lat.pct(95);
        batch_ms_max = lat.max();
        DestroyCtx(&c);
    }

    // ---- 后处理: 简单的结果拆分(不计入耗时) ----
    // 上面每轮拿到的就是整批输出; 拆分只是把 batch 维切开看, 不做任何计算。

    g_ddr_stop.store(true);
    dmc_thread.join();
    for (auto* s : stresses) s->stop.store(true);
    for (auto& t : stress_threads) t.join();

    // ---- 指标 ----
    per_frame_ms = batch_ms_mean / batch;            // 吞吐口径: 单帧等效耗时
    fps = total_frames / (elapsed / 1000.0);         // 总吞吐
    frame_latency_ms = batch_ms_mean;                // 延迟口径: 一帧要等整个 Batch

    printf("\n---------- 结果 ----------\n");
    printf("Batch 大小        : %d\n", batch);
    printf("单 Batch 平均耗时 : %.3f ms\n", batch_ms_mean);
    printf("单 Batch 中位耗时 : %.3f ms\n", batch_ms_p50);
    printf("单 Batch P95     : %.3f ms\n", batch_ms_p95);
    printf("单 Batch max     : %.3f ms\n", batch_ms_max);
    printf("【吞吐】单帧等效  : %.3f ms/帧\n", per_frame_ms);
    printf("【吞吐】总吞吐 FPS : %.2f 帧/秒 (共 %lld 帧 / %.2f s)\n",
           fps, total_frames, elapsed / 1000.0);
    printf("【延迟】单帧延迟  : %.3f ms (一帧要等整个 Batch 算完)\n", frame_latency_ms);
    printf("输入上传耗时      : %.3f ms (memcpy 通路才有)\n", upload_ms_mean);
    printf("DDR 占用(dmc busy): 均值 %.1f%%  峰值 %.1f%%  (采样 %ld 次)\n",
           g_dmc_mean.load(), g_dmc_max.load(), g_dmc_n.load());
    printf("零拷贝探测        : %s\n", zc_probe.c_str());

    if (!json_out.empty()) {
        FILE* f = fopen(json_out.c_str(), "w");
        if (f) {
            fprintf(f,
                    "{\"label\":\"%s\",\"model\":\"%s\",\"mode\":\"%s\",\"batch\":%d,"
                    "\"io_requested\":\"%s\",\"io_used\":\"%s\",\"zc_probe\":\"%s\","
                    "\"ddr_stress\":%d,\"duration_s\":%.2f,\"elapsed_s\":%.3f,"
                    "\"frames\":%lld,\"batch_ms_mean\":%.4f,\"batch_ms_p50\":%.4f,"
                    "\"batch_ms_p95\":%.4f,\"batch_ms_max\":%.4f,"
                    "\"per_frame_ms\":%.4f,\"fps\":%.4f,\"frame_latency_ms\":%.4f,"
                    "\"upload_ms_mean\":%.4f,\"dmc_mean\":%.2f,\"dmc_max\":%.2f}\n",
                    label.c_str(), model, mode.c_str(), batch,
                    io_mode.c_str(), io_zerocopy ? "zerocopy" : "memcpy",
                    zc_probe.c_str(), ddr_stress,
                    duration, elapsed / 1000.0, total_frames, batch_ms_mean,
                    batch_ms_p50, batch_ms_p95, batch_ms_max, per_frame_ms, fps,
                    frame_latency_ms, upload_ms_mean, g_dmc_mean.load(), g_dmc_max.load());
            fclose(f);
            printf("JSON      : %s\n", json_out.c_str());
        }
    }

    for (auto* s : stresses) delete s;
    return 0;
}
