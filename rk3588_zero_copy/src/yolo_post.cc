// yolo_post.cc —— YOLOv8 后处理 (移植自 AI 算法组 rknn_cpp/rknn_yolov8_infer.cc)
#include "yolo_post.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

// 瑞芯微 fork 9 输出的尺度顺序: P3(stride8) -> P4(stride16) -> P5(stride32)
const int kStrides[3] = {8, 16, 32};

float iou_of(const YoloBox& a, const YoloBox& b) {
    float ix1 = std::max(a.x1, b.x1), iy1 = std::max(a.y1, b.y1);
    float ix2 = std::min(a.x2, b.x2), iy2 = std::min(a.y2, b.y2);
    float iw = ix2 - ix1, ih = iy2 - iy1;
    if (iw <= 0 || ih <= 0) return 0.f;
    float inter = iw * ih;
    float area_a = (a.x2 - a.x1) * (a.y2 - a.y1);
    float area_b = (b.x2 - b.x1) * (b.y2 - b.y1);
    float uni = area_a + area_b - inter;
    return uni > 0 ? inter / uni : 0.f;
}

// DFL 解码: 对 16 个 logit 做 softmax, 与 [0..15] 加权求期望
//   base      : 该 anchor 在 tensor 里的起点 (即 h*W+w)
//   grid_len  : H*W, 通道间隔
//   box[0..3] : l, t, r, b (格点单位)
// 注: 这里减掉了最大值做数值稳定处理, 与不减最大值的写法数学等价
//     (softmax 平移不变), 但 exp() 不会溢出。
void compute_dfl(const OutTensor& t, uint32_t base, uint32_t grid_len, float box[4]) {
    for (int b = 0; b < 4; ++b) {
        float e[RK_DFL_LEN];
        float max_v = -3.0e38f;
        for (int i = 0; i < RK_DFL_LEN; ++i) {
            float v = t.at(base + static_cast<uint32_t>(b * RK_DFL_LEN + i) * grid_len);
            if (v > max_v) max_v = v;
        }
        float sum = 0.f;
        for (int i = 0; i < RK_DFL_LEN; ++i) {
            e[i] = expf(t.at(base + static_cast<uint32_t>(b * RK_DFL_LEN + i) * grid_len) - max_v);
            sum += e[i];
        }
        float acc = 0.f;
        for (int i = 0; i < RK_DFL_LEN; ++i) acc += (e[i] / sum) * static_cast<float>(i);
        box[b] = acc;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 分类别 NMS
// ---------------------------------------------------------------------------
void nms_per_class(std::vector<YoloBox>& objs, float conf_thr, float iou_thr) {
    std::vector<YoloBox> out;
    int max_cls = 0;
    for (const auto& o : objs) max_cls = std::max(max_cls, o.cls_id);

    for (int c = 0; c <= max_cls; ++c) {
        std::vector<YoloBox> cand;
        for (auto& o : objs)
            if (o.cls_id == c && o.score >= conf_thr) cand.push_back(o);
        if (cand.empty()) continue;
        std::sort(cand.begin(), cand.end(),
                  [](const YoloBox& x, const YoloBox& y) { return x.score > y.score; });

        std::vector<bool> dead(cand.size(), false);
        for (size_t i = 0; i < cand.size(); i++) {
            if (dead[i]) continue;
            out.push_back(cand[i]);
            for (size_t j = i + 1; j < cand.size(); j++) {
                if (dead[j]) continue;
                if (iou_of(cand[i], cand[j]) > iou_thr) dead[j] = true;
            }
        }
    }
    std::sort(out.begin(), out.end(),
              [](const YoloBox& x, const YoloBox& y) { return x.score > y.score; });
    objs.swap(out);
}

// ---------------------------------------------------------------------------
// 形态 A: 标准导出 (单输出)
//   ch0-3 = cx,cy,w,h (像素)   ch4+ = 概率(已 sigmoid, 这里不再 sigmoid)
// ---------------------------------------------------------------------------
std::vector<YoloBox> post_process_single_output(const OutTensor& t, uint32_t num_anchors,
                                                 int nc, float conf_thr, float nms_thr) {
    std::vector<YoloBox> objs;
    objs.reserve(1024);
    for (uint32_t a = 0; a < num_anchors; a++) {
        int   best_cls = -1;
        float best_scr = -1.f;
        for (int c = 0; c < nc; c++) {
            /* CHW 索引: 第 c 个通道的第 a 个 anchor */
            float s = t.at(static_cast<uint32_t>(4 + c) * num_anchors + a);
            if (s > best_scr) { best_scr = s; best_cls = c; }
        }
        if (best_cls < 0 || best_scr < conf_thr) continue;

        float cx = t.at(0 * num_anchors + a);
        float cy = t.at(1 * num_anchors + a);
        float w  = t.at(2 * num_anchors + a);
        float h  = t.at(3 * num_anchors + a);

        YoloBox o;
        o.cls_id = best_cls;
        o.score  = best_scr;
        o.x1 = cx - w * 0.5f;
        o.y1 = cy - h * 0.5f;
        o.x2 = cx + w * 0.5f;
        o.y2 = cy + h * 0.5f;
        objs.push_back(o);
    }
    nms_per_class(objs, conf_thr, nms_thr);
    return objs;
}

// ---------------------------------------------------------------------------
// 形态 B: 瑞芯微 fork 导出 (9 输出), 顺序 [box, cls, sum] * 3 尺度
//   DFL 在 CPU 上算: exp_softmax(16 个 logit) 与 [0..15] 加权求期望
// ---------------------------------------------------------------------------
std::vector<YoloBox> post_process_fork_9out(const OutTensor* outs, int n_out, int nc,
                                            float conf_thr, float nms_thr) {
    std::vector<YoloBox> objs;
    objs.reserve(1024);

    for (int s = 0; s < 3; s++) {
        if (s * 3 + 2 >= n_out) break;
        const OutTensor& bt = outs[s * 3 + 0];       // box: [1, 4*DFL_LEN, H, W]
        const OutTensor& ct = outs[s * 3 + 1];       // cls: [1, nc, H, W]
        const OutTensor& st = outs[s * 3 + 2];       // sum: [1, 1, H, W]

        if (bt.n_dims < 3 || ct.n_dims < 3 || st.n_dims < 3) continue;

        uint32_t H = bt.dims[bt.n_dims - 2];
        uint32_t W = bt.dims[bt.n_dims - 1];
        uint32_t grid_len = H * W;
        int stride = kStrides[s];
        if (grid_len == 0) continue;

        for (uint32_t i = 0; i < H; i++) {
            for (uint32_t j = 0; j < W; j++) {
                uint32_t off = i * W + j;

                /* 1) 用 sum 通道快速预过滤 (sum 是概率和, 直接比 conf) */
                float sum_v = st.at(off);
                if (sum_v <= conf_thr) continue;

                /* 2) 选最大类 (cls 已是概率, 不再 sigmoid) */
                int   best_cls = -1;
                float best_scr = -1.f;
                for (int c = 0; c < nc; c++) {
                    float v = ct.at(static_cast<uint32_t>(c) * grid_len + off);
                    if (v > best_scr) { best_scr = v; best_cls = c; }
                }
                if (best_cls < 0 || best_scr < conf_thr) continue;

                /* 3) DFL 解码 */
                float d[4];
                compute_dfl(bt, off, grid_len, d);

                float cx = (static_cast<float>(j) + 0.5f) * static_cast<float>(stride);
                float cy = (static_cast<float>(i) + 0.5f) * static_cast<float>(stride);
                YoloBox o;
                o.cls_id = best_cls;
                o.score  = best_scr;
                o.x1 = cx - d[0];
                o.y1 = cy - d[1];
                o.x2 = cx + d[2];
                o.y2 = cy + d[3];
                objs.push_back(o);
            }
        }
    }
    nms_per_class(objs, conf_thr, nms_thr);
    return objs;
}

// ---------------------------------------------------------------------------
// 形态 C: box / cls 双输出 (算法组的 ebike_int8.rknn)
//   box = [1, 4, num_anchors]  cx,cy,w,h (像素, 模型输入尺度)
//   cls = [1, nc, num_anchors] 概率(已 sigmoid, 这里不再 sigmoid)
//
// ★ 与形态 A 的唯一区别: box 和 cls 在【两个不同的张量】里, 各自有独立的量化
//   scale(box≈2.5, cls≈1/255)。所以读法还是 CHW: 第 c 个通道的第 a 个 anchor
//   -> cls.at(c * num_anchors + a)。坐标系也和形态 A 一样是模型输入像素。
// ---------------------------------------------------------------------------
std::vector<YoloBox> post_process_dual_output(const OutTensor& box, const OutTensor& cls,
                                               uint32_t num_anchors, int nc,
                                               float conf_thr, float nms_thr) {
    std::vector<YoloBox> objs;
    if (num_anchors == 0 || nc <= 0) return objs;
    objs.reserve(1024);

    /* ---------------------------------------------------------------------
     * ★ 数值自检(本项目血泪教训, 务必保留)
     *
     *   cls 通道是 sigmoid 概率, 合法取值必然落在 (0, 1]。所以:
     *     cls_max > 1.01  → 布局串了(cls 里混进了框坐标)。sigmoid 不可能 >1,
     *                      所以这个判据**永远不会误报**。
     *     cls_max == 0    → 概率被【整体清零】。
     *        成因: ① 输入喂错了(用了 FLOAT32 / 除过 255 / 转了 NCHW)
     *              ② 模型本身坏了
     *        实测: 喂 FLOAT32 时 box 通道看着完全正常(max 639),
     *              但 cls 通道恒为 0 → 一个框都检不出, 而没有任何报错。
     *
     *   ⚠️ cls_max==0 也可能是【正常】的: 镜头前确实没有人也没有车时,
     *      所有 anchor 的概率都很小, int8 量化后(尺度≈1/255)可能全部落成 0。
     *      所以**不能只看单帧** —— 只有【连续 N 帧】全 0 才判定为故障,
     *      这样"空场景"不会误报, 而真正的故障 1 秒内就会暴露。
     *
     *   失败时返回空框(而不是继续用坏数据): 空框只是"这一帧没结果",
     *   坏数据会流进跟踪器和测速, 产出看起来正常但完全错误的结论。
     * ------------------------------------------------------------------- */
    constexpr int kZeroFramesToAlarm = 30;   /* 连续 30 帧(约 1 秒)全 0 才报警 */
    {
        float cls_max = 0.f, box_max = 0.f;
        for (uint32_t a = 0; a < num_anchors; a++) {
            for (int c = 0; c < nc; c++)
                cls_max = std::max(cls_max, cls.at((uint32_t)c * num_anchors + a));
            for (int c = 0; c < 4; c++)
                box_max = std::max(box_max, box.at((uint32_t)c * num_anchors + a));
        }
        static bool reported = false;
        static int  zero_frames = 0;

        if (cls_max > 1.01f) {
            if (!reported) {
                reported = true;
                fprintf(stderr,
                        "[自检失败] 形态C: cls 通道最大值=%.4f > 1 → 布局串位"
                        "(cls 里混进了框坐标)。\n"
                        "          检查: cls 张量是不是 outputs[1]? nc 取的是 cls.dims[n_dims-2]?\n"
                        "          ★ 这个判据不会误报(sigmoid 不可能 >1), 出现就是真问题。\n",
                        cls_max);
            }
            return objs;
        }
        if (box_max <= 0.f) {
            if (!reported) {
                reported = true;
                fprintf(stderr, "[自检失败] 形态C: box 通道最大值=0, 模型没有任何输出。\n");
            }
            return objs;
        }

        if (cls_max <= 0.0f) {
            /* 连续全 0 才判故障: 单帧为 0 可能只是画面里真的没人没车 */
            if (++zero_frames == kZeroFramesToAlarm && !reported) {
                reported = true;
                fprintf(stderr,
                        "[自检失败] 形态C: 连续 %d 帧 cls 通道最大值=0 → 概率被清零!\n"
                        "          两个可能: ① 输入喂错了(用了 FLOAT32 / 除过 255 / 转了 NCHW)\n"
                        "                    ② 模型本身有问题\n"
                        "          正确喂法: uint8 + RGB + NHWC + 原始 0~255 像素\n"
                        "          (单帧为 0 不报警: 画面里没人没车时也会是 0)\n",
                        zero_frames);
            }
            if (zero_frames >= kZeroFramesToAlarm)
                return objs;      /* 已判定故障, 不再产出框 */
        } else {
            zero_frames = 0;     /* 只要有一帧非 0 就复位 */
            if (!reported) {
                reported = true;
                fprintf(stderr,
                        "[自检] 形态C: nc=%d num_anchors=%u cls_max=%.4f (须 ∈(0,1]) "
                        "box_max=%.1f  ← 正常\n", nc, num_anchors, cls_max, box_max);
            }
        }
    }

    for (uint32_t a = 0; a < num_anchors; a++) {
        int   best_cls = -1;
        float best_scr = -1.f;
        for (int c = 0; c < nc; c++) {
            /* CHW 索引: 第 c 个通道的第 a 个 anchor */
            float s = cls.at(static_cast<uint32_t>(c) * num_anchors + a);
            if (s > best_scr) { best_scr = s; best_cls = c; }
        }
        if (best_cls < 0 || best_scr < conf_thr) continue;

        float cx = box.at(0 * num_anchors + a);
        float cy = box.at(1 * num_anchors + a);
        float w  = box.at(2 * num_anchors + a);
        float h  = box.at(3 * num_anchors + a);

        YoloBox o;
        o.cls_id = best_cls;
        o.score  = best_scr;
        o.x1 = cx - w * 0.5f;
        o.y1 = cy - h * 0.5f;
        o.x2 = cx + w * 0.5f;
        o.y2 = cy + h * 0.5f;
        objs.push_back(o);
    }
    nms_per_class(objs, conf_thr, nms_thr);
    return objs;
}

// ---------------------------------------------------------------------------
void BoxMapper::apply(std::vector<YoloBox>& boxes) const {
    for (auto& b : boxes) {
        if (mode == LETTERBOX) {
            b.x1 = (b.x1 - static_cast<float>(pad_x)) * inv_scale;
            b.x2 = (b.x2 - static_cast<float>(pad_x)) * inv_scale;
            b.y1 = (b.y1 - static_cast<float>(pad_y)) * inv_scale;
            b.y2 = (b.y2 - static_cast<float>(pad_y)) * inv_scale;
        } else {
            b.x1 *= scale_x;
            b.x2 *= scale_x;
            b.y1 *= scale_y;
            b.y2 *= scale_y;
        }
        if (img_w > 0) {
            b.x1 = std::max(0.f, std::min(b.x1, static_cast<float>(img_w)));
            b.x2 = std::max(0.f, std::min(b.x2, static_cast<float>(img_w)));
        }
        if (img_h > 0) {
            b.y1 = std::max(0.f, std::min(b.y1, static_cast<float>(img_h)));
            b.y2 = std::max(0.f, std::min(b.y2, static_cast<float>(img_h)));
        }
    }
}

// ---------------------------------------------------------------------------
std::vector<YoloBox> yolov8_postprocess(const OutTensor* outs, int n_out,
                                         float conf_thr, float nms_thr,
                                         const BoxMapper& mapper) {
    std::vector<YoloBox> objs;
    if (!outs || n_out <= 0) return objs;

    int nc = 0;
    if (n_out == 1) {
        // 形态 A: [1, 4+nc, num_anchors]
        const OutTensor& a = outs[0];
        if (a.n_dims < 2) return objs;
        uint32_t num_anchors = a.dims[a.n_dims - 1];
        nc = static_cast<int>(a.dims[a.n_dims - 2]) - 4;
        objs = post_process_single_output(a, num_anchors, nc, conf_thr, nms_thr);
    } else if (n_out == 2) {
        // 形态 C: box [1,4,num_anchors] + cls [1,nc,num_anchors]
        const OutTensor& b = outs[0];
        const OutTensor& c = outs[1];
        if (b.n_dims < 2 || c.n_dims < 2) return objs;
        uint32_t num_anchors = b.dims[b.n_dims - 1];
        // ★ cls 是 [1, nc, num_anchors] -> nc 在倒数第 2 维
        //   (形态 B 的 cls 是 [1, nc, H, W] -> nc 在倒数第 3 维, 别搞混)
        nc = static_cast<int>(c.dims[c.n_dims - 2]);
        if (b.dims[b.n_dims - 2] != 4) {
            fprintf(stderr,
                    "[警告] 双输出但 box 通道数=%u (期望 4), 不是形态 C, 结果可能不对\n",
                    b.dims[b.n_dims - 2]);
        }
        objs = post_process_dual_output(b, c, num_anchors, nc, conf_thr, nms_thr);
    } else {
        // 形态 B: 9 输出, nc 来自 cls 张量 [1, nc, H, W]
        const OutTensor& c = outs[1];
        if (c.n_dims < 3) return objs;
        nc = static_cast<int>(c.dims[c.n_dims - 3]);
        objs = post_process_fork_9out(outs, n_out, nc, conf_thr, nms_thr);
    }
    mapper.apply(objs);
    return objs;
}

// ---------------------------------------------------------------------------
// 模块 A：几何先验过滤
// ---------------------------------------------------------------------------
bool is_bad_box(const YoloBox& b, int fw, int fh, const GeoFilterCfg& cfg) {
    const float bw = b.x2 - b.x1;
    const float bh = b.y2 - b.y1;
    if (bw <= 4.f || bh <= 4.f) return true;                 // 退化框
    if (bw > cfg.bad_w_ratio * fw) return true;              // 几乎铺满画面
    if (bh > cfg.bad_h_ratio * fh) return true;
    if (bw * bh > cfg.bad_area_ratio * fw * fh) return true;
    // 又宽又扁: 真正的车框不会是这个比例
    if (bw > cfg.wide_ratio * fw && bh / std::max(bw, 1e-6f) < cfg.max_aspect)
        return true;
    return false;
}

void filter_bad_boxes(std::vector<YoloBox>& boxes, int fw, int fh, const GeoFilterCfg& cfg) {
    if (fw <= 0 || fh <= 0) return;
    boxes.erase(std::remove_if(boxes.begin(), boxes.end(),
                               [&](const YoloBox& b) { return is_bad_box(b, fw, fh, cfg); }),
                boxes.end());
}
