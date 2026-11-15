#include "lprnet.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace rk {
namespace {

unsigned char* read_file(const std::string& path, size_t* out_len) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return nullptr;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0) { fclose(fp); return nullptr; }
    void* buf = malloc(static_cast<size_t>(size));
    if (!buf) { fclose(fp); return nullptr; }
    if (fread(buf, 1, static_cast<size_t>(size), fp) != static_cast<size_t>(size)) {
        free(buf);
        fclose(fp);
        return nullptr;
    }
    fclose(fp);
    *out_len = static_cast<size_t>(size);
    return static_cast<unsigned char*>(buf);
}

}  // namespace

// ---------------------------------------------------------------------------
std::string LprNet::ctc_greedy_decode(const float* logits, int T, int C,
                                       const std::string& charset) {
    std::string out;
    int prev = -1;
    for (int t = 0; t < T; ++t) {
        const float* row = logits + static_cast<size_t>(t) * C;
        int best = 0;
        float bestv = row[0];
        for (int c = 1; c < C; ++c) {
            if (row[c] > bestv) { bestv = row[c]; best = c; }
        }
        if (best != prev && best != 0) {          // 0 = CTC blank
            // ★ 索引偏移: 类别 k(1..34) -> charset[k-1]
            if (best >= 1 && best <= static_cast<int>(charset.size())) {
                out.push_back(charset[static_cast<size_t>(best - 1)]);
            }
        }
        prev = best;
    }
    return out;
}

// ---------------------------------------------------------------------------
bool LprNet::init(const std::string& model_path, const std::string& charset) {
    destroy();
    charset_ = charset;

    size_t model_size = 0;
    unsigned char* model = read_file(model_path, &model_size);
    if (!model) {
        fprintf(stderr, "[LPRNet] 打不开模型: %s\n", model_path.c_str());
        return false;
    }
    int ret = rknn_init(&ctx_, model, static_cast<uint32_t>(model_size),
                        RKNN_FLAG_PRIOR_HIGH, nullptr);
    free(model);
    if (ret < 0) {
        fprintf(stderr, "[LPRNet] rknn_init 失败: %d\n", ret);
        ctx_ = 0;
        return false;
    }

    // ---- 自检: dtype / 布局必须和预期一致, 对不上就别往下跑 ----
    rknn_tensor_attr ia;
    memset(&ia, 0, sizeof(ia));
    ia.index = 0;
    rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, &ia, sizeof(ia));
    fprintf(stderr, "[LPRNet] 输入 name=%s type=%d fmt=%d dims=[", ia.name,
            static_cast<int>(ia.type), static_cast<int>(ia.fmt));
    for (uint32_t i = 0; i < ia.n_dims; ++i) fprintf(stderr, "%u,", ia.dims[i]);
    fprintf(stderr, "] n_elems=%u\n", ia.n_elems);

    rknn_tensor_attr oa;
    memset(&oa, 0, sizeof(oa));
    oa.index = 0;
    rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &oa, sizeof(oa));
    fprintf(stderr, "[LPRNet] 输出 name=%s type=%d dims=[", oa.name,
            static_cast<int>(oa.type));
    for (uint32_t i = 0; i < oa.n_dims; ++i) fprintf(stderr, "%u,", oa.dims[i]);
    fprintf(stderr, "] n_elems=%u\n", oa.n_elems);

    // 从输出属性推 T / C: [1, T, C]
    if (oa.n_dims >= 2) {
        T_ = static_cast<int>(oa.dims[oa.n_dims - 2]);
        C_ = static_cast<int>(oa.dims[oa.n_dims - 1]);
    }

    // ---- ★ 输入形态: 完全按模型真实属性来, 不许想当然 ----
    //
    //   type=INT8(2)     -> 输入是量化张量, 板端要按 zp/scale 生成量化码
    //   type=FLOAT32(0)  -> 输入是浮点, 板端转 float32
    //   type=UINT8(3)    -> 输入就是 uint8 原样, 直接喂
    use_float_ = (ia.type == RKNN_TENSOR_FLOAT32);
    need_nchw_ = (ia.fmt == RKNN_TENSOR_NCHW);
    has_qnt_   = (ia.qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC) && (ia.scale > 0.f);
    in_scale_  = has_qnt_ ? ia.scale : 1.0f;
    in_zp_     = has_qnt_ ? ia.zp : 0;

    // 顺便核对一下尺寸: 板端裁出来的是 160x48, 对不上就报警(免得默默算错)
    int mh = 0, mw = 0;
    if (ia.n_dims >= 4) {
        if (need_nchw_) { mh = static_cast<int>(ia.dims[2]); mw = static_cast<int>(ia.dims[3]); }
        else            { mh = static_cast<int>(ia.dims[1]); mw = static_cast<int>(ia.dims[2]); }
    }
    if (mh && (mh != kLprH || mw != kLprW)) {
        fprintf(stderr, "[LPRNet] ⚠️ 模型输入尺寸是 %dx%d, 板端裁的是 %dx%d, 对不上!\n",
                mw, mh, kLprW, kLprH);
    }

    const size_t n_elems = static_cast<size_t>(kLprW) * kLprH * 3;
    if (feed_ == LprFeed::kFloat32 && !use_float_) {
        fprintf(stderr, "[LPRNet] 指定了 float32 喂法, 但模型输入不是 FLOAT32, 改用 int8 量化码\n");
        feed_ = LprFeed::kInt8Code;
    }
    if (feed_ == LprFeed::kInt8Code && !has_qnt_) {
        fprintf(stderr, "[LPRNet] 模型输入没有 AFFINE_ASYMMETRIC 量化参数, "
                       "改用 uint8 原始像素喂\n");
        feed_ = use_float_ ? LprFeed::kFloat32 : LprFeed::kInt8Code;
    }

    if (feed_ == LprFeed::kInt8Code) {
        i8_.assign(n_elems, 0);
        fprintf(stderr,
                "[LPRNet] 喂法=int8 量化码 (zp=%d, scale=%.8f) -> size=%u 字节 "
                "[runtime 要 92160, 少给会越界踩坏堆!]\n",
                in_zp_, in_scale_, static_cast<unsigned>(n_elems));
    } else {
        f32_.assign(n_elems, 0.f);
        fprintf(stderr, "[LPRNet] 喂法=float32 (x/255, [0,1]) -> size=%u 字节\n",
                static_cast<unsigned>(n_elems * sizeof(float)));
    }
    // 预分配输出缓冲: 元素数 = T*C 个 float(见下面 infer() 里的用法)
    out_f32_.assign(static_cast<size_t>(T_) * static_cast<size_t>(C_), 0.f);

    fprintf(stderr, "[LPRNet] 就绪: %dx%d %s, T=%d C=%d, 字符集=%s\n",
            kLprW, kLprH, need_nchw_ ? "NCHW" : "NHWC", T_, C_, charset_.c_str());

    return true;
}

void LprNet::destroy() {
    if (ctx_) {
        rknn_destroy(ctx_);
        ctx_ = 0;
    }
    i8_.clear();
    i8_.shrink_to_fit();
    f32_.clear();
    f32_.shrink_to_fit();
    cache_.clear();
}

// ---------------------------------------------------------------------------
bool LprNet::infer(const uint8_t* rgb, std::string* plate) {
    if (!ctx_ || !rgb || !plate) return false;

    const size_t n_elems = static_cast<size_t>(kLprW) * kLprH * 3;

    rknn_input inputs[1];
    memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].pass_through = 0;   // 0 = 让驱动按模型要求做格式/量化转换

    if (feed_ == LprFeed::kInt8Code) {
        // ---- 生成 int8 量化码 ------------------------------------------------
        //   q = clamp(round((x/255) / scale) + zp, -128, 127)
        //   (x/255 = 模型输入张量的实数域 [0,1]; 量化参数一律现查)
        //   size 必须是【量化码个数】= 23040 字节。
        //   ★ 千万别再用 uint8 + 23040 去喂: runtime 会按 92160 字节读写
        //     我们这块缓冲 -> 越界 -> 堆被踩坏 -> 进程 abort。
        if (i8_.size() != n_elems) i8_.assign(n_elems, 0);
        const float inv_scale = 1.0f / in_scale_;
        for (size_t i = 0; i < n_elems; ++i) {
            float real = static_cast<float>(rgb[i]) * (1.0f / 255.0f);
            int q = static_cast<int>(lrintf(real * inv_scale)) + in_zp_;
            if (q > 127) q = 127;
            if (q < -128) q = -128;
            i8_[i] = static_cast<int8_t>(q);
        }
        inputs[0].buf = i8_.data();
        inputs[0].size = static_cast<uint32_t>(n_elems);
        inputs[0].type = RKNN_TENSOR_INT8;
        inputs[0].fmt = need_nchw_ ? RKNN_TENSOR_NCHW : RKNN_TENSOR_NHWC;
    } else {
        // ---- float32 ----------------------------------------------------------
        if (f32_.size() != n_elems) f32_.assign(n_elems, 0.f);
        if (need_nchw_) {
            const size_t plane = static_cast<size_t>(kLprH) * kLprW;
            for (int y = 0; y < kLprH; ++y) {
                for (int x = 0; x < kLprW; ++x) {
                    const uint8_t* p = rgb + (static_cast<size_t>(y) * kLprW + x) * 3;
                    float* q = f32_.data() + static_cast<size_t>(y) * kLprW + x;
                    q[0 * plane] = p[0] * (1.0f / 255.0f);
                    q[1 * plane] = p[1] * (1.0f / 255.0f);
                    q[2 * plane] = p[2] * (1.0f / 255.0f);
                }
            }
        } else {
            for (size_t i = 0; i < n_elems; ++i) {
                f32_[i] = static_cast<float>(rgb[i]) * (1.0f / 255.0f);
            }
        }
        inputs[0].buf = f32_.data();
        inputs[0].size = static_cast<uint32_t>(n_elems * sizeof(float));
        inputs[0].type = RKNN_TENSOR_FLOAT32;
        inputs[0].fmt = need_nchw_ ? RKNN_TENSOR_NCHW : RKNN_TENSOR_NHWC;
    }

    int ret = rknn_inputs_set(ctx_, 1, inputs);
    if (ret < 0) {
        fprintf(stderr, "[LPRNet] rknn_inputs_set 失败 %d (type=%d fmt=%d size=%u)\n",
                ret, static_cast<int>(inputs[0].type), static_cast<int>(inputs[0].fmt),
                inputs[0].size);
        return false;
    }

    ret = rknn_run(ctx_, nullptr);
    if (ret < 0) { fprintf(stderr, "[LPRNet] rknn_run 失败 %d\n", ret); return false; }

    rknn_output outputs[1];
    memset(outputs, 0, sizeof(outputs));
    outputs[0].want_float = 1;            // 让驱动把量化输出反量化成 float32
    // ★ is_prealloc=1: 输出缓冲由我们自己提供, 运行时【不再每次推理都 malloc】。
    //   以前 is_prealloc=0 让运行时自己分配, 每推理一次就有一块 2.8KB 的
    //   float 缓冲进出分配器, 运行时的内部池会逐步涨 —— 活堆曲线跟着往上爬。
    //   自己预分配一次就彻底没有推理期分配了。
    if (out_f32_.empty()) out_f32_.assign(static_cast<size_t>(T_) * static_cast<size_t>(C_), 0.f);
    outputs[0].is_prealloc = 1;
    outputs[0].buf = out_f32_.data();
    outputs[0].index = 0;
    ret = rknn_outputs_get(ctx_, 1, outputs, nullptr);
    if (ret < 0) { fprintf(stderr, "[LPRNet] rknn_outputs_get 失败 %d\n", ret); return false; }

    const float* logits = static_cast<const float*>(outputs[0].buf);
    *plate = ctc_greedy_decode(logits, T_, C_, charset_);

    // 缓冲是我们自己的, 不能让运行时去 free(它只是"借用"这块内存)
    outputs[0].buf = nullptr;
    return true;
}

// ---------------------------------------------------------------------------
int LprNet::set_core_mask(int mask) {
    if (!ctx_ || mask <= 0) return -1;
    const int ret = rknn_set_core_mask(ctx_, static_cast<rknn_core_mask>(mask));
    fprintf(stderr, "[LPRNet] NPU 核绑定 mask=0x%x -> %d\n", mask, ret);
    return ret;
}

bool LprNet::infer_track(int track_id, const uint8_t* rgb, int64_t now_ms, int64_t ttl_ms,
                          std::string* plate, bool* from_cache) {
    if (from_cache) *from_cache = false;
    auto it = cache_.find(track_id);
    if (it != cache_.end() && ttl_ms > 0 && (now_ms - it->second.second) < ttl_ms) {
        if (plate) *plate = it->second.first;
        if (from_cache) *from_cache = true;
        return true;
    }
    if (!infer(rgb, plate)) return false;
    if (ttl_ms > 0) {
        cache_[track_id] = std::make_pair(*plate, now_ms);
        // ★ 过期清理(2026-11-12): 缓存是按 track_id 建的, 而 track_id 单调递增,
        //   旧 track 再也不会回来 —— 不清的话这个 map 只增不减, 长跑就是慢泄漏
        //   (实测约 +15 B/帧, 折合 ~30 MB/24h)。
        //   TTL 本来就声明了"同一条 track 的结果只保留 ttl_ms", 超期的条目
        //   对命中没有任何价值, 直接删掉。
        for (auto i = cache_.begin(); i != cache_.end();) {
            if (ttl_ms > 0 && (now_ms - i->second.second) >= ttl_ms)
                i = cache_.erase(i);
            else
                ++i;
        }
    }
    return true;
}

}  // namespace rk
