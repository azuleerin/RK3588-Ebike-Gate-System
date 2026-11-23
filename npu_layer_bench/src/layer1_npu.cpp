// src/layer1_npu.cpp —— 第 1 层: 纯 NPU 算力天花板
//
// ★ 测什么:
//   只跑 rknn_run + rknn_outputs_get, 没有解码、没有 RGA、没有后处理。
//   目的: 回答"NPU 本身到底能算多快"。
//
// ★ 为什么除核心掩码之外还要扫"每核几个 worker":
//   thread_bench 实测过: 每核 1 个 worker 时三颗核已经 99.6% 满, 但每核
//   2 个 worker 吞吐还能再涨 9.6%(驱动每次推理的非计算开销被另一个上下文
//   的计算掩盖)。那是在有排队的情况下测的; 这里用【裸推理】再测一遍,
//   看看这个收益在"无排队"的极限口径下还剩多少 —— 这直接决定
//   "真实算力天花板"这个数该怎么写。
#include "nlb/layers.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "nlb/harness.h"
#include "nlb/npu_model.h"

namespace nlb {
namespace {

using clk = std::chrono::steady_clock;

inline uint64_t now_us() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            clk::now().time_since_epoch()).count());
}

// 一个 worker = 一个 RKNN 上下文(自建 + 自绑核 + 自带零拷贝输入 + 预分配输出)。
class DetTask : public WorkerTask {
public:
    DetTask(const std::string& model, int core_mask, bool query_perf)
        : model_path_(model), core_mask_(core_mask), query_perf_(query_perf) {}

    bool setup(int worker_idx, int core, std::string* err) override {
        (void)worker_idx;
        std::string e;
        if (!model_.init(model_path_, core_mask_, -1, nullptr, 0, false, true, &e)) {
            if (err) *err = e;
            return false;
        }
        return true;
    }

    bool run_frame(int worker_idx, FrameTiming* t) override {
        (void)worker_idx;
        const uint64_t t0 = now_us();
        const int rc = model_.run();
        const uint64_t t1 = now_us();
        if (rc != 0) return false;

        t->det_busy_us = t1 - t0;
        t->frame_us = t1 - t0;
        // 驱动自报的推理耗时(放在计时区间之外)
        t->det_npu_us = query_perf_ ? model_.query_run_us() : 0;
        return true;
    }

    void teardown() override { model_.destroy(); }

    const NpuModel& model() const { return model_; }

private:
    std::string model_path_;
    int core_mask_;
    bool query_perf_;
    NpuModel model_;
};

}  // namespace

void run_layer1(const Config& cfg) {
    fprintf(stderr, "\n========== 第 1 层: 纯 NPU 算力天花板 ==========\n");
    fprintf(stderr, "模型: %s\n", cfg.det_model.c_str());
    fprintf(stderr, "每组: 预热 %.0fs + 测量 %.0fs, 重复 %d 次\n",
            cfg.warmup_sec, cfg.duration_sec, cfg.repeats);

    // ---- 用例表 ----
    // {用例名, 每核 worker 数分布}
    struct Case {
        const char* name;
        std::vector<int> cores;   // 每个 worker 绑的核
        bool query_perf;
    };
    std::vector<Case> cases;
    // 单核: 分别测 Core0/Core1/Core2 的单核算力
    cases.push_back({"core0", {0}, true});
    cases.push_back({"core1", {1}, true});
    cases.push_back({"core2", {2}, true});
    // 多核: 每核几个 worker 的扫描
    for (int w : cfg.worker_counts) {
        std::vector<int> cores;
        for (int i = 0; i < w; ++i) cores.push_back(i % 3);
        cases.push_back({nullptr, cores, true});
    }
    // 量化"驱动自报耗时"这个额外 runtime 查询本身的开销
    cases.push_back({"core0_nopq", {0}, false});

    HarnessOpts opts;
    opts.warmup_sec = cfg.warmup_sec;
    opts.duration_sec = cfg.duration_sec;
    opts.samples_per_worker = 20000;

    for (const auto& c : cases) {
        char namebuf[64];
        if (c.name) {
            snprintf(namebuf, sizeof(namebuf), "%s", c.name);
        } else {
            snprintf(namebuf, sizeof(namebuf), "w%zu", c.cores.size());
        }
        const std::string name = namebuf;

        // 每个 worker 一个 task(每个 task 自带一个上下文)
        std::vector<WorkerTask*> tasks;
        for (size_t i = 0; i < c.cores.size(); ++i) {
            tasks.push_back(new DetTask(cfg.det_model, core_mask_of_index(c.cores[i]),
                                        c.query_perf));
        }

        for (int rep = 1; rep <= cfg.repeats; ++rep) {
            const RunResult r = run_harness(name, Layer::kL1, c.cores, tasks, opts, cfg, rep);
            emit_json(r, cfg);
        }

        for (auto* t : tasks) delete t;
    }
}

}  // namespace nlb
