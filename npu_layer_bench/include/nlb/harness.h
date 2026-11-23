// nlb/harness.h —— 多 worker 测量骨架(第 1/2/3 层共用)
//
// ★ 为什么要一个骨架:
//   这三层的差别只有"每个 worker 干什么", 差别不在"怎么测"。
//   测量口径(什么时候算预热、什么时候开始计时、怎么算 FPS、怎么归一化
//   核占用)必须【完全一致】, 否则几层的数字没法互相印证。
//
// ★ 时间口径:
//   t0        : 所有 worker 都就绪的那一刻(闸门放行)
//   warmup_end= t0 + warmup_sec
//   meas_end  = warmup_end + duration_sec
//   只有【跑完落在 [warmup_end, meas_end) 区间内】的帧才计入统计。
//   FPS = 计入的帧数 / (meas_end - warmup_end)
#ifndef NLB_HARNESS_H_
#define NLB_HARNESS_H_

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "nlb/bench_types.h"
#include "nlb/stat.h"

namespace nlb {

// 一个 worker 在"跑一帧"时填回来的分段耗时(单位: 微秒)。
struct FrameTiming {
    uint64_t det_busy_us = 0;   // 检测模型 run + outputs_get
    uint64_t det_npu_us = 0;    // 驱动自报的检测推理耗时
    uint64_t ocr_busy_us = 0;   // 识别模型 run + outputs_get
    uint64_t ocr_npu_us = 0;    // 驱动自报的识别推理耗时
    uint64_t rga_us = 0;        // RGA 预处理
    uint64_t frame_us = 0;      // 单帧总墙钟(含排队)
};

// 一个 worker 干的事情。由调用方实现。
//
// ★ setup() 会在【worker 自己的线程】里被调用 —— RKNN 上下文不能跨线程。
class WorkerTask {
public:
    virtual ~WorkerTask() = default;

    // 在 worker 线程里建上下文 / 分配缓冲。
    virtual bool setup(int worker_idx, int core, std::string* err) = 0;

    // 跑一帧。返回 false 表示失败(计入 errors)。
    virtual bool run_frame(int worker_idx, FrameTiming* t) = 0;

    // 收尾(在 worker 线程里销毁上下文)。
    virtual void teardown() {}
};

// 一个 worker 的测量结果。
struct WorkerOut {
    uint64_t frames = 0;
    uint64_t errors = 0;
    uint64_t det_busy_us = 0;
    uint64_t det_npu_us = 0;
    uint64_t ocr_busy_us = 0;
    uint64_t ocr_npu_us = 0;
    uint64_t rga_us = 0;
    uint64_t frame_us = 0;

    LatencySeries ser_det;   // 检测 run+get 墙钟
    LatencySeries ser_ocr;   // 识别 run+get 墙钟
    LatencySeries ser_rga;   // RGA
    LatencySeries ser_frame; // 单帧总墙钟

    // 该 worker 绑在哪个核(用于分核归一化)。
    int core = -1;
};

// 骨架参数。
struct HarnessOpts {
    double warmup_sec = 5.0;
    double duration_sec = 30.0;
    size_t samples_per_worker = 20000;   // 每个 worker 每条序列的采样上限
    bool verbose = false;
};

// 跑一遍测量。返回聚合后的结果。
//
// cores[i] = 第 i 个 worker 绑定的核(0/1/2)。长度 = worker 数。
RunResult run_harness(const std::string& case_name, Layer layer,
                       const std::vector<int>& cores,
                       std::vector<WorkerTask*>& tasks,
                       const HarnessOpts& opts, const Config& cfg,
                       int repeat);

}  // namespace nlb

#endif  // NLB_HARNESS_H_
