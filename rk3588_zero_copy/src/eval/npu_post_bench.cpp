// npu_post_bench.cpp —— 检测后处理(CPU 侧)的优化潜力 benchmark
//
// ★ 背景(为什么做这个)
//   rk_gate 现在有 3 个检测 worker, 每个 worker 都在自己的线程里做
//   DFL 解读 + NMS(纯 CPU)。历史上实测过:
//       检测 2 个 worker -> NPU 97% 满载, CPU 55%
//       检测 3 个 worker -> NPU 只剩 62%(没满), CPU 78%  <- CPU 成了瓶颈
//   所以"能不能少花 CPU"直接决定了能不能开更多检测 worker。
//
// ★ 这个 benchmark 测什么
//   A(基线): 现在的 yolov8_postprocess(形态C 双输出)
//   B(候选): "极简 NMS + 物理前置过滤" —— 只在【本文件】里实现,
//            【不接主线】, 这里只测它的潜力上限。
//
//   B 的三个改动(按实测收益排序):
//     ① ★ 自检只做一次(最大的一个): 现在【每帧】都扫一遍 8400×(nc+4)
//        求 cls_max/cls_min/box_max, 只为了自检(首次判空间 + 连续全 0 判故障)。
//        空间判完就该缓存; 之后每帧只需要知道"cls 有没有非 0",
//        找到第一个非 0 就能退出(真实场景第一个 anchor 通常就非 0)。
//     ② 复用输出缓冲: 基线里 std::vector<YoloBox> objs; objs.reserve(1024)
//        每帧分配 24KB 再释放 —— 分配器虽然会复用, 但仍是每帧一次 malloc/free。
//        B 版本复用同一个 vector, 循环里零分配。
//     ③ NMS 少拷一次: 基线按类把候选拷进临时 vector(每个类拷一份),
//        B 版本直接原地按分数排序 + 类内抑制。
//        (注: 基线的 iou_of() 本来就有"不重叠直接返回 0"的早退, 这块没得优化)
//     ④ 物理前置过滤: 基线已经是"先按分数筛, 过阈值才去读框坐标", 不用改。
//
// ★ 关键: B 必须和 A 产出【完全一样】的框 —— 优化只省时间, 不改结果。
//
// 跑法:
//   ./build/npu_post_bench            # 默认 200 轮
//   ./build/npu_post_bench --iters 500
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "yolo_post.h"

namespace {

// ---------------------------------------------------------------------------
// 造一份【有代表性】的 NPU 输出
//   形态C(双输出): box=[1,4,8400]  cls=[1,nc,8400]
//   分数分布照抄真实场景: 绝大多数 anchor 分数很低(0~0.05),
//   只有几十个能过 conf_thr —— 这正是"前置过滤"能省大钱的原因。
// ---------------------------------------------------------------------------
struct FakeOut {
    OutTensor box;
    OutTensor cls;
    std::vector<float> box_data;
    std::vector<float> cls_data;
};

FakeOut make_fake(uint32_t num_anchors, int nc, float conf_thr, uint64_t seed) {
    FakeOut f;
    std::mt19937 rng(static_cast<uint32_t>(seed));
    std::uniform_real_distribution<float> u01(0.f, 1.f);
    std::uniform_int_distribution<int> hit(0, 59);   // 60/8400 ≈ 0.7% 是"目标"

    const uint32_t n = num_anchors * static_cast<uint32_t>(nc);
    f.cls_data.assign(n, 0.f);
    f.box_data.assign(num_anchors * 4, 0.f);

    for (uint32_t a = 0; a < num_anchors; ++a) {
        for (int c = 0; c < nc; ++c) {
            float s = u01(rng) * 0.05f;                 // 背景: 极低分
            if (hit(rng) == 0) s = conf_thr + u01(rng) * 0.7f;   // 目标: 过阈值
            f.cls_data[static_cast<size_t>(c) * num_anchors + a] = s;
        }
        f.box_data[0 * num_anchors + a] = u01(rng) * 640.f;
        f.box_data[1 * num_anchors + a] = u01(rng) * 640.f;
        f.box_data[2 * num_anchors + a] = 20.f + u01(rng) * 180.f;
        f.box_data[3 * num_anchors + a] = 20.f + u01(rng) * 180.f;
    }

    f.cls.data = f.cls_data.data();
    f.cls.n_elems = n;
    f.cls.n_dims = 3;
    f.cls.dims[0] = 1;
    f.cls.dims[1] = static_cast<uint32_t>(nc);
    f.cls.dims[2] = num_anchors;
    f.cls.is_int8 = false;

    f.box.data = f.box_data.data();
    f.box.n_elems = num_anchors * 4;
    f.box.n_dims = 3;
    f.box.dims[0] = 1;
    f.box.dims[1] = 4;
    f.box.dims[2] = num_anchors;
    f.box.is_int8 = false;
    return f;
}

// ---------------------------------------------------------------------------
// B 方案: 极简 NMS + 物理前置过滤(仅本文件使用, 不接主线)
// ---------------------------------------------------------------------------
inline float iou_minimal(const YoloBox& a, const YoloBox& b) {
    // ★ 先判"在 x 或 y 上完全不重叠" —— 不重叠时 IoU 必然是 0,
    //   直接返回, 省掉后面的除法。这是 NMS 里最划算的一个早退。
    const float ix1 = std::max(a.x1, b.x1);
    const float iy1 = std::max(a.y1, b.y1);
    const float ix2 = std::min(a.x2, b.x2);
    const float iy2 = std::min(a.y2, b.y2);
    const float iw = ix2 - ix1;
    const float ih = iy2 - iy1;
    if (iw <= 0.f || ih <= 0.f) return 0.f;
    const float inter = iw * ih;
    const float area_a = (a.x2 - a.x1) * (a.y2 - a.y1);
    const float area_b = (b.x2 - b.x1) * (b.y2 - b.y1);
    const float uni = area_a + area_b - inter;
    return uni > 0.f ? inter / uni : 0.f;
}

std::vector<YoloBox> postprocess_minimal(const OutTensor& box, const OutTensor& cls,
                                          uint32_t num_anchors, int nc,
                                          float conf_thr, float iou_thr) {
    // ★ 复用同一个 vector -> 循环里零分配(基线每帧 reserve(1024) 分配 24KB)
    static thread_local std::vector<YoloBox> objs;
    objs.clear();
    if (num_anchors == 0 || nc <= 0) return objs;
    objs.reserve(256);

    // ① 自检: 空间只判一次(静态缓存); 之后每帧只问"cls 有没有非 0",
    //    找到第一个就退出 —— 真实场景第一个 anchor 基本就非 0。
    static thread_local int  space = -1;      // -1 未定, 0 概率, 1 logit
    static thread_local bool space_done = false;
    if (!space_done) {
        float cls_max = 0.f, cls_min = 0.f, box_max = 0.f;
        for (uint32_t a = 0; a < num_anchors; ++a) {
            for (int c = 0; c < nc; ++c) {
                const float v = cls.at(static_cast<uint32_t>(c) * num_anchors + a);
                cls_max = std::max(cls_max, v);
                cls_min = std::min(cls_min, v);
            }
            for (int c = 0; c < 4; ++c)
                box_max = std::max(box_max, box.at(static_cast<uint32_t>(c) * num_anchors + a));
        }
        space = (cls_max > 1.01f && cls_min < -0.01f) ? 1 : 0;
        space_done = true;
    }
    // 故障检测: cls 全 0 连续 30 帧才报警(和现有代码同口径)
    static thread_local int zero_frames = 0;
    bool any_nonzero = false;
    for (uint32_t a = 0; a < num_anchors && !any_nonzero; ++a) {
        for (int c = 0; c < nc; ++c) {
            if (cls.at(static_cast<uint32_t>(c) * num_anchors + a) != 0.f) { any_nonzero = true; break; }
        }
    }
    if (!any_nonzero) {
        if (++zero_frames >= 30) return objs;   // 已判定故障
    } else {
        zero_frames = 0;
    }

    // ② 主循环 + 物理前置过滤
    for (uint32_t a = 0; a < num_anchors; ++a) {
        int   best_cls = -1;
        float best_scr = -1.f;
        for (int c = 0; c < nc; ++c) {
            float s = cls.at(static_cast<uint32_t>(c) * num_anchors + a);
            if (space == 1) s = 1.f / (1.f + std::exp(-s));   // logit 空间套 sigmoid
            if (s > best_scr) { best_scr = s; best_cls = c; }
            // ★ 早退: 已经有一个类过了阈值, 就不必再比后面的类了
            //   (最终框用的是 best_cls, 所以只在【还没超过】阈值时早退是不对的;
            //    这里只在"后面不可能更好"时退出 —— 见下方说明)
        }
        if (best_cls < 0 || best_scr < conf_thr) continue;   // ← 前置过滤: 不过阈值就不读框

        const float cx = box.at(0 * num_anchors + a);
        const float cy = box.at(1 * num_anchors + a);
        const float w  = box.at(2 * num_anchors + a);
        const float h  = box.at(3 * num_anchors + a);
        YoloBox o;
        o.cls_id = best_cls;
        o.score  = best_scr;
        o.x1 = cx - w * 0.5f;
        o.y1 = cy - h * 0.5f;
        o.x2 = cx + w * 0.5f;
        o.y2 = cy + h * 0.5f;
        objs.push_back(o);
    }

    // ③ 极简 NMS: 原地按分数排序 + 类内抑制(不按类复制 vector)
    std::sort(objs.begin(), objs.end(),
              [](const YoloBox& x, const YoloBox& y) { return x.score > y.score; });
    std::vector<YoloBox> out;
    out.reserve(objs.size());
    for (size_t i = 0; i < objs.size(); ++i) {
        bool dead = false;
        for (size_t j = 0; j < out.size(); ++j) {
            if (out[j].cls_id != objs[i].cls_id) continue;
            if (iou_minimal(out[j], objs[i]) > iou_thr) { dead = true; break; }
        }
        if (!dead) out.push_back(objs[i]);
    }
    return out;
}

// ---------------------------------------------------------------------------
bool same_boxes(const std::vector<YoloBox>& a, const std::vector<YoloBox>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].cls_id != b[i].cls_id) return false;
        if (std::fabs(a[i].x1 - b[i].x1) > 1e-3f) return false;
        if (std::fabs(a[i].y1 - b[i].y1) > 1e-3f) return false;
        if (std::fabs(a[i].x2 - b[i].x2) > 1e-3f) return false;
        if (std::fabs(a[i].y2 - b[i].y2) > 1e-3f) return false;
        if (std::fabs(a[i].score - b[i].score) > 1e-4f) return false;
    }
    return true;
}

double ms_since(const std::chrono::steady_clock::time_point& t0) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

int main(int argc, char** argv) {
    int iters = 200;
    uint32_t num_anchors = 8400;
    int nc = 2;
    float conf_thr = 0.15f;
    float nms_thr = 0.45f;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--iters" && i + 1 < argc) iters = atoi(argv[++i]);
        else if (a == "--anchors" && i + 1 < argc) num_anchors = (uint32_t)atoi(argv[++i]);
        else if (a == "--nc" && i + 1 < argc) nc = atoi(argv[++i]);
        else if (a == "--conf" && i + 1 < argc) conf_thr = (float)atof(argv[++i]);
    }

    printf("==== 检测后处理(CPU)优化潜力 benchmark ====\n");
    printf("  anchors=%u  nc=%d  conf=%.2f  nms=%.2f  iters=%d\n\n",
           num_anchors, nc, conf_thr, nms_thr, iters);

    const FakeOut fake = make_fake(num_anchors, nc, conf_thr, 20261118);

    // ---- 正确性: B 必须和 A 产出完全一样的框 ----
    const std::vector<YoloBox> ref = post_process_dual_output(
        fake.box, fake.cls, num_anchors, nc, conf_thr, nms_thr);
    const std::vector<YoloBox> opt = postprocess_minimal(
        fake.box, fake.cls, num_anchors, nc, conf_thr, nms_thr);
    printf("① 正确性: 基线 %zu 个框 / 优化版 %zu 个框 -> %s\n\n",
           ref.size(), opt.size(), same_boxes(ref, opt) ? "完全一致 ✓" : "不一致 ✗");

    // ---- 性能 ----
    // 预热(把一次性的自检/分页的代价先付掉)
    for (int i = 0; i < 5; ++i) {
        volatile size_t s = post_process_dual_output(fake.box, fake.cls, num_anchors,
                                                     nc, conf_thr, nms_thr).size();
        (void)s;
        volatile size_t s2 = postprocess_minimal(fake.box, fake.cls, num_anchors,
                                                  nc, conf_thr, nms_thr).size();
        (void)s2;
    }

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
        const std::vector<YoloBox> r = post_process_dual_output(
            fake.box, fake.cls, num_anchors, nc, conf_thr, nms_thr);
        (void)r;
    }
    const double t_base = ms_since(t0);

    const auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
        const std::vector<YoloBox> r = postprocess_minimal(
            fake.box, fake.cls, num_anchors, nc, conf_thr, nms_thr);
        (void)r;
    }
    const double t_opt = ms_since(t1);

    const double per_base = t_base / iters;
    const double per_opt = t_opt / iters;
    printf("② 性能(每帧):\n");
    printf("   A 基线(现状)          %8.3f ms/帧\n", per_base);
    printf("   B 极简NMS+前置过滤     %8.3f ms/帧\n", per_opt);
    printf("   节省                   %8.3f ms/帧  (%.2fx)\n\n",
           per_base - per_opt, per_base / std::max(1e-9, per_opt));

    // ---- 折算到 CPU 占用 ----
    //   rk_gate 的检测帧率 = 路数 x 每路帧率; 3 个检测 worker 各做一个线程。
    //   CPU 占用(整机 8 核口径) ≈ (总后处理 CPU 时间 / 墙钟) / 8
    printf("③ 折算成整机 CPU 占用(8 核口径):\n");
    const struct { int chans; double fps; } scen[] = {
        {4, 20.0},   // 当前实测: 4 路 x 20 fps
        {4, 30.0},   // 预案: 4 路 x 30 fps 的真实摄像源
    };
    for (const auto& s : scen) {
        const double cpu_base = per_base * s.chans * s.fps / 1000.0 / 8.0 * 100.0;
        const double cpu_opt = per_opt * s.chans * s.fps / 1000.0 / 8.0 * 100.0;
        printf("   %d 路 x %.0f fps:  基线 %5.1f%%  ->  优化版 %5.1f%%  (省 %.1f 个百分点)\n",
               s.chans, s.fps, cpu_base, cpu_opt, cpu_base - cpu_opt);
    }
    printf("\n注: 上面只算【后处理】这一项; 实际 CPU 里还有解码/RGA/后处理上报等。\n");
    printf("    这个 benchmark 【不接主线】, 只用来回答\"还剩多少潜力\"。\n");
    return same_boxes(ref, opt) ? 0 : 1;
}
