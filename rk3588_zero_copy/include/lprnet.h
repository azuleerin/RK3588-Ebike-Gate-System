// lprnet.h —— LPRNet 车牌识别 (RKNN INT8)
//
// 移植自算法组的参考实现 rknn_cpp/lprnet_rknn_ref.cc, 模型规格:
//     输入 : [1, 48, 160, 3] NHWC (W=160, H=48)
//     输出 : [1, 20, 35]  (T=20 个时间步, 35 类 = 34 字符 + CTC blank)
//
// ★★★ 四个必须记住的坑 ★★★
//  1) 【输入必须按模型自己的量化参数生成 int8 量化码】—— 2026-11-11 踩死人的坑
//     这个模型的输入 attr 是:
//         type = INT8(2), fmt = NHWC, dims = [1,48,160,3]
//         qnt_type = AFFINE_ASYMMETRIC, zp = 0, scale = 0.007843 (= 1/127.5)
//     但 RKNN 运行时 rknn_inputs_set() 实际要的是【92160 字节】(= 23040 * 4),
//     而按 uint8 喂只给了 23040 字节 —— 运行时日志会直接报:
//         E RKNN: rknn_inputs_set, param input size(23040) < model input size(92160)
//     然后它按 92160 字节去读写我们那块 23040 字节的缓冲 -> 【越界踩坏堆】
//     -> 后面随便一次 malloc / cv::Mat 就炸, 表现为
//        "malloc(): invalid size" / "OpenCV -215 roi.x ... m.cols" 随机落点。
//
//     正确喂法(板端实测, 合成车牌图 A/B 七种喂法, 只有这一种解出正确结果):
//         q = clamp(round((x/255) / scale) + zp, -128, 127)   -> int8 码
//         inputs[0].type = RKNN_TENSOR_INT8
//         inputs[0].size = 160*48*3 = 23040 【字节】(正好等于量化码个数)
//     量化参数一律从 rknn_query(RKNN_QUERY_INPUT_ATTR) 现查, 不写死。
//
//  2) 通道顺序必须是 RGB
//     训练时是 cv2.cvtColor(BGR2RGB) 之后的 RGB。
//
//  3) 归一化不要重复做
//     mean/std 已经在转换时固化进模型图里, 板端只管把 uint8 像素按上面的
//     量化公式变成码, 不要再多除一次 255。
//
//  4) CTC 解码索引要减 1
//     训练时 char_to_idx = {c: i+1}(0 留给 blank), 所以类别 k -> charset[k-1]。
//     写错会整体偏移一位: "0443M1" 会解成 "1554P2"。
#ifndef RK_LPRNET_H_
#define RK_LPRNET_H_

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "rknn_api.h"

namespace rk {

constexpr int kLprW = 160;
constexpr int kLprH = 48;

// 默认字符集(必须与训练完全一致, 顺序都不能差)
inline const char* lpr_default_charset() { return "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ"; }

// LPRNet 的输入喂法
enum class LprFeed {
    kInt8Code,   // 按模型 zp/scale 生成 int8 量化码(默认, 板端实测唯一可用)
    kFloat32,    // float32 直接喂(仅当模型输入真是 FP32 时才用)
};

class LprNet {
public:
    ~LprNet() { destroy(); }

    // charset 传字符串(34 个字符); 空则用默认
    bool init(const std::string& model_path, const std::string& charset = "");

    // ★ NPU 核绑定: 0=自动(AUTO), 1=CORE0, 2=CORE1, 4=CORE2, 3=CORE0+1, 7=三核
    //   用来做"检测模型跑 core0、车牌模型跑 core1"的调度实验。
    int set_core_mask(int mask);
    void destroy();
    bool enabled() const { return ctx_ != 0; }

    // 喂法(默认 kInt8Code)
    void set_feed(LprFeed f) { feed_ = f; }
    LprFeed feed() const { return feed_; }

    // 模型输入是不是浮点
    bool float_input() const { return use_float_; }

    // rgb: 160*48*3 字节, HWC 连续内存, uint8, RGB
    bool infer(const uint8_t* rgb, std::string* plate);

    // 带 track 缓存: 同一 track 在 ttl_ms 内不重复识别(车牌不会每帧变)
    bool infer_track(int track_id, const uint8_t* rgb, int64_t now_ms, int64_t ttl_ms,
                     std::string* plate, bool* from_cache);

    static std::string ctc_greedy_decode(const float* logits, int T, int C,
                                         const std::string& charset);

private:
    rknn_context ctx_ = 0;
    std::string charset_;
    int T_ = 20;
    int C_ = 35;

    // ---- 输入形态(全部来自 init() 里查到的模型真实属性) ----
    bool use_float_ = false;   // 模型输入逻辑类型是 FLOAT32
    bool need_nchw_ = false;   // 模型输入布局是 NCHW
    LprFeed feed_ = LprFeed::kInt8Code;

    // 量化参数(现查现用, 不写死)
    bool has_qnt_ = false;
    float in_scale_ = 1.0f;    // AFFINE_ASYMMETRIC 的 scale
    int   in_zp_ = 0;          // AFFINE_ASYMMETRIC 的 zero point

    std::vector<int8_t> i8_;   // 量化码缓冲(复用, 不每帧分配)
    std::vector<float>  f32_;  // 浮点缓冲(复用)
    // ★ 输出缓冲(预分配): rknn_outputs_get 的 is_prealloc=1 时,
    //   运行时不再每次推理都 malloc 一块 float 缓冲(700 个 float = 2.8KB)。
    //   自己持有这块缓冲, 生命周期跟着 LprNet 走, 推理期零分配。
    std::vector<float>  out_f32_;

    std::map<int, std::pair<std::string, int64_t>> cache_;
};

}  // namespace rk

#endif  // RK_LPRNET_H_
