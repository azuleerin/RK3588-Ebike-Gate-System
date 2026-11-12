// rk/npu_pipe.h —— NPU 多实例并行检测(双核负载均衡)
//
// ★ 为什么需要它
//   RK3588 的 NPU 有三个核, 但单个模型上下文默认只在一个核上跑
//   (实测 Core0 约 50%, Core1/Core2 空转)。
//   把【同一个检测模型】复制成 N 份, 每份绑一个核, 再用双缓冲让它们
//   【同时】处理相邻的两帧, 检测阶段的吞吐就能翻倍。
//
//   实测(300 帧, 1920x1080):
//     单实例 auto            30.1 ms/帧  (33.2 fps)
//     单实例 core0_1(双核)   38.2 ms/帧  (26.2 fps)  <- 反而更慢!
//     双实例 core0+core1 并行 15.9 ms/帧  (62.8 fps)  <- 1.9 倍
//
// ★ 三个坑(都踩过)
//   1) RKNN 上下文【不能跨线程使用】: 在 A 线程 rknn_init, 拿到 B 线程
//      rknn_run, 运行时内部会死锁(所有线程卡在 futex)。所以每个实例的
//      上下文必须【在它自己的工作线程里创建和使用】。
//   2) 输入缓冲不能被覆写: 同一块 dma-buf 在推理期间被 RGA 重写, 驱动读到的
//      就是半新半旧的数据。所以每个实例一块独立缓冲, 提交前要确认这块
//      上一次的推理已经结束。
//   3) 计时口径: 并行下"各阶段耗时相加"没有意义(主线程预处理和工作线程
//      推理是重叠的), 吞吐只能用【墙钟】算。
#ifndef RK_NPU_PIPE_H_
#define RK_NPU_PIPE_H_

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rknn_api.h>

#include "yolo_post.h"

namespace rk {

// 一帧的附加信息(后处理需要)
struct NpuPipeJob {
    int   img_w = 0;
    int   img_h = 0;
    float scale = 1.f;
    int   pad_x = 0;
    int   pad_y = 0;
    bool  letterbox = false;
};

class NpuPipe {
public:
    ~NpuPipe() { stop(); }

    // n_instance 个实例, core_masks[i] 是第 i 个实例绑的核
    // (1=CORE0, 2=CORE1, 4=CORE2, 3=CORE0_1, 0=自动)
    bool init(const std::string& model_path, int model_w, int model_h, int n_instance,
              const int* core_masks, float conf, float nms, int n_output);

    int         instance_count() const { return n_instance_; }
    int         fd(int i) const;              // 输入缓冲 fd(给 RGA 当目标)
    void*       ptr(int i) const;            // 输入缓冲 CPU 地址
    size_t      buf_size() const { return buf_size_; }

    // 提交第 i 个实例的推理(主线程调用)
    void submit(int instance, const NpuPipeJob& job);
    // 取最早提交、且已经算完的一帧(阻塞)
    bool acquire(std::vector<YoloBox>* out, double* last_infer_ms);

    void stop();

private:
    struct Slot {
        std::mutex m;
        std::condition_variable cv;
        int         job = -1;        // 待处理的帧下标
        bool        have = false;    // 有活
        bool        busy = false;    // 正在干
        bool        ready = false;   // 上下文建好了
        bool        stop = false;
        NpuPipeJob  desc;
        uint64_t    seq = 0;        // 本次任务的序号
        uint64_t    seq_done = 0;
        double      infer_ms = 0;
    };
    struct Result {
        uint64_t seq = 0;
        std::vector<YoloBox> boxes;
    };

    void worker_loop(int idx);

    int n_instance_ = 0;
    int model_w_ = 0, model_h_ = 0;
    float conf_ = 0.25f, nms_ = 0.45f;
    int  n_output_ = 1;
    size_t buf_size_ = 0;
    std::vector<uint8_t> model_data_;
    std::vector<int> fds_;
    std::vector<void*> ptrs_;
    std::vector<rknn_context> ctxs_;
    std::vector<std::unique_ptr<Slot>> slots_;
    std::vector<std::thread> workers_;
    std::mutex res_m_;
    std::condition_variable res_cv_;
    std::vector<Result> results_;
    uint64_t next_seq_ = 0;
    bool stop_ = false;
};

}  // namespace rk

#endif  // RK_NPU_PIPE_H_
