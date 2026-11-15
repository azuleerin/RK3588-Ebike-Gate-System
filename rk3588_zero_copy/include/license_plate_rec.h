// license_plate_rec.h —— PP-OCRv4_mobile_rec 车牌识别(RKNN)
//
// 来源: rknn_model_zoo v2.5.0 官方示例
//   examples/PPOCR/PPOCR-Rec/cpp/rknpu2/ppocr_rec.cc   (推理核心)
//   examples/PPOCR/PPOCR-Rec/cpp/postprocess.cc         (CTC 贪心解码)
//   examples/PPOCR/PPOCR-Rec/cpp/dict.h                 (6625 类字典)
//   examples/PPOCR/PPOCR-Rec/model/ppocr_keys_v1.txt    (字典原始文件)
//
// ★ 本类只做【文本识别】, 不含任何文本检测(DBNet)逻辑:
//   车牌区域由上游 YOLO 检测模型给出, 本类只负责"这块像素 -> 字符串"。
//   官方示例里的 DBNet 预处理/后处理/图像绘制/演示 main 全部剥离。
//
// ★ 模型规格(实测 ppocrv4_rec.onnx):
//   输入 : x            [1, 3, 48, 320] float32
//   输出 : softmax_11   [1, 40, 6625]   (T = 320/8 = 40 个时间步, C = 6625 类)
//   归一化: (x - 127.5) / 127.5          (官方 ppocr_rec.cc 里的写法)
//
// ★ 字典索引约定(官方 dict.h):
//   类 0    = "blank"(CTC 空白)
//   类 1..  = ppocr_keys_v1.txt 的第 1..6623 行
//   类 6624 = " " (空格)
//   ⚠️ 官方 dict.h 是脚本生成的, 里面 '%' 被写成了 "%%"(printf 转义残留),
//      本类从 ppocr_keys_v1.txt 重新构造字典, 不带这个坑。
#ifndef RK_LICENSE_PLATE_REC_H_
#define RK_LICENSE_PLATE_REC_H_

#include <cstdint>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "rga_convert.h"   // rk::RgaImage(工程里已有的帧描述)
#include "rknn_api.h"     // rknn_context / RKNN_TENSOR_*

namespace rk {

// 一次识别的结果
struct PlateRecResult {
    std::string text;        // 识别文本(已按字典还原)
    float score = 0.f;        // 整体置信度: 各有效时间步最大概率的均值(官方算法)
    int   char_count = 0;     // 有效字符数(不含 blank)
    float mean_score = 0.f;   // 同 score, 便于和上层接口对齐
};

// 输入归一化方式。
//
// ★ 官方例子里有【两种】写法, 不是笔误, 对应两种不同的模型:
//   · rknpu2/ppocr_rec.cc (C++ 部署参考):  (x - 127.5) / 127.5  -> [-1, 1]
//     这也和 PaddleOCR 推理路径 ((x/255 - 0.5)/0.5) 完全一致。
//   · python/ppocr_rec.py (ONNX 路径):      x / 255              -> [0, 1]
//     它的模型侧/运行时侧已经固化了归一化, 所以外部只做缩放。
//   官方 ppocrv4_rec.onnx 里【没有】任何前置归一化节点(第一个节点就是 backbone),
//   所以默认用 C++ 的写法; 若你转模型时把归一化固化进去了, 改成 kDiv255。
enum class PlateNormMode {
    kMinus127Div127,   // (x - 127.5) / 127.5   (默认, 官方 C++ 示例)
    kDiv255,           // x / 255
};

// 预处理输入模式
enum class PlatePreMode {
    kCpuBgr,    // 模式一: 已裁剪好的 BGR 图(等价 numpy 的 HWC/BGR/uint8 数组), CPU 预处理
    kRgaNv12,   // 模式二: NV12 物理内存帧 + 车牌框, RGA 硬件预处理
};

struct PlateRecConfig {
    std::string model_path;              // RKNN 模型路径(必填)
    std::string dict_path;               // 字典文件(一行一个字符); 空则只认 blank
    bool   swap_rb = false;              // 交换 R/B: 模型吃 RGB 时置 true(默认 BGR, 与官方示例一致)
    std::string allowed_charset;         // 只允许这些字符(车牌场景); 空 = 不过滤
    int    npu_core_mask = 0;            // >0 时绑 NPU 核(rknn_set_core_mask)
    bool   prefer_int8 = false;          // 模型输入是量化张量时, 优先按 int8 量化码喂
    PlateNormMode norm_mode = PlateNormMode::kMinus127Div127;   // 归一化方式
    // 归一化是否已经固化进模型图里(转模型时设了 mean_values/std_values 就是固化了)。
    //   固化了 -> 板端【只喂原始 0~255 像素】, 再做一次 (x-127.5)/127.5 就是双重归一化
    //            (实测会把输出毁掉)。
    bool   norm_baked_in = false;
};

// ---------------------------------------------------------------------------
// 车牌识别器: 模型加载 / 预处理 / 推理 / CTC 解码 全流程封装
//
// 典型用法:
//     rk::PlateRecConfig cfg;
//     cfg.model_path = "./model/ppocrv4_mobile_rec_int8.rknn";
//     cfg.dict_path  = "./model/ppocr_keys_v1.txt";
//     cfg.allowed_charset = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ京沪粤...";
//     rk::LicensePlateRec rec;
//     rec.init(cfg);
//
//     // 模式一: 上游已经裁好的 BGR 图
//     rk::PlateRecResult r;
//     rec.recognize_bgr(roi_bgr, &r);           // r.text / r.score
//
//     // 模式二: 原始 NV12 帧 + 框, RGA 硬件裁剪缩放(零拷贝)
//     rec.recognize_nv12(frame, x, y, w, h, &r);
// ---------------------------------------------------------------------------
class LicensePlateRec {
public:
    LicensePlateRec() {}
    ~LicensePlateRec() { destroy(); }

    // 加载模型 + 字典。返回 false 表示不可用(后续调用会直接返回 false)
    bool init(const PlateRecConfig& cfg);
    void destroy();
    bool ready() const { return ctx_ != 0; }

    // 模型信息(调试用)
    int  input_width()  const { return in_w_; }
    int  input_height() const { return in_h_; }
    int  output_seq_len() const { return out_T_; }
    int  output_channels() const { return out_C_; }

    // ---- 模式一: 已裁剪的 BGR 图 + CPU 预处理 ---------------------------
    // bgr: CV_8UC3, HWC, BGR 顺序(即 numpy 的 HWC/BGR/uint8)
    bool recognize_bgr(const cv::Mat& bgr, PlateRecResult* out);

    // ---- 模式二: NV12 帧 + 框 + RGA 硬件预处理 -------------------------
    // frame: 必须是【真 dma-buf】(fd >= 0)才会走 RGA; 否则自动退回 CPU 路径。
    //   · RGA 一次 improcess 完成 裁剪 + 缩放 + 颜色空间转换, 帧数据全程不进 CPU
    //   · CPU 只做最后一遍"归一化 + 补零"(最大 320*48*3 = 46KB)
    bool recognize_nv12(const rk::RgaImage& frame, int x, int y, int w, int h,
                        PlateRecResult* out);

    // ---- 模式三: 已经裁剪+按比例缩放好的 8bit ROI ------------------------
    // roi: 高必须 == in_h_(48), 宽 == resized_width_for() 算出来的那个,
    //      通道序由 swap_rb 决定(BGR / RGB)。
    // roi_pitch_bytes: 相邻两行之间的字节距离(= 行宽时就是 roi_w * 3)。
    //   ★ 为什么要有这个参数: RGA 的 BGR888 目标行距必须 16 字节对齐,
    //     而 PP-OCR 的目标宽度是按比例算出来的(93/122/157... 这种),
    //     基本都不是 16 的倍数。所以 RGA 那一行的实际步长是
    //     align16(dst_w) * 3, 行与行之间有一段"填充字节",
    //     建张量的时候必须按这个步长跳, 否则读出来的字全是斜的。
    //   · 用于"RGA 已经做完裁剪+缩放, CPU 只做了反白/纠偏"的流水线:
    //     直接建张量 + 推理, 不再走 recognize_bgr() 里那遍 CPU 缩放 ——
    //     二次重采样会把字糊掉, 而车牌本来就只有几十像素宽。
    //   · 宽度不足 in_w_(320) 的部分自动补 0(和官方 copyMakeBorder 一致)。
    bool recognize_roi8(const uint8_t* roi, int roi_w, int roi_pitch_bytes,
                        PlateRecResult* out);

    // 按官方 resize_norm_img 的规则, 算出 src_w x src_h 这块 ROI 缩到 in_h_ 高
    // 之后应有的宽度(= ceil(in_h_ * src_w / src_h), 上限 in_w_)。
    // main.cc 用它决定 RGA 裁剪的目标尺寸, 保证裁出来的比例和
    // recognize_bgr() 内部算的完全一致(差一个像素都会让字形变形)。
    int resized_width_for(int src_w, int src_h) const;

    // RGA 的 BGR888/RGB888 目标行距必须 16 字节对齐(= 行宽是 16 的倍数)。
    // 返回按 16 像素对齐后的行宽(像素)。dst_w=93 -> 96。
    static int aligned_stride_px(int dst_w);

    // ---- 只做 RGA 预处理, 不跑 NPU(调试/自测用) ------------------------
    // 输出是 8bit 的 resized_w x in_h_ 图像(BGR 或 RGB, 由 swap_rb 决定)
    // out_pitch_bytes: 行距(字节)。RGA 的行距必须 16 字节对齐, 所以通常
    //   > out_w * 3 —— 行与行之间有填充字节, 建张量时要按这个步长跳。
    bool preprocess_nv12_rga(const rk::RgaImage& frame, int x, int y, int w, int h,
                             uint8_t* dst, int dst_capacity, int* out_w, int* out_h,
                             int* out_pitch_bytes = nullptr);

    // ---- CTC 贪心解码(公开出来方便单测) --------------------------------
    // 语义与官方 rec_postprocess 完全一致:
    //   · 逐时间步取 argmax(若给了 allowed 掩码, 只在允许的类里取)
    //   · 类 0(blank) 不输出
    //   · 与【上一个时间步】的 argmax 相同则不重复输出(注意: blank 会打断重复)
    //   · score = 各有效时间步最大概率的平均值
    static bool ctc_greedy_decode(const float* logits, int T, int C,
                                  const std::vector<std::string>& classes,
                                  const std::vector<int>* allowed,   // nullptr = 不过滤
                                  PlateRecResult* out);

    // 读字典文件: 一行一个字符。返回的 classes[0] 固定是 "blank"
    static bool load_dict(const std::string& path, int expect_classes,
                          std::vector<std::string>* classes);

private:
    // 把 8bit ROI(宽度 roi_w, 行距 roi_pitch_bytes)变成模型输入张量(含补零到 in_w_)
    void build_input_tensor(const uint8_t* roi, int roi_w, int roi_pitch_bytes);
    bool run_rknn(PlateRecResult* out);

    rknn_context ctx_ = 0;
    PlateRecConfig cfg_;

    // ---- 字典 ----
    std::vector<std::string> classes_;      // 0 = blank
    std::vector<int> allowed_;              // allowed_charset 过滤后的类号(空 = 不过滤)

    // ---- 模型属性(全部来自 rknn_query, 不写死) ----
    int in_w_ = 320, in_h_ = 48, in_c_ = 3;
    int out_T_ = 40, out_C_ = 0;
    bool in_nchw_ = false;
    int  in_type_ = RKNN_TENSOR_FLOAT32;    // 模型输入张量的逻辑类型
    bool has_qnt_ = false;                  // 输入有 AFFINE_ASYMMETRIC 量化参数
    float in_scale_ = 1.0f;
    int   in_zp_ = 0;
    uint32_t in_elems_ = 0;
    uint32_t in_size_hint_ = 0;             // 模型输入张量字节数(报错时对照用)

    // ---- 复用缓冲(避免每帧分配) ----
    std::vector<float>  f32_;
    std::vector<int8_t> i8_;
    std::vector<uint8_t> u8_;
    // ★ 输出缓冲【预分配】(PP-OCRv4 rec: out_T_ x out_C_ 个 float,
    //   6625 类 x 25 步 = 662KB 一次!)。rknn_outputs_get 的 is_prealloc=1
    //   表示缓冲由我们自己提供, 运行时【不再每次推理都 malloc】。
    //   以前 is_prealloc=0 让运行时自己分配 —— 每推理一次就是一块几百 KB 的
    //   进出, 运行时的内部池逐步涨, 活堆曲线跟着往上爬(实测 4K 路口视频
    //   前几千帧涨 1.2MB)。自己预分配一次就彻底没有推理期分配了。
    std::vector<float> out_f32_;
    std::vector<uint8_t> roi_;               // RGA 输出的 8bit ROI
};

}  // namespace rk

#endif  // RK_LICENSE_PLATE_REC_H_
