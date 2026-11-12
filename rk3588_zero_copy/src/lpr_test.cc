// lpr_test.cc —— 单图车牌识别测试工具
//
// 用途: 拿一张车牌照片, 直接送进 rknn 模型, 打印识别结果。
//   ./lpr_test --model model/lprnet_finetuned_int8.rknn --image test_plate.jpg
//
// ★ 设计目标: 以后换了新模型(LPRNet -> PP-OCR -> 别的), 【只改命令行参数】,
//   不用改这一行代码。模型相关的量全部是运行时从 rknn 模型本身查出来的:
//     输入宽高 / 布局(NHWC|NCHW) / 数据类型 / 量化参数(zp, scale)
//     输出形状 / 字符集偏移
//   需要人工指定的只有【字符集】和【归一化】(模型外的东西)。
//
// ---------------------------------------------------------------------------
// 喂数据的三条铁律(踩过才知道):
//  1) 输入 dtype 要跟模型 attr 里查出来的 type 对上;
//     且 int8 要按模型自己的 zp/scale 生成量化码, 数量 = 元素个数(不是字节数*4)。
//     喂错 size 会让 runtime 越界读写我们的缓冲 -> 堆被踩坏 -> 随机 abort。
//  2) 通道顺序必须是 RGB(训练时 cv2.cvtColor(BGR2RGB) 的结果)。
//  3) 归一化不要重复做: 模型图里已经固化过的就别再除 255 / 再减 mean。
//     本工具默认【不】做归一化, 要做用 --mean/--std 显式指定。
// ---------------------------------------------------------------------------
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "rknn_api.h"

namespace rk {

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
double now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

const char* type_name(int t) {
    switch (t) {
        case RKNN_TENSOR_FLOAT32: return "FLOAT32";
        case RKNN_TENSOR_INT8:    return "INT8";
        case RKNN_TENSOR_UINT8:   return "UINT8";
        case RKNN_TENSOR_INT16:   return "INT16";
        default:                  return "OTHER";
    }
}

const char* fmt_name(int f) {
    return (f == RKNN_TENSOR_NCHW) ? "NCHW" : "NHWC";
}

std::vector<float> parse_floats(const char* s, int n) {
    std::vector<float> v(n, 0.f);
    if (!s) return v;
    const char* p = s;
    for (int i = 0; i < n; ++i) {
        v[i] = static_cast<float>(atof(p));
        const char* c = strchr(p, ',');
        if (!c) break;
        p = c + 1;
    }
    return v;
}

std::string read_text_file(const char* path) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return "";
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) out.append(buf, n);
    fclose(fp);
    return out;
}

// ---------------------------------------------------------------------------
// 用法
// ---------------------------------------------------------------------------
void usage(const char* argv0) {
    fprintf(stderr,
        "用法: %s --model <model.rknn> --image <图片> [选项]\n"
        "\n"
        "必填:\n"
        "  --model PATH            rknn 模型文件\n"
        "  --image PATH            待识别图片(jpg/png/bmp)\n"
        "\n"
        "模型相关(默认全部自动从模型里查, 换模型不用改):\n"
        "  --width W --height H    覆盖模型输入尺寸(默认自动)\n"
        "  --feed int8|float|uint8 覆盖喂法(默认自动: 按模型 attr 的 type 选)\n"
        "  --prefix 深圳           车牌前缀(LPRNet 只出数字字母, 前端要带省份)\n"
        "\n"
        "预处理:\n"
        "  --crop x,y,w,h          先按原图坐标裁一块再缩放\n"
        "  --resize stretch|letterbox|center  缩放方式(默认 stretch, 整图用 letterbox/center)\n"
        "  --color rgb|bgr         通道顺序(默认 rgb)\n"
        "  --mean a,b,c            逐通道减均值(默认不减, 模型里已固化就别给)\n"
        "  --std a,b,c             逐通道除标准差(默认不除)\n"
        "\n"
        "解码:\n"
        "  --charset STR           字符集(默认 LPRNet 的 34 个)\n"
        "  --charset-file PATH     从文件读字符集(每行一个字符, 忽略空行/空白)\n"
        "  --charset-offset N      类别 k -> 字符集[k-N](默认自动: C==len+1 时 N=1)\n"
        "  --min-score S           置信度低于 S 就不输出(默认 0 = 不拦)\n"
        "\n"
        "输出:\n"
        "  --save-preprocessed P   把预处理后的图存出来(肉眼核对用)\n"
        "  --json                  输出 JSON(给脚本消费)\n"
        "  --repeat N              重复推理 N 次并打印平均耗时\n",
        argv0);
}

// ---------------------------------------------------------------------------
// 输出张量形状 + 取值
// ---------------------------------------------------------------------------
struct OutShape {
    int T = 0;          // 时间步
    int C = 0;          // 类别数
    bool nchw = false;  // 布局是 NCHW(数据按类大页排布)

    // logits(t, c)
    float at(const float* p, int t, int c) const {
        return nchw ? p[static_cast<size_t>(c) * T + t]
                    : p[static_cast<size_t>(t) * C + c];
    }
};

// ---------------------------------------------------------------------------
// 预处理
// ---------------------------------------------------------------------------
enum class ResizeMode { kStretch, kLetterbox, kCenter };

cv::Mat preprocess(const cv::Mat& src, int W, int H, ResizeMode mode, bool to_rgb,
                    const std::vector<float>& mean, const std::vector<float>& stdv,
                    cv::Mat* debug_out) {
    cv::Mat img = src;

    // 1) 缩放到模型输入尺寸
    cv::Mat sized;
    if (mode == ResizeMode::kLetterbox) {
        const double s = std::min(static_cast<double>(W) / img.cols,
                                  static_cast<double>(H) / img.rows);
        const int nw = std::max(1, static_cast<int>(std::lround(img.cols * s)));
        const int nh = std::max(1, static_cast<int>(std::lround(img.rows * s)));
        cv::Mat r;
        cv::resize(img, r, cv::Size(nw, nh), 0, 0, cv::INTER_LINEAR);
        sized = cv::Mat(H, W, r.type(), cv::Scalar(0, 0, 0));
        r.copyTo(sized(cv::Rect((W - nw) / 2, (H - nh) / 2, nw, nh)));
    } else if (mode == ResizeMode::kCenter) {
        // 按目标宽高比居中裁, 再缩放
        const double ta = static_cast<double>(W) / H, ia = static_cast<double>(img.cols) / img.rows;
        cv::Rect roi(0, 0, img.cols, img.rows);
        if (ia > ta) {                       // 太宽 -> 裁左右
            const int nw = static_cast<int>(std::lround(img.rows * ta));
            roi.x = (img.cols - nw) / 2; roi.width = nw;
        } else {                             // 太高 -> 裁上下
            const int nh = static_cast<int>(std::lround(img.cols / ta));
            roi.y = (img.rows - nh) / 2; roi.height = nh;
        }
        cv::resize(img(roi), sized, cv::Size(W, H), 0, 0, cv::INTER_LINEAR);
    } else {
        cv::resize(img, sized, cv::Size(W, H), 0, 0, cv::INTER_LINEAR);
    }

    // 2) 通道顺序
    if (to_rgb && sized.channels() == 3) {
        cv::cvtColor(sized, sized, cv::COLOR_BGR2RGB);
    }

    if (debug_out) *debug_out = sized.clone();

    // 3) 归一化(默认不做 —— 模型里已经固化过的就别重复)
    //    这里只把结果存到 float 缓冲, 量化在喂数阶段做。
    (void)mean; (void)stdv;
    return sized;
}

// ---------------------------------------------------------------------------
// CTC 贪心解码 + 置信度
// ---------------------------------------------------------------------------
std::string decode_ctc(const float* logits, const OutShape& sh, const std::string& charset,
                       int offset, double* conf_out) {
    std::string out;
    double sum = 0.0;
    int n = 0;
    int prev = -1;
    for (int t = 0; t < sh.T; ++t) {
        int best = 0;
        float bestv = sh.at(logits, t, 0);
        for (int c = 1; c < sh.C; ++c) {
            const float v = sh.at(logits, t, c);
            if (v > bestv) { bestv = v; best = c; }
        }
        // softmax 置信度
        float mx = bestv;
        double s = 0.0;
        for (int c = 0; c < sh.C; ++c) {
            const float v = sh.at(logits, t, c);
            if (v > mx) mx = v;
        }
        for (int c = 0; c < sh.C; ++c) s += std::exp(sh.at(logits, t, c) - mx);
        const double p_best = 1.0 / s;

        if (best != prev && best != 0) {           // 0 = CTC blank
            const int idx = best - offset;
            if (idx >= 0 && idx < static_cast<int>(charset.size())) {
                out.push_back(charset[static_cast<size_t>(idx)]);
                sum += p_best;
                ++n;
            }
        }
        prev = best;
    }
    if (conf_out) *conf_out = n ? sum / n : 0.0;
    return out;
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    std::string model_path, image_path, charset, charset_file;
    std::string save_pre, prefix;
    std::string mean_s, std_s;
    int W = 0, H = 0, offset = -1, repeat = 1;
    double min_score = 0.0;
    bool to_rgb = true, json = false;
    ResizeMode rmode = ResizeMode::kStretch;
    std::string feed_override;
    int crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0;
    bool has_crop = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) { fprintf(stderr, "参数 %s 后面缺值\n", what); exit(2); }
            return argv[++i];
        };
        if      (a == "--model")      model_path = next("--model");
        else if (a == "--image")      image_path = next("--image");
        else if (a == "--width")      W = atoi(next("--width"));
        else if (a == "--height")     H = atoi(next("--height"));
        else if (a == "--feed")       feed_override = next("--feed");
        else if (a == "--prefix")     prefix = next("--prefix");
        else if (a == "--charset")    charset = next("--charset");
        else if (a == "--charset-file") charset_file = next("--charset-file");
        else if (a == "--charset-offset") offset = atoi(next("--charset-offset"));
        else if (a == "--min-score")  min_score = atof(next("--min-score"));
        else if (a == "--mean")       mean_s = next("--mean");
        else if (a == "--std")        std_s = next("--std");
        else if (a == "--save-preprocessed") save_pre = next("--save-preprocessed");
        else if (a == "--json")       json = true;
        else if (a == "--repeat")     repeat = atoi(next("--repeat"));
        else if (a == "--color") {
            const char* c = next("--color");
            to_rgb = (strcmp(c, "bgr") != 0);
        } else if (a == "--resize") {
            const char* m = next("--resize");
            if (!strcmp(m, "letterbox")) rmode = ResizeMode::kLetterbox;
            else if (!strcmp(m, "center")) rmode = ResizeMode::kCenter;
            else rmode = ResizeMode::kStretch;
        } else if (a == "--crop") {
            if (sscanf(next("--crop"), "%d,%d,%d,%d", &crop_x, &crop_y, &crop_w, &crop_h) == 4)
                has_crop = true;
            else { fprintf(stderr, "--crop 格式应为 x,y,w,h\n"); return 2; }
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "不认识的参数: %s\n", a.c_str());
            usage(argv[0]);
            return 2;
        }
    }

    if (model_path.empty() || image_path.empty()) { usage(argv[0]); return 2; }
    if (repeat < 1) repeat = 1;

    // ---- 字符集 ----
    if (!charset_file.empty()) {
        // 每行一个字符; 也允许一行写一串(那行多长就当多长)
        const std::string raw = read_text_file(charset_file.c_str());
        std::string cur;
        for (size_t i = 0; i <= raw.size(); ++i) {
            const char c = (i < raw.size()) ? raw[i] : '\n';
            if (c == '\n' || c == '\r') {
                if (!cur.empty()) charset += cur;
                cur.clear();
            } else if (c == ' ' || c == '\t') {
                continue;
            } else {
                cur.push_back(c);
            }
        }
        if (charset.empty()) {
            fprintf(stderr, "字符集文件是空的: %s\n", charset_file.c_str());
            return 1;
        }
    }
    if (charset.empty()) charset = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ";   // LPRNet 默认

    // ---- 读图 ----
    cv::Mat img = cv::imread(image_path, cv::IMREAD_COLOR);
    if (img.empty()) {
        fprintf(stderr, "读不了图片: %s\n", image_path.c_str());
        return 1;
    }
    if (has_crop) {
        cv::Rect r(crop_x, crop_y, crop_w, crop_h);
        r &= cv::Rect(0, 0, img.cols, img.rows);
        if (r.width <= 0 || r.height <= 0) {
            fprintf(stderr, "--crop 区域无效 (原图 %dx%d)\n", img.cols, img.rows);
            return 1;
        }
        img = img(r).clone();
    }
    const int src_w = img.cols, src_h = img.rows;

    // ---- 载入模型 ----
    FILE* fp = fopen(model_path.c_str(), "rb");
    if (!fp) { fprintf(stderr, "打不开模型: %s\n", model_path.c_str()); return 1; }
    fseek(fp, 0, SEEK_END);
    const long msize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (msize <= 0) { fclose(fp); fprintf(stderr, "模型是空的\n"); return 1; }
    std::vector<unsigned char> mbuf(static_cast<size_t>(msize));
    if (fread(mbuf.data(), 1, mbuf.size(), fp) != mbuf.size()) {
        fclose(fp); fprintf(stderr, "读模型失败\n"); return 1;
    }
    fclose(fp);

    rknn_context ctx = 0;
    int ret = rknn_init(&ctx, mbuf.data(), static_cast<uint32_t>(mbuf.size()),
                        RKNN_FLAG_PRIOR_HIGH, nullptr);
    if (ret < 0) { fprintf(stderr, "rknn_init 失败: %d\n", ret); return 1; }

    // ---- 查输入属性 ----
    rknn_tensor_attr ia;
    memset(&ia, 0, sizeof(ia));
    ia.index = 0;
    rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &ia, sizeof(ia));

    const bool in_nchw = (ia.fmt == RKNN_TENSOR_NCHW);
    int mc = 3;                                   // 通道数
    int mw = 0, mh = 0;
    if (ia.n_dims >= 4) {
        if (in_nchw) { mc = static_cast<int>(ia.dims[1]); mh = static_cast<int>(ia.dims[2]); mw = static_cast<int>(ia.dims[3]); }
        else         { mh = static_cast<int>(ia.dims[1]); mw = static_cast<int>(ia.dims[2]); mc = static_cast<int>(ia.dims[3]); }
    }
    if (mw <= 0 || mh <= 0) {
        fprintf(stderr, "模型输入属性异常: n_dims=%u dims=[", ia.n_dims);
        for (uint32_t i = 0; i < ia.n_dims; ++i) fprintf(stderr, "%u,", ia.dims[i]);
        fprintf(stderr, "]\n");
        rknn_destroy(ctx);
        return 1;
    }
    if (W > 0) mw = W;
    if (H > 0) mh = H;

    const bool has_qnt = (ia.qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC) && (ia.scale > 0.f);
    const float in_scale = has_qnt ? ia.scale : 1.0f;
    const int   in_zp    = has_qnt ? ia.zp : 0;

    // ---- 查输出属性 ----
    rknn_tensor_attr oa;
    memset(&oa, 0, sizeof(oa));
    oa.index = 0;
    rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &oa, sizeof(oa));

    uint32_t n_outputs_ = 1;
    OutShape sh;
    sh.nchw = (oa.fmt == RKNN_TENSOR_NCHW);
    if (oa.n_dims >= 3) {
        if (sh.nchw) { sh.C = static_cast<int>(oa.dims[1]); sh.T = static_cast<int>(oa.dims[2]); }
        else         { sh.T = static_cast<int>(oa.dims[oa.n_dims - 2]); sh.C = static_cast<int>(oa.dims[oa.n_dims - 1]); }
    } else if (oa.n_dims == 2) {
        sh.T = 1; sh.C = static_cast<int>(oa.dims[oa.n_dims - 1]);
    }

    if (offset < 0) {
        // C == len+1 -> blank 占 0, 字符从 1 开始 (LPRNet / PP-OCR 都是这样)
        offset = (sh.C == static_cast<int>(charset.size()) + 1) ? 1 : 0;
    }

    // ---- 模型形状自检: 别拿检测模型/分类模型硬套 CTC 解码 ----
    // 识别模型的输出应该是 [1, T, C], C == 字符集个数(+1 给 blank)。
    // 对不上就明说, 别输出一串没意义的字让人以为模型坏了。
    if (sh.C != static_cast<int>(charset.size()) &&
        sh.C != static_cast<int>(charset.size()) + 1) {
        fprintf(stderr,
                "[警告] 输出类别数 C=%d 和字符集长度 %d 对不上, 这多半不是 CTC 识别模型\n"
                "       (识别模型输出一般是 [1,T,字符集个数+1])。下面的解码结果没有意义。\n"
                "       如果这是检测模型, 请改用 --charset 指定它自己的类别数再看。\n",
                sh.C, static_cast<int>(charset.size()));
    }
    {
        // 多输出模型(比如 YOLO 检测模型有 9 个输出)本工具只解码第 0 个。
        // ★ 这个 runtime(2.3.2)要求【一次性把全部输出都取出来】, 只取 1 个
        //   会报 "param output index(...) is out of range(N)" —— main.cc 也是
        //   按这个约定写的, 这里保持一致。
        uint32_t n_out = 1;
        rknn_input_output_num ion;
        memset(&ion, 0, sizeof(ion));
        if (rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &ion, sizeof(ion)) == RKNN_SUCC &&
            ion.n_output > 0) {
            n_out = ion.n_output;
        }
        n_outputs_ = n_out;
        if (n_out > 1)
            fprintf(stderr, "[注意] 模型有 %u 个输出, 本工具只解码第 0 个\n", n_out);
    }

    // ---- 喂法 ----
    std::string feed = feed_override;
    if (feed.empty()) {
        feed = (ia.type == RKNN_TENSOR_FLOAT32) ? "float"
             : (ia.type == RKNN_TENSOR_UINT8)   ? "uint8" : "int8";
    }
    if (feed == "int8" && !has_qnt && ia.type != RKNN_TENSOR_INT8) {
        fprintf(stderr, "[注意] 模型输入没有 AFFINE_ASYMMETRIC 量化参数, 改用 %s 喂法\n",
                ia.type == RKNN_TENSOR_FLOAT32 ? "float" : "uint8");
        feed = (ia.type == RKNN_TENSOR_FLOAT32) ? "float" : "uint8";
    }

    // ---- 预处理 ----
    const std::vector<float> mean = parse_floats(mean_s.c_str(), mc);
    const std::vector<float> stdv = parse_floats(std_s.c_str(), mc);
    const bool do_norm = !mean_s.empty() || !std_s.empty();

    cv::Mat prep;
    cv::Mat rgb = preprocess(img, mw, mh, rmode, to_rgb, mean, stdv, &prep);

    if (!json) {
        printf("[模型] %s\n", model_path.c_str());
        printf("[图片] %s  (原图 %dx%d%s)\n", image_path.c_str(), src_w, src_h,
               has_crop ? ", 已裁剪" : "");
        printf("[预处理] -> %dx%d  %s  颜色=%s  归一化=%s\n", mw, mh,
               rmode == ResizeMode::kStretch ? "stretch" :
               rmode == ResizeMode::kLetterbox ? "letterbox" : "center",
               to_rgb ? "RGB" : "BGR", do_norm ? "有" : "无");
        printf("[模型输入] name=%s type=%s fmt=%s dims=[", ia.name, type_name(ia.type),
               fmt_name(ia.fmt));
        for (uint32_t i = 0; i < ia.n_dims; ++i) printf("%u,", ia.dims[i]);
        printf("] n_elems=%u", ia.n_elems);
        if (has_qnt) printf(" zp=%d scale=%.8f", in_zp, in_scale);
        printf("\n");
        printf("[模型输出] name=%s type=%s fmt=%s dims=[", oa.name, type_name(oa.type),
               fmt_name(oa.fmt));
        for (uint32_t i = 0; i < oa.n_dims; ++i) printf("%u,", oa.dims[i]);
        printf("]  -> T=%d C=%d\n", sh.T, sh.C);
        printf("[喂法] %s   [字符集] %d 个, 偏移=%d\n", feed.c_str(),
               static_cast<int>(charset.size()), offset);
        fflush(stdout);
    }

    if (!save_pre.empty()) {
        cv::imwrite(save_pre, prep);
        if (!json) printf("[已保存预处理图] %s\n", save_pre.c_str());
    }

    // ---- 组输入缓冲 ----
    //  元素个数 = H * W * C
    const size_t n_elems = static_cast<size_t>(mh) * mw * mc;
    std::vector<float>   f32(n_elems);
    std::vector<int8_t>  i8(n_elems);
    std::vector<uint8_t> u8(n_elems);

    // 实数域: [0,1] 或按 --mean/--std 归一化后的值
    for (int y = 0; y < mh; ++y) {
        for (int x = 0; x < mw; ++x) {
            const cv::Vec3b* row = prep.ptr<cv::Vec3b>(y);
            for (int c = 0; c < mc; ++c) {
                const uint8_t px = (mc >= 3) ? row[x][c] : row[x][0];
                float v = static_cast<float>(px) / 255.0f;
                if (do_norm) {
                    v = (v - mean[c]) / (stdv[c] != 0.f ? stdv[c] : 1.0f);
                }
                const size_t idx = in_nchw
                    ? (static_cast<size_t>(c) * mh + y) * mw + x
                    : (static_cast<size_t>(y) * mw + x) * mc + c;
                f32[idx] = v;
            }
        }
    }

    auto build_inputs = [&](const std::string& f, rknn_input* in, void** buf, uint32_t* size) -> bool {
        memset(in, 0, sizeof(*in));
        in->index = 0;
        in->pass_through = 0;
        in->fmt = in_nchw ? RKNN_TENSOR_NCHW : RKNN_TENSOR_NHWC;
        if (f == "float") {
            in->buf = f32.data();
            in->size = static_cast<uint32_t>(n_elems * sizeof(float));
            in->type = RKNN_TENSOR_FLOAT32;
        } else if (f == "uint8") {
            for (size_t i = 0; i < n_elems; ++i)
                u8[i] = static_cast<uint8_t>(std::lround(std::max(0.f, std::min(1.f, f32[i])) * 255.f));
            in->buf = u8.data();
            in->size = static_cast<uint32_t>(n_elems);
            in->type = RKNN_TENSOR_UINT8;
        } else {   // int8 量化码
            // q = clamp(round(real / scale) + zp, -128, 127)
            const float inv = 1.0f / (in_scale != 0.f ? in_scale : 1.0f);
            for (size_t i = 0; i < n_elems; ++i) {
                int q = static_cast<int>(lrintf(f32[i] * inv)) + in_zp;
                if (q > 127) q = 127;
                if (q < -128) q = -128;
                i8[i] = static_cast<int8_t>(q);
            }
            in->buf = i8.data();
            in->size = static_cast<uint32_t>(n_elems);   // ★ 是量化码【个数】, 不是字节数*4
            in->type = RKNN_TENSOR_INT8;
        }
        *buf = in->buf;
        *size = in->size;
        return true;
    };

    // ---- 推理(喂法失败时自动换一种再试, 换模型时省心) ----
    std::vector<std::string> tries;
    if (!feed_override.empty()) tries.push_back(feed);
    else {
        tries.push_back(feed);
        for (const char* alt : {"float", "int8", "uint8"})
            if (feed != alt) tries.push_back(alt);
    }

    std::vector<float> logits;
    double infer_ms = 0.0;
    std::string used_feed;
    bool ok = false;
    for (const std::string& f : tries) {
        rknn_input in;
        void* buf = nullptr; uint32_t bsize = 0;
        build_inputs(f, &in, &buf, &bsize);
        ret = rknn_inputs_set(ctx, 1, &in);
        if (ret < 0) {
            if (!json) fprintf(stderr, "[喂法 %s] rknn_inputs_set 失败 %d, 换下一种\n", f.c_str(), ret);
            continue;
        }
        const double t0 = now_ms();
        ret = rknn_run(ctx, nullptr);
        if (ret < 0) {
            if (!json) fprintf(stderr, "[喂法 %s] rknn_run 失败 %d, 换下一种\n", f.c_str(), ret);
            continue;
        }
        // ★ 一次性把【全部】输出都取出来(和 main.cc 一样)。这个 runtime
        //   只取 1 个会报 "param output index(...) is out of range(N)"。
        std::vector<rknn_output> outs(n_outputs_);
        memset(outs.data(), 0, sizeof(rknn_output) * outs.size());
        for (uint32_t i = 0; i < n_outputs_; ++i) {
            outs[i].want_float = 1;
            outs[i].is_prealloc = 0;
            outs[i].index = i;
        }
        ret = rknn_outputs_get(ctx, n_outputs_, outs.data(), nullptr);
        if (ret < 0) {
            if (!json) fprintf(stderr, "[喂法 %s] rknn_outputs_get 失败 %d, 换下一种\n", f.c_str(), ret);
            continue;
        }
        infer_ms = now_ms() - t0;
        const size_t n = static_cast<size_t>(sh.T) * sh.C;
        logits.assign(static_cast<const float*>(outs[0].buf),
                      static_cast<const float*>(outs[0].buf) + n);
        rknn_outputs_release(ctx, n_outputs_, outs.data());
        used_feed = f;
        ok = true;
        break;
    }
    if (!ok) {
        fprintf(stderr, "所有喂法都失败了\n");
        rknn_destroy(ctx);
        return 1;
    }
    if (!json && used_feed != feed) {
        printf("[喂法] 自动切换到 %s\n", used_feed.c_str());
    }

    // ---- 解码 ----
    double conf = 0.0;
    std::string raw = decode_ctc(logits.data(), sh, charset, offset, &conf);
    const std::string plate = prefix + raw;

    // 多次重复跑, 取平均耗时
    if (repeat > 1) {
        rknn_input in; void* b = nullptr; uint32_t s = 0;
        build_inputs(used_feed, &in, &b, &s);
        double total = infer_ms;
        for (int i = 1; i < repeat; ++i) {
            rknn_inputs_set(ctx, 1, &in);
            const double t0 = now_ms();
            rknn_run(ctx, nullptr);
            total += now_ms() - t0;
            std::vector<rknn_output> outs2(n_outputs_);
            memset(outs2.data(), 0, sizeof(rknn_output) * outs2.size());
            for (uint32_t i = 0; i < n_outputs_; ++i) {
                outs2[i].want_float = 1; outs2[i].is_prealloc = 0; outs2[i].index = i;
            }
            if (rknn_outputs_get(ctx, n_outputs_, outs2.data(), nullptr) == 0)
                rknn_outputs_release(ctx, n_outputs_, outs2.data());
        }
        infer_ms = total / repeat;
    }

    const bool low = (min_score > 0.0) && (conf < min_score);

    if (json) {
        printf("{\"model\":\"%s\",\"image\":\"%s\",\"plate\":\"%s\",\"raw\":\"%s\","
               "\"confidence\":%.4f,\"feed\":\"%s\",\"infer_ms\":%.2f,\"width\":%d,\"height\":%d}\n",
               model_path.c_str(), image_path.c_str(), plate.c_str(), raw.c_str(),
               conf, used_feed.c_str(), infer_ms, mw, mh);
    } else {
        printf("[耗时] 推理 %.2f ms%s\n", infer_ms, repeat > 1 ? " (多次平均)" : "");
        if (low) {
            printf("[识别结果] %s   (置信度 %.3f 低于阈值 %.3f, 判为不可信)\n",
                   plate.c_str(), conf, min_score);
        } else {
            printf("[识别结果] %s\n", plate.c_str());
            printf("[置信度] %.3f\n", conf);
        }
    }

    rknn_destroy(ctx);
    return 0;
}

}  // namespace rk

int main(int argc, char** argv) { return rk::main(argc, argv); }
