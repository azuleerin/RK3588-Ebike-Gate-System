// hotpath_bench.cc —— 热循环实现层浪费的量化测量台(只读, 不改生产代码)
//
// 目的: 为 Phase 1 的"量级"一栏提供【实测数字】, 而不是估算。
// 测三件事:
//   A) 检测模型: rknn_outputs_get(want_float=1) vs (want_float=0) 的耗时差
//      + yolo_post 在 float32 张量 vs int8 张量上的耗时差
//      + 两边输出的检测框是否【逐框数值等价】
//   B) 识别模型: 同上(want_float=1 走 fp16->fp32, want_float=0 直读 fp16)
//      + CTC 解码在 float32 vs fp16 上的耗时差 + 结果是否等价
//   C) 报告每次测量的均值/中位数
//
// 用法: hotpath_bench --det <det.rknn> --ocr <ocr.rknn> \
//                     --imgs <dir> --rois <dir> --iters 30 [--det-core 1] [--ocr-core 0]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <dirent.h>
#include <arm_neon.h>
#include <rknn_api.h>

#include "yolo_post.h"

static double now_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

static bool load_model(const std::string& path, std::vector<uint8_t>* data) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    data->resize((size_t)sz);
    size_t n = fread(data->data(), 1, data->size(), f);
    fclose(f);
    return n == data->size();
}

static std::vector<std::string> list_dir(const std::string& dir, const char* ext) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    struct dirent* e;
    while ((e = readdir(d))) {
        std::string n = e->d_name;
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ext) == 0) out.push_back(dir + "/" + n);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

// ---- 极简 JPEG 解码(避免引入 OpenCV 依赖): 用 dpkg 的 libjpeg? 不, 用 OpenCV ----
// 这里直接用 OpenCV
#include <opencv2/opencv.hpp>

// ============================================================================
// A) 检测模型
// ============================================================================
struct DetOut {
    std::vector<YoloBox> boxes;
    double run_ms = 0, get_ms = 0, post_ms = 0;
};

static FILE* g_dump = nullptr;

static bool run_det(const std::string& model_path,
                    const std::vector<std::string>& imgs,
                    int core_mask, int iters, int want_float,
                    std::vector<DetOut>* results,
                    double* avg_run, double* avg_get, double* avg_post) {
    std::vector<uint8_t> data;
    if (!load_model(model_path, &data)) { fprintf(stderr, "模型读不了\n"); return false; }
    rknn_context ctx = 0;
    if (rknn_init(&ctx, data.data(), (uint32_t)data.size(), RKNN_FLAG_PRIOR_HIGH, nullptr) != 0) {
        fprintf(stderr, "rknn_init 失败\n"); return false;
    }
    rknn_set_core_mask(ctx, (rknn_core_mask)core_mask);

    rknn_input_output_num io; memset(&io, 0, sizeof(io));
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
    std::vector<rknn_tensor_attr> attrs(io.n_output);
    for (uint32_t i = 0; i < io.n_output; ++i) {
        memset(&attrs[i], 0, sizeof(attrs[i]));
        attrs[i].index = i;
        rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &attrs[i], sizeof(attrs[i]));
    }

    // 预分配缓冲(生产口径: is_prealloc = 1)
    std::vector<std::vector<uint8_t>> bufs(io.n_output);
    std::vector<rknn_output> outs(io.n_output);
    for (uint32_t i = 0; i < io.n_output; ++i) {
        size_t bytes = attrs[i].n_elems * (want_float ? 4 : (attrs[i].type == RKNN_TENSOR_FLOAT16 ? 2 : 1));
        if (bytes < attrs[i].size) bytes = attrs[i].size;
        bufs[i].assign(bytes, 0);
        memset(&outs[i], 0, sizeof(outs[i]));
        outs[i].index = i;
        outs[i].want_float = want_float ? 1 : 0;
        outs[i].is_prealloc = 1;
        outs[i].buf = bufs[i].data();
        outs[i].size = (uint32_t)bufs[i].size();
    }

    const int W = 640, H = 640;
    std::vector<uint8_t> in((size_t)W * H * 3, 128);
    std::vector<OutTensor> ts(io.n_output);
    std::vector<double> v_run, v_get, v_post;

    for (int it = 0; it < iters; ++it) {
        const std::string& p = imgs[(size_t)it % imgs.size()];
        cv::Mat im = cv::imread(p, cv::IMREAD_COLOR);
        if (im.empty()) continue;
        cv::Mat r;
        cv::resize(im, r, cv::Size(W, H));
        cv::cvtColor(r, r, cv::COLOR_BGR2RGB);
        memcpy(in.data(), r.data, (size_t)W * H * 3);

        rknn_input rin; memset(&rin, 0, sizeof(rin));
        rin.index = 0; rin.type = RKNN_TENSOR_UINT8; rin.fmt = RKNN_TENSOR_NHWC;
        rin.size = (uint32_t)in.size(); rin.buf = in.data(); rin.pass_through = 0;
        rknn_inputs_set(ctx, 1, &rin);

        double t0 = now_ms();
        rknn_run(ctx, nullptr);
        double t1 = now_ms();
        rknn_outputs_get(ctx, io.n_output, outs.data(), nullptr);
        double t2 = now_ms();

        // 组张量视图
        for (uint32_t i = 0; i < io.n_output; ++i) {
            ts[i].data = outs[i].buf;
            ts[i].is_int8 = (!want_float) && attrs[i].type == RKNN_TENSOR_INT8;
            ts[i].zp = attrs[i].zp;
            ts[i].scale = attrs[i].scale;
            ts[i].n_elems = attrs[i].n_elems;
            ts[i].n_dims = attrs[i].n_dims;
            for (uint32_t k = 0; k < attrs[i].n_dims && k < 4; ++k) ts[i].dims[k] = attrs[i].dims[k];
        }
        BoxMapper mapper;
        mapper.img_w = W; mapper.img_h = H; mapper.mode = BoxMapper::STRETCH;
        mapper.scale_x = 1.f; mapper.scale_y = 1.f;
        double t3 = now_ms();
        std::vector<YoloBox> boxes = yolov8_postprocess(ts.data(), (int)ts.size(), 0.15f, 0.45f, mapper);
        double t4 = now_ms();

        if (g_dump) {
            // 逐框输出: 帧号 / 框数 / 每个框的 cls|score|x1|y1|x2|y2
            fprintf(g_dump, "frame %d nboxes %zu\n", it, boxes.size());
            for (const auto& b : boxes)
                fprintf(g_dump, "  %d %.9f %.6f %.6f %.6f %.6f\n",
                        b.cls_id, b.score, b.x1, b.y1, b.x2, b.y2);
        }
        v_run.push_back(t1 - t0);
        v_get.push_back(t2 - t1);
        v_post.push_back(t4 - t3);
        if (results) results->push_back(DetOut{std::move(boxes), t1 - t0, t2 - t1, t4 - t3});
    }
    auto avg = [](std::vector<double>& v) {
        if (v.empty()) return 0.0;
        double s = 0; for (double x : v) s += x; return s / v.size();
    };
    *avg_run = avg(v_run); *avg_get = avg(v_get); *avg_post = avg(v_post);
    rknn_destroy(ctx);
    return true;
}

// ============================================================================
// B) 识别模型 —— CTC 贪心解码 (与 license_plate_rec.cc 同逻辑, 支持 fp16 直读)
// ============================================================================
static inline float h2f(uint16_t h) { __fp16 v; memcpy(&v, &h, 2); return (float)v; }

// 白名单为空 -> 全量扫描(生产口径: 没有 --ppocr-charset)
static std::string ctc_decode_f32(const float* logits, int T, int C,
                                  const std::vector<std::string>& classes,
                                  float* out_score) {
    std::string text; float score_sum = 0.f; int count = 0, last_index = 0;
    for (int n = 0; n < T; ++n) {
        const float* row = logits + (size_t)n * C;
        float max_value = row[0]; int argmax_idx = 0;
        for (int c = 1; c < C; ++c) if (row[c] > max_value) { max_value = row[c]; argmax_idx = c; }
        if (argmax_idx > 0 && !(n > 0 && argmax_idx == last_index)) {
            score_sum += max_value; count++;
            if (argmax_idx < (int)classes.size()) text += classes[(size_t)argmax_idx];
        }
        last_index = argmax_idx;
    }
    float score = score_sum / (count + 1e-6f);
    if (count == 0 || std::isnan(score)) score = 0.f;
    *out_score = score;
    return text;
}

static std::string ctc_decode_f16(const uint16_t* logits, int T, int C,
                                  const std::vector<std::string>& classes,
                                  float* out_score) {
    std::string text; float score_sum = 0.f; int count = 0, last_index = 0;
    for (int n = 0; n < T; ++n) {
        const uint16_t* row = logits + (size_t)n * C;
        uint16_t best_bits = row[0]; int argmax_idx = 0;
        float max_value = h2f(row[0]);
        for (int c = 1; c < C; ++c) {
            float v = h2f(row[c]);
            if (v > max_value) { max_value = v; argmax_idx = c; best_bits = row[c]; }
        }
        (void)best_bits;
        if (argmax_idx > 0 && !(n > 0 && argmax_idx == last_index)) {
            score_sum += max_value; count++;
            if (argmax_idx < (int)classes.size()) text += classes[(size_t)argmax_idx];
        }
        last_index = argmax_idx;
    }
    float score = score_sum / (count + 1e-6f);
    if (count == 0 || std::isnan(score)) score = 0.f;
    *out_score = score;
    return text;
}

struct OcrOut { std::string text; float score = 0; };

// ---- NEON 版 fp16 CTC 解码: 目标是把 265K 次串行比较改成 8 路并行 ----
// fp16 -> fp32 是【精确】转换(fp32 能无损表示任何 fp16), 所以最大值和索引
// 与 float32 版逐位一致。
static std::string ctc_decode_f16_neon(const uint16_t* logits, int T, int C,
                                       const std::vector<std::string>& classes,
                                       float* out_score) {
    std::string text; float score_sum = 0.f; int count = 0, last_index = 0;
    for (int n = 0; n < T; ++n) {
        const uint16_t* row = logits + (size_t)n * C;
        // 1) 向量化求最大值
        float max_value = -3.0e38f;
        int c = 0;
        for (; c + 8 <= C; c += 8) {
            float16x8_t h = vld1q_f16((const __fp16*)(row + c));
            float32x4_t lo = vcvt_f32_f16(vget_low_f16(h));
            float32x4_t hi = vcvt_f32_f16(vget_high_f16(h));
            float32x4_t mx = vmaxq_f32(vmaxq_f32(lo, hi), vdupq_n_f32(max_value));
            max_value = vmaxvq_f32(mx);
        }
        for (; c < C; ++c) {
            float v = h2f(row[c]);
            if (v > max_value) max_value = v;
        }
        // 2) 找索引(与标量版同规则: 取【第一个】等于最大值的)
        int argmax_idx = 0;
        for (int i = 0; i < C; ++i) {
            if (h2f(row[i]) == max_value) { argmax_idx = i; break; }
        }
        if (argmax_idx > 0 && !(n > 0 && argmax_idx == last_index)) {
            score_sum += max_value; count++;
            if (argmax_idx < (int)classes.size()) text += classes[(size_t)argmax_idx];
        }
        last_index = argmax_idx;
    }
    float score = score_sum / (count + 1e-6f);
    if (count == 0 || std::isnan(score)) score = 0.f;
    *out_score = score;
    return text;
}

static bool run_ocr(const std::string& model_path, const std::string& dict_path,
                    const std::vector<std::string>& rois,
                    int core_mask, int iters, int want_float, int decode_mode,
                    std::vector<OcrOut>* results,
                    double* avg_run, double* avg_get, double* avg_ctc) {
    std::vector<uint8_t> data;
    if (!load_model(model_path, &data)) return false;
    rknn_context ctx = 0;
    if (rknn_init(&ctx, data.data(), (uint32_t)data.size(), RKNN_FLAG_PRIOR_HIGH, nullptr) != 0) return false;
    rknn_set_core_mask(ctx, (rknn_core_mask)core_mask);

    // 字典
    std::vector<std::string> classes;
    { FILE* f = fopen(dict_path.c_str(), "r");
      if (f) { char line[512]; while (fgets(line, sizeof(line), f)) {
          std::string s(line); while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
          classes.push_back(s); } fclose(f); } }

    rknn_input_output_num io; memset(&io, 0, sizeof(io));
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
    rknn_tensor_attr a; memset(&a, 0, sizeof(a)); a.index = 0;
    rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &a, sizeof(a));
    const int T = a.dims[a.n_dims - 2], C = a.dims[a.n_dims - 1];
    const bool native_fp16 = (a.type == RKNN_TENSOR_FLOAT16);

    size_t bytes = a.n_elems * (want_float ? 4 : (native_fp16 ? 2 : 1));
    if (bytes < a.size) bytes = a.size;
    std::vector<uint8_t> buf(bytes, 0);
    rknn_output o; memset(&o, 0, sizeof(o));
    o.index = 0; o.want_float = want_float ? 1 : 0; o.is_prealloc = 1;
    o.buf = buf.data(); o.size = (uint32_t)buf.size();

    const int IW = 320, IH = 48;
    std::vector<uint8_t> in((size_t)IW * IH * 3, 0);
    std::vector<double> v_run, v_get, v_ctc;

    for (int it = 0; it < iters; ++it) {
        const std::string& p = rois[(size_t)it % rois.size()];
        cv::Mat im = cv::imread(p, cv::IMREAD_COLOR);
        if (im.empty()) continue;
        // ★ 与生产一致: ROI 按比例缩放到高 48(宽 = min(ceil(48*w/h), 320)),
        //   右侧补 0 到 320 —— 对应 license_plate_rec.cc 的 calc_resized_w +
        //   build_input_tensor 的补零逻辑。
        std::fill(in.begin(), in.end(), 0);
        int rw = im.cols;
        if (im.rows != IH) {
            const double ratio = (double)im.cols / (double)im.rows;
            rw = std::min((int)std::ceil((double)IH * ratio), IW);
            cv::Mat rs;
            cv::resize(im, rs, cv::Size(rw, IH));
            for (int y = 0; y < IH; ++y)
                memcpy(in.data() + (size_t)y * IW * 3, rs.ptr(y), (size_t)rw * 3);
        } else {
            rw = std::min(im.cols, IW);
            for (int y = 0; y < IH; ++y)
                memcpy(in.data() + (size_t)y * IW * 3, im.ptr(y), (size_t)rw * 3);
        }

        rknn_input rin; memset(&rin, 0, sizeof(rin));
        rin.index = 0; rin.type = RKNN_TENSOR_UINT8; rin.fmt = RKNN_TENSOR_NHWC;
        rin.size = (uint32_t)in.size(); rin.buf = in.data(); rin.pass_through = 0;
        rknn_inputs_set(ctx, 1, &rin);

        double t0 = now_ms();
        rknn_run(ctx, nullptr);
        double t1 = now_ms();
        rknn_outputs_get(ctx, 1, &o, nullptr);
        double t2 = now_ms();

        float score = 0.f; std::string text;
        if (want_float) {
            text = ctc_decode_f32((const float*)buf.data(), T, C, classes, &score);
        } else if (native_fp16 && decode_mode == 2) {
            text = ctc_decode_f16_neon((const uint16_t*)buf.data(), T, C, classes, &score);
        } else if (native_fp16) {
            text = ctc_decode_f16((const uint16_t*)buf.data(), T, C, classes, &score);
        } else {
            // int8: 现场反量化
            float* tmp = new float[(size_t)T * C];
            for (size_t i = 0; i < (size_t)T * C; ++i)
                tmp[i] = (((float)((const int8_t*)buf.data())[i]) - (float)a.zp) * a.scale;
            text = ctc_decode_f32(tmp, T, C, classes, &score);
            delete[] tmp;
        }
        double t3 = now_ms();

        v_run.push_back(t1 - t0); v_get.push_back(t2 - t1); v_ctc.push_back(t3 - t2);
        if (results) results->push_back(OcrOut{text, score});
    }
    auto avg = [](std::vector<double>& v) {
        if (v.empty()) return 0.0; double s = 0; for (double x : v) s += x; return s / v.size(); };
    *avg_run = avg(v_run); *avg_get = avg(v_get); *avg_ctc = avg(v_ctc);
    rknn_destroy(ctx);
    return true;
}

// ============================================================================
int main(int argc, char** argv) {
    std::string det, ocr, dict, imgs_dir, rois_dir, dump_path;
    int iters = 30, det_core = 1, ocr_core = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--det") && i + 1 < argc) det = argv[++i];
        else if (!strcmp(argv[i], "--ocr") && i + 1 < argc) ocr = argv[++i];
        else if (!strcmp(argv[i], "--dict") && i + 1 < argc) dict = argv[++i];
        else if (!strcmp(argv[i], "--imgs") && i + 1 < argc) imgs_dir = argv[++i];
        else if (!strcmp(argv[i], "--rois") && i + 1 < argc) rois_dir = argv[++i];
        else if (!strcmp(argv[i], "--iters") && i + 1 < argc) iters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump_path = argv[++i];
        else if (!strcmp(argv[i], "--det-core") && i + 1 < argc) det_core = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ocr-core") && i + 1 < argc) ocr_core = atoi(argv[++i]);
    }
    if (det.empty() || imgs_dir.empty()) {
        fprintf(stderr, "用法: --det <rknn> --imgs <dir> [--ocr <rknn> --dict <txt> --rois <dir>]\n");
        return 2;
    }

    std::vector<std::string> imgs = list_dir(imgs_dir, ".jpg");
    if (imgs.empty()) imgs = list_dir(imgs_dir, ".png");
    fprintf(stderr, "检测图 %zu 张\n", imgs.size());

    if (!dump_path.empty()) {
        g_dump = fopen(dump_path.c_str(), "w");
        if (!g_dump) { fprintf(stderr, "dump 文件打不开\n"); return 2; }
    }
    std::vector<DetOut> res_f, res_i;
    double fr, fg, fp, ir, ig, ip;
    fprintf(stderr, "\n=== A) 检测: want_float=1 (当前生产) ===\n");
    run_det(det, imgs, det_core, iters, 1, &res_f, &fr, &fg, &fp);
    fprintf(stderr, "  run=%.3fms  outputs_get=%.3fms  post=%.3fms  合计=%.3fms\n",
            fr, fg, fp, fr + fg + fp);
    fprintf(stderr, "\n=== A) 检测: want_float=0 (int8 直读) ===\n");
    run_det(det, imgs, det_core, iters, 0, &res_i, &ir, &ig, &ip);
    fprintf(stderr, "  run=%.3fms  outputs_get=%.3fms  post=%.3fms  合计=%.3fms\n",
            ir, ig, ip, ir + ig + ip);
    fprintf(stderr, "  >> outputs_get 差 = %.3fms ; post 差 = %.3fms ; 合计差 = %.3fms/帧\n",
            fg - ig, fp - ip, (fr + fg + fp) - (ir + ig + ip));

    // ---- 逐框对拍 ----
    size_t n = std::min(res_f.size(), res_i.size());
    int mismatch = 0; float max_dscore = 0.f, max_dcoord = 0.f; int cnt_mismatch = 0;
    for (size_t k = 0; k < n; ++k) {
        if (res_f[k].boxes.size() != res_i[k].boxes.size()) { mismatch++; cnt_mismatch++; continue; }
        for (size_t j = 0; j < res_f[k].boxes.size(); ++j) {
            const YoloBox& a = res_f[k].boxes[j];
            const YoloBox& b = res_i[k].boxes[j];
            if (a.cls_id != b.cls_id) { mismatch++; continue; }
            max_dscore = std::max(max_dscore, std::fabs(a.score - b.score));
            max_dcoord = std::max(max_dcoord, std::max(std::fabs(a.x1 - b.x1),
                                        std::max(std::fabs(a.y1 - b.y1),
                                        std::max(std::fabs(a.x2 - b.x2), std::fabs(a.y2 - b.y2)))));
        }
    }
    fprintf(stderr, "  >> 对拍 %zu 帧: 框数不一致帧=%d  字段不一致=%d  max|Δscore|=%.8f  max|Δcoord|=%.8f\n",
            n, cnt_mismatch, mismatch, max_dscore, max_dcoord);

    // ---- B) 识别 ----
    if (!ocr.empty() && !rois_dir.empty()) {
        std::vector<std::string> rois = list_dir(rois_dir, ".jpg");
        fprintf(stderr, "\n识别 ROI %zu 张\n", rois.size());
        std::vector<OcrOut> o_f, o_i;
        double orr, og, oc, irr, igg, ic;
        fprintf(stderr, "\n=== B) 识别: want_float=1 (当前生产, fp16->fp32) ===\n");
        run_ocr(ocr, dict, rois, ocr_core, iters, 1, 0, &o_f, &orr, &og, &oc);
        fprintf(stderr, "  run=%.3fms  outputs_get=%.3fms  ctc=%.3fms  合计=%.3fms\n",
                orr, og, oc, orr + og + oc);
        fprintf(stderr, "\n=== B) 识别: want_float=0 (fp16 直读, 标量 CTC) ===\n");
        run_ocr(ocr, dict, rois, ocr_core, iters, 0, 1, &o_i, &irr, &igg, &ic);
        fprintf(stderr, "  run=%.3fms  outputs_get=%.3fms  ctc=%.3fms  合计=%.3fms\n",
                irr, igg, ic, irr + igg + ic);
        std::vector<OcrOut> o_n;
        double nrr, ngg, ncc;
        fprintf(stderr, "\n=== B) 识别: want_float=0 (fp16 直读, NEON CTC) ===\n");
        run_ocr(ocr, dict, rois, ocr_core, iters, 0, 2, &o_n, &nrr, &ngg, &ncc);
        fprintf(stderr, "  run=%.3fms  outputs_get=%.3fms  ctc=%.3fms  合计=%.3fms\n",
                nrr, ngg, ncc, nrr + ngg + ncc);
        fprintf(stderr, "  >> outputs_get 差 = %.3fms ; ctc 差 = %.3fms ; 合计差 = %.3fms/次识别\n",
                og - igg, oc - ic, (orr + og + oc) - (irr + igg + ic));

        size_t m = std::min(o_f.size(), o_i.size());
        int tdiff = 0; float max_dsc = 0.f;
        for (size_t k = 0; k < m; ++k) {
            if (o_f[k].text != o_i[k].text) tdiff++;
            max_dsc = std::max(max_dsc, std::fabs(o_f[k].score - o_i[k].score));
        }
        fprintf(stderr, "  >> 对拍 %zu 次: 文本不一致=%d  max|Δscore|=%.8f\n", m, tdiff, max_dsc);
        {   // NEON 版 vs float32 版 对拍
            size_t m2 = std::min(o_f.size(), o_n.size());
            int tdiff2 = 0; float max_dsc2 = 0.f;
            for (size_t k = 0; k < m2; ++k) {
                if (o_f[k].text != o_n[k].text) tdiff2++;
                max_dsc2 = std::max(max_dsc2, std::fabs(o_f[k].score - o_n[k].score));
            }
            fprintf(stderr, "  >> NEON 对拍 %zu 次: 文本不一致=%d  max|Δscore|=%.8f\n", m2, tdiff2, max_dsc2);
            int nonempty = 0;
            for (size_t k = 0; k < m2; ++k) if (!o_f[k].text.empty()) nonempty++;
            fprintf(stderr, "  >> 有效文本(非空)样本数 = %d / %zu\n", nonempty, m2);
        }
        for (size_t k = 0; k < std::min<size_t>(m, 8); ++k)
            fprintf(stderr, "     [%zu] '%s' %.6f  vs  '%s' %.6f\n", k,
                    o_f[k].text.c_str(), o_f[k].score, o_i[k].text.c_str(), o_i[k].score);
    }
    return 0;
}
