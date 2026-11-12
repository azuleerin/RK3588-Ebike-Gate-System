// yolo_post.h —— YOLOv8 后处理
//
// 本文件移植自 AI 算法组的板端推理代码 rknn_cpp/rknn_yolov8_infer.cc，
// 支持三种导出形态，按输出张量个数自动识别：
//
// [形态 A] 标准 Ultralytics 导出 —— 单输出
//     output0 = [1, 4+nc, num_anchors]   (CHW!)
//       ch0..3 = cx, cy, w, h    单位=模型输入图的像素(已乘 stride)
//       ch4..  = 类别概率         **已经过 sigmoid, 不要再 sigmoid 一次**
//     -> DFL 已在 ONNX 图内解完, CPU 侧不需要 DFL
//     -> 读法: out[c * num_anchors + a]  (绝对不要 reshape 成 [num_anchors, 4+nc])
//
// [形态 B] 瑞芯微 fork `format=rknn` 导出 —— 9 输出
//     每尺度 3 个, 顺序为 [box, cls, sum]:
//       box = [1, 4*DFL_LEN, H, W]  原始 DFL logits, 未解码 -> CPU 上要算 DFL
//       cls = [1, nc, H, W]         概率 (**fork 里已 sigmoid**)
//       sum = [1, 1, H, W]          各类别概率之和(clamp 0~1), 用于快速预过滤
//     stride 依次为 8 / 16 / 32
//
// [形态 C] 算法组的 ebike_int8.rknn —— 2 输出 (box / cls 拆开)
//     box = [1, 4, num_anchors]   cx, cy, w, h (像素, 模型输入尺度)
//     cls = [1, nc, num_anchors]  概率 (**已 sigmoid**)
//     ★ 必须拆开: 单输出 int8 时 box 和 cls 共用一个量化 scale, cls 会被压成 0
//
// 踩坑备忘 (与 AI 算法组 test_sigmoid_double.py 的实证一致):
//   1) cls 在两种导出形态里 **都已经是概率**。再套一层 sigmoid,
//      分数会被压到 [0.5, 0.731], 真负样本也变 0.5, 阈值完全失效。
//   2) 阈值必须和被比较的量在同一空间: 比概率 -> 阈值直接用 conf。
//   3) 量化参数 (zp/scale) 用 rknn_query 的 OUTPUT_ATTR 取, 不要写死。
#ifndef RK_YOLO_POST_H_
#define RK_YOLO_POST_H_

#include <cstdint>
#include <vector>

#define RK_DFL_LEN 16

// 输出张量统一访问器: float 直读; int8 按 (q - zp) * scale 现场反量化
struct OutTensor {
    const void* data    = nullptr;
    bool        is_int8 = false;
    int32_t     zp      = 0;
    float       scale   = 1.0f;
    uint32_t    n_elems = 0;
    uint32_t    dims[4] = {0, 0, 0, 0};
    uint32_t    n_dims  = 0;

    inline float at(uint32_t i) const {
        if (is_int8) {
            float q = static_cast<float>(reinterpret_cast<const int8_t*>(data)[i]);
            return (q - static_cast<float>(zp)) * scale;
        }
        return reinterpret_cast<const float*>(data)[i];
    }
};

struct YoloBox {
    int   cls_id = 0;
    float score  = 0.f;
    float x1 = 0.f, y1 = 0.f, x2 = 0.f, y2 = 0.f;
};

// 模型输入坐标 -> 原始画面坐标
//   STRETCH   : 预处理是拉伸缩放(AI 算法组板端代码的默认做法)
//               x_orig = x_model * (orig_w / model_w)
//   LETTERBOX : 预处理是等比 letterbox
//               x_orig = (x_model - pad_x) * inv_scale
struct BoxMapper {
    enum Mode { STRETCH, LETTERBOX };

    Mode  mode = STRETCH;
    float scale_x = 1.f, scale_y = 1.f;  // STRETCH 用
    float inv_scale = 1.f;               // LETTERBOX 用
    int   pad_x = 0, pad_y = 0;          // LETTERBOX 用
    int   img_w = 0, img_h = 0;

    // 把模型输入坐标系下的框映射回原始画面并裁剪到画面内
    void apply(std::vector<YoloBox>& boxes) const;
};

// 形态 A 后处理: 单输出 [1, 4+nc, num_anchors]
std::vector<YoloBox> post_process_single_output(const OutTensor& t, uint32_t num_anchors,
                                                 int nc, float conf_thr, float nms_thr);

// 形态 B 后处理: 瑞芯微 fork 的 9 输出, 顺序 [box, cls, sum] * 3 尺度
std::vector<YoloBox> post_process_fork_9out(const OutTensor* outs, int n_out, int nc,
                                            float conf_thr, float nms_thr);

// 形态 C 后处理: box / cls **双输出**
//   输出0 = box [1, 4, num_anchors]  cx,cy,w,h (像素, 模型输入尺度)
//   输出1 = cls [1, nc, num_anchors] 已经过 sigmoid 的概率
//
// ★ 为什么必须拆成两个输出 (实测, 不是推测):
//   Ultralytics 标准导出把 box 和 cls 拼在同一个输出张量
//       output0 = Concat([box(cx,cy,w,h 0~640 像素), cls(sigmoid 0~1 概率)], axis=1)
//   RK3588 的 int8 只支持 w8a8, 而一个输出张量只有【一个】量化 scale,
//   它由量级大的 box 决定 (实测 ≈2.548) -> cls 的 0~1 全被量化到码 0
//   -> 类别通道恒为 0, 一个框都检不出来。
//   坑的是这时余弦相似度仍有 0.9998, 光看余弦完全发现不了。
//   拆成两个输出后各自量化: box scale≈2.5, cls scale≈1/255, 两者互不干扰。
//   代价: 板端 C++ 要多读一个输出张量(就是这里)。
std::vector<YoloBox> post_process_dual_output(const OutTensor& box, const OutTensor& cls,
                                               uint32_t num_anchors, int nc,
                                               float conf_thr, float nms_thr);

// 自动分发: n_out == 1 走形态 A, n_out == 2 走形态 C, n_out >= 9 走形态 B
// 返回的框已经过 mapper 映射到原始画面坐标
std::vector<YoloBox> yolov8_postprocess(const OutTensor* outs, int n_out,
                                         float conf_thr, float nms_thr,
                                         const BoxMapper& mapper);

// 分类别 NMS (移植自 AI 算法组实现)
void nms_per_class(std::vector<YoloBox>& objs, float conf_thr, float iou_thr);

// ---------------------------------------------------------------------------
// 模块 A：几何先验过滤
//
// 模型会间歇性吐出几乎铺满画面的"扁宽"假框（实测宽度可达画面 99%）。
// 它和真框 IoU 只有 0.07~0.37, NMS 压不掉; 置信度又高于 new_track_thresh,
// 于是被当成新目标建轨 —— 画面上就多出一个全屏框("同一车出现两个框")。
//
// 判据来自 Python 侧 816 个检测框的统计: 真正的"人+车"框长宽比 h/w 中位 0.94
// (竖高), 而这些异常框 h/w 全部 <= 0.53 (扁宽)。所以"又宽又扁"是最干净的判据:
// 命中 85/85 个异常框, 对 731 个正常框零误杀。
//
// ⚠️ 这条过滤必须放在【跟踪之前】。放到跟踪之后, 那个假框已经占了一个 track ID,
//    再过滤也来不及了。
// ---------------------------------------------------------------------------
struct GeoFilterCfg {
    float bad_w_ratio    = 0.70f;   // 框宽  / 画面宽  上限
    float bad_h_ratio    = 0.92f;   // 框高  / 画面高  上限
    float bad_area_ratio = 0.35f;   // 框面积/ 画面面积 上限
    float wide_ratio     = 0.50f;   // "宽且扁"判据: 宽度占比
    float max_aspect     = 0.60f;   // "宽且扁"判据: h/w 上限
};

// 判断是不是"扁宽型"误检框
bool is_bad_box(const YoloBox& b, int fw, int fh, const GeoFilterCfg& cfg);

// 就地剔除异常框 —— 必须在交给跟踪器之前调用
void filter_bad_boxes(std::vector<YoloBox>& boxes, int fw, int fh, const GeoFilterCfg& cfg);

#endif  // RK_YOLO_POST_H_
