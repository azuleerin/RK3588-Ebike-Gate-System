// ocr_decode_equiv.cc —— 用【生产代码的两个解码实现】做等价性验证
//
// 对比对象(都在 src/license_plate_rec.cc 里):
//   · ctc_greedy_decode      (float32 版, 改造前的老路)
//   · ctc_greedy_decode_f16  (fp16 直读 + NEON, 改造后的新路)
//
// 方法: 同一个模型、同一批真实 ROI、同一份原始输出(fp16),
//       分别喂给两个解码器, 逐次比较:
//         · 文本: 必须【完全相同】
//         · 分数: 必须【逐位相同】(memcmp, 不是"差不多")
//         · 字符数: 必须相同
// 两种模式都要测: 全量扫描(生产口径, 没传 --ppocr-charset) + 白名单模式。
//
// 用法: ocr_decode_equiv --ocr <rknn> --dict <txt> --imgs <dir> [--iters N] [--core 0]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <dirent.h>
#include <opencv2/opencv.hpp>
#include <rknn_api.h>

#include "license_plate_rec.h"

static inline float h2f(uint16_t h) { __fp16 v; memcpy(&v, &h, 2); return (float)v; }

// ===========================================================================
// 合成压力测试: 随机 fp16 张量, 逼出解码器的各种边界
//   · 大量并列最大值(考验"取第一个"的语义)
//   · 负数 / 零 / 次正规数 / 无穷 / NaN
//   · 全部相同 / 全部为零
// 不需要 NPU, 纯 CPU, 跑得飞快。
// ===========================================================================
static int synthetic_stress(int rounds, int T, int C, uint32_t seed) {
    std::vector<std::string> classes;
    for (int i = 0; i < C; ++i) classes.push_back(std::string(1, (char)('a' + (i % 26))));
    std::vector<uint16_t> h((size_t)T * C);
    std::vector<float> f((size_t)T * C);
    std::vector<int> allowed;
    allowed.push_back(0);
    for (int c = 1; c < C; c += 7) allowed.push_back(c);

    uint32_t s = seed ? seed : 1u;
    auto rnd = [&s]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; };

    int diff_full = 0, diff_wl = 0, sc_full = 0, sc_wl = 0;
    for (int r = 0; r < rounds; ++r) {
        const int mode = r % 6;
        for (size_t i = 0; i < h.size(); ++i) {
            uint16_t v;
            switch (mode) {
                case 0: v = (uint16_t)(rnd() & 0x7FFFu); break;         // 全随机(非负)
                case 1: v = (uint16_t)(rnd() & 0xFFFFu); break;         // 全随机(含负)
                case 2: v = (uint16_t)((rnd() % 5) * 0x3C00u); break;   // 少量取值 -> 大量并列
                case 3: v = 0x0000u; break;                             // 全 0
                case 4: v = 0x3C00u; break;                             // 全相同 (1.0)
                default: v = (uint16_t)((rnd() % 3 == 0) ? 0x7C00u :    // +inf
                                        ((rnd() % 3 == 1) ? 0xFC00u :  // -inf
                                                          (rnd() & 0x03FFu))); // 次正规/极小
            }
            h[i] = v;
            f[i] = h2f(v);
        }
        rk::PlateRecResult a, b, aw, bw;
        rk::LicensePlateRec::ctc_greedy_decode(f.data(), T, C, classes, nullptr, &a);
        rk::LicensePlateRec::ctc_greedy_decode_f16(h.data(), T, C, classes, nullptr, &b);
        rk::LicensePlateRec::ctc_greedy_decode(f.data(), T, C, classes, &allowed, &aw);
        rk::LicensePlateRec::ctc_greedy_decode_f16(h.data(), T, C, classes, &allowed, &bw);
        if (a.text != b.text) diff_full++;
        if (memcmp(&a.score, &b.score, sizeof(float)) != 0) sc_full++;
        if (aw.text != bw.text) diff_wl++;
        if (memcmp(&aw.score, &bw.score, sizeof(float)) != 0) sc_wl++;
    }
    fprintf(stderr, "\n=== 合成压力测试 (%d 轮 x %dx%d 随机张量) ===\n", rounds, T, C);
    fprintf(stderr, "  [全量扫描] 文本不一致=%d  分数逐位不一致=%d\n", diff_full, sc_full);
    fprintf(stderr, "  [白名单]   文本不一致=%d  分数逐位不一致=%d\n", diff_wl, sc_wl);
    const bool ok = (diff_full == 0 && sc_full == 0 && diff_wl == 0 && sc_wl == 0);
    fprintf(stderr, "  结论: %s\n", ok ? "★ 逐位一致" : "!!! 存在差异 !!!");
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    std::string model, dict, dir;
    int iters = 60, core = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--ocr") && i + 1 < argc) model = argv[++i];
        else if (!strcmp(argv[i], "--dict") && i + 1 < argc) dict = argv[++i];
        else if (!strcmp(argv[i], "--imgs") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--iters") && i + 1 < argc) iters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--core") && i + 1 < argc) core = atoi(argv[++i]);
    }
    // ---- 合成压力测试(不需要图, 先跑) ----
    int synth_rc = synthetic_stress(4000, 40, 6625, 0xC0FFEEu);

    std::vector<std::string> imgs;
    if (dir.empty()) { return synth_rc; }
    { DIR* d = opendir(dir.c_str()); if (!d) return 2; struct dirent* e;
      while ((e = readdir(d))) { std::string n = e->d_name;
        if (n.size() > 4 && n.compare(n.size()-4, 4, ".jpg") == 0) imgs.push_back(dir + "/" + n); }
      closedir(d); std::sort(imgs.begin(), imgs.end()); }
    if (imgs.empty()) { fprintf(stderr, "没有图\n"); return 2; }

    // ---- 字典 ----
    std::vector<std::string> classes;
    { FILE* f = fopen(dict.c_str(), "r");
      if (!f) { fprintf(stderr, "字典读不了\n"); return 2; }
      char line[512];
      while (fgets(line, sizeof(line), f)) {
          std::string s(line);
          while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
          classes.push_back(s);
      }
      fclose(f); }
    // 生产口径: dict 少了 3 个类时补空格(和 load_dict 一致)
    while (classes.size() < 6625) classes.push_back(" ");

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
    const int T = (int)a.dims[a.n_dims - 2], C = (int)a.dims[a.n_dims - 1];
    const size_t NE = a.n_elems;

    std::vector<uint8_t> buf_h(NE * 2, 0);
    rknn_output oh; memset(&oh, 0, sizeof(oh));
    oh.index = 0; oh.want_float = 0; oh.is_prealloc = 1;
    oh.buf = buf_h.data(); oh.size = (uint32_t)buf_h.size();

    const int IW = 320, IH = 48;
    std::vector<uint8_t> in((size_t)IW * IH * 3, 0);
    std::vector<float> f32(NE);

    // 白名单(模拟 --ppocr-charset: 数字 + 大写字母 + blank)
    std::vector<int> allowed;
    { allowed.push_back(0);
      const std::string chars = "0123" "456789ABCDEFGHJKLMNPQRSTUVWXYZ";
      for (int c = 1; c < (int)classes.size(); ++c)
          if (chars.find(classes[(size_t)c]) != std::string::npos) allowed.push_back(c); }

    double t_f32 = 0, t_f16 = 0, t_f32w = 0, t_f16w = 0;
    int n = 0, txt_diff_full = 0, txt_diff_wl = 0, sc_diff_full = 0, sc_diff_wl = 0;
    int cnt_diff_full = 0, cnt_diff_wl = 0, nonempty = 0;
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
        rknn_outputs_get(ctx, 1, &oh, nullptr);

        // 同一份 fp16 输出 -> 两份表示
        for (size_t i = 0; i < NE; ++i) f32[i] = h2f(((const uint16_t*)buf_h.data())[i]);

        rk::PlateRecResult r32, r16, r32w, r16w;
        auto now_ms = [](){ return std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now().time_since_epoch()).count(); };
        double t0 = now_ms();
        rk::LicensePlateRec::ctc_greedy_decode(f32.data(), T, C, classes, nullptr, &r32);
        double t1 = now_ms();
        rk::LicensePlateRec::ctc_greedy_decode_f16((const uint16_t*)buf_h.data(), T, C, classes, nullptr, &r16);
        double t2 = now_ms();
        rk::LicensePlateRec::ctc_greedy_decode(f32.data(), T, C, classes, &allowed, &r32w);
        double t3 = now_ms();
        rk::LicensePlateRec::ctc_greedy_decode_f16((const uint16_t*)buf_h.data(), T, C, classes, &allowed, &r16w);
        double t4 = now_ms();
        t_f32 += t1 - t0; t_f16 += t2 - t1; t_f32w += t3 - t2; t_f16w += t4 - t3;

        if (r32.text != r16.text) txt_diff_full++;
        if (memcmp(&r32.score, &r16.score, sizeof(float)) != 0) sc_diff_full++;
        if (r32.char_count != r16.char_count) cnt_diff_full++;
        if (r32w.text != r16w.text) txt_diff_wl++;
        if (memcmp(&r32w.score, &r16w.score, sizeof(float)) != 0) sc_diff_wl++;
        if (r32w.char_count != r16w.char_count) cnt_diff_wl++;
        if (!r32.text.empty() || !r32w.text.empty()) nonempty++;
        n++;
    }
    fprintf(stderr, "\n=== 生产解码实现等价性验证 (%d 帧真实 ROI) ===\n", n);
    fprintf(stderr, "  [全量扫描模式] 文本不一致=%d  分数逐位不一致=%d  字符数不一致=%d\n",
            txt_diff_full, sc_diff_full, cnt_diff_full);
    fprintf(stderr, "  [白名单模式]   文本不一致=%d  分数逐位不一致=%d  字符数不一致=%d\n",
            txt_diff_wl, sc_diff_wl, cnt_diff_wl);
    fprintf(stderr, "  有非空识别结果的样本 = %d / %d\n", nonempty, n);
    fprintf(stderr, "  [耗时] float32 解码 = %.3f ms/次 ; fp16 NEON 解码 = %.3f ms/次 (省 %.3f ms)\n",
            t_f32 / n, t_f16 / n, (t_f32 - t_f16) / n);
    fprintf(stderr, "  [耗时] float32 白名单 = %.3f ms/次 ; fp16 白名单 = %.3f ms/次\n",
            t_f32w / n, t_f16w / n);
    const bool ok = (txt_diff_full == 0 && sc_diff_full == 0 && cnt_diff_full == 0 &&
                     txt_diff_wl == 0 && sc_diff_wl == 0 && cnt_diff_wl == 0);
    fprintf(stderr, "  结论: %s\n", ok ? "★ 两个解码器逐位一致" : "!!! 存在差异 !!!");
    rknn_destroy(ctx);
    return (ok && synth_rc == 0) ? 0 : 1;
}
