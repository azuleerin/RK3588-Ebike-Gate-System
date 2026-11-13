// license_plate_rec.cc —— PP-OCRv4_mobile_rec 车牌识别实现
//
// 官方来源: rknn_model_zoo v2.5.0 examples/PPOCR/PPOCR-Rec/
//   · rknpu2/ppocr_rec.cc  -> 推理核心(模型加载 + 预处理 + rknn 跑)
//   · postprocess.cc       -> rec_postprocess(CTC 贪心解码)
//   · dict.h / ppocr_keys_v1.txt -> 字符表
//
// 本文件相对官方示例做的改动(以及为什么):
//   1) 剥离 DBNet(文本检测)相关的一切: 这里只保留识别链路。
//   2) 剥离演示代码: 文件读图、画框、printf 调试输出全部去掉, 改成返回值 + 日志。
//   3) 字典不再编译进二进制(dict.h 46KB 头文件), 改成运行时读 ppocr_keys_v1.txt,
//      顺便修掉官方 dict.h 里 '%' 被写成 "%%" 的转义残留。
//   4) 模型输入的 dtype / 布局 / 量化参数一律 rknn_query 现查, 不写死
//      (官方示例是固定 float32 + NHWC, 换量化模型就会踩坑)。
//   5) 新增 RGA 硬件预处理路径: 裁剪 + 缩放 + 颜色空间一次 improcess 完成。
#include "license_plate_rec.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include <opencv2/imgproc.hpp>

#include "im2d.h"
#include "rga.h"

namespace rk {
namespace {

// 官方 PP-OCR rec 的输入归一化(默认): (x - 127.5) / 127.5
constexpr float kNormMean = 127.5f;
constexpr float kNormStd  = 127.5f;

// 官方 PP-OCR rec 的时间步长度 = 输入宽度 / 8(兜底用; 正常从输出属性推)
constexpr int kSeqLenDiv = 8;

bool read_whole_file(const std::string& path, std::vector<uint8_t>* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }
    out->resize(static_cast<size_t>(sz));
    const size_t rd = fread(out->data(), 1, out->size(), f);
    fclose(f);
    return rd == out->size();
}

// 官方 resized_w 计算: 高度固定成 imgH, 宽度按原图比例算, 超过 imgW 就截断。
// (PaddleOCR 的 resize_norm_img 同一套逻辑)
inline int calc_resized_w(int src_w, int src_h, int img_h, int img_w) {
    if (src_h <= 0 || src_w <= 0) return img_w;
    const float ratio = static_cast<float>(src_w) / static_cast<float>(src_h);
    int resized_w = static_cast<int>(std::ceil(img_h * ratio));
    if (resized_w > img_w) resized_w = img_w;
    if (resized_w < 1) resized_w = 1;
    return resized_w;
}

}  // namespace

// ===========================================================================
// 字典
// ===========================================================================
bool LicensePlateRec::load_dict(const std::string& path, int expect_classes,
                                 std::vector<std::string>* classes) {
    if (!classes) return false;
    classes->clear();
    classes->push_back("blank");        // 类 0 = CTC 空白(官方约定)

    if (!path.empty()) {
        std::ifstream in(path);
        if (!in) {
            fprintf(stderr, "[PlateRec] 字典文件打不开: %s\n", path.c_str());
            return false;
        }
        std::string line;
        while (std::getline(in, line)) {
            // 去掉行尾 \r(Windows 换行的字典文件)
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            if (line.empty()) continue;
            classes->push_back(line);
        }
    }

    // 官方 dict.h = ['blank'] + ppocr_keys_v1.txt(6623 行) + [' '] = 6625 类
    // 字典文件缺尾巴时用空格补齐, 保证 CTC 的类号不会越界
    if (expect_classes > 0) {
        while (static_cast<int>(classes->size()) < expect_classes) classes->push_back(" ");
        if (static_cast<int>(classes->size()) > expect_classes) {
            fprintf(stderr, "[PlateRec] 字典有 %zu 类, 模型只要 %d 类, 截断尾部\n",
                    classes->size(), expect_classes);
            classes->resize(static_cast<size_t>(expect_classes));
        }
    }
    fprintf(stderr, "[PlateRec] 字典 %zu 类 (含 blank)\n", classes->size());
    return true;
}

// ===========================================================================
// 初始化
// ===========================================================================
bool LicensePlateRec::init(const PlateRecConfig& cfg) {
    destroy();
    cfg_ = cfg;

    // ---- 1) 加载模型 ----
    std::vector<uint8_t> model_data;
    if (!read_whole_file(cfg_.model_path, &model_data)) {
        fprintf(stderr, "[PlateRec] 模型文件读不了: %s\n", cfg_.model_path.c_str());
        return false;
    }
    int ret = rknn_init(&ctx_, model_data.data(),
                        static_cast<uint32_t>(model_data.size()),
                        RKNN_FLAG_PRIOR_HIGH, nullptr);
    if (ret < 0) {
        fprintf(stderr, "[PlateRec] rknn_init 失败: %d\n", ret);
        ctx_ = 0;
        return false;
    }

    // ---- 2) 查输入输出属性 ----
    rknn_input_output_num io_num;
    memset(&io_num, 0, sizeof(io_num));
    if (rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num)) != RKNN_SUCC) {
        fprintf(stderr, "[PlateRec] rknn_query(IN_OUT_NUM) 失败\n");
        destroy();
        return false;
    }

    rknn_tensor_attr ia;
    memset(&ia, 0, sizeof(ia));
    ia.index = 0;
    if (rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, &ia, sizeof(ia)) != RKNN_SUCC) {
        fprintf(stderr, "[PlateRec] rknn_query(INPUT_ATTR) 失败\n");
        destroy();
        return false;
    }
    rknn_tensor_attr oa;
    memset(&oa, 0, sizeof(oa));
    oa.index = 0;
    rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &oa, sizeof(oa));

    fprintf(stderr, "[PlateRec] 输入 name=%s type=%d fmt=%d dims=[", ia.name,
            static_cast<int>(ia.type), static_cast<int>(ia.fmt));
    for (uint32_t i = 0; i < ia.n_dims; ++i) fprintf(stderr, "%u,", ia.dims[i]);
    fprintf(stderr, "] n_elems=%u size=%u\n", ia.n_elems, ia.size);

    fprintf(stderr, "[PlateRec] 输出 name=%s type=%d fmt=%d dims=[", oa.name,
            static_cast<int>(oa.type), static_cast<int>(oa.fmt));
    for (uint32_t i = 0; i < oa.n_dims; ++i) fprintf(stderr, "%u,", oa.dims[i]);
    fprintf(stderr, "] n_elems=%u\n", oa.n_elems);

    // ---- 3) 输入尺寸(现查, 不写死) ----
    in_nchw_ = (ia.fmt == RKNN_TENSOR_NCHW);
    if (ia.n_dims >= 4) {
        if (in_nchw_) { in_c_ = static_cast<int>(ia.dims[1]);
                        in_h_ = static_cast<int>(ia.dims[2]);
                        in_w_ = static_cast<int>(ia.dims[3]); }
        else          { in_h_ = static_cast<int>(ia.dims[1]);
                        in_w_ = static_cast<int>(ia.dims[2]);
                        in_c_ = static_cast<int>(ia.dims[3]); }
    }
    if (in_w_ <= 0 || in_h_ <= 0) {
        fprintf(stderr, "[PlateRec] 模型输入尺寸异常: %dx%dx%d\n", in_c_, in_h_, in_w_);
        destroy();
        return false;
    }
    in_elems_ = static_cast<uint32_t>(in_w_ * in_h_ * in_c_);
    in_size_hint_ = ia.size;

    // ---- 4) 输出形状 [1, T, C] ----
    if (oa.n_dims >= 2) {
        out_T_ = static_cast<int>(oa.dims[oa.n_dims - 2]);
        out_C_ = static_cast<int>(oa.dims[oa.n_dims - 1]);
    }
    if (out_T_ <= 0) out_T_ = in_w_ / kSeqLenDiv;   // 官方兜底公式
    if (out_C_ <= 0) out_C_ = 6625;                 // 官方 MODEL_OUT_CHANNEL

    // ---- 5) 输入 dtype / 量化参数 ----
    in_type_  = static_cast<int>(ia.type);
    has_qnt_  = (ia.qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC) && (ia.scale > 0.f);
    in_scale_ = has_qnt_ ? ia.scale : 1.0f;
    in_zp_    = has_qnt_ ? ia.zp : 0;
    if (cfg_.prefer_int8 && in_type_ != RKNN_TENSOR_INT8 && has_qnt_) {
        fprintf(stderr, "[PlateRec] prefer_int8: 模型输入类型是 %d, 仍按 int8 码喂\n", in_type_);
    }

    // ---- 6) 字典 ----
    if (!load_dict(cfg_.dict_path, out_C_, &classes_)) {
        destroy();
        return false;
    }

    // ---- 7) 车牌字符白名单 ----
    if (!cfg_.allowed_charset.empty()) {
        allowed_.clear();
        for (int c = 1; c < static_cast<int>(classes_.size()); ++c) {
            const std::string& ch = classes_[static_cast<size_t>(c)];
            if (ch.size() == 1 && cfg_.allowed_charset.find(ch[0]) != std::string::npos) {
                allowed_.push_back(c);
            }
        }
        fprintf(stderr, "[PlateRec] 字符白名单: %zu 个类 (白名单为空字符的类会被排除)\n",
                allowed_.size());
        if (allowed_.empty()) {
            fprintf(stderr, "[PlateRec] ⚠️ 白名单与字典没有交集, 会识别不出任何字符\n");
        }
    }

    // ---- 8) 复用缓冲 ----
    f32_.assign(in_elems_, 0.f);
    i8_.assign(in_elems_, 0);
    u8_.assign(in_elems_, 0);
    roi_.assign(static_cast<size_t>(in_w_) * in_h_ * in_c_, 0);

    // ---- 9) 绑核(多路并发时, 一个实例固定一颗核, 避免互相抢) ----
    if (cfg_.npu_core_mask > 0) {
        const int r = rknn_set_core_mask(ctx_,
                                         static_cast<rknn_core_mask>(cfg_.npu_core_mask));
        fprintf(stderr, "[PlateRec] rknn_set_core_mask(0x%x) = %d\n", cfg_.npu_core_mask, r);
    }

    fprintf(stderr,
            "[PlateRec] 就绪: 输入 %dx%dx%d (%s, type=%d) 输出 T=%d C=%d 字典=%zu类 "
            "白名单=%zu 字符序=%s\n",
            in_c_, in_h_, in_w_, in_nchw_ ? "NCHW" : "NHWC", in_type_,
            out_T_, out_C_, classes_.size(), allowed_.size(),
            cfg_.swap_rb ? "RGB" : "BGR");
    return true;
}

void LicensePlateRec::destroy() {
    if (ctx_) {
        rknn_destroy(ctx_);
        ctx_ = 0;
    }
    classes_.clear();
    allowed_.clear();
    f32_.clear();
    i8_.clear();
    u8_.clear();
    roi_.clear();
}

// ===========================================================================
// CTC 贪心解码(逐字对齐官方 rec_postprocess)
// ===========================================================================
bool LicensePlateRec::ctc_greedy_decode(const float* logits, int T, int C,
                                         const std::vector<std::string>& classes,
                                         const std::vector<int>* allowed,
                                         PlateRecResult* out) {
    if (!logits || T <= 0 || C <= 0 || !out) return false;

    std::string text;
    float score_sum = 0.f;
    int count = 0;
    int last_index = 0;          // 上一个时间步的 argmax(官方语义: blank 会打断重复)

    for (int n = 0; n < T; ++n) {
        const float* row = logits + static_cast<size_t>(n) * C;

        int argmax_idx = 0;
        float max_value = 0.f;
        if (allowed && !allowed->empty()) {
            // 白名单模式: 只在允许的类里找最大值(车牌场景白名单很小, 反而更快)
            max_value = -1e30f;
            for (int c : *allowed) {
                if (c <= 0 || c >= C) continue;
                if (row[c] > max_value) { max_value = row[c]; argmax_idx = c; }
            }
            if (max_value < -1e29f) { argmax_idx = 0; max_value = 0.f; }
        } else {
            // 全量模式: 官方写法(max_element 两次, 这里合成一次少扫一遍)
            max_value = row[0];
            argmax_idx = 0;
            for (int c = 1; c < C; ++c) {
                if (row[c] > max_value) { max_value = row[c]; argmax_idx = c; }
            }
        }

        // 类 0 = blank, 不输出; 连续相同类只输出一次
        if (argmax_idx > 0 && !(n > 0 && argmax_idx == last_index)) {
            score_sum += max_value;
            count += 1;
            if (argmax_idx < static_cast<int>(classes.size())) {
                text += classes[static_cast<size_t>(argmax_idx)];
            }
        }
        last_index = argmax_idx;
    }

    float score = score_sum / (count + 1e-6f);
    if (count == 0 || std::isnan(score)) score = 0.f;

    out->text = text;
    out->score = score;
    out->mean_score = score;
    out->char_count = count;
    return true;
}

// ===========================================================================
// 输入张量构造: 8bit ROI -> 模型输入
//   · float32: (x - 127.5) / 127.5   (官方归一化)
//   · int8   : q = clamp(round((x/255) / scale) + zp, -128, 127)
//   · uint8  : 原样
// 宽度不足的部分补 0(官方 copyMakeBorder(BORDER_CONSTANT, 0) 的等价物)
//
// ★ 性能敏感点: 这是每帧都要跑的一遍, 遍历的元素数 = W*H*C(48*320*3 = 46080),
//   所以用一次性遍历 + 预分配缓冲, 别在循环里做分支或分配。
// ===========================================================================
void LicensePlateRec::build_input_tensor(const uint8_t* roi, int roi_w) {
    const int W = in_w_, H = in_h_, C = in_c_;
    const size_t plane = static_cast<size_t>(W) * H;

    if (in_type_ == RKNN_TENSOR_FLOAT32) {
        // 归一化 + 右侧补 0
        //   性能敏感点: 这里是最内层循环, 46k 次/帧, 所以把模式判断提到循环外。
        const bool div255 = (cfg_.norm_mode == PlateNormMode::kDiv255);
        const bool baked = cfg_.norm_baked_in;
        const float inv_std = 1.0f / kNormStd;
        for (int y = 0; y < H; ++y) {
            const uint8_t* src = roi + static_cast<size_t>(y) * roi_w * C;
            float* dst = f32_.data() + static_cast<size_t>(y) * W * C;
            const int n_valid = std::min(roi_w, W) * C;
            if (baked) {
                // 归一化已经烤进模型图里了 -> 喂原始 [0,255], 别再除一次
                for (int i = 0; i < n_valid; ++i) dst[i] = static_cast<float>(src[i]);
            } else if (div255) {
                for (int i = 0; i < n_valid; ++i) {
                    dst[i] = static_cast<float>(src[i]) * (1.0f / 255.0f);
                }
            } else {
                for (int i = 0; i < n_valid; ++i) {
                    dst[i] = (static_cast<float>(src[i]) - kNormMean) * inv_std;
                }
            }
            for (int i = n_valid; i < W * C; ++i) dst[i] = 0.f;
        }
        if (in_nchw_) {
            // NCHW: 需要把 NHWC 的像素转成 CHW 的平面
            static thread_local std::vector<float> tmp;
            tmp.resize(plane * C);
            for (int y = 0; y < H; ++y) {
                const float* src = f32_.data() + static_cast<size_t>(y) * W * C;
                for (int x = 0; x < W; ++x) {
                    for (int c = 0; c < C; ++c) {
                        tmp[static_cast<size_t>(c) * plane + static_cast<size_t>(y) * W + x] =
                            src[static_cast<size_t>(x) * C + c];
                    }
                }
            }
            f32_.swap(tmp);
        }
    } else if (in_type_ == RKNN_TENSOR_INT8 && !cfg_.norm_baked_in) {
        // 量化码: q = clamp(round((x/255) / scale) + zp)
        // 补零区填 zp(量化后的 0), 不是 0
        const float inv_scale = 1.0f / in_scale_;
        const int pad_q = in_zp_;
        for (int y = 0; y < H; ++y) {
            const uint8_t* src = roi + static_cast<size_t>(y) * roi_w * C;
            int8_t* dst = i8_.data() + static_cast<size_t>(y) * W * C;
            const int n_valid = std::min(roi_w, W) * C;
            for (int i = 0; i < n_valid; ++i) {
                const float real = static_cast<float>(src[i]) * (1.0f / 255.0f);
                int q = static_cast<int>(std::lround(real * inv_scale)) + in_zp_;
                if (q > 127) q = 127;
                if (q < -128) q = -128;
                dst[i] = static_cast<int8_t>(q);
            }
            for (int i = n_valid; i < W * C; ++i) dst[i] = static_cast<int8_t>(pad_q);
        }
        if (in_nchw_) {
            static thread_local std::vector<int8_t> tmp;
            tmp.resize(plane * C);
            for (int y = 0; y < H; ++y) {
                const int8_t* src = i8_.data() + static_cast<size_t>(y) * W * C;
                for (int x = 0; x < W; ++x) {
                    for (int c = 0; c < C; ++c) {
                        tmp[static_cast<size_t>(c) * plane + static_cast<size_t>(y) * W + x] =
                            src[static_cast<size_t>(x) * C + c];
                    }
                }
            }
            i8_.swap(tmp);
        }
    } else {
        // UINT8: 原样喂
        for (int y = 0; y < H; ++y) {
            const uint8_t* src = roi + static_cast<size_t>(y) * roi_w * C;
            uint8_t* dst = u8_.data() + static_cast<size_t>(y) * W * C;
            const int n_valid = std::min(roi_w, W) * C;
            memcpy(dst, src, static_cast<size_t>(n_valid));
            memset(dst + n_valid, 0, static_cast<size_t>(W * C - n_valid));
        }
        if (in_nchw_) {
            static thread_local std::vector<uint8_t> tmp;
            tmp.resize(plane * C);
            for (int y = 0; y < H; ++y) {
                const uint8_t* src = u8_.data() + static_cast<size_t>(y) * W * C;
                for (int x = 0; x < W; ++x) {
                    for (int c = 0; c < C; ++c) {
                        tmp[static_cast<size_t>(c) * plane + static_cast<size_t>(y) * W + x] =
                            src[static_cast<size_t>(x) * C + c];
                    }
                }
            }
            u8_.swap(tmp);
        }
    }
}

// ===========================================================================
// RKNN 推理
// ===========================================================================
bool LicensePlateRec::run_rknn(PlateRecResult* out) {
    rknn_input inputs[1];
    memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].pass_through = 0;    // 0 = 让驱动按模型要求做格式/量化转换

    // ★ size 必须是"元素数 × 你声明的类型的字节数", 给小了 runtime 会按它自己
    //   的尺寸去读写我们的缓冲 -> 越界踩堆(工程里踩过, 见 lprnet.h 的注释)。
    if (in_type_ == RKNN_TENSOR_FLOAT32) {
        inputs[0].buf = f32_.data();
        inputs[0].size = in_elems_ * static_cast<uint32_t>(sizeof(float));
        inputs[0].type = RKNN_TENSOR_FLOAT32;
    } else if (in_type_ == RKNN_TENSOR_INT8 && !cfg_.norm_baked_in) {
        inputs[0].buf = i8_.data();
        inputs[0].size = in_elems_;
        inputs[0].type = RKNN_TENSOR_INT8;
    } else {
        // ★ 量化模型 + 归一化已固化 -> 喂原始 uint8 像素, 由运行时自己量化。
        //   这和工程里 main.cc 的 kInUint8Raw 一致(src/main.cc:1490-1524)。
        //   ★ 千万别喂 type=INT8 + 原始字节: 运行时会把它们当成量化码 -> 输出全乱。
        inputs[0].buf = u8_.data();
        inputs[0].size = in_elems_;
        inputs[0].type = RKNN_TENSOR_UINT8;
    }
    inputs[0].fmt = in_nchw_ ? RKNN_TENSOR_NCHW : RKNN_TENSOR_NHWC;

    int ret = rknn_inputs_set(ctx_, 1, inputs);
    if (ret < 0) {
        fprintf(stderr, "[PlateRec] rknn_inputs_set 失败 %d (type=%d fmt=%d size=%u, "
                        "模型要 %u)\n",
                ret, inputs[0].type, inputs[0].fmt, inputs[0].size, in_size_hint_);
        return false;
    }

    ret = rknn_run(ctx_, nullptr);
    if (ret < 0) { fprintf(stderr, "[PlateRec] rknn_run 失败 %d\n", ret); return false; }

    rknn_output outputs[1];
    memset(outputs, 0, sizeof(outputs));
    outputs[0].want_float = 1;      // 让驱动把量化输出反量化成 float32(softmax 概率)
    outputs[0].is_prealloc = 0;
    outputs[0].index = 0;
    ret = rknn_outputs_get(ctx_, 1, outputs, nullptr);
    if (ret < 0) { fprintf(stderr, "[PlateRec] rknn_outputs_get 失败 %d\n", ret); return false; }

    const float* logits = static_cast<const float*>(outputs[0].buf);
    const bool ok = ctc_greedy_decode(logits, out_T_, out_C_, classes_,
                                       allowed_.empty() ? nullptr : &allowed_, out);

    rknn_outputs_release(ctx_, 1, outputs);
    return ok;
}

// ===========================================================================
// 模式一: 已裁剪的 BGR 图 + CPU 预处理
//   严格照官方 ppocr_rec.cc 的做法:
//     ratio = w/h;  resized_w = min(ceil(imgH*ratio), imgW)
//     resize -> float32 -> (x-127.5)/127.5 -> 右侧补 0 到 imgW
// ===========================================================================
bool LicensePlateRec::recognize_bgr(const cv::Mat& bgr, PlateRecResult* out) {
    if (!ctx_ || !out) return false;
    if (bgr.empty() || bgr.type() != CV_8UC3) {
        fprintf(stderr, "[PlateRec] recognize_bgr: 需要 CV_8UC3 的 BGR 图\n");
        return false;
    }

    const int imgW = in_w_, imgH = in_h_, C = in_c_;
    const int resized_w = calc_resized_w(bgr.cols, bgr.rows, imgH, imgW);

    // ---- 缩放 ----
    cv::Mat small;
    cv::resize(bgr, small, cv::Size(resized_w, imgH), 0, 0, cv::INTER_LINEAR);

    // ---- 通道序: 默认 BGR(与官方示例一致); swap_rb=true 时转 RGB ----
    if (cfg_.swap_rb) {
        cv::cvtColor(small, small, cv::COLOR_BGR2RGB);
    }

    // ---- 摆进 ROI 缓冲(右侧留白, 由 build_input_tensor 补零) ----
    roi_.assign(static_cast<size_t>(resized_w) * imgH * C, 0);
    const size_t row_bytes = static_cast<size_t>(resized_w) * C;
    for (int y = 0; y < imgH; ++y) {
        memcpy(roi_.data() + static_cast<size_t>(y) * row_bytes,
               small.ptr(y), row_bytes);
    }

    build_input_tensor(roi_.data(), resized_w);
    return run_rknn(out);
}

// ===========================================================================
// 模式二: NV12 帧 + 框 + RGA 硬件预处理
//
// ★ 零拷贝的含义(说清楚免得误会):
//   · 1080P 的原始帧【全程不进 CPU】: RGA 直接从 dma-buf 读, 裁剪/缩放/颜色
//     转换都在硬件里完成, 写出一张 320x48 的小图。
//   · CPU 只做最后一遍"归一化 + 补零", 最多 320*48*3 = 46KB, 几十微秒。
//   · 如果模型输入是 int8 量化张量, 这一步就是量化码生成, 同样是几十微秒。
//
// ★ 前提: 源必须是【真 dma-buf】(fd >= 0)。
//   用虚拟地址(vir_addr) 走 RGA 会落到 librga 的软件 MMU, 本板子上实测会
//   "RGA_MMU unsupported memory larger than 4G" 并写坏堆 —— 所以这里直接
//   退回 CPU 路径, 不冒险。
// ===========================================================================
bool LicensePlateRec::preprocess_nv12_rga(const rk::RgaImage& frame, int x, int y, int w, int h,
                                          uint8_t* dst, int dst_capacity, int* out_w,
                                          int* out_h) {
    if (!dst || dst_capacity <= 0) return false;
    if (frame.width <= 0 || frame.height <= 0) return false;

    // ---- 夹框 ----
    int sx = x, sy = y, sw = w, sh = h;
    if (sx < 0) { sw += sx; sx = 0; }
    if (sy < 0) { sh += sy; sy = 0; }
    if (sx + sw > frame.width) sw = frame.width - sx;
    if (sy + sh > frame.height) sh = frame.height - sy;
    if (sw <= 0 || sh <= 0) return false;

    const int imgH = in_h_, imgW = in_w_;
    const int resized_w = calc_resized_w(sw, sh, imgH, imgW);
    // ★ 目标缓冲要留【余量行】: RGA 在非整数倍缩放时可能多写 1~2 行,
    //   写出去就踩坏相邻堆对象(与 main.cc 的 kRgaDstPadRows 同一约定)。
    //   注意这里算的是【字节】: RGB888 一行 = resized_w 像素 x 3 字节。
    constexpr int kPadRows = 16;
    const size_t need_bytes =
        static_cast<size_t>(resized_w) * static_cast<size_t>(imgH + kPadRows) * 3;
    if (need_bytes > static_cast<size_t>(dst_capacity)) return false;

    // ---- RGA: 裁剪 + 缩放 + 颜色空间 一次 improcess 完成 ----
    rga_buffer_t src;
    memset(&src, 0, sizeof(src));
    src.fd = frame.fd;
    // ★ 有 fd 就【不要】再填 vir_addr: librga 在"软件 MMU"路径下会用这个虚拟
    //   地址自己映射物理地址, 而这个 VA 背后的页随时可能被换掉(PPA 错配),
    //   RGA 就会把输出写进别的分配 —— rga_convert.cc 的规则与此一致。
    if (frame.fd < 0) src.vir_addr = frame.vir_addr;
    src.width = frame.width;
    src.height = frame.height;
    // librga 的 wstride 单位是【像素】, 而 RgaImage 的 wstride 是【字节】。
    // 这里是 NV12(luma 1 字节/像素), 两者数值相同 —— 但显式换算, 免得
    // 以后把源换成 RGB 又踩同一个坑(wstride 单位错误是 P0-4 的根因)。
    src.wstride = (frame.format == RK_FORMAT_YCbCr_420_SP)
                      ? frame.wstride          // NV12: 1 字节/像素
                      : (frame.wstride / 3);   // RGB888/BGR888: 3 字节/像素
    src.hstride = frame.hstride;
    src.format = frame.format;                 // RK_FORMAT_YCbCr_420_SP

    rga_buffer_t d;
    memset(&d, 0, sizeof(d));
    d.fd = -1;
    d.vir_addr = dst;
    d.width = resized_w;
    d.height = imgH;
    // ★ librga 的 wstride 单位是【像素】, 不是字节!
    //   原来写的是 resized_w * 3, 行距因此大了 3 倍, RGA 按 5760 字节/行去写,
    //   实际缓冲只有 resized_w*3 字节/行 -> 越界 2,457,600 字节,
    //   直接踩烂相邻堆对象(实测首个越界字节在偏移 1,232,640 = 第 214 行起点)。
    //   这就是 P0-4, 现在改对。
    d.wstride = resized_w;
    d.hstride = imgH;
    d.format = cfg_.swap_rb ? RK_FORMAT_RGB_888 : RK_FORMAT_BGR_888;

    rga_buffer_t pat;
    memset(&pat, 0, sizeof(pat));

    im_rect srect;
    srect.x = sx; srect.y = sy; srect.width = sw; srect.height = sh;
    im_rect drect;
    drect.x = 0; drect.y = 0; drect.width = resized_w; drect.height = imgH;
    im_rect prect;
    prect.x = 0; prect.y = 0; prect.width = 0; prect.height = 0;

    // 官方工程里的同一个重载: improcess(src, dst, pat, srect, drect, prect,
    //                                  acquire_fence_fd, release_fence_ptr, opt, usage)
    const IM_STATUS st = improcess(src, d, pat, srect, drect, prect,
                                   -1, nullptr, nullptr, 0);
    if (st != IM_STATUS_NOERROR && st != IM_STATUS_SUCCESS) {
        fprintf(stderr, "[PlateRec] RGA 预处理失败: %s\n", imStrError(st));
        return false;
    }
    if (out_w) *out_w = resized_w;
    if (out_h) *out_h = imgH;
    return true;
}

bool LicensePlateRec::recognize_nv12(const rk::RgaImage& frame, int x, int y, int w, int h,
                                     PlateRecResult* out) {
    if (!ctx_ || !out) return false;

    // 没有真 dma-buf 就不走 RGA(软件 MMU 在本板子上会写坏堆)
    if (frame.fd < 0) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            fprintf(stderr, "[PlateRec] 源不是 dma-buf(fd<0), RGA 路径不可用, "
                            "请走 recognize_bgr()\n");
        }
        return false;
    }

    const int imgH = in_h_, imgW = in_w_;
    int sx = x, sy = y, sw = w, sh = h;
    if (sx < 0) { sw += sx; sx = 0; }
    if (sy < 0) { sh += sy; sy = 0; }
    if (sx + sw > frame.width) sw = frame.width - sx;
    if (sy + sh > frame.height) sh = frame.height - sy;
    if (sw <= 0 || sh <= 0) return false;

    const int resized_w = calc_resized_w(sw, sh, imgH, imgW);
    // 目标缓冲留余量行(RGA 非整数倍缩放可能多写 1~2 行)
    constexpr int kPadRows = 16;
    const size_t roi_need =
        static_cast<size_t>(resized_w) * static_cast<size_t>(imgH + kPadRows) * 3;
    if (roi_.size() < roi_need) {
        roi_.assign(roi_need, 0);
    }

    int got_w = 0, got_h = 0;
    if (!preprocess_nv12_rga(frame, x, y, w, h, roi_.data(),
                             static_cast<int>(roi_.size()), &got_w, &got_h)) {
        return false;
    }

    build_input_tensor(roi_.data(), got_w);
    return run_rknn(out);
}

}  // namespace rk
