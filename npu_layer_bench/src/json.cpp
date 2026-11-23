// src/json.cpp —— 把一次测量结果写成一行 JSON(JSONL)
//
// 为什么用 JSONL 而不是 JSON 数组:
//   压测跑几十分钟, 中途可能被 Ctrl-C 打断。JSONL 的话,
//   已经落盘的行都是完整的, 脚本随时可以汇总; JSON 数组一旦少一个逗号就全废。
#include "nlb/bench_types.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace nlb {
namespace {

std::string esc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", c);
                    o += b;
                } else {
                    o += c;
                }
        }
    }
    return o;
}

std::string q(const std::string& s) { return "\"" + esc(s) + "\""; }

// 数字: 非有限值统一写成 null, 免得下游 JSON 解析器炸掉。
std::string num(double v) {
    if (!std::isfinite(v)) return "null";
    char b[64];
    snprintf(b, sizeof(b), "%.4f", v);
    return b;
}

std::string num3(double v) {
    if (!std::isfinite(v)) return "null";
    char b[64];
    snprintf(b, sizeof(b), "%.3f", v);
    return b;
}

std::string numi(long long v) {
    char b[32];
    snprintf(b, sizeof(b), "%lld", v);
    return b;
}

}  // namespace

void emit_json(const RunResult& r, const Config& cfg) {
    std::string s;
    s.reserve(2048);
    s += "{";
    s += "\"layer\":" + q(layer_name(r.layer));
    s += ",\"case\":" + q(r.case_name);
    s += ",\"repeat\":" + numi(r.repeat);
    s += ",\"duration_s\":" + num(r.duration_s);
    s += ",\"warmup_s\":" + num(r.warmup_s);

    s += ",\"frames\":" + numi(static_cast<long long>(r.frames));
    s += ",\"errors\":" + numi(static_cast<long long>(r.errors));
    s += ",\"fps\":" + num3(r.fps);
    s += ",\"det_fps\":" + num3(r.det_fps);
    s += ",\"ocr_fps\":" + num3(r.ocr_fps);

    s += ",\"det_wall_ms\":" + num3(r.det_wall_ms);
    s += ",\"ocr_wall_ms\":" + num3(r.ocr_wall_ms);
    s += ",\"rga_ms\":" + num3(r.rga_ms);
    s += ",\"frame_ms\":" + num3(r.frame_ms);
    s += ",\"det_npu_ms\":" + num3(r.det_npu_ms);
    s += ",\"ocr_npu_ms\":" + num3(r.ocr_npu_ms);
    s += ",\"queue_ms\":" + num3(r.queue_ms);

    s += ",\"det_p50\":" + num3(r.det_p50);
    s += ",\"det_p99\":" + num3(r.det_p99);
    s += ",\"det_max\":" + num3(r.det_max);
    s += ",\"ocr_p50\":" + num3(r.ocr_p50);
    s += ",\"ocr_p99\":" + num3(r.ocr_p99);
    s += ",\"ocr_max\":" + num3(r.ocr_max);
    s += ",\"frame_p50\":" + num3(r.frame_p50);
    s += ",\"frame_p99\":" + num3(r.frame_p99);
    s += ",\"frame_max\":" + num3(r.frame_max);

    s += ",\"core_app_pct\":[";
    for (int i = 0; i < 3; ++i) {
        if (i) s += ",";
        s += num3(r.core_app_pct[i]);
    }
    s += "],\"core_drv_pct\":[";
    for (int i = 0; i < 3; ++i) {
        if (i) s += ",";
        s += num3(r.core_drv_pct[i]);
    }
    s += "],\"ctx_per_core\":[";
    for (int i = 0; i < 3; ++i) {
        if (i) s += ",";
        s += numi(r.ctx_per_core[i]);
    }
    s += "]";

    s += ",\"cpu_pct\":" + num3(r.cpu_pct);
    s += ",\"cpu_cores\":" + num3(r.cpu_cores);
    s += ",\"rss_mb\":" + num3(r.rss_mb);
    s += ",\"rss_peak_mb\":" + num3(r.rss_peak_mb);
    s += ",\"heap_slope_kb_min\":" + num3(r.heap_slope_kb_min);

    s += ",\"npu_load_ok\":";
    s += r.npu_load_ok ? "true" : "false";
    s += ",\"npu_load\":[";
    for (int i = 0; i < 3; ++i) {
        if (i) s += ",";
        s += num3(r.npu_load[i]);
    }
    s += "]";

    s += ",\"npu_freq_min_mhz\":" + num3(r.npu_freq_min_mhz);
    s += ",\"npu_freq_max_mhz\":" + num3(r.npu_freq_max_mhz);

    s += ",\"det_model\":" + q(cfg.det_model);
    if (!cfg.ocr_model.empty()) s += ",\"ocr_model\":" + q(cfg.ocr_model);
    if (!r.note.empty()) s += ",\"note\":" + q(r.note);

    for (const auto& kv : r.extra) {
        s += "," + q(kv.first) + ":" + q(kv.second);
    }
    s += "}";

    // stdout: 人类可读的一行摘要 + JSON(用 # 前缀区分, 便于脚本 grep)
    printf("%s  %-22s rep%d  fps=%7.2f  det=%6.2fms  ocr=%6.2fms  rga=%5.2fms  "
           "core=%5.1f/%5.1f/%5.1f  cpu=%5.1f%%\n",
           layer_name(r.layer), r.case_name.c_str(), r.repeat, r.fps,
           r.det_wall_ms, r.ocr_wall_ms, r.rga_ms,
           r.core_app_pct[0], r.core_app_pct[1], r.core_app_pct[2],
           r.cpu_pct);
    fflush(stdout);

    // JSONL 行单独走 stdout 的一行, 前面加 "@" 标记, 避免和人类摘要混在一起
    printf("@%s\n", s.c_str());
    fflush(stdout);

    if (!cfg.out_jsonl.empty()) {
        FILE* f = fopen(cfg.out_jsonl.c_str(), "a");
        if (f) {
            fprintf(f, "%s\n", s.c_str());
            fclose(f);
        }
    }
}

}  // namespace nlb
