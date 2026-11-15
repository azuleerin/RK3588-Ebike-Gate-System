// npu_batch_bench.cpp —— RK3588 NPU Batch 推理的纯算力对比
//
// 回答的问题: "12 路并发要不要用 Batch 推理?"
//   循环 4 次单张推理(4 个 batch=1 的上下文) vs 一次 [4,3,640,640] 推理,
//   看总耗时差多少, 分摊到单帧是多少。
//
// 用法:
//   ./build/npu_batch_bench --model-b4  b4.rknn --model-b1 b1.rknn \
//                           --batch 4 --iters 20 --cores 1,2,3
//
// 说明:
//   · 用 rknn_inputs_set(纯内存) 喂数据, 计时分成三段:
//       set     = 主机内存 -> NPU 的拷贝
//       run     = NPU 真正的计算
//       get     = NPU -> 主机内存 的拷贝
//     "纯算力"看 run 那一段。
//   · rknn_set_batch_core_num(ctx, n) 控制这次 Batch 推理用几个 NPU 核 ——
//     这是 Batch 能不能吃满 3 个核的关键开关。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "rknn_api.h"

namespace {

double now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

struct Model {
    rknn_context ctx = 0;
    int in_w = 0, in_h = 0;
    int in_batch = 1;
    int in_type = RKNN_TENSOR_UINT8;
    int in_fmt = RKNN_TENSOR_NHWC;
    int out_cnt = 0;
    std::vector<rknn_tensor_attr> out_attrs;
    std::vector<rknn_output> outs;
};

bool load_model(const char* path, Model* m, std::string* err) {
    FILE* f = fopen(path, "rb");
    if (!f) { *err = std::string("打不开模型: ") + path; return false; }
    fseek(f, 0, SEEK_END);
    const size_t sz = static_cast<size_t>(ftell(f));
    fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> buf(sz);
    if (fread(buf.data(), 1, sz, f) != sz) { fclose(f); *err = "读模型失败"; return false; }
    fclose(f);

    int ret = rknn_init(&m->ctx, buf.data(), static_cast<uint32_t>(sz),
                        RKNN_FLAG_PRIOR_HIGH, nullptr);
    if (ret < 0) { *err = "rknn_init 失败: " + std::to_string(ret); return false; }

    rknn_tensor_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.index = 0;
    if (rknn_query(m->ctx, RKNN_QUERY_INPUT_ATTR, &attr, sizeof(attr)) != RKNN_SUCC) {
        *err = "查输入属性失败"; return false;
    }
    m->in_fmt = attr.fmt;
    m->in_type = attr.type;
    // dims: [batch, ch, h, w] (NCHW) 或 [batch, h, w, ch] (NHWC)
    if (attr.n_dims < 4) { *err = "输入维数不对"; return false; }
    if (attr.fmt == RKNN_TENSOR_NHWC) {
        m->in_batch = attr.dims[0];
        m->in_h = attr.dims[1];
        m->in_w = attr.dims[2];
    } else {
        m->in_batch = attr.dims[0];
        m->in_h = attr.dims[2];
        m->in_w = attr.dims[3];
    }

    // ★ 用 RKNN_QUERY_IN_OUT_NUM 拿输出个数。
    //   千万别用 OUTPUT_ATTR 的 n_elems —— 那是"输出 0 的元素个数"
    //   (640*80*80*4 这种), 拿它当输出个数会循环几百万次(踩过, 刷了 800MB 日志)。
    rknn_input_output_num io_num;
    memset(&io_num, 0, sizeof(io_num));
    rknn_query(m->ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    m->out_cnt = io_num.n_output;
    for (int i = 0; i < m->out_cnt; ++i) {
        rknn_tensor_attr a;
        memset(&a, 0, sizeof(a));
        a.index = i;
        rknn_query(m->ctx, RKNN_QUERY_OUTPUT_ATTR, &a, sizeof(a));
        m->out_attrs.push_back(a);
        rknn_output o;
        memset(&o, 0, sizeof(o));
        o.index = i;
        o.want_float = 1;
        o.is_prealloc = 0;
        m->outs.push_back(o);
    }
    return true;
}

// 准备一批输入(NHWC, uint8 原始像素 —— 和主程序一样, 归一化烤在模型里)
struct Batch {
    std::vector<uint8_t> data;   // batch * h * w * 3
    int batch = 0, w = 0, h = 0;
};

bool make_batch(const std::vector<std::string>& imgs, int batch, int w, int h, Batch* b) {
    b->batch = batch; b->w = w; b->h = h;
    b->data.assign(static_cast<size_t>(batch) * w * h * 3, 0);
    for (int i = 0; i < batch; ++i) {
        cv::Mat im = cv::imread(imgs[i % imgs.size()], cv::IMREAD_COLOR);
        if (im.empty()) continue;
        cv::Mat r;
        cv::resize(im, r, cv::Size(w, h));
        cv::cvtColor(r, r, cv::COLOR_BGR2RGB);
        memcpy(b->data.data() + static_cast<size_t>(i) * w * h * 3,
               r.data, static_cast<size_t>(w) * h * 3);
    }
    return true;
}

struct Timing {
    double set_ms = 0, run_ms = 0, get_ms = 0;
    double total() const { return set_ms + run_ms + get_ms; }
};

// 一次批量推理。want_float=0 时取原始 INT8 输出(后端自己做反量化) ——
// ★ Batch 场景下这个开关很关键: 转 float 的量是 batch 倍, 4 帧就是 40MB,
//   驱动转一次要 300ms+ —— 比推理本身还慢。
Timing run_batch(Model* m, const Batch& b, int core_num, bool want_float) {
    Timing t;
    if (core_num > 0) rknn_set_batch_core_num(m->ctx, core_num);

    rknn_input in;
    memset(&in, 0, sizeof(in));
    in.index = 0;
    in.type = RKNN_TENSOR_UINT8;
    in.fmt = RKNN_TENSOR_NHWC;
    in.size = static_cast<uint32_t>(b.data.size());
    in.buf = const_cast<uint8_t*>(b.data.data());
    in.pass_through = 0;

    double t0 = now_ms();
    rknn_inputs_set(m->ctx, 1, &in);
    double t1 = now_ms();
    rknn_run(m->ctx, nullptr);
    double t2 = now_ms();
    for (auto& o : m->outs) {
        o.want_float = want_float ? 1 : 0;
        rknn_outputs_get(m->ctx, 1, &o, nullptr);
    }
    double t3 = now_ms();

    t.set_ms = t1 - t0;
    t.run_ms = t2 - t1;
    t.get_ms = t3 - t2;
    return t;
}

}  // namespace

int main(int argc, char** argv) {
    const char* model_b4 = nullptr;
    const char* model_b1 = nullptr;
    int batch = 4, iters = 20;
    std::vector<std::string> imgs;
    std::vector<int> cores;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--model-b4") && i + 1 < argc) model_b4 = argv[++i];
        else if (!strcmp(argv[i], "--model-b1") && i + 1 < argc) model_b1 = argv[++i];
        else if (!strcmp(argv[i], "--batch") && i + 1 < argc) batch = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--iters") && i + 1 < argc) iters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--img") && i + 1 < argc) imgs.push_back(argv[++i]);
        else if (!strcmp(argv[i], "--cores") && i + 1 < argc) {
            const char* p = argv[++i];
            while (*p) {
                if (*p >= '0' && *p <= '9') cores.push_back(*p - '0');
                ++p;
            }
        }
    }
    if (cores.empty()) cores = {1};

    if (imgs.empty()) {
        fprintf(stderr, "用法: %s --model-b4 <rknn> [--model-b1 <rknn>] --batch 4 "
                        "--iters 20 --img a.jpg --img b.jpg ... [--cores 1,2,3]\n", argv[0]);
        return 1;
    }

    Model mb4, mb1;
    std::string err;
    if (model_b4) {
        if (!load_model(model_b4, &mb4, &err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
        printf("batch 模型: %s\n", model_b4);
        printf("  输入: batch=%d %dx%d  fmt=%d type=%d  输出数=%d\n",
               mb4.in_batch, mb4.in_w, mb4.in_h, mb4.in_fmt, mb4.in_type, mb4.out_cnt);
    }
    if (model_b1) {
        if (!load_model(model_b1, &mb1, &err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
        printf("单张模型: %s  输入 batch=%d %dx%d\n", model_b1, mb1.in_batch, mb1.in_w, mb1.in_h);
    }

    const int W = mb4.in_w, H = mb4.in_h;

    // ---------------- 情况 A: 循环 batch 次单张推理 ----------------
    if (model_b1) {
        Batch one;
        make_batch(imgs, 1, W, H, &one);
        printf("\n=== 情况A: 循环 %d 次单张推理(batch=1 的模型, %d 个上下文) ===\n",
               batch, batch);
        printf("%-14s %-8s %-12s %-12s %-12s %-12s\n",
               "方式", "输出", "set ms", "run ms", "get ms", "合计 ms");
        for (int mode = 0; mode < 2; ++mode) {
         for (int wf = 0; wf < 2; ++wf) {
          const bool want_float = (wf == 0);
            // mode 0: 同一个上下文循环跑; mode 1: 每个"路"一个独立上下文(更贴近 12 路场景)
            std::vector<Model> ctxs;
            Model* use = &mb1;
            if (mode == 1) {
                ctxs.resize(batch);
                for (int i = 0; i < batch; ++i) {
                    std::string e;
                    if (!load_model(model_b1, &ctxs[i], &e)) { fprintf(stderr, "%s\n", e.c_str()); return 1; }
                }
                use = &ctxs[0];
            }
            // 预热
            for (int i = 0; i < 3; ++i) {
                if (mode == 1) { for (int k = 0; k < batch; ++k) run_batch(&ctxs[k], one, 0, want_float); }
                else run_batch(use, one, 0, want_float);
            }
            Timing acc;
            for (int it = 0; it < iters; ++it) {
                for (int k = 0; k < batch; ++k) {
                    Model* mm = (mode == 1) ? &ctxs[k] : use;
                    const Timing t = run_batch(mm, one, 0, want_float);
                    acc.set_ms += t.set_ms; acc.run_ms += t.run_ms; acc.get_ms += t.get_ms;
                }
            }
            const double n = iters;
            printf("%-14s %-8s %-12.3f %-12.3f %-12.3f %-12.3f\n",
                   mode == 0 ? "单上下文循环" : "多上下文并行",
                   want_float ? "float" : "int8",
                   acc.set_ms / n, acc.run_ms / n, acc.get_ms / n, acc.total() / n);
         }
        }
    }

    // ---------------- 情况 B: 一次 Batch=4 推理 ----------------
    if (model_b4) {
        Batch b4;
        make_batch(imgs, batch, W, H, &b4);
        printf("\n=== 情况B: 一次 [batch=%d,3,%d,%d] 推理 ===\n", batch, H, W);
        printf("%-10s %-8s %-12s %-12s %-12s %-12s %-14s\n",
               "batch核数", "输出", "set ms", "run ms", "get ms", "合计 ms", "分摊/帧 ms");
        for (int c : cores) {
         for (int wf = 0; wf < 2; ++wf) {
          const bool want_float = (wf == 0);
          for (int i = 0; i < 3; ++i) run_batch(&mb4, b4, c, want_float);   // 预热
          Timing acc;
          for (int it = 0; it < iters; ++it) {
              const Timing t = run_batch(&mb4, b4, c, want_float);
              acc.set_ms += t.set_ms; acc.run_ms += t.run_ms; acc.get_ms += t.get_ms;
          }
          const double n = iters;
          printf("%-10d %-8s %-12.3f %-12.3f %-12.3f %-12.3f %-14.3f\n",
                 c, want_float ? "float" : "int8",
                 acc.set_ms / n, acc.run_ms / n, acc.get_ms / n,
                 acc.total() / n, acc.total() / n / batch);
         }
        }
    }
    return 0;
}
