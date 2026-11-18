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
    // ★ 一次性把轨迹表的容量开到位(见 update() 里那段注释):
    //   Track 单条约 4KB(含三组环形缓冲), 场景轨迹数涨上去时 vector 翻倍扩容
    //   会造成几百 KB 一级的重新分配, 活堆曲线就成了一阶��上台阶。
    //   一次 reserve 到位 + 上限约束 = 永不重分配 = 活堆绝对平线。
    //   注意 clear() 不会释放已 reserve 的容量, 所以只在启动时做一次就够。
    tracks_.reserve(kMaxTracks);
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
    // 最小二乘斜率:  v = (n·Σ(t·x) − Σt·Σx) / (n·Σt² − (Σt)²)
    // ★ 这里原来有两个错(2026-11-15 修):
    //   ① y 项用了 sx*sx 而不是 sx*sy;
    //   ② x 项也用了 sx*sx(应该是 sx*Σx)。
    //   结果: 目标只沿一个方向走时, 算出来的"另一个方向速度"是天文数字
    //   (实测 27km/h 的目标算出 2066km/h)。默认估计器是 chord, 所以生产没暴露。
    double sx = 0, sxx = 0, syy = 0, stt = 0, stx = 0, sty = 0;
    for (int i = 0; i < n; ++i) {
        int idx = (tr.h_oldest() + i) % Track::kMaxH;
        const double t = tr.h_t[idx] - t0;      // 平移原点, 减小数值误差
        sx += t;
        sxx += tr.h_x[idx];
        syy += tr.h_y[idx];
        stt += t * t;
        stx += t * tr.h_x[idx];
        sty += t * tr.h_y[idx];
    }
    const double den = n * stt - sx * sx;      // = n * Σ(t-t̄)²
    if (std::fabs(den) < 1e-9) return 0.f;
    const double vx = (n * stx - sx * sxx) / den;   // px/s
    const double vy = (n * sty - sx * syy) / den;
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
// ★ 运动预测(抽帧推理 --infer-stride N 时用)
//
// 只把轨迹的框按当前像素速度往前推, 别的什么都不动:
//   · 不加检测样本(h_* 速度历史)—— 测速只用真实检测, 不掺"没去看"的帧
//   · 不动 hits/age/missed —— 跳帧不是漏检
//   · 不产生 TrackResult —— 输出还是上一次真实检测的结果(画面上框"卡顿")
//
// 为什么必须做: 关联是拿新检测和轨迹的框算 IoU(match_thresh=0.80)。
//   抽了 N 帧之后目标已经移动, 旧框和新框的 IoU 可能掉到阈值以下 ->
//   关联失败 -> 旧轨迹 missed++、新检测另开一条 -> 【ID 乱跳】。
//   先把框推到"它现在应该在哪", 下一个真实检测再跟预测位置比, IoU 就回来了。
//
// 位移上限: 单次预测最多推 kMaxPredictShift 像素 —— 预测错了(比如目标突然
//   拐弯/被遮挡)也不至于把框推到画面外面去, 反而匹配不上。
// ---------------------------------------------------------------------------
namespace {
constexpr float kMaxPredictShift = 120.0f;   // 像素
}

void SpeedTracker::predict(double t) {
    if (!predict_enabled_) return;
    for (auto& tr : tracks_) {
        if (!tr.vel_valid || tr.det_t <= 0.0) continue;
        // ★ 增量: 从【上次预测】到现在, 不是从上次检测到现在 ——
        //   后者是累计时间, 每个跳过的帧都推一次会把位移叠成 N 倍。
        const double base_t = (tr.pred_t > 0.0) ? tr.pred_t : tr.det_t;
        const double dt = t - base_t;
        if (dt <= 0.0 || dt > 5.0) continue;          // 太久远的预测没有意义
        float dx = static_cast<float>(tr.vel_x * dt);
        float dy = static_cast<float>(tr.vel_y * dt);
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len > kMaxPredictShift) {
            const float k = kMaxPredictShift / len;
            dx *= k;
            dy *= k;
        }
        tr.box.x1 += dx;
        tr.box.x2 += dx;
        tr.box.y1 += dy;
        tr.box.y2 += dy;
        // ★ 注意: det_cx/det_cy/det_t 【不动】 —— 它们记录的是"上一次真实检测的
        //   位置和时刻", 是算速度的基准; 预测只是把 box 挪到"现在应该在哪"。
        //   预测不产生新的检测样本(det_t 不动), 但要记下这次预测的时刻。
        tr.pred_t = t;
    }
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
                // ★ 记录"差一点就配上"的关联 —— 这就是抽帧推理的风险量化:
                //   抽 N 帧之后目标移动了, IoU 会从 0.9 掉到 0.5,
                //   掉到 match_thresh(0.80) 以下就换 ID 了。
                //   统计 IoU 分布, 就能看出"离换 ID 还有多远"。
                if (iou >= cfg.match_thresh) pairs.push_back({iou, di, static_cast<int>(ti)});
                else if (iou > 0.3f) {
                    ++near_miss_;
                    iou_near_sum_ += iou;
                    if (iou < iou_near_min_) iou_near_min_ = iou;
                }
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

    // ---- 兜底关联: IoU 配不上时, 用【中心距离】再试一次 ------------------
    //
    // ★ 为什么需要:
    //   抽帧推理时, 相邻两次真实检测之间目标可能移动了"小半个框"那么多,
    //   IoU 直接掉到 match_thresh(0.80) 以下 -> 配不上 -> 旧轨迹挂着等超时,
    //   新目标另开一条 -> 【同一个目标换 ID】(而且速度也因为历史被清空而出不来)。
    //   实测: 80px 宽的框, 目标每帧走 6px, stride=2 时两次检测差 12px,
    //   IoU 只有 0.74 < 0.80 -> 一次都配不上 -> 1000 帧开了 30 条轨迹。
    //
    // ★ 只对【过期轨迹】用(上一次检测不是这一帧):
    //   正常逐帧推理(默认 stride=1)时每条轨迹都是当帧的, 走不到这个分支,
    //   行为【一个字节都不变】。
    //
    // ★ 距离阈值取检测框宽的 0.6 倍: 足够覆盖"目标移动了半个框"的场景,
    //   又不至于把挨在一起的两个目标(骑车人 + 行人)换错 ID。
    if (cfg.match_fallback) {
        std::vector<int> rest_d;
        for (size_t i = 0; i < dets.size(); ++i)
            if (det_to_track[i] < 0) rest_d.push_back(static_cast<int>(i));
        for (int di : rest_d) {
            const YoloBox& d = dets[di];
            const double dcx = 0.5 * (d.x1 + d.x2);
            const double dcy = 0.5 * (d.y1 + d.y2);
            const double dw = std::fabs(d.x2 - d.x1);
            const double lim = cfg.fallback_dist_ratio * std::max(dw, 1.0);
            int best = -1;
            double best_d = 1e18;
            for (size_t ti = 0; ti < tracks_.size(); ++ti) {
                if (track_used[ti]) continue;
                if (tracks_[ti].det_frame < 0) continue;
                // ★ 用【时间】判断, 不能用 frame_no_:
                //   frame_no_ 数的是 update 次数, 抽帧时两次 update 之间隔着 N 帧,
                //   用它判断"过期"会把真正过期的轨迹当成新鲜的(兜底就永远不触发)。
                //   判据: 距上次真实检测的时间 > 1.5 个 update 间隔。
                const double stale = (t - tracks_[ti].det_t) > 1.5 * frame_dt_;
                if (!stale) continue;           // 不是过期轨迹 -> 不走兜底
                const double tcx = 0.5 * (tracks_[ti].box.x1 + tracks_[ti].box.x2);
                const double tcy = 0.5 * (tracks_[ti].box.y1 + tracks_[ti].box.y2);
                const double dist = std::sqrt((dcx - tcx) * (dcx - tcx) +
                                              (dcy - tcy) * (dcy - tcy));
                if (dist <= lim && dist < best_d) { best_d = dist; best = static_cast<int>(ti); }
            }
            if (best >= 0) {
                det_to_track[di] = best;
                track_used[best] = 1;
                ++fallback_hits_;       // 兜底救回来的次数
            }
        }
    }

    // ---- 未匹配的高分框 -> 开新 track --------------------------------------
    // 注意: 假框之所以能变成"两个框", 就是因为它分数高于 new_track_thresh
    //       被开了新 ID —— 所以几何过滤必须在【这一步之前】做完。
    //
    // ★ 轨迹数上限: 轨迹表是 std::vector<Track>, 而 Track 里有三组环形缓冲
    //   (h_*/s_*/p_*), 单条约 4KB。场景里轨迹数涨上去时, vector 会按
    //   1→2→4→…→128 翻倍扩容, 每扩容一次就是几百 KB 的【重新分配】——
    //   表现就是活堆曲线一阶���上台阶(实测 4K 路口视频: 前 2600 帧涨了 1.2MB,
    //   涨的每一步都对应轨迹数 +15 左右)。
    //   reset() 里已经一次性 reserve 好了容量, 这里再兜一道上限,
    //   保证 size 永远不会超过 capacity -> vector 永不重分配 ->
    //   活堆从第一帧起就是一条【绝对水平线】。
    //   128 条同时在册的轨迹, 按 track_buffer=30 帧算, 相当于 1.2 秒内
    //   出现了 128 个不同的目标 —— 真实路口场景远达不到(实测峰值 87)。
    for (size_t i = 0; i < dets.size(); ++i) {
        if (det_to_track[i] >= 0) continue;
        if (dets[i].score < cfg.new_track_thresh) continue;
        if (tracks_.size() >= kMaxTracks) {
            // 超过上限: 不再开新轨迹(现有的照常跟踪)。
            // 只提醒一次, 免得刷屏 —— 上限被碰到说明场景密度超出预期,
            // 该调的是上限值或者提高 new_track_thresh, 不是让它偷偷涨内存。
            static bool warned = false;
            if (!warned) {
                warned = true;
                fprintf(stderr,
                        "[跟踪] 轨迹数达到上限 %zu, 后续新目标不再开 ID"
                        "(内存不会涨, 但这些目标不会有车牌/速度)\n",
                        kMaxTracks);
            }
            continue;
        }
        Track tr;
        tr.id = next_id_++;
        tr.box = dets[i];
        tr.score = dets[i].score;

        // ★★ ID 漂移探针: 新开的这条轨迹, 如果和某条【已经在册】的轨迹
        //   明显重叠(IoU > 0.2), 基本就是"同一个目标换了个 ID" ——
        //   也就是抽帧推理最典型的翻车方式: 上一次检测时目标在左边,
        //   隔了 N 帧再检测时它跑到右边, IoU 掉到 match_thresh(0.80) 以下,
        //   关联失败 -> 旧 ID 挂着等超时, 新目标另开一条 -> 画面上同一辆车
        //   出现两个 ID, 来回跳。
        //   这个计数器只统计、不改变任何行为, 用来量化"ID 稳不稳"。
        for (size_t ti = 0; ti < tracks_.size(); ++ti) {
            if (box_iou(dets[i], tracks_[ti].box) > 0.2f) {
                ++dup_tracks_;
                break;
            }
        }

        tracks_.push_back(tr);
        det_to_track[i] = static_cast<int>(tracks_.size()) - 1;
    }

    // ---- 估计两次 update 的间隔(用来判断轨迹是否"过期") -------------------
    if (last_update_t_ > 0.0) {
        const double dt = t - last_update_t_;
        if (dt > 0.001 && dt < 2.0) {
            frame_dt_ = (frame_dt_ <= 0.0) ? dt : (frame_dt_ * 0.8 + dt * 0.2);
        }
    }
    last_update_t_ = t;

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

        // ---- 记录像素速度(给 predict() 的运动预测用) ----
        //
        // ★ 为什么放在这里:
        //   抽帧推理时, 相邻两次真实检测之间隔了 N 帧, 目标已经移动了。
        //   下一次关联要拿新检测和轨迹的【预测位置】比 IoU, 才不会 ID 乱跳。
        //   速度只能从【真实检测】的位移算 —— 跳帧那些帧是"没去看", 不是"目标没动"。
        {
            const float cx = 0.5f * (dets[i].x1 + dets[i].x2);
            const float cy = 0.5f * (dets[i].y1 + dets[i].y2);
            if (tr.det_t > 0.0 && t > tr.det_t) {
                const double ddt = t - tr.det_t;
                const float nvx = static_cast<float>((cx - tr.det_cx) / ddt);
                const float nvy = static_cast<float>((cy - tr.det_cy) / ddt);
                if (!tr.vel_valid) {
                    tr.vel_x = nvx;
                    tr.vel_y = nvy;
                    tr.vel_valid = true;
                } else {
                    // 指数平滑: 抑制单帧抖动, 但不把真实加速度抹掉
                    tr.vel_x = 0.5f * tr.vel_x + 0.5f * nvx;
                    tr.vel_y = 0.5f * tr.vel_y + 0.5f * nvy;
                }
            }
            tr.det_t = t;
            tr.det_cx = cx;
            tr.det_cy = cy;
            tr.det_frame = frame_no_;
            tr.pred_t = 0.0;      // 框已被真实检测覆盖, 预测基准清零
        }

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
        r.center_x = 0.5f * (dets[i].x1 + dets[i].x2);   // ★ 框中心(过线计数用)
        r.center_y = 0.5f * (dets[i].y1 + dets[i].y2);
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
