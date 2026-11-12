// test_speed.cpp —— 与 Python 侧 speed_demo.py 的对拍测试
//
// 只依赖 ipm.cc / speed_track.cc / yolo_post.cc（不碰 RKNN/RGA/OpenCV/FFmpeg），
// 所以可以在 x86 容器里直接编译运行，验证板端测速逻辑与 Python 基线一致。
//
// 用法: ./test_speed > out_cpp.txt ; python3 py_reference.py > out_py.txt ; diff
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

#include "ipm.h"
#include "speed_track.h"
#include "yolo_post.h"

using namespace rk;

namespace {

// 与 py_reference.py 完全一致的测试点
const int kImgW = 1920, kImgH = 1080;
const double kFov = 84.0;
const double kWidthM = 7.0;
const double kPpm = 100.0;
const Pt kQuad[4] = {{300.0, 622.0}, {1500.0, 643.0}, {1450.0, 877.0}, {550.0, 819.0}};

// 探测点: 4 个角 + 中心 + 梯形内外各若干
const double kProbe[8][2] = {
    {300.0, 622.0}, {1500.0, 643.0}, {1450.0, 877.0}, {550.0, 819.0},
    {900.0, 730.0}, {200.0, 400.0}, {1800.0, 950.0}, {960.0, 300.0},
};

// 几何过滤的测试框: {x1,y1,x2,y2,期望被剔除}
struct BoxCase { float x1, y1, x2, y2; int expect_bad; };
const BoxCase kBoxCases[] = {
    {100, 100, 200, 300, 0},        // 正常竖高框
    {0, 0, 1900, 1000, 1},          // 铺满画面
    {10, 900, 1800, 1050, 1},        // 又宽又扁(骑手脚那种)
    {100, 100, 1400, 200, 1},        // 宽且扁
    {900, 100, 1100, 1000, 0},       // 竖高细框
    {800, 400, 810, 405, 0},         // 10x5px 不算退化
    {800, 400, 802, 402, 1},         // 真正退化(<=4px)
    {0, 0, 1300, 1080, 1},          // 面积超 35%
};

}  // namespace

int main() {
    // ---------------- 1) IPM ----------------
    const double focal = (kImgW / 2.0) / std::tan(kFov * M_PI / 360.0);

    GroundPlane gp;
    std::string err;
    if (!GroundPlane::solve(kQuad, kImgW, kImgH, focal, &gp, &err)) {
        printf("IPM solve 失败: %s\n", err.c_str());
        return 1;
    }
    printf("F %.10f\n", gp.f);
    printf("LEN_NORM %.10f\n", gp.len_norm);
    printf("WIDTH_NORM %.10f\n", gp.width_norm);
    printf("RECT_ERR %.10f\n", gp.rect_err);

    const double scale_c = kWidthM / gp.width_norm;
    BevView bev;
    if (!BevView::build(kQuad, gp, scale_c, kPpm, &bev, &err)) {
        printf("BevView build 失败: %s\n", err.c_str());
        return 1;
    }
    printf("DST %d %d\n", bev.dst_w, bev.dst_h);
    printf("WIDTH_M %.10f\n", bev.width_m);
    printf("LENGTH_M %.10f\n", bev.length_m);

    for (const auto& p : kProbe) {
        double bx = 0, by = 0;
        bev.project(p[0], p[1], &bx, &by);
        printf("PROJ %.4f %.4f %.6f %.6f %d\n", p[0], p[1], bx, by,
               bev.contains(p[0], p[1]) ? 1 : 0);
    }

    // ---------------- 2) 矩形修正 ----------------
    Pt fixed[4], v1, v2;
    if (correct_quad_to_rectangle(kQuad, focal, kImgW, kImgH, fixed, &v1, &v2, &err)) {
        printf("FIXED");
        for (int i = 0; i < 4; ++i) printf(" %.6f %.6f", fixed[i].x, fixed[i].y);
        printf("\n");
        printf("V1 %.6f %.6f\n", v1.x, v1.y);
        printf("V2 %.6f %.6f\n", v2.x, v2.y);
    } else {
        printf("FIXED_FAIL %s\n", err.c_str());
    }

    // ---------------- 3) 几何先验过滤 ----------------
    GeoFilterCfg geo;
    for (const auto& bc : kBoxCases) {
        YoloBox b;
        b.x1 = bc.x1; b.y1 = bc.y1; b.x2 = bc.x2; b.y2 = bc.y2;
        b.cls_id = 0; b.score = 0.9f;
        const int bad = is_bad_box(b, kImgW, kImgH, geo) ? 1 : 0;
        printf("BAD %d %d\n", bad, bc.expect_bad);
    }

    // ---------------- 4) 中值滤波 ----------------
    {
        const float vals[7] = {100.f, 105.f, 3.f, 102.f, 98.f, 110.f, 101.f};
        Med5 med;
        for (float v : vals) printf("MED %.4f\n", med.push(v));
    }

    // ---------------- 5) 跟踪 + 测速 ----------------
    // 单点比例尺: p1=(0,0) p2=(1000,0) = 10 m -> 0.01 m/px
    // 目标每帧右移 50 px, 10 fps -> 0.5 m/s = 18 km/h
    {
        SpeedCfg cfg;
        cfg.conf_thr = 0.25f;
        cfg.nms_iou = 0.60f;
        cfg.min_track_samples = 8;
        cfg.min_speed_samples = 6;
        cfg.min_speed_dt = 0.12;
        cfg.speed_win_sec = 0.80;
        cfg.display_span = 0.80;
        cfg.speed_limit_kmh = 1000.f;

        SpeedCalib calib;
        calib.p1 = {0.0, 0.0};
        calib.p2 = {3000.0, 0.0};      // 标定段 3000px, 保证整个运动范围都在段内
        calib.real_meters = 30.0;      // -> 0.01 m/px

        SpeedTracker tracker;
        std::vector<YoloBox> dets;
        std::vector<TrackResult> out;
        for (int k = 0; k < 40; ++k) {
            const double t = k * 0.1;
            // 框宽 200px, 每帧移 10px -> 相邻帧 IoU = 190/210 = 0.905 > 0.8, 能关联上
            const float x = static_cast<float>(100 + k * 10);
            YoloBox b;
            b.x1 = x; b.y1 = 400.f; b.x2 = x + 200.f; b.y2 = 700.f;
            b.cls_id = 0; b.score = 0.9f;
            dets.assign(1, b);
            tracker.update(dets, t, cfg, calib, &out);
            if (!out.empty() && out[0].speed_valid && k >= 30) {
                printf("SPEED %.6f ID %d\n", out[0].speed_kmh, out[0].track_id);
            }
        }
    }

    return 0;
}
