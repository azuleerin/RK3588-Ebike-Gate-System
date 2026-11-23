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
#include "rk/core_mask.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>

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

// RGA 的 BGR888/RGB888 目标行距必须 16 字节对齐。
// 行距(字节) = 行宽(像素) * 3, 而 3 和 16 互质 -> 行宽必须是 16 的倍数。
// PP-OCR 的目标宽度是按比例算出来的(93/122/157... 这种), 基本都不是 16 的倍数,
// 所以这里把行宽【向上】对齐到 16 的倍数, 多出来的那几个像素当行距填充。
inline int align_up_16(int x) { return (x + 15) & ~15; }

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

    // ---- 4b) 输出张量的【原生类型】与量化参数 ----
    //
    // ★★ 2026-11-20 新增: PP-OCRv4 rec 的输出原生是 fp16(1x40x6625)。
    //   以前 want_float=1 让驱动把它展成 float32 —— 每次识别多写 530 KB,
    //   实测 4.94 ms, 而 CTC 贪心解码真正用到的只有 40 个元素的值。
    //   现在直读原生类型: 缓冲小一半, 驱动也不用做全量反量化。
    //
    //   只对 FLOAT16 / FLOAT32 走"直读"这条路:
    //     · FLOAT16 -> 自己按 IEEE 精确转换(见 ctc_greedy_decode_f16)
    //     · FLOAT32 -> 本来就是 float32, 直读零成本
    //   INT8 / UINT8 保持 want_float=1(让驱动按它自己的量化公式转,
    //   免得我们复刻公式复刻错了)。
    out_type_  = static_cast<int>(oa.type);
    out_qnt_   = (oa.qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC) && (oa.scale > 0.f);
    out_zp_    = out_qnt_ ? oa.zp : 0;
    out_scale_ = out_qnt_ ? oa.scale : 1.0f;
    size_t bpe = 0;
    switch (oa.type) {
        case RKNN_TENSOR_FLOAT32: bpe = 4; break;
        case RKNN_TENSOR_FLOAT16: bpe = 2; break;
        default: bpe = 0; break;      // int8/uint8/未知 -> 走 want_float=1 老路
    }
    out_native_ = (bpe != 0);
    out_bytes_  = out_native_
                 ? (static_cast<size_t>(out_T_) * static_cast<size_t>(out_C_) * bpe)
                 : 0;

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
    //
    // ★★★ 两个必须记住的坑 ★★★
    //
    // (1) 必须把类 0(blank) 也放进白名单
    //   CTC 靠 blank 分隔字符, 而且 blank 时间步占【大多数】(40 个时间步里
    //   真正出字符的只有 7~8 个)。白名单模式是在【每个时间步】里取 argmax,
    //   如果把 blank 排除在外, 模型想吐 blank 的时间步会被迫挑一个白名单
    //   字符出来 -> 识别结果变成 "LECTNACTCLN" 这种一长串垃圾(实测:
    //   真牌 "苏A" 被读成 10 个字, 置信度 0.01, 而关掉白名单时是 "苏A" 0.51)。
    //   所以白名单只该限制【字符类】, 不能限制 blank。
    //
    // (2) 不能用 ch.size() == 1 判断"是不是一个字符"
    //   那是 ASCII 的写法。汉字在 UTF-8 里是【3 个字节】, size()==3,
    //   于是所有省份汉字都被过滤掉了 —— 白名单里明明写了 "苏", 结果永远
    //   读不出 "苏"(实测: 白名单开着读 "A", 关掉读 "苏A")。
    //   正确做法: 按 UTF-8 码点切开白名单, 再按【整串】比较。
    if (!cfg_.allowed_charset.empty()) {
        // 白名单按 UTF-8 码点切开(ASCII 1 字节 / 汉字 3 字节 / 生僻字 4 字节)
        std::set<std::string> want;
        for (size_t i = 0; i < cfg_.allowed_charset.size();) {
            const unsigned char c = static_cast<unsigned char>(cfg_.allowed_charset[i]);
            size_t len = 1;
            if (c >= 0xF0)      len = 4;
            else if (c >= 0xE0) len = 3;
            else if (c >= 0xC0) len = 2;
            if (i + len > cfg_.allowed_charset.size()) len = 1;   // 尾巴被截断, 当 1 字节
            want.insert(cfg_.allowed_charset.substr(i, len));
            i += len;
        }

        allowed_.clear();
        allowed_.push_back(0);              // 类 0 = CTC blank, 永远放行
        int n_char = 0;
        int n_cjk = 0;
        for (int c = 1; c < static_cast<int>(classes_.size()); ++c) {
            const std::string& ch = classes_[static_cast<size_t>(c)];
            if (want.find(ch) != want.end()) {
                allowed_.push_back(c);
                ++n_char;
                if (ch.size() > 1) ++n_cjk;      // 多字节 = 汉字/符号
            }
        }
        fprintf(stderr, "[PlateRec] 字符白名单: %d 个字符类 (%d 个多字节/汉字) + blank "
                        "(共 %zu 类)\n",
                n_char, n_cjk, allowed_.size());
        if (n_char == 0) {
            fprintf(stderr, "[PlateRec] ⚠️ 白名单与字典没有交集, 会识别不出任何字符\n");
        }
        if (n_cjk == 0 && want.size() > 34) {
            fprintf(stderr, "[PlateRec] ⚠️ 白名单里有汉字但一个都没匹配上 —— "
                            "检查字典是不是 UTF-8\n");
        }
    }

    // ---- 8) 复用缓冲 ----
    f32_.assign(in_elems_, 0.f);
    i8_.assign(in_elems_, 0);
    u8_.assign(in_elems_, 0);
    roi_.assign(static_cast<size_t>(in_w_) * in_h_ * in_c_, 0);
    // 输出缓冲: 按【原生类型】预分配(见 4b)。启动时一次开好, 推理期零分配。
    // ★ 只开【真正要用】的那一个: 直读原生类型时 out_f32_ 用不上,
    //   两边都开就是白白多占 1,060,000 字节/实例 x 3 个识别 worker = 3.2 MB。
    if (out_native_) {
        out_raw_.assign(out_bytes_, 0);
        out_f32_.clear();
    } else {
        out_f32_.assign(static_cast<size_t>(out_T_) * static_cast<size_t>(out_C_), 0.f);
        out_raw_.clear();
    }

    // ---- 9) 绑核(多路并发时, 一个实例固定一颗核, 避免互相抢) ----
    if (cfg_.npu_core_mask > 0) {
        // ★ 0x7(RKNN_NPU_CORE_0_1_2)禁用 —— 在【驱动边界】上挡住。
        //   单流版 main.cc 会把 --ocr-core 原样传进来, 那里可以传 0,1,2。
        const int safe_mask = rk::sanitize_core_mask(cfg_.npu_core_mask, "PlateRec 识别上下文");
        const int r = rknn_set_core_mask(ctx_,
                                         static_cast<rknn_core_mask>(safe_mask));
        fprintf(stderr, "[PlateRec] rknn_set_core_mask(0x%x) = %d\n", safe_mask, r);
    }

    fprintf(stderr,
            "[PlateRec] 就绪: 输入 %dx%dx%d (%s, type=%d) 输出 T=%d C=%d 字典=%zu类 "
            "白名单=%zu 字符序=%s\n",
            in_c_, in_h_, in_w_, in_nchw_ ? "NCHW" : "NHWC", in_type_,
            out_T_, out_C_, classes_.size(), allowed_.size(),
            cfg_.swap_rb ? "RGB" : "BGR");
    // ★ 输出取数方式(2026-11-20): 直读原生类型 vs 让驱动转 float32
    if (out_native_) {
        fprintf(stderr, "[PlateRec] 输出取数: 直读原生类型(type=%s, 缓冲 %zu 字节) —— "
                        "省掉驱动侧的全量反量化\n",
                out_type_ == RKNN_TENSOR_FLOAT16 ? "FP16" : "FP32", out_bytes_);
    } else {
        fprintf(stderr, "[PlateRec] 输出取数: want_float=1(驱动转 float32, 缓冲 %zu 字节)\n",
                out_elems_bytes());
    }
    return true;
}

size_t LicensePlateRec::out_elems_bytes() const {
    return static_cast<size_t>(out_T_) * static_cast<size_t>(out_C_) * sizeof(float);
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
    out_raw_.clear();
    out_f32_.clear();
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
            // ★ 类 0(blank) 必须在白名单里参与竞争: CTC 靠 blank 分隔字符,
            //   而且 blank 时间步占大多数。把 blank 挡在外面 -> 模型想吐 blank
            //   的时间步被逼吐一个白名单字符 -> 结果变成 "LECTNACTCLN" 这种
            //   长串垃圾(实测真牌 "苏A" -> 10 个字, 置信度 0.01)。
            //   白名单只该限制【字符类】, 不能限制 blank。
            max_value = -1e30f;
            for (int c : *allowed) {
                if (c < 0 || c >= C) continue;
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
// fp16 直读版 CTC 贪心解码 (NEON 向量化) —— 2026-11-20 新增
//
// ★ 为什么要有这一份
//   PP-OCRv4 rec 的输出是 [1, 40, 6625] 的【fp16】张量(26.5 万元素)。
//   以前 want_float=1 让驱动把它展成 float32: 每次识别多写 530 KB,
//   实测光这一下就要 4.94 ms —— 而 CTC 贪心解码真正用到的只有
//   【每个时间步的 argmax 和它的值】, 26.5 万个元素里只读了 40 个。
//
// ★ 数值等价性(不是"看起来差不多", 是穷举证明)
//   IEEE 754 的 fp16 -> fp32 是【精确】转换(fp32 能无损表示任何 fp16 值),
//   所以:
//     · argmax 的下标: 完全相同(fp16 转 fp32 单调, 严格保序)
//     · argmax 的取值: 逐位相同(fp16 -> fp32 无损)
//   穷举验证: 120 帧 x 265000 元素 = 3180 万个元素逐位比较, 0 差异
//   (工具: opt_audit/tools/fp16_equiv.cc)
//
// ★ 语义严格对齐原版(license_plate_rec.cc 的 ctc_greedy_decode)
//   原版是【严格大于才更新】, 所以并列时取【第一个】最大值。
//   这里的做法: 先向量化求出全局最大值, 再找【第一个】等于它的下标 —— 等价。
//   (原版的 running max 单调不减, 第一次达到全局最大值的地方就是第一次出现)
// ===========================================================================
#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#endif

// fp16 -> fp32 的精确转换(IEEE 754: fp32 能无损表示任何 fp16 值)
// ★ 只用【基线 ARMv8 就有】的指令: vdupq_n_u32 + vreinterpretq + vcvt_f32_f16。
//   (vdupq_f16 / vceqq_f16 属于 FP16 算术扩展, 本工程的编译基线没有, 不能用)
static inline float fp16_to_f32(uint16_t h) {
#if defined(__aarch64__) || defined(__ARM_NEON)
    const float16x8_t v8 = vreinterpretq_f16_u32(vdupq_n_u32(h));
    const float16x4_t v = vget_low_f16(v8);
    return vgetq_lane_f32(vcvt_f32_f16(v), 0);
#else
    // 纯软件兜底(仅供非 ARM 平台编译时用)
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    const uint32_t exp  = (h >> 10) & 0x1Fu;
    const uint32_t man  = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (man == 0) bits = sign;
        else {   // 次正规数
            int e = -1;
            uint32_t m = man;
            do { m <<= 1; ++e; } while (!(m & 0x400u));
            bits = sign | ((uint32_t)(127 - 15 - e) << 23) | ((m & 0x3FFu) << 13);
        }
    } else if (exp == 0x1Fu) {
        bits = sign | 0x7F800000u | (man << 13);
    } else {
        bits = sign | ((exp + (127 - 15)) << 23) | (man << 13);
    }
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
#endif
}

bool LicensePlateRec::ctc_greedy_decode_f16(const uint16_t* logits, int T, int C,
                                            const std::vector<std::string>& classes,
                                            const std::vector<int>* allowed,
                                            PlateRecResult* out) {
    if (!logits || T <= 0 || C <= 0 || !out) return false;

    std::string text;
    float score_sum = 0.f;
    int count = 0;
    int last_index = 0;          // 上一个时间步的 argmax(官方语义: blank 会打断重复)

    for (int n = 0; n < T; ++n) {
        const uint16_t* row = logits + static_cast<size_t>(n) * C;
        int   argmax_idx = 0;
        float max_value = 0.f;

        if (allowed && !allowed->empty()) {
            // ---- 白名单模式: 类很少(几十个), 标量就够快 ----
            //   严格复刻原版: max_value 起始 -1e30, 严格大于才更新。
            max_value = -1e30f;
            for (int c : *allowed) {
                if (c < 0 || c >= C) continue;
                const float v = fp16_to_f32(row[c]);
                if (v > max_value) { max_value = v; argmax_idx = c; }
            }
            if (max_value < -1e29f) { argmax_idx = 0; max_value = 0.f; }
        } else {
#if defined(__aarch64__) || defined(__ARM_NEON)
            // ---- 全量模式: 6625 类 x 40 步 = 26.5 万次比较 ----
            //   原版是串行的 running max, 每次迭代都依赖上一轮 -> 延迟受限。
            //   这里改成 8 路并行求最大值, 再找第一个等于它的下标。
            float best = -3.0e38f;
            int c = 0;
            for (; c + 8 <= C; c += 8) {
                const float16x8_t h = vld1q_f16(reinterpret_cast<const __fp16*>(row + c));
                const float32x4_t lo = vcvt_f32_f16(vget_low_f16(h));
                const float32x4_t hi = vcvt_f32_f16(vget_high_f16(h));
                const float32x4_t mx = vmaxq_f32(vmaxq_f32(lo, hi), vdupq_n_f32(best));
                best = vmaxvq_f32(mx);
            }
            for (; c < C; ++c) {
                const float v = fp16_to_f32(row[c]);
                if (v > best) best = v;
            }
            if (std::isnan(best)) {
                // ★ 这一行里有 NaN: 退回标量路径, 和原版逐位一致。
                //   原因: ARM 的 FMAX(vmaxq_f32)是【NaN 传播】语义 —— 只要有一个
                //   NaN, 结果就是 NaN; 而原版的 running max 是"严格大于才更新",
                //   NaN 会被【跳过】(只有 row[0] 是 NaN 时结果才是 NaN)。
                //   两者语义不同, 所以这里老老实实退回标量。
                //   (真实模型输出是 softmax, 不可能有 NaN; 这是纯防御。)
                best = fp16_to_f32(row[0]);
                argmax_idx = 0;
                for (int i = 1; i < C; ++i) {
                    const float v = fp16_to_f32(row[i]);
                    if (v > best) { best = v; argmax_idx = i; }
                }
                max_value = best;
            } else {
                // 找【第一个】等于最大值的下标(向量化)
                //   做法: 每个 lane 存"自己的序号"(0/1/2/3), 相等就保留序号,
                //   不等就换成 0xFFFFFFFF; 再用 vminvq_u32 取最小序号 ->
                //   就是【第一个】命中的 lane。
                //   (不能用 vmvnq + ctz: 取反后最小值只能说明"有没有命中",
                //    命中的位置信息已经丢了。)
                const float32x4_t target = vdupq_n_f32(best);
                const uint32x4_t lane_idx = {0u, 1u, 2u, 3u};
                const uint32x4_t kNoHit = vdupq_n_u32(0xFFFFFFFFu);
                int found = -1;
                c = 0;
                for (; c + 8 <= C; c += 8) {
                    const float16x8_t h = vld1q_f16(reinterpret_cast<const __fp16*>(row + c));
                    const float32x4_t lo = vcvt_f32_f16(vget_low_f16(h));
                    const float32x4_t hi = vcvt_f32_f16(vget_high_f16(h));
                    const uint32_t m_lo = vminvq_u32(vbslq_u32(vceqq_f32(lo, target), lane_idx, kNoHit));
                    if (m_lo != 0xFFFFFFFFu) { found = c + (int)m_lo; break; }
                    const uint32_t m_hi = vminvq_u32(vbslq_u32(vceqq_f32(hi, target), lane_idx, kNoHit));
                    if (m_hi != 0xFFFFFFFFu) { found = c + 4 + (int)m_hi; break; }
                }
                if (found < 0) {
                    for (int i = 0; i < C; ++i)
                        if (fp16_to_f32(row[i]) == best) { found = i; break; }
                }
                argmax_idx = (found >= 0) ? found : 0;
                max_value = best;
            }
#else
            // 没有 NEON 的平台: 标量兜底, 语义与原版一致
            max_value = fp16_to_f32(row[0]);
            argmax_idx = 0;
            for (int i = 1; i < C; ++i) {
                const float v = fp16_to_f32(row[i]);
                if (v > max_value) { max_value = v; argmax_idx = i; }
            }
#endif
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
// ★ roi_pitch_bytes: 相邻两行的字节距离。RGA 写出来的行距是 16 字节对齐的
//   (align16(dst_w) * 3), 比"行宽 * 3"大一点 —— 行尾那几个填充字节必须跳过去,
//   否则每一行都会往右偏一点, 整行字是斜的。
//
// ★ 性能敏感点: 这是每帧都要跑的一遍, 遍历的元素数 = W*H*C(48*320*3 = 46080),
//   所以用一次性遍历 + 预分配缓冲, 别在循环里做分支或分配。
// ===========================================================================
void LicensePlateRec::build_input_tensor(const uint8_t* roi, int roi_w, int roi_pitch_bytes) {
    const int W = in_w_, H = in_h_, C = in_c_;
    const size_t plane = static_cast<size_t>(W) * H;
    if (roi_pitch_bytes <= 0) roi_pitch_bytes = roi_w * C;

    if (in_type_ == RKNN_TENSOR_FLOAT32) {
        // 归一化 + 右侧补 0
        //   性能敏感点: 这里是最内层循环, 46k 次/帧, 所以把模式判断提到循环外。
        const bool div255 = (cfg_.norm_mode == PlateNormMode::kDiv255);
        const bool baked = cfg_.norm_baked_in;
        const float inv_std = 1.0f / kNormStd;
        for (int y = 0; y < H; ++y) {
            const uint8_t* src = roi + static_cast<size_t>(y) * roi_pitch_bytes;
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
            const uint8_t* src = roi + static_cast<size_t>(y) * roi_pitch_bytes;
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
            const uint8_t* src = roi + static_cast<size_t>(y) * roi_pitch_bytes;
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
    // ★★ 2026-11-20: 输出缓冲按【原生类型】走, 不再让驱动展成 float32。
    //   fp16 模型(在用): want_float=0 -> 驱动直接写 530,000 字节的 fp16,
    //     省掉一次 265000 元素的反量化 + 530 KB 的额外写入(实测 4.94 ms/次)。
    //   int8/未知类型: 退回 want_float=1, 让驱动按它自己的量化公式转
    //     (不自己复刻量化公式 —— 复刻错了就是静默的识别错误)。
    const size_t out_elems = static_cast<size_t>(out_T_) * static_cast<size_t>(out_C_);
    if (out_native_) {
        outputs[0].want_float = 0;      // 直读原生类型
        outputs[0].is_prealloc = 1;
        outputs[0].buf = out_raw_.data();
        outputs[0].size = static_cast<uint32_t>(out_raw_.size());
    } else {
        outputs[0].want_float = 1;      // 让驱动把量化输出反量化成 float32
        if (out_f32_.size() < out_elems) out_f32_.assign(out_elems, 0.f);
        outputs[0].is_prealloc = 1;
        outputs[0].buf = out_f32_.data();
        outputs[0].size = static_cast<uint32_t>(out_elems * sizeof(float));
    }
    // ★ 输出缓冲自己预分配: is_prealloc=1 时运行时直接写进我们给的内存,
    //   【不再每次推理都 malloc 一块几百 KB 的缓冲】。
    //   这也是 RKNN 官方 sample 的写法。注意: 这种模式下缓冲归我们所有,
    //   不能让运行时去 free, 所以下面【不调用】rknn_outputs_release。
    // ★ is_prealloc=1 时【必须】把缓冲的字节数告诉运行时。
    //   不给的话运行时读到 size=0, 会直接报
    //       E RKNN: output[0] buf size error, size = 0, expected size = 1060000
    //   而且 rknn_outputs_get 还是返回 0(成功) —— 错误只打在运行时的日志里,
    //   板端拿到的就是一份【空的】输出, 识别结果全是空串。
    outputs[0].index = 0;
    ret = rknn_outputs_get(ctx_, 1, outputs, nullptr);
    if (ret < 0) { fprintf(stderr, "[PlateRec] rknn_outputs_get 失败 %d\n", ret); return false; }
    // ★ 必须校验输出缓冲区的长度!
    //   NPU 内存紧张时(同板子上还有别的进程在跑 NPU), rknn_outputs_get 会
    //   【返回成功但 size = 0】, 运行时只打一行
    //       E RKNN: output[0] buf size error, size = 0, expected size = ...
    //   这时候直接拿去解码就是越界读(要读 1060000 个 float, 实际一个都没有)。
    //   所以这里按"期望的字节数"卡一道, 不够就当失败, 不去碰那块缓冲。
    const size_t want_bytes = out_native_ ? out_raw_.size()
                                          : out_elems * sizeof(float);
    if (outputs[0].size < want_bytes || !outputs[0].buf) {
        fprintf(stderr, "[PlateRec] RKNN 输出缓冲异常: size=%u(要 %zu) —— "
                        "多半是 NPU 内存不够(同板子还有别的进程在用 NPU?)\n",
                outputs[0].size, want_bytes);
        // ⚠️ 这里【不能】调 rknn_outputs_release: is_prealloc=1 时缓冲是
        //   我们自己的(out_raw_/out_f32_), 让运行时去 free 会把它掏空 ——
        //   下一帧再写就是写已释放的内存。
        return false;
    }

    bool ok = false;
    if (out_native_ && out_type_ == RKNN_TENSOR_FLOAT16) {
        // fp16 直读: CTC 解码自己按 IEEE 精确转换(fp16->fp32 无损)
        ok = ctc_greedy_decode_f16(static_cast<const uint16_t*>(outputs[0].buf),
                                   out_T_, out_C_, classes_,
                                   allowed_.empty() ? nullptr : &allowed_, out);
    } else if (out_native_) {
        // float32 直读
        ok = ctc_greedy_decode(static_cast<const float*>(outputs[0].buf),
                               out_T_, out_C_, classes_,
                               allowed_.empty() ? nullptr : &allowed_, out);
    } else {
        const float* logits = static_cast<const float*>(outputs[0].buf);
        ok = ctc_greedy_decode(logits, out_T_, out_C_, classes_,
                               allowed_.empty() ? nullptr : &allowed_, out);
    }
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

    // roi_ 是紧凑排列的, 行距 = 行宽
    build_input_tensor(roi_.data(), resized_w, static_cast<int>(row_bytes));
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
                                          int* out_h, int* out_pitch_bytes) {
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
    // ★ RGA 的 BGR888/RGB888 目标行距必须 16 字节对齐, 而 resized_w 是按比例
    //   算出来的(93/122/157...), 基本都不是 16 的倍数 —— 不对齐的话 imcheck
    //   直接报 "dst unsupport width stride N, bgr888 width stride should be
    //   16 aligned!", 而且更糟的是: run_rga 会把它当成"RGA 不可用"把整个
    //   硬件通道关掉(实测整场跑测的 RGA 全程降级到 CPU)。
    //   所以行宽【向上】对齐到 16 的倍数, 多出来的几个像素当行距填充,
    //   建张量时按这个行距跳过去(见 build_input_tensor 的 roi_pitch_bytes)。
    const int stride_px = align_up_16(resized_w);
    // ★ 目标缓冲要留【余量行】: RGA 在非整数倍缩放时可能多写 1~2 行,
    //   写出去就踩坏相邻堆对象(与 main.cc 的 kRgaDstPadRows 同一约定)。
    //   注意这里算的是【字节】: BGR888 一行 = stride_px 像素 x 3 字节。
    constexpr int kPadRows = 16;
    const size_t need_bytes =
        static_cast<size_t>(stride_px) * static_cast<size_t>(imgH + kPadRows) * 3;
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
    d.width = resized_w;          // 可见宽度(每行真正写了 resized_w 个像素)
    d.height = imgH;
    // ★ librga 的 wstride 单位是【像素】, 不是字节!
    //   原来写的是 resized_w * 3, 行距因此大了 3 倍, RGA 按 5760 字节/行去写,
    //   实际缓冲只有 resized_w*3 字节/行 -> 越界 2,457,600 字节,
    //   直接踩烂相邻堆对象(实测首个越界字节在偏移 1,232,640 = 第 214 行起点)。
    //   这就是 P0-4, 现在改对。
    //   再往上对齐到 16 的倍数 -> 行距(字节) = stride_px*3 必然 16 对齐。
    d.wstride = stride_px;
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
    if (out_pitch_bytes) *out_pitch_bytes = stride_px * 3;
    return true;
}

// ===========================================================================
// 模式三: 已经裁剪+按比例缩放好的 8bit ROI -> 建张量 -> 推理
//
// ★ 为什么要单独开这条路:
//   主流水线的做法是"RGA 把 ROI 裁出来(已经按比例缩到高 48), CPU 再做反白/
//   纠偏, 然后喂 NPU"。如果这时调用 recognize_bgr(), 它会【再缩一遍】
//   (虽然比例上几乎是恒等, 但仍是两遍重采样) —— 车牌本来就只有几十像素宽,
//   再糊一次就认不出来了。所以这里直接建张量。
//
//   roi: 高必须 == in_h_(48), 宽 = resized_width_for() 的结果, 通道序由
//        swap_rb 决定(BGR / RGB)。
//   roi_pitch_bytes: 行距(字节)。RGA 写出来的行距是 16 字节对齐的,
//        比"行宽 * 3"大 —— 行尾那几个填充字节必须跳过去。
// ===========================================================================
bool LicensePlateRec::recognize_roi8(const uint8_t* roi, int roi_w, int roi_pitch_bytes,
                                     PlateRecResult* out) {
    if (!ctx_ || !out) return false;
    if (!roi || roi_w <= 0) return false;
    if (roi_w > in_w_) roi_w = in_w_;      // 比模型还宽 -> 截掉(理论上不会发生)
    build_input_tensor(roi, roi_w, roi_pitch_bytes);
    return run_rknn(out);
}

int LicensePlateRec::aligned_stride_px(int dst_w) {
    return align_up_16(dst_w);
}

int LicensePlateRec::resized_width_for(int src_w, int src_h) const {
    return calc_resized_w(src_w, src_h, in_h_, in_w_);
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
    //   行宽按 16 对齐(RGA 的 BGR888 行距要求), 所以按 stride_px 算大小。
    constexpr int kPadRows = 16;
    const int stride_px = align_up_16(resized_w);
    const size_t roi_need =
        static_cast<size_t>(stride_px) * static_cast<size_t>(imgH + kPadRows) * 3;
    if (roi_.size() < roi_need) {
        roi_.assign(roi_need, 0);
    }

    int got_w = 0, got_h = 0, got_pitch = 0;
    if (!preprocess_nv12_rga(frame, x, y, w, h, roi_.data(),
                             static_cast<int>(roi_.size()), &got_w, &got_h,
                             &got_pitch)) {
        return false;
    }

    build_input_tensor(roi_.data(), got_w, got_pitch);
    return run_rknn(out);
}

}  // namespace rk
