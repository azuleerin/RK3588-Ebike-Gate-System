// npu_bench.cc —— 按 Rockchip 官方基准口径测量"纯 NPU 推理"耗时
//
// 官方口径 (rknn_model_zoo README "Model performance benchmark(FPS)"):
//   只统计 rknn_run + rknn_outputs_get, 不含预处理/后处理,
//   且在"最大 NPU 频率"下测, RK3588 为 single_core。
//
// 用法:
//   npu_bench <model.rknn> [--iters N] [--warmup N] [--core MASK] [--perfdetail]
//     --core MASK : 0=AUTO 1=CORE0 2=CORE1 4=CORE2 3=CORE0+1 7=CORE0+1+2
//     --perfdetail: 打印逐层耗时 (需 RKNN_FLAG_COLLECT_PERF_MASK)
//     --batch N   : rknn_set_batch_core_num + 动态 batch (默认 1)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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
    double pct(double p) const {
        if (v.empty()) return 0;
        std::vector<double> t = v;
        std::sort(t.begin(), t.end());
        size_t i = std::min(t.size() - 1, (size_t)(p / 100.0 * t.size()));
        return t[i];
    }
    double min() const { return v.empty() ? 0 : *std::min_element(v.begin(), v.end()); }
    double max() const { return v.empty() ? 0 : *std::max_element(v.begin(), v.end()); }
};

void PrintStat(const char* name, const Stat& s) {
    if (s.v.empty()) return;
    printf("  %-14s mean %7.3f ms | min %7.3f | p50 %7.3f | p90 %7.3f | max %7.3f\n",
           name, s.mean(), s.min(), s.pct(50), s.pct(90), s.max());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "用法: %s <model.rknn> [--iters N] [--warmup N] [--core MASK] [--perfdetail]\n",
                argv[0]);
        return 2;
    }
    const char* model_path = argv[1];
    int iters = 200, warmup = 30, core_mask = 0, batch = 1;
    bool perf_detail = false;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--iters") && i + 1 < argc) iters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) warmup = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--core") && i + 1 < argc) core_mask = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--batch") && i + 1 < argc) batch = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--perfdetail")) perf_detail = true;
    }

    rknn_context ctx = 0;
    uint32_t flags = RKNN_FLAG_PRIOR_HIGH;
    if (perf_detail) flags |= RKNN_FLAG_COLLECT_PERF_MASK;

    int ret = rknn_init(&ctx, const_cast<char*>(model_path), 0, flags, nullptr);
    if (ret != RKNN_SUCC) {
        fprintf(stderr, "[FATAL] rknn_init(%s) = %d\n", model_path, ret);
        return 1;
    }
    printf("模型: %s\n", model_path);

    // SDK / 驱动版本
    rknn_sdk_version ver;
    if (rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver)) == RKNN_SUCC) {
        printf("SDK: %s | driver: %s\n", ver.api_version, ver.drv_version);
    }

    // 权重/内部内存占用 (判断权重是否可能驻留 SRAM)
    rknn_mem_size mem_size;
    if (rknn_query(ctx, RKNN_QUERY_MEM_SIZE, &mem_size, sizeof(mem_size)) == RKNN_SUCC) {
        printf("内存: weight %u MB | internal %u MB | sram free %u/%u MB\n",
               mem_size.total_weight_size / (1024 * 1024),
               mem_size.total_internal_size / (1024 * 1024),
               mem_size.free_sram_size / (1024 * 1024),
               mem_size.total_sram_size / (1024 * 1024));
    }

    rknn_input_output_num io_num;
    if (rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num)) != RKNN_SUCC) {
        fprintf(stderr, "[FATAL] rknn_query(IN_OUT_NUM) 失败\n");
        return 1;
    }
    printf("输入 %u 个 / 输出 %u 个\n", io_num.n_input, io_num.n_output);

    rknn_tensor_attr in_attr;
    memset(&in_attr, 0, sizeof(in_attr));
    if (rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr)) != RKNN_SUCC) {
        fprintf(stderr, "[FATAL] rknn_query(INPUT_ATTR) 失败\n");
        return 1;
    }
    printf("输入 attr: fmt=%d type=%d n_dims=%u dims=[", in_attr.fmt, in_attr.type, in_attr.n_dims);
    for (uint32_t i = 0; i < in_attr.n_dims; i++)
        printf("%u%s", in_attr.dims[i], i + 1 < in_attr.n_dims ? "," : "");
    printf("] size=%u bytes\n", in_attr.size);

    // 输出属性: 打印每个输出的 shape 与大小 —— 判断是不是 Rockchip 优化头 (9 路 fork)
    for (uint32_t i = 0; i < io_num.n_output; i++) {
        rknn_tensor_attr oa;
        memset(&oa, 0, sizeof(oa));
        oa.index = i;
        if (rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &oa, sizeof(oa)) == RKNN_SUCC) {
            printf("  out[%u] fmt=%d type=%d dims=[", i, oa.fmt, oa.type);
            for (uint32_t d = 0; d < oa.n_dims; d++)
                printf("%u%s", oa.dims[d], d + 1 < oa.n_dims ? "," : "");
            printf("] size=%u\n", oa.size);
        }
    }

    if (core_mask > 0) {
        ret = rknn_set_core_mask(ctx, static_cast<rknn_core_mask>(core_mask));
        printf("core_mask=0x%x -> %d\n", core_mask, ret);
    }
    if (batch > 1) {
        ret = rknn_set_batch_core_num(ctx, batch);
        printf("batch_core_num=%d -> %d\n", batch, ret);
    }

    // ---- 准备输入缓冲: 64B 对齐, 内容用确定性图案 ----
    size_t in_size = in_attr.size;
    if (in_size == 0) in_size = 640 * 640 * 3;
    void* in_buf = aligned_alloc(64, (in_size + 63) / 64 * 64);
    if (!in_buf) { fprintf(stderr, "[FATAL] 输入缓冲分配失败\n"); return 1; }
    memset(in_buf, 114, in_size);
    unsigned char* p = (unsigned char*)in_buf;
    for (size_t i = 0; i < in_size; i += 7) p[i] = (unsigned char)(i * 31 + 17);

    rknn_input input;
    memset(&input, 0, sizeof(input));
    input.index = 0;
    input.buf = in_buf;
    input.size = (uint32_t)in_size;
    input.pass_through = 0;
    input.type = RKNN_TENSOR_UINT8;
    input.fmt = RKNN_TENSOR_NHWC;

    std::vector<rknn_output> outs(io_num.n_output);
    for (uint32_t i = 0; i < io_num.n_output; i++) {
        memset(&outs[i], 0, sizeof(rknn_output));
        outs[i].index = i;
        outs[i].want_float = 0;
    }

    // ---- 预热 (含首次 NPU 唤醒, 不计入统计) ----
    for (int i = 0; i < warmup; i++) {
        if (rknn_inputs_set(ctx, 1, &input) != RKNN_SUCC) { fprintf(stderr, "[FATAL] inputs_set 失败\n"); return 1; }
        if (rknn_run(ctx, nullptr) != RKNN_SUCC) { fprintf(stderr, "[FATAL] rknn_run 失败\n"); return 1; }
        rknn_output o;
        memset(&o, 0, sizeof(o));
        o.index = 0;
        rknn_outputs_get(ctx, 1, &o, nullptr);
        rknn_outputs_release(ctx, 1, &o);
    }

    // ---- 正式测量 ----
    Stat t_set, t_run, t_get, t_total;
    int64_t drv_run_us = 0;
    for (int i = 0; i < iters; i++) {
        double a = NowMs();
        rknn_inputs_set(ctx, 1, &input);
        double b = NowMs();
        rknn_run(ctx, nullptr);
        double c = NowMs();
        rknn_output o;
        memset(&o, 0, sizeof(o));
        o.index = 0;
        rknn_outputs_get(ctx, 1, &o, nullptr);
        double d = NowMs();
        rknn_outputs_release(ctx, 1, &o);
        double e = NowMs();

        t_set.push(b - a);
        t_run.push(c - b);
        t_get.push(d - c);
        t_total.push(e - a);
    }

    // 驱动自报的单次推理耗时 (us)
    rknn_perf_run prun;
    memset(&prun, 0, sizeof(prun));
    if (rknn_query(ctx, RKNN_QUERY_PERF_RUN, &prun, sizeof(prun)) == RKNN_SUCC) drv_run_us = prun.run_duration;

    printf("\n===== NPU-only 测量 (%d 次) =====\n", iters);
    PrintStat("inputs_set", t_set);
    PrintStat("rknn_run", t_run);
    PrintStat("outputs_get", t_get);
    PrintStat("整轮", t_total);
    printf("  官方口径 FPS (按 run+get 均值): %.2f\n",
           1000.0 / (t_run.mean() + t_get.mean()));
    printf("  整轮 FPS (含 set):             %.2f\n", 1000.0 / t_total.mean());
    if (drv_run_us > 0)
        printf("  驱动自报 run_duration:         %lld us -> %.2f FPS\n",
               (long long)drv_run_us, 1e6 / drv_run_us);

    if (perf_detail) {
        rknn_perf_detail pd;
        memset(&pd, 0, sizeof(pd));
        if (rknn_query(ctx, RKNN_QUERY_PERF_DETAIL, &pd, sizeof(pd)) == RKNN_SUCC && pd.perf_data) {
            printf("\n===== 逐层耗时 (RKNN_QUERY_PERF_DETAIL) =====\n%s\n", pd.perf_data);
        } else {
            printf("\n[perf detail 不可用]\n");
        }
    }

    rknn_destroy(ctx);
    free(in_buf);
    return 0;
}
