// src/layer2_dual.cpp —— 第 2 层: 检测 + OCR 双模型纯推理
//
// ★ 测什么:
//   同一个 worker 线程里, 先跑检测推理, 再跑识别推理, 循环。
//   没有解码、没有 RGA、没有后处理。
//   目的: 回答"一帧业务(一个检测 + 一次识别)在 NPU 上要占多少核时间"。
//
// ★ 为什么要有"单核串行"和"三核并发"两种:
//   · 单核串行: 检测和识别都挤在一颗核上 —— 这是"识别钉在 Core0"的极端形态。
//   · 三核并发: 每个 worker 有自己的一对上下文, worker i 钉在核 i ——
//     检测和识别在【同一个 worker 内】仍然是串行的, 但不同 worker 之间并行。
//   两者的差值 = "把识别分散到多颗核"能拿到多少收益。
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

// 一个 worker = 检测上下文 + 识别上下文, 两者【串行】跑。
class DualTask : public WorkerTask {
public:
    DualTask(const std::string& det, const std::string& ocr, int core_mask)
        : det_path_(det), ocr_path_(ocr), core_mask_(core_mask) {}

    bool setup(int worker_idx, int core, std::string* err) override {
        (void)worker_idx;
        std::string e;
        if (!det_.init(det_path_, core_mask_, -1, nullptr, 0, false, true, &e)) {
            if (err) *err = "检测模型: " + e;
            return false;
        }
        if (!ocr_.init(ocr_path_, core_mask_, -1, nullptr, 0, false, true, &e)) {
            if (err) *err = "识别模型: " + e;
            return false;
        }
        return true;
    }

    bool run_frame(int worker_idx, FrameTiming* t) override {
        (void)worker_idx;
        const uint64_t f0 = now_us();

        const uint64_t a0 = now_us();
        const int rc1 = det_.run();
        const uint64_t a1 = now_us();
        if (rc1 != 0) return false;

        const uint64_t b0 = now_us();
        const int rc2 = ocr_.run();
        const uint64_t b1 = now_us();
        if (rc2 != 0) return false;

        const uint64_t f1 = now_us();
        t->det_busy_us = a1 - a0;
        t->ocr_busy_us = b1 - b0;
        t->frame_us = f1 - f0;
        t->det_npu_us = det_.query_run_us();
        t->ocr_npu_us = ocr_.query_run_us();
        return true;
    }

    void teardown() override {
        det_.destroy();
        ocr_.destroy();
    }

private:
    std::string det_path_, ocr_path_;
    int core_mask_;
    NpuModel det_, ocr_;
};

}  // namespace

void run_layer2(const Config& cfg) {
    fprintf(stderr, "\n========== 第 2 层: 检测 + OCR 双模型纯推理 ==========\n");
    fprintf(stderr, "检测模型: %s\n", cfg.det_model.c_str());
    fprintf(stderr, "识别模型: %s\n", cfg.ocr_model.c_str());
    fprintf(stderr, "每组: 预热 %.0fs + 测量 %.0fs, 重复 %d 次\n",
            cfg.warmup_sec, cfg.duration_sec, cfg.repeats);

    struct Case {
        std::string name;
        std::vector<int> cores;
    };
    std::vector<Case> cases;
    // 单核串行: 检测和识别都在同一颗核上
    cases.push_back({"serial_c0", {0}});
    cases.push_back({"serial_c1", {1}});
    cases.push_back({"serial_c2", {2}});
    // 三核并发: 每核 1 个 worker(worker 内 det+ocr 仍串行)
    cases.push_back({"tri_1per", {0, 1, 2}});
    // 每核 2 个 worker
    cases.push_back({"tri_2per", {0, 1, 2, 0, 1, 2}});

    HarnessOpts opts;
    opts.warmup_sec = cfg.warmup_sec;
    opts.duration_sec = cfg.duration_sec;
    opts.samples_per_worker = 20000;

    for (const auto& c : cases) {
        std::vector<WorkerTask*> tasks;
        for (size_t i = 0; i < c.cores.size(); ++i) {
            tasks.push_back(new DualTask(cfg.det_model, cfg.ocr_model,
                                         core_mask_of_index(c.cores[i])));
        }
        for (int rep = 1; rep <= cfg.repeats; ++rep) {
            const RunResult r = run_harness(c.name, Layer::kL2, c.cores, tasks, opts, cfg, rep);
            emit_json(r, cfg);
        }
        for (auto* t : tasks) delete t;
    }
}

}  // namespace nlb
