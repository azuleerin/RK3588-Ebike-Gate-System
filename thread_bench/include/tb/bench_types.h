// tb/bench_types.h —— thread_bench 的公共类型定义
//
// ★ 本测试台【完全不接入主工程】(rk3588_zero_copy):
//   不共用它的任何 .cc/.h, 不改它的 CMakeLists, 只借用板上的
//   librknnrt.so + rknn_api.h。目的是在【隔离环境】里单独回答:
//   "RK3588 三核 NPU 在多路并发下, 最佳工作线程数 T 是多少?"
//
// ★ 术语对齐(与主工程 npu_pool 一致, 免得两套口径打架):
//   run / npu ms : rknn_run + rknn_outputs_get —— 这才是【真正占着 NPU 核】的时间
//   e2e ms       : 生产者入队 -> 结果就绪 —— 业务口径的端到端延迟(含排队)
//   两者之差      = NPU 排队时间
#ifndef TB_BENCH_TYPES_H_
#define TB_BENCH_TYPES_H_

#include <cstdint>
#include <string>
#include <vector>

namespace tb {

// ---------------------------------------------------------------------------
// 核绑定策略
//
//   RKNN 的核掩码约定(rknn_set_core_mask 的入参):
//     0 = CORE_AUTO(驱动自己选)  1 = CORE0  2 = CORE1  4 = CORE2  7 = 三核全开
// ---------------------------------------------------------------------------
enum class Strategy {
    kAuto = 0,   // A: 所有 worker 用 RKNN_NPU_CORE_AUTO(0), 交给驱动调度
    kRound = 1,  // B: worker i 绑 core (i % 3), 掩码 1/2/4 轮流
    kAll3 = 2,   // C: 所有 worker 用 RKNN_NPU_CORE_0_1_2(7), 测 RKNN 内部调度开销
};

inline const char* strategy_name(Strategy s) {
    switch (s) {
        case Strategy::kAuto: return "auto";
        case Strategy::kRound: return "round";
        case Strategy::kAll3: return "all3";
    }
    return "?";
}

// worker i 在该策略下的核掩码。
inline int strategy_mask(Strategy s, int worker_idx) {
    switch (s) {
        case Strategy::kAuto: return 0;                  // RKNN_NPU_CORE_AUTO
        case Strategy::kRound: return 1 << (worker_idx % 3);  // 1/2/4
        case Strategy::kAll3: return 7;                  // CORE_0_1_2
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Producer 模式
// ---------------------------------------------------------------------------
enum class ProducerMode {
    kNormal = 0,    // 按 --fps 错相推流(每路 fps, 合计 fps*channels)
    kUnlimited = 1, // 死循环狂推, 无限速 —— 压"丢帧防雪崩"这条路径
};

inline const char* producer_mode_name(ProducerMode m) {
    return m == ProducerMode::kUnlimited ? "unlimited" : "normal";
}

// ---------------------------------------------------------------------------
// CPU 亲和(pthread_setaffinity_np)
// ---------------------------------------------------------------------------
enum class CpuAffinity {
    kNone = 0,  // 不绑
    kBig = 1,   // 绑到 A76 大核(RK3588 上是 cpu4-7)
    kAll = 2,   // 绑到全部核
};

inline const char* cpu_affinity_name(CpuAffinity a) {
    switch (a) {
        case CpuAffinity::kNone: return "none";
        case CpuAffinity::kBig: return "big";
        case CpuAffinity::kAll: return "all";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// 延迟采样序列
// ---------------------------------------------------------------------------
enum LatSeries {
    kLatE2E = 0,     // 入队 -> 结果就绪(含排队等待)
    kLatModel1 = 1,  // 模型1: rknn_run + outputs_get
    kLatModel2 = 2,  // 模型2: rknn_run + outputs_get
    kLatSeriesCount = 3
};

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------
struct Config {
    // ---- 模型 ----
    std::string model;                  // 主模型(检测)
    std::string model2;                 // 第二模型(识别); 空 = 单模型模式
    int model2_core_mask = -1;          // -1 = 与主模型同掩码; 否则显式指定

    // ---- 测试矩阵 ----
    std::vector<int> threads{4, 6, 8, 12};
    std::vector<Strategy> strategies{Strategy::kAuto, Strategy::kRound, Strategy::kAll3};
    std::vector<size_t> queue_sizes{64};

    // ---- 负载 ----
    int channels = 4;                   // 模拟几路摄像头
    double fps = 25.0;                  // 每路帧率
    ProducerMode producer_mode = ProducerMode::kNormal;
    uint64_t unlimited_yield_every = 4096;  // unlimited 模式下每推 N 帧让一次 CPU

    // ---- 时序 ----
    double warmup_sec = 5.0;            // 预热(不计入统计)
    double duration_sec = 30.0;         // 测量窗口

    // ---- 其它 ----
    CpuAffinity cpu_affinity = CpuAffinity::kNone;
    bool per_channel_buffers = false;   // 每 worker 每路各一块输入缓冲(默认关)
    std::string npu_load_path;          // debugfs NPU load 节点; 空 = 自动探测
    std::string out_json;               // 结果 JSONL 输出路径; 空 = 只打 stdout
    bool quiet = false;                 // 压制监控线程的每秒打印
};

// ---------------------------------------------------------------------------
// 延迟统计
// ---------------------------------------------------------------------------
struct SeriesStat {
    uint64_t count = 0;
    double avg_ms = 0.0;
    double p50_ms = 0.0;
    double p90_ms = 0.0;
    double p99_ms = 0.0;
    double max_ms = 0.0;
    double min_ms = 0.0;
};

// ---------------------------------------------------------------------------
// 单次配置的结果
// ---------------------------------------------------------------------------
struct RunResult {
    // ---- 身份 ----
    int threads = 0;
    Strategy strategy = Strategy::kAuto;
    size_t queue_size = 0;
    ProducerMode producer_mode = ProducerMode::kNormal;
    int channels = 0;
    double duration_sec = 0.0;
    bool dual_model = false;

    // ---- 吞吐 ----
    uint64_t frames = 0;         // 窗口内完成的推理帧数
    uint64_t pushed = 0;         // 生产者推入次数
    uint64_t dropped = 0;        // 队列满时被丢掉的最旧帧数
    uint64_t leftover = 0;       // 窗口结束时仍在队列里的帧数
    double throughput_fps = 0.0; // 成功推理帧数 / 窗口秒数
    double arrival_fps = 0.0;    // 推入帧数 / 窗口秒数
    double drop_rate = 0.0;      // dropped / pushed

    // ---- 延迟 ----
    SeriesStat lat[kLatSeriesCount];

    // ---- 分通道 ----
    uint64_t chan_frames[16] = {0};

    // ---- 资源 ----
    double cpu_pct = 0.0;              // 整机 CPU 占用(8 核归一)
    int64_t rss_peak_kb = 0;           // VmHWM
    int64_t rss_end_kb = 0;            // 窗口结束时的 VmRSS
    double heap_slope_kb_min = 0.0;    // 堆增长斜率(KB/min) —— 内存平线的证据

    // ---- NPU 占用: 两个口径 ----
    // npu_busy_agg_pct : 墙钟口径(rknn_run + outputs_get)合计。
    //   同一颗核上挂多个上下文时【会超过 100%】—— 那是"排队", 不是利用率。
    // npu_compute_agg_pct: 驱动口径(RKNN_QUERY_PERF_RUN 报的真实推理耗时)合计,
    //   这才是核真正在算的时间, 用来和 300%(三核满载)比。
    double npu_busy_agg_pct = 0.0;
    double npu_compute_agg_pct = 0.0;

    // ---- 分核占用 ----
    // core_total_pct: 该核的【总】占用(模型1 + 模型2), 已按"该核上的 worker 数"
    //   归一化, 上限 100%。双模型时两个上下文在同一个 worker 里是【串行】的,
    //   所以必须把两个模型的耗时加在一起再归一化, 否则会低估一半。
    // core_m1_pct / core_m2_pct: 按模型拆开的明细(各自归一化), 用于看是哪个模型
    //   先被拖慢。
    // -1 = 无法归属, 例如 AUTO 策略下不知道跑在哪颗核上。
    double core_total_pct[3] = {-1.0, -1.0, -1.0};
    double core_total_sum_pct[3] = {-1.0, -1.0, -1.0};
    double core_m1_pct[3] = {-1.0, -1.0, -1.0};
    double core_m2_pct[3] = {-1.0, -1.0, -1.0};
    // 原始和(未归一化): 同一核上多个上下文时 > 100%, 那就是排队放大倍数
    double core_m1_sum_pct[3] = {-1.0, -1.0, -1.0};
    double core_m2_sum_pct[3] = {-1.0, -1.0, -1.0};
    // 每颗核上挂了几个 worker
    int workers_per_core1[3] = {0, 0, 0};
    int workers_per_core2[3] = {0, 0, 0};

    // ---- debugfs NPU load(需 root; 不可读时全 -1) ----
    bool npu_load_ok = false;
    double npu_load[3] = {-1.0, -1.0, -1.0};
    double npu_load_max[3] = {-1.0, -1.0, -1.0};

    // ---- NPU 侧内存(每个 ctx 一份权重, T 个 worker 就是 T 份) ----
    uint64_t npu_weight_bytes = 0;     // 单个 ctx 的权重大小
    uint64_t npu_internal_bytes = 0;   // 单个 ctx 的内部(激活)缓冲大小
    uint64_t npu_dma_bytes = 0;        // 单个 ctx 的 DMA 分配总量

    uint64_t errors = 0;
    std::string note;
};

}  // namespace tb

#endif  // TB_BENCH_TYPES_H_
