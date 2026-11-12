// speed_track.cc —— 跟踪 + 测速
//
// 按架构组 docs/speed_stabilization_cpp.md 的骨架移植, 口径对齐 Python 侧
// speed_demo.py（后者已在 5 个视频 / 7 条真实轨迹上验证: 读数波动中位数 -77%）。
#include "speed_track.h"

#include <algorithm>
#include <cmath>

namespace rk {
namespace {

float box_iou(const YoloBox& a, const YoloBox& b) {
    float x1 = std::max(a.x1, b.x1), y1 = std::max(a.y1, b.y1);
    float x2 = std::min(a.x2, b.x2), y2 = std::min(a.y2, b.y2);
    float iw = x2 - x1, ih = y2 - y1;
    if (iw <= 0.f || ih <= 0.f) return 0.f;
    float inter = iw * ih;
    float area_a = (a.x2 - a.x1) * (a.y2 - a.y1);
    float area_b = (b.x2 - b.x1) * (b.y2 - b.y1);
    float uni = area_a + area_b - inter;
    return uni > 0.f ? inter / uni : 0.f;
}

}  // namespace

// ---------------------------------------------------------------------------
// 标定
// ---------------------------------------------------------------------------
bool SpeedCalib::to_metric(double gx, double gy, double* mx, double* my) const {
    if (bev) {
        // IPM 模式: 点必须落在标定梯形内, 否则投影出来的坐标没有意义
        if (!bev->contains(gx, gy)) return false;
        double bx = 0, by = 0;
        if (!bev->project(gx, gy, &bx, &by)) return false;
        *mx = bx;
        *my = by;
        return true;
    }
    if (real_meters <= 0.0) return false;

    // 单点比例尺模式: 原图像素 + 标定段(p1->p2)判定
    double ux = p2.x - p1.x, uy = p2.y - p1.y;
    double len = std::sqrt(ux * ux + uy * uy);
    if (len < 1e-9) return false;
    ux /= len;
    uy /= len;
    double s = (gx - p1.x) * ux + (gy - p1.y) * uy;
    if (s < -zone_margin_px || s > len + zone_margin_px) return false;

    *mx = gx;
    *my = gy;
    return true;
}

double SpeedCalib::meters_per_pixel() const {
    if (bev) return bev->meters_per_pixel;
    if (real_meters <= 0.0) return 0.0;
    double dx = p2.x - p1.x, dy = p2.y - p1.y;
    double len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-9) return 0.0;
    return real_meters / len;
}

// ---------------------------------------------------------------------------
void SpeedTracker::reset() {
    tracks_.clear();
    next_id_ = 0;
}

// 方案 1（默认，与 Python 侧口径一致）：窗口首尾弦长 / 时间
float SpeedTracker::speed_from_hist_chord(const Track& tr, double mpp) {
    if (tr.h_cnt < 2) return 0.f;
    double dt = tr.h_t[tr.h_newest()] - tr.h_t[tr.h_oldest()];
    if (dt <= 1e-9) return 0.f;
    float dx = tr.h_x[tr.h_newest()] - tr.h_x[tr.h_oldest()];
    float dy = tr.h_y[tr.h_newest()] - tr.h_y[tr.h_oldest()];
    return std::hypot(dx, dy) * mpp / dt * 3.6f;
}

// 方案 2：窗口内最小二乘斜率（抗端点噪声；物体减速时比方案 1 低 4~5%）
float SpeedTracker::speed_from_hist_lsq(const Track& tr, double mpp) {
    const int n = tr.h_cnt;
    if (n < 2) return 0.f;
    const double t0 = tr.h_t[tr.h_oldest()];
    double sx = 0, stt = 0, stx = 0, sty = 0;
    for (int i = 0; i < n; ++i) {
        int idx = (tr.h_oldest() + i) % Track::kMaxH;
        const double t = tr.h_t[idx] - t0;      // 平移原点, 减小数值误差
        sx += t;
        stt += t * t;
        stx += t * tr.h_x[idx];
        sty += t * tr.h_y[idx];
    }
    const double den = n * stt - sx * sx;      // = n * Σ(t-t̄)²
    if (std::fabs(den) < 1e-9) return 0.f;
    const double vx = (n * stx - sx * sx) / den;   // px/s
    const double vy = (n * sty - sx * sx) / den;
    return static_cast<float>(std::hypot(vx, vy) * mpp * 3.6);
}

// 显示值 = 滑动窗口内中位数
// 不用瞬时值, 不用 EMA。相邻帧位移往往只有十几像素, 单帧估计噪声能到 ±20 km/h;
// 原来的 EMA(alpha=0.4)+11帧中位数反而放大波动 —— EMA 对相邻帧高度相关的样本
// 做平滑, 只是把噪声延后, 没有削掉。
float SpeedTracker::display_median(const Track& tr, double t, double span) {
    float tmp[Track::kMaxS];
    int n = 0;
    for (int i = 0; i < tr.s_cnt; ++i) {
        int idx = (tr.s_oldest() + i) % Track::kMaxS;
        if (tr.s_t[idx] >= t - span) tmp[n++] = tr.s_v[idx];
    }
    if (n == 0) return 0.f;
    std::sort(tmp, tmp + n);
    return tmp[n / 2];
}

// ---------------------------------------------------------------------------
// ★ 模块 F: 静态背景判定
//
// 现场有一类"鬼影": 画面里固定位置的广告牌/贴纸被模型当成车或人, 而且它
// 【永远不动】—— track ID 会一直挂着, 每帧都送去 LPRNet 推理, 白费算力,
// 偶尔还会读出一串假车牌上报给业务层。
//
// 判据: 最近 window 帧里, 框中心相对当前位置的【最大】位移 < min_disp 像素。
//
// ★ 为什么用"最大位移"而不是"首尾位移":
//     万一它左右抖 4px 再抖回来, 首尾位移是 0, 但它其实在动 ——
//     那种应该当"抖动的真目标"留着, 别误杀。
//
// ★ 为什么要求 min_hits 个样本:
//     刚进画面、还没来得及动的真目标, 头几帧位移当然也很小。
//     样本不够就下结论, 会把真车误判成广告牌。
// ---------------------------------------------------------------------------
bool SpeedTracker::is_static_bg(const Track& tr, const SpeedCfg& cfg, float* out_disp) {
    if (out_disp) *out_disp = 0.f;
    if (!cfg.static_filter) return false;
    if (tr.p_cnt < cfg.static_min_hits) return false;

    const int newest = tr.p_newest();
    const int cutoff = tr.p_f[newest] - cfg.static_window;
    const float cx = tr.p_x[newest];
    const float cy = tr.p_y[newest];

    int n = 0;
    float max_disp = 0.f;
    for (int i = 0; i < tr.p_cnt; ++i) {
        const int idx = (tr.p_head - tr.p_cnt + i + Track::kMaxP) % Track::kMaxP;
        if (tr.p_f[idx] < cutoff) continue;      // 太老的样本不算
        const float dx = tr.p_x[idx] - cx;
        const float dy = tr.p_y[idx] - cy;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d > max_disp) max_disp = d;
        ++n;
    }
    if (out_disp) *out_disp = max_disp;
    if (n < cfg.static_min_hits) return false;
    return max_disp < cfg.static_min_disp;
}

// ---------------------------------------------------------------------------
void SpeedTracker::update(const std::vector<YoloBox>& dets, double t, const SpeedCfg& cfg,
                           const SpeedCalib& calib, std::vector<TrackResult>* out) {
    out->clear();
    ++frame_no_;                      // 模块 F: 帧号
    if (dets.empty() && tracks_.empty()) return;

    // ---- 分两阶段匹配: 高分框先关联, 低分框再关联一次 ----------------------
    // 这样短暂漏检不会换 ID（沿用 Python 侧 ultralytics bytetrack.yaml）
    std::vector<int> det_to_track(dets.size(), -1);
    std::vector<char> track_used(tracks_.size(), 0);

    std::vector<int> high, low;
    for (size_t i = 0; i < dets.size(); ++i) {
        if (dets[i].score >= cfg.track_high_thresh) high.push_back(static_cast<int>(i));
        else if (dets[i].score >= cfg.track_low_thresh) low.push_back(static_cast<int>(i));
    }

    auto greedy_match = [&](const std::vector<int>& cand) {
        struct Pair { float iou; int di; int ti; };
        std::vector<Pair> pairs;
        for (int di : cand) {
            for (size_t ti = 0; ti < tracks_.size(); ++ti) {
                if (track_used[ti]) continue;
                float iou = box_iou(dets[di], tracks_[ti].box);
                if (iou >= cfg.match_thresh) pairs.push_back({iou, di, static_cast<int>(ti)});
            }
        }
        std::sort(pairs.begin(), pairs.end(),
                  [](const Pair& a, const Pair& b) { return a.iou > b.iou; });
        for (const Pair& p : pairs) {
            if (det_to_track[p.di] >= 0 || track_used[p.ti]) continue;
            det_to_track[p.di] = p.ti;
            track_used[p.ti] = 1;
        }
    };
    greedy_match(high);
    greedy_match(low);

    // ---- 未匹配的高分框 -> 开新 track --------------------------------------
    // 注意: 假框之所以能变成"两个框", 就是因为它分数高于 new_track_thresh
    //       被开了新 ID —— 所以几何过滤必须在【这一步之前】做完。
    for (size_t i = 0; i < dets.size(); ++i) {
        if (det_to_track[i] >= 0) continue;
        if (dets[i].score < cfg.new_track_thresh) continue;
        Track tr;
        tr.id = next_id_++;
        tr.box = dets[i];
        tr.score = dets[i].score;
        tracks_.push_back(tr);
        det_to_track[i] = static_cast<int>(tracks_.size()) - 1;
    }

    // ---- 更新每条 track, 并算速度 -----------------------------------------
    for (size_t i = 0; i < dets.size(); ++i) {
        int ti = det_to_track[i];
        if (ti < 0) continue;
        Track& tr = tracks_[ti];
        tr.box = dets[i];
        tr.score = dets[i].score;
        tr.hits++;
        tr.age++;
        tr.missed = 0;

        // 模块 C: 地面接地点 = 框底中心, 过 5 帧滑动中值（x/y 分开）
        //   框底常常落在【骑手的脚】上而不是轮胎接地点, 脚一落地框底掉 5~10px,
        //   IPM 会把它放大 10~17 倍。
        float gx = 0.5f * (dets[i].x1 + dets[i].x2);
        float gy = dets[i].y2;
        gx = tr.med_x.push(gx);
        gy = tr.med_y.push(gy);

        // 模块 D: IPM 空间先验 —— 点在标定区外就作废这段历史
        TrackResult r;
        r.track_id = tr.id;
        r.box = tr.box;
        r.score = tr.score;
        r.in_trap = tr.in_trap;

        // ★ 模块 F: 记一个像素级位置样本, 并判定是否静止背景
        //   (框中心, 不是地面点 —— 地面点会因为骑手落脚而抖)
        tr.p_push(frame_no_, 0.5f * (dets[i].x1 + dets[i].x2),
                              0.5f * (dets[i].y1 + dets[i].y2));
        r.static_bg = is_static_bg(tr, cfg, &r.static_disp);

        double mx = 0, my = 0;
        if (!calib.to_metric(gx, gy, &mx, &my)) {
            tr.h_clear();          // 出梯形 -> 历史清空, 这段不给速度
            tr.in_trap = false;
            r.in_trap = false;
            out->push_back(r);     // 框照画, 只是这一帧不给速度
            continue;
        }
        tr.in_trap = true;
        r.in_trap = true;
        tr.h_push(t, static_cast<float>(mx), static_cast<float>(my));

        // 只保留测速窗口内的历史
        const double cutoff = t - cfg.speed_win_sec;
        while (tr.h_cnt > 0 && tr.h_t[tr.h_oldest()] < cutoff) tr.h_cnt--;

        // 模块 E: 速度估计
        bool ok = false;
        float v = 0.f;
        if (tr.h_cnt >= cfg.min_speed_samples &&
            tr.h_t[tr.h_newest()] - tr.h_t[tr.h_oldest()] >= cfg.min_speed_dt) {
            const double mpp = calib.meters_per_pixel();
            v = cfg.lsq ? speed_from_hist_lsq(tr, mpp) : speed_from_hist_chord(tr, mpp);
            ok = true;
        }
        if (ok) tr.s_push(t, v);

        // 样本不足先不出数（防幽灵轨迹报"超速"假警）
        if (tr.s_cnt >= cfg.min_track_samples) {
            r.speed_kmh = display_median(tr, t, cfg.display_span);
            r.speed_valid = true;
            r.over_speed = r.speed_kmh > cfg.speed_limit_kmh;
        }
        out->push_back(r);
    }

    // ---- 未被匹配的 track: 累计丢失帧数, 超过 buffer 就删 --------------------
    for (size_t ti = 0; ti < tracks_.size();) {
        bool updated = false;
        for (size_t i = 0; i < det_to_track.size(); ++i) {
            if (det_to_track[i] == static_cast<int>(ti)) { updated = true; break; }
        }
        if (updated) { ++ti; continue; }
        tracks_[ti].missed++;
        if (tracks_[ti].missed > cfg.track_buffer) {
            tracks_.erase(tracks_.begin() + static_cast<long>(ti));
        } else {
            ++ti;
        }
    }
}

}  // namespace rk
