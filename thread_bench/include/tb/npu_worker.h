// tb/npu_worker.h —— NPU 工作线程
//
// ★ 一个 worker = 一个(或两个)【独立的 RKNN 上下文】+ 各自的零拷贝输入缓冲
//
// ★★ 硬约束(主工程踩过, 注释在 rk3588_zero_copy/src/rk_base/npu_pool.cc:269):
//   RKNN 上下文【不能跨线程使用】。在 A 线程 rknn_init, 拿 B 线程 rknn_run,
//   运行时内部会死锁 —— 表现是所有线程全卡在 futex, 一帧都不动。
//   所以 ctx 的【创建 / 使用 / 销毁】必须全部发生在同一个 worker 线程内。
//   本类把这件事做成结构性的: loop() 是唯一接触 ctx 的地方, 它就是那个线程。
//
// ★ 零拷贝:
//   输入缓冲用 rknn_create_mem 让运行时分配一块【物理连续、可被 NPU 直接读】
//   的内存, 用 rknn_set_io_mem 绑到模型输入张量上。
//   我们只在【启动时】往这块缓冲里写一次合成图案, 热循环里一次都不碰它 ——
//   所以循环里没有 memcpy, 也没有任何形式的内存拷贝。
#ifndef TB_NPU_WORKER_H_
#define TB_NPU_WORKER_H_

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "tb/bench_types.h"
#include "tb/frame_queue.h"
#include "tb/latency_rec.h"

namespace tb {

// 模型字节(启动期一次性读入, 之后所有 ctx 共享这一份只读副本)
struct ModelBlob {
    std::string path;
    std::vector<uint8_t> bytes;
};

class NpuWorker {
public:
    NpuWorker(int idx, int core_mask, int core_mask2,
              const ModelBlob* m1, const ModelBlob* m2,
              LatencyRecorder* rec, const Config* cfg, FrameQueue* queue);
    ~NpuWorker();

    NpuWorker(const NpuWorker&) = delete;
    NpuWorker& operator=(const NpuWorker&) = delete;

    void start();
    void stop();
    void join();

    // CPU 亲和(空 = 不绑)
    void set_cpus(std::vector<int> cpus) { cpus_ = std::move(cpus); }

    bool ready() const { return ready_.load(std::memory_order_acquire); }
    bool failed() const { return failed_.load(std::memory_order_acquire); }
    const std::string& error() const { return err_; }
    int index() const { return idx_; }

    void reset_counters();

    uint64_t frames() const { return frames_.load(std::memory_order_relaxed); }
    uint64_t errors() const { return errors_.load(std::memory_order_relaxed); }
    uint64_t chan_frames(int c) const;
    // 墙钟口径: rknn_run + outputs_get 的总时间(含在 NPU 队列里【等待】的时间)。
    //   注意: 同一颗核上挂多个上下文时, 等待时间会被重复计一遍, 所以这个数
    //         可以超过 100%。它反映的是"核上有多少请求在排队"。
    double busy1_ms() const { return busy1_us_.load(std::memory_order_relaxed) / 1000.0; }
    double busy2_ms() const { return busy2_us_.load(std::memory_order_relaxed) / 1000.0; }
    // 驱动口径: rknn_query(RKNN_QUERY_PERF_RUN) 报的【真实推理耗时】。
    //   这是核真正在算的时间, 同一颗核上多个上下文时不会重复计 —— 用它算核占用率。
    double npu1_ms() const { return npu1_us_.load(std::memory_order_relaxed) / 1000.0; }
    double npu2_ms() const { return npu2_us_.load(std::memory_order_relaxed) / 1000.0; }

    int core_mask1() const { return core_mask1_; }
    int core_mask2() const { return core_mask2_; }

    // NPU 侧内存(该 worker 的 ctx 合计)
    uint64_t npu_weight_bytes() const { return npu_weight_bytes_; }
    uint64_t npu_internal_bytes() const { return npu_internal_bytes_; }
    uint64_t npu_dma_bytes() const { return npu_dma_bytes_; }

    // 模型输入尺寸(第一个 ctx 的)
    uint32_t in_h() const { return in_h_; }
    uint32_t in_w() const { return in_w_; }

private:
    void loop();
    void apply_affinity() const;

    int idx_ = 0;
    int core_mask1_ = 0;
    int core_mask2_ = 0;

    const ModelBlob* m1_ = nullptr;
    const ModelBlob* m2_ = nullptr;
    LatencyRecorder* rec_ = nullptr;
    const Config* cfg_ = nullptr;
    FrameQueue* queue_ = nullptr;

    std::thread th_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> failed_{false};
    std::atomic<uint64_t> frames_{0};
    std::atomic<uint64_t> errors_{0};
    std::atomic<uint64_t> busy1_us_{0};
    std::atomic<uint64_t> busy2_us_{0};
    std::atomic<uint64_t> npu1_us_{0};
    std::atomic<uint64_t> npu2_us_{0};
    std::atomic<uint64_t> chan_[16];

    std::string err_;
    std::vector<int> cpus_;

    uint32_t in_h_ = 0;
    uint32_t in_w_ = 0;
    uint64_t npu_weight_bytes_ = 0;
    uint64_t npu_internal_bytes_ = 0;
    uint64_t npu_dma_bytes_ = 0;
};

}  // namespace tb

#endif  // TB_NPU_WORKER_H_
