// fp16_equiv.cc —— 识别模型输出的【逐元素穷举等价性】验证
//
// 目的: 证明 "want_float=1 (驱动把 fp16 转成 float32)" 与
//       "want_float=0 (直读 fp16)" 两条路径拿到的【每一个元素】完全相同。
//
// 为什么这个证明比"比一比识别出来的字符串"强得多:
//   只要 265000 个元素逐个相等, 那么【任何】下游解码(CTC 贪心 / 白名单 CTC /
//   softmax / top-k)得到的输出必然逐位相同 —— 这不是抽样, 是穷举。
//
// 用法: fp16_equiv --ocr <rknn> --imgs <dir> --iters N [--core 0]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <dirent.h>
#include <arm_neon.h>
#include <opencv2/opencv.hpp>
#include <rknn_api.h>

static double now_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
static inline float h2f(uint16_t h) { __fp16 v; memcpy(&v, &h, 2); return (float)v; }

int main(int argc, char** argv) {
    std::string model, dir;
    int iters = 30, core = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--ocr") && i + 1 < argc) model = argv[++i];
        else if (!strcmp(argv[i], "--imgs") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--iters") && i + 1 < argc) iters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--core") && i + 1 < argc) core = atoi(argv[++i]);
    }
    std::vector<std::string> imgs;
    { DIR* d = opendir(dir.c_str()); if (!d) return 2; struct dirent* e;
      while ((e = readdir(d))) { std::string n = e->d_name;
        if (n.size() > 4 && n.compare(n.size()-4, 4, ".jpg") == 0) imgs.push_back(dir + "/" + n); }
      closedir(d); std::sort(imgs.begin(), imgs.end()); }
    if (imgs.empty()) { fprintf(stderr, "没有图\n"); return 2; }

    FILE* f = fopen(model.c_str(), "rb"); if (!f) return 2;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> mdata((size_t)sz);
    if (fread(mdata.data(), 1, mdata.size(), f) != mdata.size()) return 2;
    fclose(f);
    rknn_context ctx = 0;
    if (rknn_init(&ctx, mdata.data(), (uint32_t)mdata.size(), RKNN_FLAG_PRIOR_HIGH, nullptr) != 0) return 2;
    rknn_set_core_mask(ctx, (rknn_core_mask)core);

    rknn_input_output_num io; memset(&io, 0, sizeof(io));
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
    rknn_tensor_attr a; memset(&a, 0, sizeof(a)); a.index = 0;
    rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &a, sizeof(a));
    const size_t NE = a.n_elems;
    fprintf(stderr, "输出: type=%d n_elems=%zu n_dims=%u dims=[", (int)a.type, NE, a.n_dims);
    for (uint32_t k = 0; k < a.n_dims && k < 4; ++k) fprintf(stderr, "%u,", a.dims[k]);
    fprintf(stderr, "]\n");

    std::vector<uint8_t> buf_f(NE * 4, 0), buf_h(NE * 2, 0);
    rknn_output of; memset(&of, 0, sizeof(of));
    of.index = 0; of.want_float = 1; of.is_prealloc = 1; of.buf = buf_f.data(); of.size = (uint32_t)buf_f.size();
    rknn_output oh; memset(&oh, 0, sizeof(oh));
    oh.index = 0; oh.want_float = 0; oh.is_prealloc = 1; oh.buf = buf_h.data(); oh.size = (uint32_t)buf_h.size();

    const int IW = 320, IH = 48;
    std::vector<uint8_t> in((size_t)IW * IH * 3, 0);
    size_t total_cmp = 0, mismatch = 0;
    float max_abs = 0.f;
    int frames = 0;
    double t_f = 0, t_h = 0;
    for (int it = 0; it < iters; ++it) {
        cv::Mat im = cv::imread(imgs[(size_t)it % imgs.size()], cv::IMREAD_COLOR);
        if (im.empty()) continue;
        std::fill(in.begin(), in.end(), 0);
        int rw = im.cols;
        if (im.rows != IH) {
            const double ratio = (double)im.cols / (double)im.rows;
            rw = std::min((int)std::ceil((double)IH * ratio), IW);
            cv::Mat rs; cv::resize(im, rs, cv::Size(rw, IH));
            for (int y = 0; y < IH; ++y) memcpy(in.data() + (size_t)y*IW*3, rs.ptr(y), (size_t)rw*3);
        } else {
            rw = std::min(im.cols, IW);
            for (int y = 0; y < IH; ++y) memcpy(in.data() + (size_t)y*IW*3, im.ptr(y), (size_t)rw*3);
        }
        rknn_input rin; memset(&rin, 0, sizeof(rin));
        rin.index = 0; rin.type = RKNN_TENSOR_UINT8; rin.fmt = RKNN_TENSOR_NHWC;
        rin.size = (uint32_t)in.size(); rin.buf = in.data(); rin.pass_through = 0;
        rknn_inputs_set(ctx, 1, &rin);

        rknn_run(ctx, nullptr);
        double t0 = now_ms();
        rknn_outputs_get(ctx, 1, &of, nullptr);
        double t1 = now_ms();
        rknn_outputs_get(ctx, 1, &oh, nullptr);
        double t2 = now_ms();
        t_f += t1 - t0; t_h += t2 - t1;

        // ---- 逐元素穷举比对 ----
        const float* pf = (const float*)buf_f.data();
        const uint16_t* ph = (const uint16_t*)buf_h.data();
        for (size_t i = 0; i < NE; ++i) {
            const float vf = pf[i];
            const float vh = h2f(ph[i]);
            total_cmp++;
            if (memcmp(&vf, &vh, sizeof(float)) != 0) {   // 逐位比较, 不只看数值
                mismatch++;
                if (std::fabs(vf - vh) > max_abs) max_abs = std::fabs(vf - vh);
            }
        }
        frames++;
    }
    fprintf(stderr, "\n=== 识别输出逐元素穷举等价性 (%d 帧) ===\n", frames);
    fprintf(stderr, "  比对元素总数 : %zu\n", total_cmp);
    fprintf(stderr, "  逐位不相等数 : %zu\n", mismatch);
    fprintf(stderr, "  最大绝对差    : %g\n", max_abs);
    fprintf(stderr, "  结论          : %s\n",
            mismatch == 0 ? "★ 完全等价(fp16->fp32 是精确转换, 一个 bit 都没差)"
                          : "!!! 存在差异 !!!");
    fprintf(stderr, "\n=== outputs_get 耗时对比 ===\n");
    fprintf(stderr, "  want_float=1 (驱动转 float32) : %.3f ms\n", t_f / frames);
    fprintf(stderr, "  want_float=0 (直读 fp16)      : %.3f ms\n", t_h / frames);
    fprintf(stderr, "  节省                            : %.3f ms/次\n", (t_f - t_h) / frames);
    rknn_destroy(ctx);
    return mismatch == 0 ? 0 : 1;
}
