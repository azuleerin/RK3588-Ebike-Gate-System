// src/main.cpp —— npu_layer_bench 的入口与编排
//
// ★ 设计原则:
//   这个工具【不修改主工程任何代码】, 也不共用它的任何 .cc/.h。
//   它只借用板上的 librknnrt.so / librga.so + rknn_api.h / im2d.h。
//   第 1/2/3 层在本进程里自建流水线; 第 4/5 层把生产二进制当黑盒驱动。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nlb/bench_types.h"
#include "nlb/layers.h"

namespace {

void usage(const char* argv0) {
    fprintf(stderr,
        "用法: %s --det-model M.rknn [选项]\n"
        "\n"
        "  --layer <spec>       要跑哪几层, 逗号分隔: all,L1,L2,L3,L4,L5,EX (默认 all)\n"
        "  --det-model <path>   检测模型(必填)\n"
        "  --ocr-model <path>   识别模型(L2 必填)\n"
        "  --labels <path>      标签文件(第 4/5 层用)\n"
        "  --ocr-dict <path>    字符字典(第 4/5 层用)\n"
        "  --gate-bin <path>    生产二进制 rk_gate 的路径(第 4/5 层用)\n"
        "  --ch <spec>          一路: name:src:near|high[:IPM四点] (可重复)\n"
        "  --fullchain-max-fps <n>  传给 rk_gate 的 --max-fps (0 = 解除限帧, 默认 0)\n"
        "  --duration <sec>     每组测量窗口秒数 (默认 30)\n"
        "  --warmup <sec>       每组预热秒数 (默认 5)\n"
        "  --repeats <n>        每组重复次数 (默认 3)\n"
        "  --workers <list>     第 1 层扫描的 worker 数 (逗号分隔, 默认 1,2,4,6,9)\n"
        "  --src-w --src-h      第 3 层模拟的 NV12 源尺寸 (默认 1920x1080)\n"
        "  --res-model-640 <p>  分辨率扫描: 640 的模型\n"
        "  --res-model-576 <p>  分辨率扫描: 576 的模型\n"
        "  --res-model-512 <p>  分辨率扫描: 512 的模型\n"
        "  --out <path>         JSONL 结果输出路径\n",
        argv0);
}

bool parse_int_list(const std::string& s, std::vector<int>* out) {
    out->clear();
    size_t pos = 0;
    while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        if (comma == std::string::npos) comma = s.size();
        const std::string tok = s.substr(pos, comma - pos);
        if (!tok.empty()) out->push_back(atoi(tok.c_str()));
        pos = comma + 1;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    nlb::Config cfg;
    std::string layer_spec = "all";

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "参数 %s 缺值\n", what);
                exit(2);
            }
            return argv[++i];
        };

        if (a == "--layer") layer_spec = next("--layer");
        else if (a == "--det-model") cfg.det_model = next("--det-model");
        else if (a == "--ocr-model") cfg.ocr_model = next("--ocr-model");
        else if (a == "--labels") cfg.det_dict = next("--labels");
        else if (a == "--ocr-dict") cfg.ocr_dict = next("--ocr-dict");
        else if (a == "--gate-bin") cfg.gate_bin = next("--gate-bin");
        else if (a == "--ch") cfg.ch_specs.push_back(next("--ch"));
        else if (a == "--fullchain-max-fps") cfg.fullchain_max_fps = atof(next("--fullchain-max-fps").c_str());
        else if (a == "--duration") cfg.duration_sec = atof(next("--duration").c_str());
        else if (a == "--warmup") cfg.warmup_sec = atof(next("--warmup").c_str());
        else if (a == "--repeats") cfg.repeats = atoi(next("--repeats").c_str());
        else if (a == "--workers") parse_int_list(next("--workers"), &cfg.worker_counts);
        else if (a == "--src-w") cfg.src_w = atoi(next("--src-w").c_str());
        else if (a == "--src-h") cfg.src_h = atoi(next("--src-h").c_str());
        else if (a == "--res-model-640") cfg.res_models[0] = next("--res-model-640");
        else if (a == "--res-model-576") cfg.res_models[1] = next("--res-model-576");
        else if (a == "--res-model-512") cfg.res_models[2] = next("--res-model-512");
        else if (a == "--out") cfg.out_jsonl = next("--out");
        else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else { fprintf(stderr, "未知参数: %s\n", a.c_str()); usage(argv[0]); return 2; }
    }

    if (cfg.det_model.empty()) {
        fprintf(stderr, "[FATAL] 必须给 --det-model\n");
        return 2;
    }
    if (cfg.repeats < 1) cfg.repeats = 1;
    if (cfg.duration_sec <= 0) cfg.duration_sec = 30.0;
    if (cfg.warmup_sec < 0) cfg.warmup_sec = 0.0;

    // 解析要跑哪几层
    std::vector<std::string> layers;
    {
        size_t pos = 0;
        while (pos <= layer_spec.size()) {
            size_t comma = layer_spec.find(',', pos);
            if (comma == std::string::npos) comma = layer_spec.size();
            std::string tok = layer_spec.substr(pos, comma - pos);
            if (!tok.empty()) layers.push_back(tok);
            if (comma == layer_spec.size()) break;
            pos = comma + 1;
        }
    }
    bool want_all = false;
    for (const auto& l : layers) {
        if (l == "all") want_all = true;
    }
    auto has = [&](const char* n) {
        if (want_all) return true;
        for (const auto& l : layers) {
            if (l == n) return true;
        }
        return false;
    };

    fprintf(stderr, "=========================================================\n");
    fprintf(stderr, " npu_layer_bench —— RK3588 NPU 分层压测台\n");
    fprintf(stderr, " 板子: RK3588 (3 核 NPU)   检测模型: %s\n", cfg.det_model.c_str());
    fprintf(stderr, " 每组: 预热 %.0fs + 测量 %.0fs, 重复 %d 次\n",
            cfg.warmup_sec, cfg.duration_sec, cfg.repeats);
    if (!cfg.out_jsonl.empty()) fprintf(stderr, " 结果: %s\n", cfg.out_jsonl.c_str());
    fprintf(stderr, "=========================================================\n");

    if (has("L1")) nlb::run_layer1(cfg);
    if (has("L2")) {
        if (cfg.ocr_model.empty()) {
            fprintf(stderr, "[跳过 L2] 没有给 --ocr-model\n");
        } else {
            nlb::run_layer2(cfg);
        }
    }
    if (has("L3")) nlb::run_layer3(cfg);
    if (has("L4")) {
        if (cfg.gate_bin.empty() || cfg.ch_specs.empty()) {
            fprintf(stderr, "[跳过 L4] 需要 --gate-bin 和 --ch\n");
        } else {
            nlb::run_layer_full(cfg, nlb::Layer::kL4);
        }
    }
    if (has("L5")) {
        if (cfg.gate_bin.empty() || cfg.ch_specs.size() < 2) {
            fprintf(stderr, "[跳过 L5] 需要 --gate-bin 和至少 2 个 --ch\n");
        } else {
            nlb::run_layer_full(cfg, nlb::Layer::kL5);
        }
    }
    if (has("EX")) nlb::run_layer_extra(cfg);

    fprintf(stderr, "\n全部完成。\n");
    return 0;
}
