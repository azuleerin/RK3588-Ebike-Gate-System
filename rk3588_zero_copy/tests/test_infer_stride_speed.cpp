// test_infer_stride_speed.cpp —— 抽帧推理对测速精度的影响(带真值的合成测试)
//
// 为什么要有这个测试:
//   抽帧推理(--infer-stride N)让【真实检测】的间隔变成 N 帧, 测速估计器
//   (chord / lsq)的样本密度也跟着变 N 倍。这直接影响两件事:
//     1. 速度窗里的样本够不够(min_speed_samples=6, 窗口 0.8s)
//     2. 速度估计本身有没有偏差(样本变稀, chord 的基线变长)
//   真机场景要拿到"路面 + 标定"才能测, 这里直接用【合成轨迹】做:
//     · 已知速度的目标匀速穿过画面
//     · 每 N 帧才给一次检测(模拟抽帧)
//     · 比较估计速度 vs 真值
//
// 只依赖 speed_track.cc / yolo_post.cc, 不碰 RKNN/RGA/MPP。
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

#include "speed_track.h"
#include "yolo_post.h"

using namespace rk;

namespace {

// 合成标定: p1->p2 这段像素 = real_meters 米, 且地面点必须在 p1 沿线方向上
SpeedCalib make_calib(double meters_per_px) {
    SpeedCalib c;
    c.bev = nullptr;
    c.p1 = Pt{0.0, 0.0};
    c.p2 = Pt{1000.0, 0.0};                 // 1000 px
    c.real_meters = 1000.0 * meters_per_px;  // -> meters_per_pixel = meters_per_px
    c.zone_margin_px = 1e9;                   // 不做区域限制
    return c;
}

// 一个匀速直线运动的目标(地面接地点 = 框底中心)
YoloBox make_box(double gx, double gy, double w, double h, float score) {
    YoloBox b;
    b.x1 = static_cast<float>(gx - w * 0.5);
    b.x2 = static_cast<float>(gx + w * 0.5);
    b.y1 = static_cast<float>(gy - h);
    b.y2 = static_cast<float>(gy);
    b.cls_id = 1;
    b.score = score;
    return b;
}

struct Result {
    double true_kmh = 0.0;
    double est_kmh = 0.0;
    int    samples = 0;      // 速度窗里的样本数
    int    tracks = 0;
};

// 跑一遍: 每 stride 帧给一次检测, 目标匀速
Result run_case(int stride, double fps, double meters_per_px,
                double px_per_frame, int n_frames, bool estimator_lsq) {
    SpeedTracker tracker;
    SpeedCfg cfg;
    if (estimator_lsq) cfg.lsq = true;
    const SpeedCalib calib = make_calib(meters_per_px);

    // 目标沿 x 方向匀速; 地面点(框底中心)从 x0 开始
    const double x0 = 100.0;
    const double y = 700.0;
    const double w = 80.0, h = 120.0;

    std::vector<TrackResult> out;
    double last_speed = 0.0;
    int n_valid = 0;
    double t = 1000.0;                       // 起始时刻(秒)

    for (int f = 0; f < n_frames; ++f) {
        t += 1.0 / fps;
        if (f % stride != 0) {
            // 抽帧: 这一帧没有新检测 —— 只做运动预测, 不喂检测样本
            tracker.predict(t);
            continue;
        }
        const double gx = x0 + px_per_frame * f;
        std::vector<YoloBox> dets;
        dets.push_back(make_box(gx, y, w, h, 0.9f));
        out.clear();
        tracker.update(dets, t, cfg, calib, &out);
        for (const auto& r : out) {
            if (r.speed_valid) { last_speed = r.speed_kmh; ++n_valid; }
        }
    }

    Result r;
    r.true_kmh = px_per_frame * fps * meters_per_px * 3.6;
    r.est_kmh = last_speed;
    r.samples = n_valid;
    r.tracks = tracker.track_count();
    return r;
}

}  // namespace

int main() {
    const double fps = 25.0;             // 生产相机的帧率
    const double mpp = 0.05;             // 5 cm/px
    const double px_per_frame = 6.0;     // 每帧走 6 px -> 6*25*0.05 = 7.5 m/s = 27 km/h

    printf("== 合成场景: 25fps, 5cm/px, 目标匀速 %.1f px/帧 (真值 %.1f km/h) ==\n",
           px_per_frame, px_per_frame * fps * mpp * 3.6);
    printf("%-10s %-12s %-12s %-10s %-10s %-10s\n",
           "stride", "估计km/h", "误差%", "有效样本", "轨迹数", "结论");
    for (int stride : {1, 2, 3, 5, 8, 10}) {
        const Result r = run_case(stride, fps, mpp, px_per_frame, 400, false);
        const double err = (r.true_kmh > 0.0)
                               ? 100.0 * (r.est_kmh - r.true_kmh) / r.true_kmh
                               : 0.0;
        printf("%-10d %-12.2f %-12.1f %-10d %-10d %-10s\n",
               stride, r.est_kmh, err, r.samples, r.tracks,
               r.samples > 0 ? (fabs(err) < 8.0 ? "可用" : "有偏差") : "★没有速度");
    }

    // ---- 限制A: 目标移动太快(相对框宽) -> 关联配不上, ID 会漂 ----
    printf("\n== 限制A: 目标每帧移动多少像素(框宽 80px, 关联兜底能兜住 0.6 倍框宽)==\n");
    printf("%-10s %-10s %-10s %-10s %-10s\n", "px/帧", "stride", "位移/框宽", "轨迹数", "速度");
    for (double pxf : {2.0, 6.0, 12.0, 24.0}) {
        for (int stride : {2, 5}) {
            const Result r = run_case(stride, fps, mpp, pxf, 400, false);
            printf("%-10.1f %-10d %-10.2f %-10d %.2f%s\n", pxf, stride,
                   pxf * stride / 80.0, r.tracks, r.est_kmh,
                   r.tracks > 3 ? "  <- ID 漂了" : "");
        }
    }

    // ---- 限制B: 推理太稀 -> 速度窗里的样本不够(min_speed_samples=6) ----
    printf("\n== 限制B: 推理频率 vs 速度窗样本数(25fps, 窗口 0.8s, 至少要 6 个样本)==\n");
    printf("%-10s %-14s %-12s %-12s\n", "stride", "推理次数/秒", "窗内样本", "结论");
    for (int stride : {1, 2, 3, 4, 5, 8, 10}) {
        const double rate = fps / stride;
        printf("%-10d %-14.1f %-12.1f %-12s\n", stride, rate, rate * 0.8,
               rate * 0.8 >= 6.0 ? "速度可算" : "★样本不够, 没有速度");
    }

    printf("\n== 同样的场景, lsq 估计器 ==\n");
    for (int stride : {1, 2, 5, 10}) {
        const Result r = run_case(stride, fps, mpp, px_per_frame, 400, true);
        const double err = (r.true_kmh > 0.0)
                               ? 100.0 * (r.est_kmh - r.true_kmh) / r.true_kmh
                               : 0.0;
        printf("stride=%-3d 估计=%-7.2f 误差=%-6.1f%% 有效样本=%-4d %s\n",
               stride, r.est_kmh, err, r.samples,
               r.samples > 0 ? (fabs(err) < 8.0 ? "可用" : "有偏差") : "★没有速度");
    }
    return 0;
}
