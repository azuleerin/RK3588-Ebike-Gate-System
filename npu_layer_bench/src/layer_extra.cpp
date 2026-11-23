// src/layer_extra.cpp —— 额外验证项
//
//   EX-1  分辨率扫描: 640 / 576 / 512 三种输入尺寸下的纯推理帧率。
//         目的: 验证"分辨率与帧率的对应关系", 给"降分辨率换吞吐"这条
//         优化路线一个可量化的斜率。
//
//   EX-2  算子回落检查: 统计两个模型里有多少算子回落到了 CPU 执行,
//         分别是哪些类型、各占多少时间。目的: 评估"把回落算子优化掉"
//         的潜在收益到底有多大。
//
// ★ EX-1 的模型从哪来:
//   生产模型(ebike_int8_rk2332.rknn)是固定 640×640 编译的, 不能改尺寸。
//   所以分辨率扫描用【同一架构】的 YOLOv8n ONNX 在板端重新编译成
//   640/576/512 三个 rknn 模型。
//   ★ 为什么这样测出来的帧率能代表生产模型:
//     INT8 卷积的耗时由【形状】决定, 与权重取值无关(数据无关的 GEMM)。
//     所以同一个网络结构换输入尺寸, 每帧的计算量按面积缩放,
//     测出来的分辨率-帧率曲线对生产模型同样成立。
//     (这一点在报告里会写清楚, 不含糊。)
//
// ★ EX-2 的采集方式:
//   rknn_init 时打开 RKNN_FLAG_COLLECT_PERF_MASK, 之后用
//   rknn_query(RKNN_QUERY_PERF_DETAIL) 拿到逐层明细, 其中每个算子都有
//   "Target" 列(NPU / CPU / GPU), 末尾还有一张按算子类型汇总的排名表
//   (CPUTime / GPUTime / NPUTime / TotalTime / TimeRatio)。
//   ★ 注意: 打开 COLLECT_PERF 会显著拖慢推理(逐层打点), 所以它
//     【只】用于这一项, 绝不能开在计时路径上。
#include "nlb/layers.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
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

class DetTask : public WorkerTask {
public:
    explicit DetTask(const std::string& model, int core_mask)
        : model_path_(model), core_mask_(core_mask) {}

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
        t->det_npu_us = model_.query_run_us();
        return true;
    }

    void teardown() override { model_.destroy(); }

private:
    std::string model_path_;
    int core_mask_;
    NpuModel model_;
};

// ---------------------------------------------------------------------------
// 逐层明细解析
// ---------------------------------------------------------------------------
struct OpStats {
    int total_ops = 0;
    int npu_ops = 0;
    int cpu_ops = 0;
    int gpu_ops = 0;
    double cpu_time_us = 0;
    double gpu_time_us = 0;
    double npu_time_us = 0;
    double total_time_us = 0;
    std::map<std::string, int> cpu_op_types;      // 类型 -> 回落次数
    std::map<std::string, double> cpu_op_time_us; // 类型 -> 回落耗时
};

// 逐层表的一行:
//   91   ConvExSwish        INT8     NPU    (1,256,20,20),...  ... 244  Conv:/model.22/...
//   111  OutputOperator     INT8     CPU    (1,2,8400)          \    ...
// 第 4 个空白分隔字段就是 Target。
bool scan_layer_table(const std::string& detail, OpStats* st) {
    std::istringstream in(detail);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        // 切出前几个空白分隔字段
        std::istringstream ls(line);
        std::string f[4];
        for (int i = 0; i < 4; ++i) {
            if (!(ls >> f[i])) break;
            if (i < 3) continue;
        }
        // 第 1 个字段必须是纯数字(ID), 第 4 个字段是 NPU/CPU/GPU
        if (f[0].empty() || !isdigit(static_cast<unsigned char>(f[0][0]))) continue;
        if (f[3] != "NPU" && f[3] != "CPU" && f[3] != "GPU") continue;

        st->total_ops++;
        if (f[3] == "NPU") st->npu_ops++;
        else if (f[3] == "CPU") st->cpu_ops++;
        else if (f[3] == "GPU") st->gpu_ops++;
    }
    return st->total_ops > 0;
}

// 排名表的一行:
//   ConvExSwish        57           0            0            19271        19271          41.84%
//   OutputOperator     2            64           0            0            64             0.14%
//
// ★ Total 行的列数【不一样】: 它只有 4 个数字, 没有 GPUTime:
//   Total                           2928         0            30023        32951
//   (calls, cpu, npu, total —— gpu 被省掉了)
// 所以这里按"把行首之后的整数全抓出来"来解析, 再按个数分派。
bool scan_rank_table(const std::string& detail, OpStats* st) {
    std::istringstream in(detail);
    std::string line;
    bool in_table = false;
    while (std::getline(in, line)) {
        // 逐层表上面那两行汇总: "Total Operator Elapsed Per Frame Time(us): 32951"
        if (line.find("Total Operator Elapsed Per Frame Time(us)") != std::string::npos) {
            const size_t colon = line.find_last_of(':');
            if (colon != std::string::npos) {
                st->total_time_us = atof(line.c_str() + colon + 1);
            }
            continue;
        }
        if (line.find("Operator Time Consuming Ranking Table") != std::string::npos) {
            in_table = true;
            continue;
        }
        if (!in_table) continue;
        if (line.find("-----") != std::string::npos) continue;
        if (line.find("OpType") != std::string::npos) continue;

        // 切词: 第 0 个是算子名, 后面连续的整数是各列
        std::vector<std::string> tok;
        {
            std::istringstream ls(line);
            std::string w;
            while (ls >> w) tok.push_back(w);
        }
        if (tok.size() < 2) continue;
        const std::string& op = tok[0];

        std::vector<long long> nums;
        for (size_t i = 1; i < tok.size(); ++i) {
            const std::string& t = tok[i];
            if (t.empty() || !isdigit(static_cast<unsigned char>(t[0]))) break;
            nums.push_back(atoll(t.c_str()));
        }

        if (op == "Total") {
            // ★ Total 行的列是【cpu, gpu, npu, total】—— CallNumber 被省掉了。
            //   证据: det 模型这行是 "68 0 45995 46063", 而 68 正好等于
            //   OutputOperator(64) + InputOperator(4) 的 CPU 耗时之和;
            //   且 68 + 45995 = 46063 成立。
            if (nums.size() >= 4) {
                st->cpu_time_us += static_cast<double>(nums[0]);
                st->gpu_time_us += static_cast<double>(nums[1]);
                st->npu_time_us += static_cast<double>(nums[2]);
                if (st->total_time_us <= 0) st->total_time_us = static_cast<double>(nums[3]);
            }
            continue;
        }
        if (nums.size() >= 5) {
            // calls, cpu, gpu, npu, total
            st->gpu_time_us += static_cast<double>(nums[2]);
            if (nums[1] > 0) {
                st->cpu_op_types[op] += static_cast<int>(nums[0]);
                st->cpu_op_time_us[op] += static_cast<double>(nums[1]);
            }
        } else if (nums.size() >= 3 && nums[1] > 0) {
            st->cpu_op_types[op] += static_cast<int>(nums[0]);
            st->cpu_op_time_us[op] += static_cast<double>(nums[1]);
        }
    }
    return st->total_time_us > 0;
}

std::string join_types(const std::map<std::string, int>& m) {
    std::string s;
    for (const auto& kv : m) {
        if (!s.empty()) s += ",";
        s += kv.first + ":" + std::to_string(kv.second);
    }
    return s;
}

std::string join_times(const std::map<std::string, double>& m) {
    std::string s;
    for (const auto& kv : m) {
        if (!s.empty()) s += ",";
        char b[64];
        snprintf(b, sizeof(b), "%s:%.0f", kv.first.c_str(), kv.second);
        s += b;
    }
    return s;
}

// 算子回落统计: 建一个带 COLLECT_PERF 的上下文, 跑几帧, 取明细。
void analyze_ops(const Config& cfg, const std::string& model_path,
                 const std::string& tag, const std::string& out_dir) {
    fprintf(stderr, "\n---- 算子回落检查: %s (%s) ----\n", model_path.c_str(), tag.c_str());

    NpuModel m;
    std::string err;
    if (!m.init(model_path, 0, -1, nullptr, 0, /*collect_perf=*/true, true, &err)) {
        fprintf(stderr, "  [EX-2] 打开模型失败: %s\n", err.c_str());
        return;
    }

    // 跑几帧让驱动把逐层数据采全
    for (int i = 0; i < 5; ++i) m.run();
    const std::string detail = m.perf_detail();
    m.destroy();

    if (detail.empty()) {
        fprintf(stderr, "  [EX-2] 拿不到逐层明细(RKNN_QUERY_PERF_DETAIL 返回空)\n");
        return;
    }

    const std::string path = out_dir + "/perf_detail_" + tag + ".txt";
    {
        std::ofstream f(path);
        if (f) f << detail;
    }

    OpStats st;
    scan_layer_table(detail, &st);
    scan_rank_table(detail, &st);

    const double ratio = st.total_time_us > 0
        ? 100.0 * st.cpu_time_us / st.total_time_us : 0.0;

    fprintf(stderr, "  逐层算子总数: %d   (NPU %d / CPU %d / GPU %d)\n",
            st.total_ops, st.npu_ops, st.cpu_ops, st.gpu_ops);
    fprintf(stderr, "  回落 CPU 的算子类型: %s\n",
            st.cpu_op_types.empty() ? "(无)" : join_types(st.cpu_op_types).c_str());
    fprintf(stderr, "  各类型 CPU 耗时(us): %s\n",
            st.cpu_op_time_us.empty() ? "(无)" : join_times(st.cpu_op_time_us).c_str());
    fprintf(stderr, "  算子耗时合计: CPU %.0f us / NPU %.0f us / 总 %.0f us -> CPU 占比 %.2f%%\n",
            st.cpu_time_us, st.npu_time_us, st.total_time_us, ratio);
    fprintf(stderr, "  逐层明细已存: %s\n", path.c_str());

    RunResult r;
    r.layer = Layer::kEX;
    r.case_name = "op_fallback_" + tag;
    r.repeat = 1;
    r.duration_s = 0;
    r.note = "RKNN_FLAG_COLLECT_PERF_MASK 采集(此模式会拖慢推理, 不用于计时)";
    r.extra.emplace_back("total_ops", std::to_string(st.total_ops));
    r.extra.emplace_back("npu_ops", std::to_string(st.npu_ops));
    r.extra.emplace_back("cpu_ops", std::to_string(st.cpu_ops));
    r.extra.emplace_back("gpu_ops", std::to_string(st.gpu_ops));
    r.extra.emplace_back("cpu_time_us", std::to_string(static_cast<long long>(st.cpu_time_us)));
    r.extra.emplace_back("npu_time_us", std::to_string(static_cast<long long>(st.npu_time_us)));
    r.extra.emplace_back("total_op_time_us", std::to_string(static_cast<long long>(st.total_time_us)));
    r.extra.emplace_back("cpu_time_ratio_pct", std::to_string(ratio));
    r.extra.emplace_back("cpu_op_types", join_types(st.cpu_op_types));
    r.extra.emplace_back("cpu_op_time_us", join_times(st.cpu_op_time_us));
    r.extra.emplace_back("perf_detail_file", path);
    emit_json(r, cfg);
}

}  // namespace

void run_layer_extra(const Config& cfg) {
    const std::string out_dir =
        cfg.out_jsonl.empty() ? std::string(".") : cfg.out_jsonl.substr(0, cfg.out_jsonl.find_last_of('/'));

    // ---- EX-1: 分辨率扫描 ----
    const int widths[3] = {640, 576, 512};
    bool any_res = false;
    for (int i = 0; i < 3; ++i) {
        if (!cfg.res_models[i].empty()) any_res = true;
    }

    if (any_res) {
        fprintf(stderr, "\n========== 额外验证项 1: 分辨率扫描 ==========\n");
        fprintf(stderr, "每种分辨率: 三核各 1 个 worker, 预热 %.0fs + 测量 %.0fs, 重复 %d 次\n",
                cfg.warmup_sec, cfg.duration_sec, cfg.repeats);

        for (int i = 0; i < 3; ++i) {
            if (cfg.res_models[i].empty()) continue;
            // ★ 两种拓扑各测一遍:
            //   1 worker  -> 纯算力标尺(单帧耗时不受排队影响, 曲线最干净)
            //   3 worker  -> 三核总吞吐(和第 1 层同口径, 能直接比)
            const std::vector<std::vector<int>> topologies = {{0}, {0, 1, 2}};
            const char* tname[2] = {"1w", "3w"};
            for (int ti = 0; ti < 2; ++ti) {
                const std::vector<int>& cores = topologies[static_cast<size_t>(ti)];
                std::vector<WorkerTask*> tasks;
                for (size_t k = 0; k < cores.size(); ++k) {
                    tasks.push_back(new DetTask(cfg.res_models[i], core_mask_of_index(cores[k])));
                }
                HarnessOpts opts;
                opts.warmup_sec = cfg.warmup_sec;
                opts.duration_sec = cfg.duration_sec;
                opts.samples_per_worker = 20000;
                for (int rep = 1; rep <= cfg.repeats; ++rep) {
                    RunResult r = run_harness("res_" + std::to_string(widths[i]) + "_" + tname[ti],
                                              Layer::kEX, cores, tasks, opts, cfg, rep);
                    r.extra.emplace_back("input_width", std::to_string(widths[i]));
                    r.extra.emplace_back("model", cfg.res_models[i]);
                    r.extra.emplace_back("workers", std::to_string(cores.size()));
                    emit_json(r, cfg);
                }
                for (auto* t : tasks) delete t;
            }
        }
    }

    // ---- EX-2: 算子回落检查 ----
    fprintf(stderr, "\n========== 额外验证项 2: 算子回落检查 ==========\n");
    analyze_ops(cfg, cfg.det_model, "det", out_dir);
    if (!cfg.ocr_model.empty()) {
        analyze_ops(cfg, cfg.ocr_model, "ocr", out_dir);
    }
}

}  // namespace nlb
