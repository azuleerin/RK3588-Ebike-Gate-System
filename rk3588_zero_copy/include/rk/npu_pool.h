// rk/npu_pool.h —— 全局 NPU 任务池(多路共享一个调度器)
//
// ★ 为什么需要它
//
//   4 路大门原来是【4 个独立进程】(run_4ch_gate_demo.sh 各起一个 rk_zero_copy):
//     Ch1/Ch3 近景: 检测绑 Core0, 车牌识别绑 Core2
//     Ch2/Ch4 高位: 检测绑 Core1
//   进程之间没法共享 RKNN 上下文, 于是:
//     · 同一个核上挤着两个进程 -> 谁先谁后由 NPU 驱动的队列决定, 我们管不了
//     · 识别(PP-OCR, 一次 ~24ms)和检测(YOLO, 一次 ~30ms)权重一样高,
//       近景车道的识别会被检测任务压住 -> 识别通道【饥饿】
//     · 4 份进程各带一份模型 + 一份推理栈, 内存和 CPU 都是 4 倍
//
//   这个池把 4 路收进【一个进程】, 共用一条按优先级排的任务队列:
//     · 每颗 NPU 核 = 一个 worker, worker 拥有自己的 RKNN 上下文并绑核
//     · 识别任务优先级高于检测任务 -> 识别永远排在检测前面, 不会饿死
//     · 一条路的车牌识别不会再被另一路的检测任务挤掉
//
// ★ 三个硬约束(都踩过)
//   1) RKNN 上下文【不能跨线程使用】: 在 A 线程 rknn_init, 拿 B 线程
//      rknn_run, 运行时内部会死锁。所以每个上下文必须在【它自己的线程】里
//      创建和使用 —— worker 线程一起来就建自己的上下文。
//   2) 输入缓冲不能被覆写: 同一块 dma-buf 在推理期间被 RGA 重写, 驱动读到的
//      就是半新半旧的数据。所以每个 slot 一块独立缓冲, 用完才还。
//   3) 计时口径: 并行下"各阶段耗时相加"没有意义(主线程预处理和工作线程
//      推理是重叠的), 吞吐只能用【墙钟】算。
#ifndef RK_NPU_POOL_H_
#define RK_NPU_POOL_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <rknn_api.h>

#include "license_plate_rec.h"
#include "yolo_post.h"

namespace rk {

// ---------------------------------------------------------------------------
// 任务类型。kOcr 的优先级高于 kDet —— 这是"识别优先"的落点。
// ---------------------------------------------------------------------------
enum class NpuTaskKind { kDet = 0, kOcr = 1 };

// ---------------------------------------------------------------------------
// 检测任务
//
//   buf_id : 输入 slot 的编号(RGA 已经把图写进那块 dma-buf)
//   ts_ms  : 这一帧【被采集到】的时刻(单调时钟毫秒)
//            跟踪/测速必须用它自己的时间戳, 不能用"现在" ——
//            否则 dt 算错, 速度会系统性偏大。
// ---------------------------------------------------------------------------
struct DetTask {
    int      chan = 0;
    int      slot = -1;        // 输入 slot(RGA 的目标)
    int      img_w = 0;
    int      img_h = 0;
    float    scale = 1.f;
    int      pad_x = 0;
    int      pad_y = 0;
    bool     letterbox = false;
    int64_t  ts_ms = 0;
};

struct DetResult {
    int      chan = 0;
    int      slot = -1;
    std::vector<YoloBox> boxes;
    double   infer_ms = 0.0;   // 这一次的真实推理耗时(worker 里测的)
    int64_t  ts_ms = 0;
    bool     ok = false;
};

// ---------------------------------------------------------------------------
// 识别任务
//
//   一块【已经裁好 + 按比例缩放好】的车牌图: 紧凑行距, 每行 roi_w*3 字节,
//   共 roi_h 行。通道序 BGR(和官方 PP-OCR 示例一致)。
//
//   group/slot: 同一块车牌可能要试几个切分点(两行牌的上行要试 5 个),
//   所以一次识别会提交多个候选, 用 group 归组、slot 编号。
// ---------------------------------------------------------------------------
struct OcrTask {
    int      chan = 0;
    int      group = 0;        // 同一块车牌的一组候选
    int      slot = 0;         // 候选编号
    bool     is_top = true;    // 上行候选(挑"认出字符最多"的)还是下行
    std::vector<uint8_t> roi;  // 紧凑 BGR, roi_h 行 x (roi_w*3) 字节
    int      roi_w = 0;
    int      roi_h = 0;
    int64_t  ts_ms = 0;
};

struct OcrResult {
    int      chan = 0;
    int      group = 0;
    int      slot = 0;
    std::string text;
    float    score = 0.f;
    double   infer_ms = 0.0;
    bool     ok = false;
};

// ---------------------------------------------------------------------------
// 每核的忙闲统计(应用内自测口径)
//
//   不用 /sys/kernel/debug/rknpu/load —— 那个节点要 root, 现场一般读不到。
//   这里直接累加本核上 rknn_run 的墙钟耗时:
//       某核占用% = 该核上所有推理耗时之和 / 采样窗口长度
//   比 debugfs 的"驱动忙碌百分比"更贴近业务口径: 它精确地告诉我们
//   "这颗核上跑了多少模型推理时间", 而不只是"驱动有多忙"。
// ---------------------------------------------------------------------------
struct CoreStat {
    double   busy_ms = 0.0;    // 该核上累计的【真 NPU】耗时(rknn_run + 取输出)
    double   total_ms = 0.0;   // 加上 CPU 后处理的总占用
    uint64_t tasks = 0;        // 该核上完成的任务数
    int      core_mask = 0;    // 该 worker 绑的核
    int      kind = 0;         // 0=检测 1=识别
};

// ---------------------------------------------------------------------------
class NpuPool {
public:
    struct Config {
        // ---- 检测模型 ----
        std::string det_model;
        int   det_w = 640;
        int   det_h = 640;
        int   det_n_output = 1;
        float conf = 0.25f;
        float nms  = 0.45f;

        // ---- 核绑定(掩码) ----
        //   det_core_mask: 检测可用核集合(1=core0 2=core1 4=core2)
        //   ocr_core_mask: 识别可用核集合
        //   例: det=6 (core1+core2), ocr=1 (core0) —— 本次落地方案
        int   det_core_mask = 0;
        int   ocr_core_mask = 0;

        // ---- 每个检测 worker 几个 slot(双缓冲 = 2) ----
        //   slot = (一个 RKNN 上下文 + 一块输入 dma-buf)。
        //   给 2 个是为了: RGA 往 slot1 写的时候, worker 还在跑 slot0,
        //   两颗核就不���"写完再等"的空泡。
        int   det_slots_per_worker = 2;

        // ---- 识别模型 ----
        PlateRecConfig ocr_cfg;
    };

    ~NpuPool() { stop(); }

    // 起 worker 线程(每个线程里建自己的 RKNN 上下文)。
    bool init(const Config& cfg);
    void stop();

    // ---- 检测 slot 的借还 ----
    //   借一块 slot(RGA 的目标), 提交完任务后由 worker 归还。
    //   借不到就阻塞(背压)。这是 4 路之间的天然限流阀。
    int  acquire_det_slot();
    // 借了没用(比如 RGA 失败)就还回去 —— 否则这块 slot 永远回不到池里。
    void release_det_slot(int slot);
    int  slot_fd(int slot) const;
    void* slot_ptr(int slot) const;

    // ---- 提交 ----
    bool submit_det(DetTask&& t);
    bool submit_ocr(OcrTask&& t);

    // ---- 取结果 ----
    //   检测: 按通道取(4 个通道各自等自己的结果)
    bool take_det(int chan, DetResult* out, int timeout_ms);
    //   识别: 按 (通道, group, slot) 取
    bool take_ocr(int chan, int group, int slot, OcrResult* out, int timeout_ms);

    // ---- 统计 ----
    std::vector<CoreStat> core_stats() const;
    //   从进程启动到现在过了多少毫秒(算每核占用率的分母)
    double uptime_ms() const;
    //   排队中的任务数(诊断用: 一直不为 0 说明 NPU 是瓶颈)
    int  pending() const;

private:
    // 一个 slot = 一个 RKNN 上下文 + 一块输入 dma-buf
    struct DetSlot {
        int            id = -1;
        int            worker = -1;      // 归属的 worker(决定谁来跑它)
        rknn_context   ctx = 0;
        rknn_tensor_mem* in_mem = nullptr;
        int            fd = -1;
        void*          ptr = nullptr;
        size_t         buf_size = 0;
        size_t         buf_alloc = 0;
        std::vector<rknn_tensor_attr> out_attrs;
        bool           ready = false;
        bool           busy = false;     // 已被某个任务占用
    };

    struct DetWorker {
        int         core = 0;            // 核号 0..2
        int         core_mask = 0;
        std::vector<int> slots;          // 归属的 slot
        std::thread th;
        std::atomic<bool> ready{false};  // 上下文建好了(给主线程看)
    };

    struct OcrWorker {
        int         core = 0;
        int         core_mask = 0;
        std::unique_ptr<LicensePlateRec> rec;
        std::thread th;
        std::atomic<bool> ready{false};
    };

    // 全局队列里的一项
    struct Task {
        NpuTaskKind kind = NpuTaskKind::kDet;
        int         priority = 0;        // 越大越优先(识别 = 1)
        uint64_t    seq = 0;
        int         worker = -1;         // 只有该 worker 能跑(检测用)
        int         slot = -1;
        int         chan = 0;
        int         group = 0;
        int         slot_idx = 0;
        DetTask     det;
        OcrTask     ocr;
    };

    // ---- 每核闸门: 同一颗核被多个 worker 共用时, 按优先级排队 ----
    //
    //   ★ 为什么需要: --ocr-core 0 --det-core 0,1,2 时, core0 上有
    //     一个识别 worker 和一个检测 worker, 谁先跑?
    //     没有闸门的话就是驱动随手决定, 识别可能一直被检测压住。
    //   闸门保证: 等在同一颗核上的任务里, 识别永远排在检测前面。
    class CoreGate {
    public:
        void acquire(int priority);
        void release();
    private:
        struct Waiter { int priority; uint64_t ticket; };
        std::mutex m_;
        std::condition_variable cv_;
        bool busy_ = false;
        uint64_t next_ticket_ = 0;
        // key = 优先级(大者优先), 同优先级按 ticket 先来先服务
        std::multimap<int, Waiter, std::greater<int>> waiters_;
    };

    void det_worker_loop(int widx);
    void ocr_worker_loop(int widx);

    // 从全局队列里取一个【该 worker 能跑】且优先级最高的任务。
    bool pop_task(int worker_idx, NpuTaskKind kind, Task* out);
    bool pop_ocr_task(Task* out);

    void publish_det(const DetResult& r);
    void publish_ocr(const OcrResult& r);

    bool load_model_file(const std::string& path, std::vector<uint8_t>* out) const;

    Config cfg_;

    // ---- 检测模型字节(只读一份, 各 worker 的上下文各自拷贝) ----
    std::vector<uint8_t> det_model_data_;

    // ---- 检测 slot ----
    std::vector<DetSlot> det_slots_;
    // ★ worker 用 unique_ptr 存: DetWorker/OcrWorker 里有 std::thread 和
    //   std::atomic, 两者都【不可拷贝】(atomic 连移动都没有),
    //   直接放进 vector 编译不过 —— 必须用指针装。
    std::vector<std::unique_ptr<DetWorker>> det_workers_;
    std::vector<std::unique_ptr<OcrWorker>> ocr_workers_;

    // ---- 每核一个闸门(索引 = 核号) ----
    std::vector<std::unique_ptr<CoreGate>> gates_;

    // slot 的借还
    std::mutex slot_m_;
    std::condition_variable slot_cv_;
    std::deque<int> free_slots_;

    // ---- 全局优先级队列 ----
    mutable std::mutex q_m_;
    std::condition_variable q_cv_;
    // key = 优先级(大者优先), 同优先级 FIFO
    std::multimap<int, Task, std::greater<int>> queue_;
    uint64_t next_seq_ = 0;
    bool stop_ = false;

    // ---- 结果回收 ----
    std::mutex res_m_;
    std::condition_variable res_cv_;
    std::map<int, std::deque<DetResult>> det_results_;    // 按通道
    std::map<std::tuple<int,int,int>, std::deque<OcrResult>> ocr_results_;

    // ---- 每核统计 ----
    mutable std::mutex stat_m_;
    std::vector<CoreStat> stats_;

    std::chrono::steady_clock::time_point t0_;
};

}  // namespace rk

#endif  // RK_NPU_POOL_H_
