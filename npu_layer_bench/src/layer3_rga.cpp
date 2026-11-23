// src/layer3_rga.cpp —— 第 3 层: RGA 预处理 + 检测推理
//
// ★ 测什么:
//   模拟一路 NV12 视频帧 -> RGA 做 NV12->RGB888 + 缩放 -> 直送 NPU 推理。
//   目的: 回答"RGA 这一环在整条链路里占多少, 加上它之后总吞吐是多少"。
//
// ★ 零拷贝链路的形状:
//   [NV12 dma-buf] --RGA--> [RGB888 dma-buf] == [NPU 输入 dma-buf]
//   RGA 直接写进 NPU 要读的那块物理内存, 中间没有任何 CPU memcpy。
//
// ★ 两个用例:
//   rga_1w : 一个 worker, RGA + 推理串行(测单路的真实成本)
//   rga_3w : 三个 worker, 每个都做 RGA + 推理 ——
//            librga 在一个进程里是串行的(每次调用都要 importbuffer_fd
//            注册缓冲, 那张全局 handle 表是进程内共享 + 加锁的),
//            所以这里能直接量出"RGA 抢锁排队"把单次 RGA 拖慢多少。
#include "nlb/layers.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>

#include "nlb/dma_buf.h"
#include "nlb/harness.h"
#include "nlb/npu_model.h"
#include "nlb/rga_pre.h"

namespace nlb {
namespace {

using clk = std::chrono::steady_clock;

inline uint64_t now_us() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            clk::now().time_since_epoch()).count());
}

// 一个 worker = 一块 NV12 源 + 一块【同时是 NPU 输入】的 RGB888 目标 + 一个上下文。
class RgaTask : public WorkerTask {
public:
    RgaTask(const std::string& det, int src_w, int src_h)
        : det_path_(det), src_w_(src_w), src_h_(src_h) {}

    ~RgaTask() override { cleanup(); }

    bool setup(int worker_idx, int core, std::string* err) override {
        (void)worker_idx;
        std::string e;

        // ---- 源: NV12 ----
        if (!rga_.init_src(src_w_, src_h_, &e)) {
            if (err) *err = "RGA 源: " + e;
            return false;
        }
        rga_.fill_src_pattern();   // 启动期写一次, 热循环里再也不写

        // ---- 目标: RGB888, 这块内存【就是】NPU 的输入 ----
        //   先建上下文拿到模型输入尺寸, 再按它分配目标缓冲。
        NpuModel probe;
        if (!probe.init(det_path_, core_mask_of_index(core), -1, nullptr, 0, false, true, &e)) {
            if (err) *err = "检测模型: " + e;
            return false;
        }
        const int dst_w = static_cast<int>(probe.in_w());
        const int dst_h = static_cast<int>(probe.in_h());
        probe.destroy();

        dst_w_ = dst_w;
        dst_h_ = dst_h;
        // RGA 在缩放比不是整数倍时会多写 1~2 行 -> 目标留 16 行余量
        dst_size_ = static_cast<size_t>(dst_w) * 3 * (dst_h + kRgaDstPadRows);
        dst_fd_ = dma_buf_alloc(dst_size_, /*uncached=*/true);
        if (dst_fd_ < 0) {
            if (err) *err = std::string("目标 dma-buf 分配失败: ") + dma_buf_backend();
            return false;
        }
        dst_ptr_ = dma_buf_map(dst_fd_, dst_size_);
        if (!dst_ptr_) {
            if (err) *err = "目标 dma-buf mmap 失败";
            return false;
        }

        // ---- NPU 上下文: 输入直接绑到那块目标 dma-buf ----
        if (!model_.init(det_path_, core_mask_of_index(core), dst_fd_, dst_ptr_,
                         dst_size_, false, true, &e)) {
            if (err) *err = "检测模型(外部 fd): " + e;
            return false;
        }
        return true;
    }

    bool run_frame(int worker_idx, FrameTiming* t) override {
        (void)worker_idx;
        const uint64_t f0 = now_us();

        const uint64_t r0 = now_us();
        const int rrc = rga_.convert(dst_fd_, dst_ptr_, dst_size_, dst_w_, dst_h_);
        const uint64_t r1 = now_us();
        if (rrc != 0) return false;

        const uint64_t a0 = now_us();
        const int rc = model_.run();
        const uint64_t a1 = now_us();
        if (rc != 0) return false;

        const uint64_t f1 = now_us();
        t->rga_us = r1 - r0;
        t->det_busy_us = a1 - a0;
        t->frame_us = f1 - f0;
        t->det_npu_us = model_.query_run_us();
        return true;
    }

    void teardown() override { cleanup(); }

private:
    void cleanup() {
        model_.destroy();
        rga_.destroy();
        if (dst_ptr_) {
            dma_buf_unmap(dst_ptr_, dst_size_);
            dst_ptr_ = nullptr;
        }
        if (dst_fd_ >= 0) {
            ::close(dst_fd_);
            dst_fd_ = -1;
        }
    }

    std::string det_path_;
    int src_w_, src_h_;
    int dst_w_ = 0, dst_h_ = 0;
    size_t dst_size_ = 0;
    int dst_fd_ = -1;
    void* dst_ptr_ = nullptr;
    RgaConverter rga_;
    NpuModel model_;
};

}  // namespace

void run_layer3(const Config& cfg) {
    fprintf(stderr, "\n========== 第 3 层: RGA 预处理 + 检测推理 ==========\n");
    fprintf(stderr, "模型: %s\n", cfg.det_model.c_str());
    fprintf(stderr, "源: NV12 %dx%d -> RGB888(模型尺寸) -> NPU 输入(同一块 dma-buf)\n",
            cfg.src_w, cfg.src_h);
    fprintf(stderr, "每组: 预热 %.0fs + 测量 %.0fs, 重复 %d 次\n",
            cfg.warmup_sec, cfg.duration_sec, cfg.repeats);

    struct Case {
        std::string name;
        std::vector<int> cores;
    };
    std::vector<Case> cases;
    cases.push_back({"rga_1w", {0}});
    cases.push_back({"rga_3w", {0, 1, 2}});

    HarnessOpts opts;
    opts.warmup_sec = cfg.warmup_sec;
    opts.duration_sec = cfg.duration_sec;
    opts.samples_per_worker = 20000;

    for (const auto& c : cases) {
        std::vector<WorkerTask*> tasks;
        for (size_t i = 0; i < c.cores.size(); ++i) {
            tasks.push_back(new RgaTask(cfg.det_model, cfg.src_w, cfg.src_h));
        }
        for (int rep = 1; rep <= cfg.repeats; ++rep) {
            const RunResult r = run_harness(c.name, Layer::kL3, c.cores, tasks, opts, cfg, rep);
            emit_json(r, cfg);
        }
        for (auto* t : tasks) delete t;
    }
}

}  // namespace nlb
