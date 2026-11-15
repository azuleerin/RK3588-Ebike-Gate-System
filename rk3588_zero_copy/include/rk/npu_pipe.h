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
    // ★ 这一帧【被采集到】的时刻(单调时钟毫秒)。
    //   并行模式下结果会比当前帧晚 K 帧才回来(流水线深度 K),
    //   跟踪/测速必须用【这一帧自己的】时间戳, 不能用"现在" ——
    //   否则 dt 算错, 速度会系统性偏大。
    int64_t ts_ms = 0;
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
    bool acquire(std::vector<YoloBox>* out, double* last_infer_ms,
                 int64_t* out_ts_ms = nullptr);

    // 非阻塞地取一帧已算完的结果; 没有就返回 false。
    // ★ 主循环要靠它【攒够在途帧数再取】, 多个实例才会同时在跑 ——
    //   每帧都阻塞 acquire 的话, 同一时刻只有一个实例有活干,
    //   "--npu-pipe 3" 就退化成了单实例(实测: 3 实例和 1 实例一样慢)。
    bool try_acquire(std::vector<YoloBox>* out, double* last_infer_ms,
                     int64_t* out_ts_ms = nullptr);

    // 等第 i 个实例把手上的活干完。
    // ★ 必须在 RGA 往【这个实例的输入缓冲】里写之前调用 ——
    //   否则 RGA 会把 NPU 正在读的那块 dma-buf 覆写掉(画面会花)。
    //   (以前这个等待藏在 submit() 里, 已经在 RGA 之后了, 顺序是错的。)
    bool wait_slot_free(int instance, int timeout_ms = 5000);

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
        // 这个实例【本次】推理+后处理实际花了多少毫秒。
        // ★ 以前这个值只存在 Slot 里, 从来没传给主线程 —— 于是并行模式下
        //   acquire() 只能返回 0, 统计里的 npu_run 永远是 0, 没法做基准。
        double infer_ms = 0.0;
        // 这一帧被采集到的时刻(跟 boxes 一起回来)
        int64_t ts_ms = 0;
    };

    void worker_loop(int idx);

    int n_instance_ = 0;
    int model_w_ = 0, model_h_ = 0;
    float conf_ = 0.25f, nms_ = 0.45f;
    int  n_output_ = 1;
    size_t buf_size_ = 0;
    // 每个实例绑哪个核(1=CORE0, 2=CORE1, 4=CORE2)。
    // ★ 必须逐实例记下来 —— 以前 worker 里写死 "idx==0?core0:core1",
    //   第三个实例会和第二个抢核, "三核并行" 根本不存在。
    std::vector<int> core_masks_;
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
