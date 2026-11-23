// nlb/bench_types.h —— npu_layer_bench 的公共类型与指标口径
//
// ★ 本测试台【完全不接入主工程】(rk3588_zero_copy):
//   不共用它的任何 .cc/.h, 不改它的 CMakeLists, 只借用板上的
//   librknnrt.so / librga.so + rknn_api.h / im2d.h。
//
// ★ 术语与口径(和主工程 npu_pool、thread_bench 对齐, 免得几套口径打架):
//
//   run / npu ms : rknn_run + rknn_outputs_get —— 这才是【真正占着 NPU 核】的时间
//   wall ms      : 线程从"开始处理这一帧"到"结果就绪"的墙钟 —— 含排队
//   两者之差      = NPU 排队时间
//
//   核占用(应用侧): Σ(该核上所有上下文的 run 墙钟) / 测量窗口
//                   —— 同一颗核挂多个上下文时会 > 100%, 那正是"排队"的体现
//   核占用(驱动侧): Σ(RKNN_QUERY_PERF_RUN 报的推理耗时) / 测量窗口
//                   —— 驱动自报的"真正在算"的时间
#ifndef NLB_BENCH_TYPES_H_
#define NLB_BENCH_TYPES_H_

#include <cstdint>
#include <string>
#include <vector>

namespace nlb {

// ---------------------------------------------------------------------------
// 分层编号
// ---------------------------------------------------------------------------
enum class Layer {
    kL1 = 1,   // 纯 NPU 算力极限
    kL2 = 2,   // 检测 + OCR 双模型纯推理
    kL3 = 3,   // RGA 预处理 + 检测推理
    kL4 = 4,   // 单路全链路
    kL5 = 5,   // 4 路全链路极限
    kEX = 9,   // 额外验证项(分辨率扫描 / 算子回落)
};

inline const char* layer_name(Layer l) {
    switch (l) {
        case Layer::kL1: return "L1";
        case Layer::kL2: return "L2";
        case Layer::kL3: return "L3";
        case Layer::kL4: return "L4";
        case Layer::kL5: return "L5";
        case Layer::kEX: return "EX";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// 核掩码约定(rknn_set_core_mask 的入参):
//   0 = CORE_AUTO(驱动自己选)  1 = CORE0  2 = CORE1  4 = CORE2  7 = 三核全开
//
// ★ 7 (RKNN_NPU_CORE_0_1_2) 的语义是"把【这一次】推理拆到 3 颗核上并行算",
//   不是"让 3 个上下文各自独占一颗核"。thread_bench 实测它是所有策略里最差的:
//   吞吐只有 1/2.6, e2e P99 高 25 倍。所以本测试台默认【不用】它。
// ---------------------------------------------------------------------------
inline int core_mask_of_index(int core_idx) { return 1 << core_idx; }
inline const char* core_mask_name(int mask) {
    switch (mask) {
        case 0: return "AUTO";
        case 1: return "Core0";
        case 2: return "Core1";
        case 4: return "Core2";
        case 7: return "Core0+1+2";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// 采样序列(每个序列一条独立的分位数采样器)
// ---------------------------------------------------------------------------
enum Series {
    kSerDetWall = 0,   // 检测: run + outputs_get 墙钟
    kSerOcrWall = 1,   // 识别: run + outputs_get 墙钟
    kSerRga = 2,       // RGA 预处理墙钟
    kSerFrame = 3,     // 单帧总墙钟(含排队)
    kSerDetNpu = 4,    // 驱动自报: 检测推理耗时
    kSerOcrNpu = 5,    // 驱动自报: 识别推理耗时
    kSeriesCount = 6,
};

inline const char* series_name(int s) {
    switch (s) {
        case kSerDetWall: return "det_wall";
        case kSerOcrWall: return "ocr_wall";
        case kSerRga: return "rga";
        case kSerFrame: return "frame";
        case kSerDetNpu: return "det_npu";
        case kSerOcrNpu: return "ocr_npu";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// 单次测量(一个配置跑一遍)的结果
// ---------------------------------------------------------------------------
struct RunResult {
    // ---- 身份 ----
    Layer layer = Layer::kL1;
    std::string case_name;       // 例如 "single_core0" / "tri_core_1per" / "4ch_unlimited"
    int repeat = 0;              // 第几次重复(1-based)
    double duration_s = 0.0;     // 实际测量窗口秒数
    double warmup_s = 0.0;

    // ---- 吞吐 ----
    uint64_t frames = 0;         // 窗口内完成的"帧"(L1/L2/L3 = 一次推理; L4/L5 = 一路一次分析)
    uint64_t errors = 0;
    double fps = 0.0;            // 帧数 / 窗口秒数

    // ---- 分模型吞吐(L2 用得上: 检测帧数和识别帧数是 1:1) ----
    double det_fps = 0.0;
    double ocr_fps = 0.0;

    // ---- 耗时(ms) ----
    double det_wall_ms = 0.0;    // 检测 run+get 墙钟均值
    double ocr_wall_ms = 0.0;    // 识别 run+get 墙钟均值
    double rga_ms = 0.0;         // RGA 预处理均值
    double frame_ms = 0.0;       // 单帧总墙钟均值
    double det_npu_ms = 0.0;     // 驱动自报检测推理耗时
    double ocr_npu_ms = 0.0;     // 驱动自报识别推理耗时

    // ---- 分位数(ms) ----
    double det_p50 = 0.0, det_p99 = 0.0, det_max = 0.0;
    double ocr_p50 = 0.0, ocr_p99 = 0.0, ocr_max = 0.0;
    double frame_p50 = 0.0, frame_p99 = 0.0, frame_max = 0.0;

    // ---- 排队时间(ms): 帧总墙钟 - (检测 + 识别 + RGA) ----
    double queue_ms = 0.0;

    // ---- 分核占用(%) ----
    // app_pct: 墙钟口径(Σ 该核上所有上下文的 run 时间 / 窗口)
    // drv_pct: 驱动口径(Σ RKNN_QUERY_PERF_RUN / 窗口)
    // -1 = 该核没有上下文 / 无法归属
    double core_app_pct[3] = {-1.0, -1.0, -1.0};
    double core_drv_pct[3] = {-1.0, -1.0, -1.0};
    int ctx_per_core[3] = {0, 0, 0};   // 每颗核上挂了几个上下文

    // ---- 整机资源 ----
    double cpu_pct = 0.0;          // 整机口径(8 核归一, 100% = 8 核跑满)
    double cpu_cores = 0.0;        // 折合多少个满载 CPU 核(整机% × 8)
    double rss_mb = 0.0;
    double rss_peak_mb = 0.0;
    double heap_slope_kb_min = 0.0;

    // ---- debugfs NPU load(需 root; 不可读时全 -1) ----
    bool npu_load_ok = false;
    double npu_load[3] = {-1.0, -1.0, -1.0};

    // ---- 测量窗口内的 NPU 频率(MHz) ----
    // NPU 是 300MHz~1GHz 可调的; 如果锁频没生效, 同一个配置的耗时
    // 会在 1000MHz 和 900MHz 之间差 11%, 而数据表面看不出来。
    // 把频率区间记下来, 报告里才能证明这一组数是在什么频率下测的。
    double npu_freq_min_mhz = -1.0;
    double npu_freq_max_mhz = -1.0;

    // ---- 附加信息 ----
    std::string note;
    std::vector<std::pair<std::string, std::string>> extra;  // 附加键值对(写进 JSON)
};

// ---------------------------------------------------------------------------
// 全局配置
// ---------------------------------------------------------------------------
struct Config {
    // ---- 模型 ----
    std::string det_model;       // 检测模型(必填)
    std::string ocr_model;       // 识别模型(L2 用)
    std::string res_models[3];   // 分辨率扫描模型(640/576/512)

    // ---- 时序 ----
    double warmup_sec = 5.0;     // 预热(不计入统计), 让 NPU 频率爬上来
    double duration_sec = 30.0;  // 每组测量窗口
    int repeats = 3;             // 每组重复次数(取均值)

    // ---- L1/L2/L3 的线程拓扑 ----
    // L1: 1/2/4/6/9 个 worker 均匀分核 —— 找出"每核几个 worker"的最优点
    std::vector<int> worker_counts{1, 2, 4, 6, 9};

    // ---- L3 的 RGA 输入 ----
    int src_w = 1920, src_h = 1080;   // 模拟 NV12 源分辨率

    // ---- L4/L5 ----
    std::string gate_bin;        // 生产二进制 rk_gate 的路径
    std::string det_dict;        // 标签文件
    std::string ocr_dict;        // 字符字典
    std::vector<std::string> ch_specs;   // --ch 参数(每个形如 name:src:near|high[:ipm])
    double fullchain_max_fps = 0.0;      // 传给 rk_gate 的 --max-fps(0 = 解除限帧)

    // ---- 输出 ----
    std::string out_jsonl;       // JSONL 结果文件; 空 = 只打 stdout
    bool quiet = false;          // 压制每秒监控打印
};

// ---------------------------------------------------------------------------
// JSONL 输出: 一行一个 JSON 对象, 便于脚本汇总
// ---------------------------------------------------------------------------
void emit_json(const RunResult& r, const Config& cfg);

}  // namespace nlb

#endif  // NLB_BENCH_TYPES_H_
