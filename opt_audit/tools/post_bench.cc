// post_bench.cc —— 形态C 后处理内部耗时拆解(只读)
//
// 目的: 量化 yolo_post.cc 里"每帧自检多扫一遍张量"这件事值多少 ms。
// 这段 CPU 时间是【占着 NPU 核闸门】跑的 -> 直接放大 NPU 排队。
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <dirent.h>
#include <opencv2/opencv.hpp>
#include <rknn_api.h>

#include "yolo_post.h"

static double now_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
    std::string model, dir;
    int iters = 40, core = 1;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--det") && i + 1 < argc) model = argv[++i];
        else if (!strcmp(argv[i], "--imgs") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--iters") && i + 1 < argc) iters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--core") && i + 1 < argc) core = atoi(argv[++i]);
    }
    // 列目录
    std::vector<std::string> imgs;
    {
        DIR* d = opendir(dir.c_str());
        if (!d) { fprintf(stderr, "打不开 %s\n", dir.c_str()); return 2; }
        struct dirent* e;
        while ((e = readdir(d))) {
            std::string n = e->d_name;
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".jpg") == 0) imgs.push_back(dir + "/" + n);
        }
        closedir(d);
        std::sort(imgs.begin(), imgs.end());
    }
    if (imgs.empty()) return 2;

    FILE* f = fopen(model.c_str(), "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> mdata((size_t)sz);
    if (fread(mdata.data(), 1, mdata.size(), f) != mdata.size()) return 2;
    fclose(f);

    rknn_context ctx = 0;
    if (rknn_init(&ctx, mdata.data(), (uint32_t)mdata.size(), RKNN_FLAG_PRIOR_HIGH, nullptr) != 0) return 2;
    rknn_set_core_mask(ctx, (rknn_core_mask)core);
    rknn_input_output_num io; memset(&io, 0, sizeof(io));
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
    std::vector<rknn_tensor_attr> attrs(io.n_output);
    for (uint32_t i = 0; i < io.n_output; ++i) {
        memset(&attrs[i], 0, sizeof(attrs[i])); attrs[i].index = i;
        rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &attrs[i], sizeof(attrs[i]));
    }
    std::vector<std::vector<uint8_t>> bufs(io.n_output);
    std::vector<rknn_output> outs(io.n_output);
    for (uint32_t i = 0; i < io.n_output; ++i) {
        size_t bytes = attrs[i].n_elems * 4; if (bytes < attrs[i].size) bytes = attrs[i].size;
        bufs[i].assign(bytes, 0);
        memset(&outs[i], 0, sizeof(outs[i]));
        outs[i].index = i; outs[i].want_float = 1; outs[i].is_prealloc = 1;
        outs[i].buf = bufs[i].data(); outs[i].size = (uint32_t)bufs[i].size();
    }

    const int W = 640, H = 640;
    std::vector<uint8_t> in((size_t)W * H * 3, 128);
    double sum_self = 0, sum_main = 0, sum_alloc = 0, sum_post = 0, sum_self_c = 0, sum_self_b = 0;
    int n = 0, nboxes = 0;
    for (int it = 0; it < iters; ++it) {
        cv::Mat im = cv::imread(imgs[(size_t)it % imgs.size()], cv::IMREAD_COLOR);
        if (im.empty()) continue;
        cv::Mat r; cv::resize(im, r, cv::Size(W, H)); cv::cvtColor(r, r, cv::COLOR_BGR2RGB);
        memcpy(in.data(), r.data, (size_t)W * H * 3);
        rknn_input rin; memset(&rin, 0, sizeof(rin));
        rin.index = 0; rin.type = RKNN_TENSOR_UINT8; rin.fmt = RKNN_TENSOR_NHWC;
        rin.size = (uint32_t)in.size(); rin.buf = in.data(); rin.pass_through = 0;
        rknn_inputs_set(ctx, 1, &rin);
        rknn_run(ctx, nullptr);
        rknn_outputs_get(ctx, io.n_output, outs.data(), nullptr);

        OutTensor box, cls;
        box.data = outs[0].buf; box.zp = attrs[0].zp; box.scale = attrs[0].scale;
        box.n_elems = attrs[0].n_elems; box.n_dims = attrs[0].n_dims;
        for (uint32_t k = 0; k < attrs[0].n_dims && k < 4; ++k) box.dims[k] = attrs[0].dims[k];
        cls.data = outs[1].buf; cls.zp = attrs[1].zp; cls.scale = attrs[1].scale;
        cls.n_elems = attrs[1].n_elems; cls.n_dims = attrs[1].n_dims;
        for (uint32_t k = 0; k < attrs[1].n_dims && k < 4; ++k) cls.dims[k] = attrs[1].dims[k];
        const uint32_t NA = box.dims[box.n_dims - 1];
        const int NC = (int)cls.dims[cls.n_dims - 2];

        // (1) 自检块
        double t0 = now_ms();
        volatile float vsink = 0.f;
        float cls_max = 0.f, cls_min = 0.f, box_max = 0.f; bool first = true;
        for (uint32_t a = 0; a < NA; a++) {
            for (int c = 0; c < NC; c++) {
                const float v = cls.at((uint32_t)c * NA + a);
                cls_max = std::max(cls_max, v);
                if (first) { cls_min = v; first = false; } else cls_min = std::min(cls_min, v);
            }
            for (int c = 0; c < 4; c++) box_max = std::max(box_max, box.at((uint32_t)c * NA + a));
        }
        vsink = cls_max + cls_min + box_max;
        double t1 = now_ms();
        // (2) 主循环
        double t2 = now_ms();
        std::vector<YoloBox> objs;
        for (uint32_t a = 0; a < NA; a++) {
            int best_cls = -1; float best_scr = -1.f;
            for (int c = 0; c < NC; c++) {
                float v = cls.at((uint32_t)c * NA + a);
                if (v > best_scr) { best_scr = v; best_cls = c; }
            }
            if (best_cls < 0 || best_scr < 0.15f) continue;
            float cx = box.at(0 * NA + a), cy = box.at(1 * NA + a);
            float w = box.at(2 * NA + a), h = box.at(3 * NA + a);
            YoloBox o; o.cls_id = best_cls; o.score = best_scr;
            o.x1 = cx - w * 0.5f; o.y1 = cy - h * 0.5f; o.x2 = cx + w * 0.5f; o.y2 = cy + h * 0.5f;
            objs.push_back(o);
        }
        double t3 = now_ms();
        // (3) NMS 的 3 次 vector 分配(每次调用都重新分配)
        double t4 = now_ms();
        { std::vector<YoloBox> out; std::vector<YoloBox> cand; std::vector<bool> dead; }
        double t5 = now_ms();
        (void)vsink;
        // (4) 整个 yolov8_postprocess(含 reserve(1024) + NMS)
        // (3b) 自检块改成【通道在外、anchor 在内】的连续访问
        double t3b0 = now_ms();
        {
            volatile float vs2 = 0.f;
            float cmx = 0.f, cmn = 0.f, bmx = 0.f; bool fst = true;
            const float* cd = (const float*)cls.data;
            const float* bd = (const float*)box.data;
            for (int c = 0; c < NC; c++) {
                const float* p = cd + (size_t)c * NA;
                for (uint32_t a = 0; a < NA; a++) {
                    const float v = p[a];
                    cmx = std::max(cmx, v);
                    if (fst) { cmn = v; fst = false; } else cmn = std::min(cmn, v);
                }
            }
            for (int c = 0; c < 4; c++) {
                const float* p = bd + (size_t)c * NA;
                for (uint32_t a = 0; a < NA; a++) bmx = std::max(bmx, p[a]);
            }
            vs2 = cmx + cmn + bmx;
            (void)vs2;
        }
        double t3b = now_ms();
        sum_self_c += t3b - t3b0;
        // (3c) 连续访问 + 无分支(min/max 都不带 first 判断)
        double t3c0 = now_ms();
        {
            volatile float vs3 = 0.f;
            float cmx = -3.0e38f, cmn = 3.0e38f, bmx = -3.0e38f;
            const float* cd = (const float*)cls.data;
            const float* bd = (const float*)box.data;
            for (int c = 0; c < NC; c++) {
                const float* p = cd + (size_t)c * NA;
                for (uint32_t a = 0; a < NA; a++) {
                    const float v = p[a];
                    cmx = cmx > v ? cmx : v;
                    cmn = cmn < v ? cmn : v;
                }
            }
            for (int c = 0; c < 4; c++) {
                const float* p = bd + (size_t)c * NA;
                for (uint32_t a = 0; a < NA; a++) {
                    const float v = p[a];
                    bmx = bmx > v ? bmx : v;
                }
            }
            vs3 = cmx + cmn + bmx;
            (void)vs3;
        }
        double t3c = now_ms();
        sum_self_b += t3c - t3c0;
        OutTensor ts[2] = {box, cls};
        BoxMapper mp; mp.img_w = W; mp.img_h = H; mp.mode = BoxMapper::STRETCH; mp.scale_x = 1; mp.scale_y = 1;
        std::vector<YoloBox> full = yolov8_postprocess(ts, 2, 0.15f, 0.45f, mp);
        double t6 = now_ms();

        sum_self += t1 - t0; sum_main += t3 - t2; sum_alloc += t5 - t4; sum_post += t6 - t5;
        nboxes += (int)full.size(); ++n;
    }
    fprintf(stderr, "=== C) 形态C 后处理内部拆解 (平均 %d 帧, 平均 %.1f 个框) ===\n",
            n, n ? (double)nboxes / n : 0.0);
    fprintf(stderr, "  自检块(每帧全量扫 cls+box, %u anchors x %d cls) = %.3f ms\n",
            8400, 2, sum_self / n);
    fprintf(stderr, "  自检块[连续访问版](通道在外)                   = %.3f ms\n", sum_self_c / n);
    fprintf(stderr, "  自检块[连续+无分支]                          = %.3f ms\n", sum_self_b / n);
    fprintf(stderr, "  主循环(扫 cls 取最大类)                        = %.3f ms\n", sum_main / n);
    fprintf(stderr, "  NMS 的 3 次 vector 分配/析构                     = %.4f ms\n", sum_alloc / n);
    fprintf(stderr, "  yolov8_postprocess 全程(含 reserve(1024)+NMS)  = %.3f ms\n", sum_post / n);
    rknn_destroy(ctx);
    return 0;
}
