// yolo_post.cc —— YOLOv8 后处理 (移植自 AI 算法组 rknn_cpp/rknn_yolov8_infer.cc)
#include "yolo_post.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// 瑞芯微 fork 9 输出的尺度顺序: P3(stride8) -> P4(stride16) -> P5(stride32)
const int kStrides[3] = {8, 16, 32};

// ===========================================================================
// cls 空间自检 —— 防止"重复 sigmoid" / "漏了 sigmoid"
//
// ★ 为什么要做这件事
//
//   YOLOv8 的 cls 分支在【不同导出形态】下可能是概率, 也可能是 logit:
//     · 标准 Ultralytics 导出: 图里带 Sigmoid 节点 -> 输出是概率, 落在 [0,1]
//     · 某些裁剪过/重新量化过的导出: 把 Sigmoid 去掉 -> 输出是 logit(任意实数)
//
//   后处理如果【不管】是哪一种, 都会出硬错:
//     · 给【概率】再套一次 sigmoid -> 分数被压进 [0.5, 0.731],
//       真负样本也变 0.5, 于是 conf=0.25 形同虚设, 满屏都是框;
//     · 给【logit】不套 sigmoid -> 分数是负数或大数, 阈值同样完全失效。
//
// ★ 判据是硬的, 不靠猜:
//   sigmoid 的输出不可能 > 1, 所以 "cls 最大值 > 1" 一定【不是】概率空间。
//   于是首帧扫一遍 cls 通道:
//     max <= 1.0  -> 概率空间 -> 不套 sigmoid
//     max >  1.0  -> logit 空间 -> 套 sigmoid
//
// ★ 本工程三个模型的实测结论(2026-11-16):
//     ebike_int8.rknn   输出张量名就叫 "/model.22/Sigmoid_output_0",
//                       量化区间 ori_max=0.9699 -> 概率空间
//     best.rknn (fork9) cls 张量 ori_min=0.0 ori_max=0.8645 -> 概率空间
//   所以这段代码【一次都不会】改变现有模型的输出 —— 框数必须逐帧完全一致
//   (回归脚本: scripts/cls_space_regression.py)。
//   它防的是【以后】有人重新导出一个去掉 Sigmoid 的模型时, 静默错成满屏框。
//
// ★ 强制指定: 环境变量 RK_CLS_SPACE = auto(默认) | prob | logit
//   auto 判错了的时候可以手工钉死, 不用改代码。
// ===========================================================================

enum ClsSpace { kSpaceUndecided = 0, kSpaceProb = 1, kSpaceLogit = 2 };

inline float sigmoid_f(float x) {
    return 1.0f / (1.0f + std::exp(-x));
}

// 扫 cls 通道求最大值。base_ch = 第一个 cls 通道号(形态 A 的 cls 从第 4 通道起)。
float cls_max_over(const OutTensor& t, int base_ch, int n_ch, uint32_t ch_stride) {
    float m = 0.f;
    if (n_ch <= 0 || ch_stride == 0) return m;
    for (int c = 0; c < n_ch; ++c) {
        const uint32_t base = static_cast<uint32_t>(base_ch + c) * ch_stride;
        for (uint32_t i = 0; i < ch_stride; ++i) m = std::max(m, t.at(base + i));
    }
    return m;
}

// 扫 cls 通道求最小值(用来区分"logit"和"布局串位", 见形态 C 的注释)。
float cls_min_over(const OutTensor& t, int base_ch, int n_ch, uint32_t ch_stride) {
    float m = 0.f;
    bool first = true;
    if (n_ch <= 0 || ch_stride == 0) return m;
    for (int c = 0; c < n_ch; ++c) {
        const uint32_t base = static_cast<uint32_t>(base_ch + c) * ch_stride;
        for (uint32_t i = 0; i < ch_stride; ++i) {
            const float v = t.at(base + i);
            if (first) { m = v; first = false; }
            else m = std::min(m, v);
        }
    }
    return m;
}

// 环境变量强制指定(0=自动)。只读一次。
int forced_cls_space() {
    static const int v = []() {
        const char* e = getenv("RK_CLS_SPACE");
        if (!e || !*e) return kSpaceUndecided;
        if (strcmp(e, "prob") == 0 || strcmp(e, "probability") == 0) return kSpaceProb;
        if (strcmp(e, "logit") == 0 || strcmp(e, "logits") == 0) return kSpaceLogit;
        return kSpaceUndecided;
    }();
    return v;
}

// 判定并缓存。state 是每种形态各自的一份(用原子量, 多个 worker 线程会同时进来)。
// 返回 kSpaceProb 或 kSpaceLogit。
int cls_space_decide(std::atomic<int>& state, const char* form, float cls_max) {
    const int forced = forced_cls_space();
    if (forced != kSpaceUndecided) {
        int cur = state.load(std::memory_order_relaxed);
        if (cur == kSpaceUndecided) {
            state.store(forced, std::memory_order_relaxed);
            fprintf(stderr,
                    "[cls空间] %s: 由 RK_CLS_SPACE 强制指定为 %s (cls_max=%.4f)\n",
                    form, forced == kSpaceLogit ? "logit(套 sigmoid)" : "概率(不套 sigmoid)",
                    cls_max);
        }
        return forced;
    }

    int cur = state.load(std::memory_order_relaxed);
    if (cur != kSpaceUndecided) return cur;

    const int want = (cls_max > 1.0f) ? kSpaceLogit : kSpaceProb;
    int expect = kSpaceUndecided;
    if (state.compare_exchange_strong(expect, want)) {
        fprintf(stderr,
                "[cls空间] %s: cls_max=%.4f -> %s\n"
                "          %s\n",
                form, cls_max,
                want == kSpaceLogit ? "logit 空间 -> 后处理自动套 sigmoid"
                                    : "概率 空间 -> 后处理【不】套 sigmoid",
                want == kSpaceLogit
                    ? "★ 模型导出时去掉了 Sigmoid, 后处理已自动补偿;"
                    : "★ 模型输出本身就是 sigmoid 概率(常见的正确形态), 后处理不重复套。");
    }
    return expect == kSpaceUndecided ? want : expect;
}

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

// ===========================================================================
// DFL 解码: 对 16 个 logit 做 softmax, 与 [0..15] 加权求期望
//   base      : 该 anchor 在 tensor 里的起点 (即 h*W+w)
//   grid_len  : H*W, 通道间隔
//   box[0..3] : l, t, r, b (格点单位)
// 注: 这里减掉了最大值做数值稳定处理, 与不减最大值的写法数学等价
//     (softmax 平移不变), 但 exp() 不会溢出。
//
// ---------------------------------------------------------------------------
// ★★ DFL 的内存布局 —— 改这个函数之前【必须】先读这段 (2026-11-22 补)
//
//   瑞芯微 fork 导出的 box 输出张量是 [1, 4*reg_max, H, W] = [1, 64, H, W],
//   **NCHW** 布局 (RKNN 运行时按声明的 NCHW 原样返回, 实测同序相关系数 0.9975,
//   换成 NHWC 排列只有 0.005 —— 见 docs/task2_fork9_ab.md)。
//
//   64 个通道的含义是 **c = box * 16 + bin**:
//       c =  0..15  -> box0(l) 的 16 个 bin
//       c = 16..31  -> box1(t) 的 16 个 bin
//       c = 32..47  -> box2(r) 的 16 个 bin
//       c = 48..63  -> box3(b) 的 16 个 bin
//   这与 ultralytics 的 DFL 模块完全一致:
//       class DFL:  x.view(b, 4, self.c1, a).transpose(2, 1).softmax(1)
//       —— 64 通道被 view 成 (4, 16), 即 c = box*16 + bin。
//
//   ⇒ 同一个 anchor 的 16 个 bin 在内存里【相隔 grid_len】(NCHW 的通道维),
//     不是连续的 16 个! 所以第 (b*16+i) 个 bin 的地址是:
//
//         addr(bin) = base + (b*16 + bin) * grid_len        ← 本函数的写法
//
//     而【不是】 base + b*16*grid_len + bin(连续读法)。
//
//   ⚠ 踩过的坑: 参考工具 accuracy/acc_batch.cc 里的 decode_dfl_9out 用的是
//     【连续读法】, 于是它读到的其实是"同一个 channel 的连续 16 个 anchor 的
//     logits", 算出来的框是错的 —— 它和生产 compute_dfl 对不上, 一度让人
//     以为生产代码有 bug。2026-11-22 已把 acc_batch 那处也改成跨步读法。
//
//   ★ 顺带说清楚【单位】: softmax 在 16 个 bin 上做, 期望值天然落在 [0, 15],
//     所以解出来的距离【一定是格点单位】。而且把 logits 整体乘一个常数
//     不会改变 softmax 分布 —— 所以"导出时把 stride 烤进 logits"在数学上
//     不可能, stride 只能在解码【之后】乘(见 post_process_fork_9out)。
//     标准 ultralytics 导出也是这么做的(见 include/yolo_post.h:
//     "ch0..3 = cx, cy, w, h 单位=模型输入图的像素(已乘 stride)")。
// ===========================================================================
void compute_dfl(const OutTensor& t, uint32_t base, uint32_t grid_len, float box[4]) {
    for (int b = 0; b < 4; ++b) {
        float e[RK_DFL_LEN];
        float max_v = -3.0e38f;
        for (int i = 0; i < RK_DFL_LEN; ++i) {
            // ★ 跨步读: 第 (b*16+i) 个 bin 的地址 = base + (b*16+i)*grid_len
            //   (NCHW 布局下 16 个 bin 隔着一个 anchor 的距离)
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

    // ★ cls 空间自检(只判一次): cls 通道从第 4 个通道起。
    //   概率空间(本工程的常态)什么都不做; logit 空间才套 sigmoid。
    static std::atomic<int> g_space{kSpaceUndecided};
    int space = g_space.load(std::memory_order_relaxed);
    if (space == kSpaceUndecided && nc > 0 && num_anchors > 0) {
        space = cls_space_decide(g_space, "形态A(单输出)",
                                  cls_max_over(t, 4, nc, num_anchors));
    }

    for (uint32_t a = 0; a < num_anchors; a++) {
        int   best_cls = -1;
        float best_scr = -1.f;
        for (int c = 0; c < nc; c++) {
            /* CHW 索引: 第 c 个通道的第 a 个 anchor */
            float s = t.at(static_cast<uint32_t>(4 + c) * num_anchors + a);
            if (space == kSpaceLogit) s = sigmoid_f(s);
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

    // ★ cls 空间自检(只判一次): fork 9 输出的 cls 在 outs[s*3+1], 是 [1, nc, H, W]。
    //   三个尺度一起扫, 取最大值。
    static std::atomic<int> g_space{kSpaceUndecided};
    int space = g_space.load(std::memory_order_relaxed);
    if (space == kSpaceUndecided && nc > 0) {
        float mx = 0.f;
        for (int s = 0; s < 3 && s * 3 + 1 < n_out; ++s) {
            const OutTensor& ct = outs[s * 3 + 1];
            if (ct.n_dims < 3) continue;
            const uint32_t H = ct.dims[ct.n_dims - 2];
            const uint32_t W = ct.dims[ct.n_dims - 1];
            mx = std::max(mx, cls_max_over(ct, 0, nc, H * W));
        }
        space = cls_space_decide(g_space, "形态B(fork 9输出)", mx);
    }

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

                /* 1) 用 sum 通道快速预过滤 (sum 是概率和, 直接比 conf)
                 *    ★ 只有在【概率空间】下 sum 才有意义(sum = 各类别概率之和)。
                 *      logit 空间下 sum 是 logit 的和, 拿它比 conf 是错的,
                 *      所以 logit 空间下跳过这个预过滤, 靠下面的类别最大值来筛。 */
                if (space == kSpaceProb) {
                    float sum_v = st.at(off);
                    if (sum_v <= conf_thr) continue;
                }

                /* 2) 选最大类 (概率空间下 cls 已是概率, 不再 sigmoid) */
                int   best_cls = -1;
                float best_scr = -1.f;
                for (int c = 0; c < nc; c++) {
                    float v = ct.at(static_cast<uint32_t>(c) * grid_len + off);
                    if (space == kSpaceLogit) v = sigmoid_f(v);
                    if (v > best_scr) { best_scr = v; best_cls = c; }
                }
                if (best_cls < 0 || best_scr < conf_thr) continue;

                /* 3) DFL 解码 */
                float d[4];
                compute_dfl(bt, off, grid_len, d);

                // ★★ d[] 是【格点单位】(compute_dfl 的注释写明了: "l, t, r, b (格点单位)")。
                //   anchor 中心 cx/cy 已经是【像素】, 所以距离必须先 ×stride 换成像素,
                //   否则就是"像素 - 格点", 单位混用。
                //   2026-11-22 实测: 不乘的话框坐标平均偏 28~47 px(最大 382 px),
                //   框宽会小到只剩 1/4 ~ 1/8 —— 跟 acc_batch.cc:58-65 记的是同一个坑。
                const float st = static_cast<float>(stride);
                const float dl = d[0] * st;
                const float dt = d[1] * st;
                const float dr = d[2] * st;
                const float db = d[3] * st;

                float cx = (static_cast<float>(j) + 0.5f) * st;
                float cy = (static_cast<float>(i) + 0.5f) * st;
                YoloBox o;
                o.cls_id = best_cls;
                o.score  = best_scr;
                o.x1 = cx - dl;
                o.y1 = cy - dt;
                o.x2 = cx + dr;
                o.y2 = cy + db;
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

    // ★ cls 空间(概率 or logit)。判一次就定下来, 后面每帧直接用。
    //   形态 C 的 cls 张量是 [1, nc, num_anchors], 通道步长 = num_anchors。
    static std::atomic<int> g_space{kSpaceUndecided};
    int space = kSpaceProb;

    {
        float cls_max = 0.f, cls_min = 0.f, box_max = 0.f;
        bool  first = true;
        for (uint32_t a = 0; a < num_anchors; a++) {
            for (int c = 0; c < nc; c++) {
                const float v = cls.at((uint32_t)c * num_anchors + a);
                cls_max = std::max(cls_max, v);
                if (first) { cls_min = v; first = false; }
                else cls_min = std::min(cls_min, v);
            }
            for (int c = 0; c < 4; c++)
                box_max = std::max(box_max, box.at((uint32_t)c * num_anchors + a));
        }
        // ★ thread_local: 这两个是【每上下文】的自检状态, 不是进程级的。
        //   rk_gate 里有两个检测 worker(core1/core2)在各自的线程里调本函数,
        //   共用进程级 static 就是数据竞争(两个线程同时写 reported/zero_frames)。
        //   单线程的 rk_zero_copy 行为完全不变(thread_local 在单线程下等价于 static)。
        thread_local bool reported = false;
        thread_local int  zero_frames = 0;

        const int cached = g_space.load(std::memory_order_relaxed);
        if (cached == kSpaceLogit) {
            // 已经判定过是 logit 空间(模型导出时去掉了 Sigmoid) -> 直接套。
            space = kSpaceLogit;
        } else if (cls_max > 1.01f) {
            /* cls 最大值 > 1 -> 不可能是概率(sigmoid 输出不可能 >1)。
             *   但有两种可能, 用【最小值】区分:
             *     ① cls_min < 0 -> logit 空间。sigmoid 输出不可能为负,
             *        框坐标也不可能为负, 所以出现负值只可能是 logit。
             *        -> 后处理套 sigmoid, 正常工作。
             *     ② cls_min >= 0 -> 布局串位(cls 里混进了框坐标)。
             *        -> 保持原来的行为: 报警 + 本帧不产出框。 */
            if (cls_min < -0.01f) {
                space = kSpaceLogit;
                cls_space_decide(g_space, "形态C(双输出)", cls_max);
            } else {
                if (!reported) {
                    reported = true;
                    fprintf(stderr,
                            "[自检失败] 形态C: cls 通道最大值=%.4f > 1 且最小值=%.4f >= 0 "
                            "→ 布局串位(cls 里混进了框坐标)。\n"
                            "          检查: cls 张量是不是 outputs[1]? nc 取的是 cls.dims[n_dims-2]?\n"
                            "          ★ 概率空间不可能出现这个组合(sigmoid 输出既不会 >1 也不会 <0)。\n",
                            cls_max, cls_min);
                }
                return objs;
            }
        } else {
            space = cls_space_decide(g_space, "形态C(双输出)", cls_max);
        }
        if (box_max <= 0.f) {
            if (!reported) {
                reported = true;
                fprintf(stderr, "[自检失败] 形态C: box 通道最大值=0, 模型没有任何输出。\n");
            }
            return objs;
        }

        if (space == kSpaceProb && cls_max <= 0.0f) {
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
            // ★ 只有 logit 空间才套 sigmoid; 概率空间原样用(不重复套)。
            if (space == kSpaceLogit) s = sigmoid_f(s);
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
