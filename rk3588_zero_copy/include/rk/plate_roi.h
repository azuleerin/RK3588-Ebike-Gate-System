// rk/plate_roi.h —— 车牌 ROI 几何 + 识别结果缓存(多路部署共用)
//
// ★ 为什么要抽出来
//
//   单流版 rk_zero_copy 的 main.cc 里有一份, 4 路版 rk_gate 也需要同一份。
//   ROI 几何是【精度相关】的代码: 差一个像素, 字形就变形, 识别率就掉。
//   两处各写一份的话, 改了一处忘了另一处, 就会出现"两路 ROI 不一样"的
//   灵异问题 —— 所以必须只有一份实现。
//
//   (main.cc 里原本是 static 函数; 本文件是它们的搬家, 逻辑逐字未改。)
#ifndef RK_PLATE_ROI_H_
#define RK_PLATE_ROI_H_

#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <utility>

#include "yolo_post.h"

namespace rk {

// 两行车牌: 上行试 3 个切分点, 相邻候选间隔 5%
constexpr int   kRowSplitTrials = 3;
constexpr float kRowSplitStep = 0.05f;
// ROI 宽高比小于这个值就当成两行牌
constexpr float kTwoRowAspect = 0.62f;

// 文本里有没有汉字(UTF-8 里 >= 0xE0 的字节就是多字节字符的开头)。
// 车牌上行应该含省份字("苏""京""粤"...) —— 判"是不是两行牌/切在哪"时,
// 含汉字的候选优先。
inline bool text_has_cjk(const std::string& t) {
    for (size_t i = 0; i < t.size(); ++i) {
        if (static_cast<unsigned char>(t[i]) >= 0xE0) return true;
    }
    return false;
}

// 两个候选上行谁更好: 含汉字 > 字符多 > 分数高
//
// ★ 判据优先级不能按分数排: 实测三个候选分别是
//   "A"(0.46) / "苏A"(0.46) / "AL"(0.53),
//   按分数挑会选中又长又自信的垃圾 "AL", 把对的 "苏A" 丢掉。
inline bool better_top_row(const std::string& a, float a_score,
                            const std::string& b, float b_score) {
    const bool ca = text_has_cjk(a), cb = text_has_cjk(b);
    if (ca != cb) return ca;                          // 1) 含汉字的优先
    if (a.size() != b.size()) return a.size() > b.size();   // 2) 字符多的优先
    return a_score > b_score;                         // 3) 分数高的优先
}

// 把 ROI 夹回画面内; 夹完太小就返回 false。
inline bool clamp_roi(int* x, int* y, int* w, int* h, int fw, int fh,
                      int min_w = 1, int min_h = 1) {
    if (!x || !y || !w || !h) return false;
    if (fw <= 0 || fh <= 0) return false;

    int rx = *x, ry = *y, rw = *w, rh = *h;
    if (rw < 0) rw = 0;
    if (rh < 0) rh = 0;

    // 左上角拉回画面内(同时把尺寸一起挪, 保持右边界)
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    // 右下角裁回画面内
    if (rx > fw) rx = fw;
    if (ry > fh) ry = fh;
    if (rx + rw > fw) rw = fw - rx;
    if (ry + rh > fh) rh = fh - ry;

    if (rw < min_w || rh < min_h) return false;   // 夹完没了 -> 放弃

    *x = rx; *y = ry; *w = rw; *h = rh;
    return true;
}

// ---------------------------------------------------------------------------
// 车牌 ROI 几何先验: 从"车体框(ebike)"切出"车牌区域"
//
// 板端只有电动车框, 没有车牌框 —— 车牌挂在车尾, 也就是框的下部。
// 所以 ROI 用【相对车体框】的比例来取, 而不是绝对像素:
//
//   水平: 宽度 = 框宽 * roi_ratio,  中心 = 框中心 + 框宽 * roi_shift
//   垂直: 顶边 = 框顶 + 框高 * roi_y,  再往下延伸 框高 * roi_below
//         高度 = 框高 * roi_height
//
// 返回 false 表示这个框切不出可用 ROI。
inline bool plate_roi_from_box(const YoloBox& b, int fw, int fh,
                                float ratio, float roi_y, float height,
                                float shift, float below,
                                int* rx, int* ry, int* rw, int* rh) {
    const float bw = b.x2 - b.x1;
    const float bh = b.y2 - b.y1;
    if (bw < 4.f || bh < 4.f) return false;

    float w = bw * ratio;
    float h = bh * height;
    if (w < 8.f) w = 8.f;
    if (h < 6.f) h = 6.f;

    const float cx = 0.5f * (b.x1 + b.x2) + bw * shift;
    float left = cx - w * 0.5f;
    float top = b.y1 + bh * roi_y + bh * below;

    // 夹到画面内
    if (left < 0.f) { w += left; left = 0.f; }
    if (top < 0.f) { h += top; top = 0.f; }
    if (left + w > fw) w = static_cast<float>(fw) - left;
    if (top + h > fh) h = static_cast<float>(fh) - top;
    if (w < 8.f || h < 6.f) return false;

    *rx = static_cast<int>(lround(left));
    *ry = static_cast<int>(lround(top));
    *rw = static_cast<int>(lround(w));
    *rh = static_cast<int>(lround(h));
    // 最后一道整型硬夹紧(lround 的取整误差 + 贴边框都可能越界)
    return clamp_roi(rx, ry, rw, rh, fw, fh, 8, 6);
}

// ---------------------------------------------------------------------------
// 车牌识别结果缓存: 同一条 track 在 TTL 内不重复跑 NPU
//
// ★ 为什么需要: 每次重新识别要占 ~24ms NPU(比检测本身还贵),
//   而 2 秒的 TTL 在真实车流里命中率只有 42% —— 等于一半以上的识别是白跑的。
//   提到 5 秒后命中率显著上升, NPU 直接省下来, 【精度零损失】。
// ---------------------------------------------------------------------------
class PlateTrackCache {
public:
    // 命中返回 true, *plate / *cached 被填好
    bool get(int track_id, int64_t now_ms, int64_t ttl_ms,
             std::string* plate, bool* cached) {
        if (cached) *cached = false;
        auto it = map_.find(track_id);
        if (it != map_.end() && ttl_ms > 0 && (now_ms - it->second.second) < ttl_ms) {
            if (plate) *plate = it->second.first;
            if (cached) *cached = true;
            return true;
        }
        return false;
    }

    void put(int track_id, int64_t now_ms, int64_t ttl_ms, const std::string& plate) {
        if (ttl_ms <= 0) return;
        map_[track_id] = std::make_pair(plate, now_ms);
        for (auto i = map_.begin(); i != map_.end();) {
            if ((now_ms - i->second.second) >= ttl_ms) i = map_.erase(i);
            else ++i;
        }
    }

private:
    std::map<int, std::pair<std::string, int64_t>> map_;
};

}  // namespace rk

#endif  // RK_PLATE_ROI_H_
