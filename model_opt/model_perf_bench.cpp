// model_perf_bench.cpp —— 模型性能对比测试程序(统一口径)
//
// 用途: 对多组 RKNN 模型做【纯推理】耗时 A/B, 保证变量唯一。
//
// ★ 口径(与主工程 npu_pool.cc:390-395 一致):
//       t0 = now_ms();
//       rknn_run(ctx, nullptr);
//       rknn_outputs_get(ctx, n, outs, nullptr);
//       dt  = now_ms() - t0;
//   只含 rknn_run + rknn_outputs_get, 不含任何前后处理 / RGA / 排队。
//
// ★ 零拷贝输入: rknn_create_mem + rknn_set_io_mem(与主工程检测路径同构),
//   不走 rknn_inputs_set, 所以测的是纯 NPU 算力, 不含 CPU 上传开销。
// ★ 输入尺寸从模型自身查(RKNN_QUERY_INPUT_ATTR), 不写死 —— 同一程序跑
//   640/576/512 三种分辨率的模型。
// ★ 单核运行: rknn_set_core_mask(1/2/4), 每次只跑一颗核, 保证组间可比。
// ★ 稳态测试: 先预热 --warmup 秒, 再测 --duration 秒, 取该段内所有单帧耗时做统计。
//
// 用法:
//   model_perf_bench <model.rknn> [--duration 30] [--warmup 3] [--core 1]
//                    [--label A0] [--json out.json]
//
//   --duration  正式测量时长(秒, 默认 30)
//   --warmup    预热时长(秒, 默认 3)
//   --core      核掩码 1=Core0 2=Core1 4=Core2 (默认 1)
//   --label     模型标签(写进 JSON, 便于批量对比)
//   --json      结果写 JSON

#include <algorithm>
#include <chrono>
#include <cmath>
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
    // 最近秩百分位
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

const char* TypeName(int t) {
    switch (t) {
        case 0: return "FLOAT32";
        case 1: return "FLOAT16";
        case 2: return "INT8";
        case 3: return "UINT8";
        case 4: return "INT16";
        case 5: return "UINT16";
        case 6: return "INT32";
        case 7: return "UINT32";
        case 8: return "INT64";
        case 9: return "BOOL";
        default: return "UNKNOWN";
    }
}

const char* FmtName(int f) {
    switch (f) {
        case 0: return "NCHW";
        case 1: return "NHWC";
        case 2: return "NC1HWC2";
        default: return "UNDEF";
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
                "用法: %s <model.rknn> [--duration 30] [--warmup 3] [--core 1] "
                "[--label NAME] [--json FILE]\n",
                argv[0]);
        return 2;
    }
    const char* model = argv[1];
    double duration = 30.0, warmup = 3.0;
    int core_mask = 1;
    std::string label = "", json_out = "";

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--duration") && i + 1 < argc) duration = atof(argv[++i]);
        else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) warmup = atof(argv[++i]);
        else if (!strcmp(argv[i], "--core") && i + 1 < argc) core_mask = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--label") && i + 1 < argc) label = argv[++i];
        else if (!strcmp(argv[i], "--json") && i + 1 < argc) json_out = argv[++i];
    }

    printf("=========== model_perf_bench ===========\n");
    printf("模型      : %s%s%s\n", model, label.empty() ? "" : "  [",
           label.empty() ? "" : label.c_str());
    printf("label     : %s\n", label.empty() ? "(未指定)" : label.c_str());
    printf("口径      : rknn_run + rknn_outputs_get (纯推理, 零拷贝输入)\n");
    printf("核掩码    : 0x%x | 预热 %.1fs | 测量 %.1fs\n", core_mask, warmup, duration);

    rknn_context ctx = 0;
    int ret = rknn_init(&ctx, const_cast<char*>(model), 0, RKNN_FLAG_PRIOR_HIGH, nullptr);
    if (ret != RKNN_SUCC) {
        fprintf(stderr, "[FATAL] rknn_init(%s) = %d\n", model, ret);
        return 1;
    }

    rknn_sdk_version ver;
    memset(&ver, 0, sizeof(ver));
    if (rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver)) == RKNN_SUCC)
        printf("Runtime   : API %s | driver %s\n", ver.api_version, ver.drv_version);

    rknn_set_core_mask(ctx, static_cast<rknn_core_mask>(core_mask));

    rknn_input_output_num io;
    memset(&io, 0, sizeof(io));
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));

    rknn_tensor_attr in_attr;
    memset(&in_attr, 0, sizeof(in_attr));
    in_attr.index = 0;
    rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr));
    printf("输入      : type=%s fmt=%s dims=[", TypeName(in_attr.type), FmtName(in_attr.fmt));
    for (uint32_t i = 0; i < in_attr.n_dims; i++)
        printf("%u%s", in_attr.dims[i], i + 1 < in_attr.n_dims ? "," : "");
    printf("] size=%u B\n", in_attr.size);

    // ---- 输出缓冲预分配 (is_prealloc=1) ----
    std::vector<rknn_output> outs(io.n_output);
    std::vector<std::vector<float>> out_bufs(io.n_output);
    for (uint32_t i = 0; i < io.n_output; i++) {
        rknn_tensor_attr oa;
        memset(&oa, 0, sizeof(oa));
        oa.index = i;
        rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &oa, sizeof(oa));
        printf("输出[%u]   : %-28s type=%-8s fmt=%s dims=[", i, oa.name,
               TypeName(oa.type), FmtName(oa.fmt));
        for (uint32_t d = 0; d < oa.n_dims; d++)
            printf("%u%s", oa.dims[d], d + 1 < oa.n_dims ? "," : "");
        printf("]\n");
        size_t bytes = (size_t)oa.n_elems * sizeof(float);
        if (bytes == 0) bytes = 4096;
        out_bufs[i].assign(bytes, 0);
        outs[i] = {};
        outs[i].index = i;
        outs[i].want_float = 1;
        outs[i].is_prealloc = 1;
        outs[i].buf = out_bufs[i].data();
        outs[i].size = (uint32_t)out_bufs[i].size();
    }

    // ---- 零拷贝输入: rknn_create_mem + rknn_set_io_mem ----
    uint32_t in_size = in_attr.size ? in_attr.size : 640 * 640 * 3;
    rknn_tensor_mem* mem = rknn_create_mem(ctx, in_size);
    if (!mem) {
        fprintf(stderr, "[FATAL] rknn_create_mem 失败\n");
        rknn_destroy(ctx);
        return 1;
    }
    rknn_tensor_attr bind;
    memset(&bind, 0, sizeof(bind));
    bind = in_attr;                       // ★ 整体拷贝 INPUT_ATTR (同 npu_pool.cc:320)
    bind.index = 0;
    bind.pass_through = 0;
    bind.type = RKNN_TENSOR_UINT8;        // 应用侧喂 UINT8 RGB888
    bind.fmt = RKNN_TENSOR_NHWC;
    if (rknn_set_io_mem(ctx, mem, &bind) != RKNN_SUCC) {
        fprintf(stderr, "[FATAL] rknn_set_io_mem 失败\n");
        rknn_destroy_mem(ctx, mem);
        rknn_destroy(ctx);
        return 1;
    }
    // 填确定性内容(保证模型真的在算, 不走数据相关的空路径)
    {
        unsigned char* p = static_cast<unsigned char*>(mem->virt_addr);
        for (uint32_t i = 0; i < in_size; i++) p[i] = (unsigned char)(i * 31 + 17);
    }
    printf("输入通路  : rknn_create_mem + rknn_set_io_mem (零拷贝)\n\n");

    // ---- 预热 ----
    double t_w0 = NowMs();
    long long warm_iters = 0;
    while (NowMs() - t_w0 < warmup * 1000.0) {
        rknn_run(ctx, nullptr);
        rknn_outputs_get(ctx, io.n_output, outs.data(), nullptr);
        warm_iters++;
    }

    // ---- 正式测量 ----
    Stat lat, run_s, get_s;
    double t0 = NowMs();
    long long iters = 0;
    while (NowMs() - t0 < duration * 1000.0) {
        double a = NowMs();
        rknn_run(ctx, nullptr);
        double b = NowMs();
        rknn_outputs_get(ctx, io.n_output, outs.data(), nullptr);
        double c = NowMs();
        run_s.push(b - a);
        get_s.push(c - b);
        lat.push(c - a);
        iters++;
    }
    double elapsed = NowMs() - t0;

    // 驱动自报的单次推理耗时
    rknn_perf_run prun;
    memset(&prun, 0, sizeof(prun));
    int drv_us = 0;
    if (rknn_query(ctx, RKNN_QUERY_PERF_RUN, &prun, sizeof(prun)) == RKNN_SUCC)
        drv_us = (int)prun.run_duration;

    // 模型文件大小
    long file_bytes = 0;
    {
        FILE* f = fopen(model, "rb");
        if (f) { fseek(f, 0, SEEK_END); file_bytes = ftell(f); fclose(f); }
    }

    double fps = iters / (elapsed / 1000.0);
    printf("---------- 结果 ----------\n");
    printf("预热迭代  : %lld (%.1f s)\n", warm_iters, warmup);
    printf("测量迭代  : %lld (%.2f s)\n", iters, elapsed / 1000.0);
    printf("单帧平均  : %.3f ms\n", lat.mean());
    printf("单帧中位  : %.3f ms\n", lat.pct(50));
    printf("P95       : %.3f ms\n", lat.pct(95));
    printf("P99       : %.3f ms\n", lat.pct(99));
    printf("min/max   : %.3f / %.3f ms\n", lat.min(), lat.max());
    printf("rknn_run  : 平均 %.3f ms\n", run_s.mean());
    printf("outputs_get: 平均 %.3f ms\n", get_s.mean());
    if (drv_us > 0)
        printf("驱动自报  : %d us (%.2f FPS)\n", drv_us, 1e6 / drv_us);
    printf("单核 FPS  : %.2f\n", fps);
    printf("模型体积  : %ld 字节 (%.2f MB)\n", file_bytes, file_bytes / 1048576.0);

    if (!json_out.empty()) {
        FILE* f = fopen(json_out.c_str(), "w");
        if (f) {
            fprintf(f,
                    "{\"label\":\"%s\",\"model\":\"%s\",\"core_mask\":%d,"
                    "\"input_dims\":[%u,%u,%u,%u],\"input_type\":\"%s\",\"input_fmt\":\"%s\","
                    "\"duration_s\":%.2f,\"iters\":%lld,"
                    "\"mean_ms\":%.4f,\"median_ms\":%.4f,\"p95_ms\":%.4f,\"p99_ms\":%.4f,"
                    "\"min_ms\":%.4f,\"max_ms\":%.4f,\"run_ms\":%.4f,\"get_ms\":%.4f,"
                    "\"fps\":%.4f,\"file_bytes\":%ld,\"file_mb\":%.4f,"
                    "\"drv_run_us\":%d,\"api_version\":\"%s\",\"driver_version\":\"%s\"}\n",
                    label.c_str(), model, core_mask,
                    in_attr.dims[0], in_attr.dims[1], in_attr.dims[2], in_attr.dims[3],
                    TypeName(in_attr.type), FmtName(in_attr.fmt),
                    duration, iters, lat.mean(), lat.pct(50), lat.pct(95), lat.pct(99),
                    lat.min(), lat.max(), run_s.mean(), get_s.mean(), fps,
                    file_bytes, file_bytes / 1048576.0, drv_us,
                    ver.api_version, ver.drv_version);
            fclose(f);
            printf("JSON      : %s\n", json_out.c_str());
        }
    }

    rknn_destroy_mem(ctx, mem);
    rknn_destroy(ctx);
    return 0;
}
