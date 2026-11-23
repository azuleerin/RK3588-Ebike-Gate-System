// fork9_cpu_bench.cc —— 【把 DFL 解码搬到 CPU 之后, CPU 每帧多花多少】(只读)
//
// 目的: fork9 的收益是 NPU 少跑 DFL 子图; 但 DFL 不是消失, 是【搬到 CPU】。
//       形态B 的解码要在 3 个尺度 × 8400 个 anchor 上做 16-bin softmax + 期望,
//       这个 CPU 开销必须实测, 否则 NPU 省下的时间会被 CPU 吃掉。
//
// 做法: 直接调用【生产解码器】rk3588_zero_copy/src/yolo_post.cc 里的
//       yolov8_postprocess()(形态B / 形态C 两条路径), 计时。
//       ★ 生产代码里形态B 的 ×stride 已经修好(2026-11-22), 这里用的就是修好的版本。
//
// 编译(板端):
//   g++ -O2 -std=c++17 -o fork9_cpu_bench fork9_cpu_bench.cc \
//       <repo>/rk3588_zero_copy/src/yolo_post.cc \
//       -I<repo>/rk3588_zero_copy/include -I<repo>/rk3588_zero_copy/third_party/include \
//       <repo>/rk3588_zero_copy/third_party/lib/librknnrt.so \
//       $(pkg-config --cflags --libs opencv4)
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "rknn_api.h"
#include "yolo_post.h"

static double now_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Model {
    rknn_context ctx = 0;
    rknn_input_output_num io{};
    std::vector<rknn_tensor_attr> attrs;
    std::vector<std::vector<uint8_t>> bufs;
    std::vector<rknn_output> outs;
};

static bool load_model(const std::string& path, Model& m) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data((size_t)sz);
    if (fread(data.data(), 1, data.size(), f) != data.size()) { fclose(f); return false; }
    fclose(f);
    if (rknn_init(&m.ctx, data.data(), (uint32_t)data.size(),
                  RKNN_FLAG_PRIOR_HIGH, nullptr) != 0) return false;
    rknn_set_core_mask(m.ctx, (rknn_core_mask)1);
    memset(&m.io, 0, sizeof(m.io));
    rknn_query(m.ctx, RKNN_QUERY_IN_OUT_NUM, &m.io, sizeof(m.io));
    m.attrs.resize(m.io.n_output);
    for (uint32_t i = 0; i < m.io.n_output; ++i) {
        memset(&m.attrs[i], 0, sizeof(m.attrs[i]));
        m.attrs[i].index = i;
        rknn_query(m.ctx, RKNN_QUERY_OUTPUT_ATTR, &m.attrs[i], sizeof(m.attrs[i]));
    }
    m.bufs.resize(m.io.n_output);
    m.outs.resize(m.io.n_output);
    for (uint32_t i = 0; i < m.io.n_output; ++i) {
        size_t bytes = m.attrs[i].n_elems * 4;
        if (bytes < m.attrs[i].size) bytes = m.attrs[i].size;
        m.bufs[i].assign(bytes, 0);
        memset(&m.outs[i], 0, sizeof(m.outs[i]));
        m.outs[i].index = i; m.outs[i].want_float = 1;
        m.outs[i].is_prealloc = 1;
        m.outs[i].buf = m.bufs[i].data();
        m.outs[i].size = (uint32_t)m.bufs[i].size();
    }
    return true;
}

int main(int argc, char** argv) {
    std::string model, img;
    int iters = 50;
    float conf = 0.25f, nms = 0.45f;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc) model = argv[++i];
        else if (!strcmp(argv[i], "--img") && i + 1 < argc) img = argv[++i];
        else if (!strcmp(argv[i], "--iters") && i + 1 < argc) iters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--conf") && i + 1 < argc) conf = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--nms") && i + 1 < argc) nms = (float)atof(argv[++i]);
    }
    if (model.empty() || img.empty()) {
        fprintf(stderr, "用法: fork9_cpu_bench --model M.rknn --img x.jpg [--iters 50]\n");
        return 2;
    }
    Model m;
    if (!load_model(model, m)) { fprintf(stderr, "模型加载失败\n"); return 2; }
    fprintf(stderr, "输出数: %d\n", m.io.n_output);

    cv::Mat im = cv::imread(img, cv::IMREAD_COLOR);
    if (im.empty()) { fprintf(stderr, "读图失败\n"); return 2; }
    const int W = 640, H = 640;
    cv::Mat r; cv::resize(im, r, cv::Size(W, H)); cv::cvtColor(r, r, cv::COLOR_BGR2RGB);
    std::vector<uint8_t> in((size_t)W * H * 3);
    memcpy(in.data(), r.data, (size_t)W * H * 3);

    std::vector<OutTensor> ts(m.io.n_output);
    double sum_post = 0, sum_run = 0;
    int n = 0, nboxes = 0;
    for (int it = 0; it < iters; ++it) {
        rknn_input rin; memset(&rin, 0, sizeof(rin));
        rin.index = 0; rin.type = RKNN_TENSOR_UINT8; rin.fmt = RKNN_TENSOR_NHWC;
        rin.size = (uint32_t)in.size(); rin.buf = in.data(); rin.pass_through = 0;
        rknn_inputs_set(m.ctx, 1, &rin);
        double t0 = now_ms();
        rknn_run(m.ctx, nullptr);
        rknn_outputs_get(m.ctx, m.io.n_output, m.outs.data(), nullptr);
        sum_run += now_ms() - t0;

        for (uint32_t i = 0; i < m.io.n_output; ++i) {
            ts[i].data = m.outs[i].buf;
            ts[i].is_int8 = false; ts[i].zp = 0; ts[i].scale = 1.f;
            ts[i].n_elems = m.attrs[i].n_elems;
            ts[i].n_dims = m.attrs[i].n_dims;
            for (uint32_t k = 0; k < m.attrs[i].n_dims && k < 4; ++k)
                ts[i].dims[k] = m.attrs[i].dims[k];
        }
        BoxMapper mapper;
        mapper.mode = BoxMapper::STRETCH;
        mapper.scale_x = (float)im.cols / W;
        mapper.scale_y = (float)im.rows / H;
        mapper.img_w = im.cols; mapper.img_h = im.rows;

        double t1 = now_ms();
        std::vector<YoloBox> boxes =
            yolov8_postprocess(ts.data(), (int)ts.size(), conf, nms, mapper);
        sum_post += now_ms() - t1;
        nboxes += (int)boxes.size();
        n++;
        rknn_outputs_release(m.ctx, m.io.n_output, m.outs.data());
    }
    printf("model=%s outputs=%d iters=%d\n", model.c_str(), m.io.n_output, n);
    printf("  NPU 推理(含取输出) : 平均 %.3f ms\n", sum_run / n);
    printf("  CPU 后处理(解码)   : 平均 %.3f ms   (平均框数 %.1f)\n",
           sum_post / n, (double)nboxes / n);
    rknn_destroy(m.ctx);
    return 0;
}
