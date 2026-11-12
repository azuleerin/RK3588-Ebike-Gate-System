// test_static_filter.cpp —— 模块 F(静态背景过滤)的单元测试
//
// 要验的:
//   ① 同一个位置的框连续出现 -> 判为静止背景
//   ② 慢慢移动的框 -> 【不能】判成静止背景(否则真目标会被误杀)
//   ③ 左右抖动的框 -> 也不能判成静止(首尾位移是 0, 但它其实在动)
//   ④ 样本不够时 -> 不下结论(刚进画面还没来得及动的真目标)
//   ⑤ 关掉开关 -> 永远不判静止
//   ⑥ 静止判定【不能】影响测速结果
#include <cstdio>
#include <cmath>
#include <vector>

#include "speed_track.h"

using namespace rk;

static int g_fail = 0;
static void expect(bool cond, const char* what) {
    printf("  [%s] %s\n", cond ? " OK " : "FAIL", what);
    if (!cond) ++g_fail;
}

// 造一个固定尺寸的框, 左上角 (x, y)
static YoloBox mk_box(float x, float y, float w = 80.f, float h = 120.f) {
    YoloBox b;
    b.cls_id = 1;
    b.score = 0.5f;
    b.x1 = x; b.y1 = y; b.x2 = x + w; b.y2 = y + h;
    return b;
}

int main() {
    SpeedCfg cfg;
    cfg.static_filter = true;
    cfg.static_window = 30;
    cfg.static_min_disp = 5.f;
    cfg.static_min_hits = 10;

    // ---- ① 完全不动 -> 判静止 --------------------------------------------
    {
        printf("① 同一位置的框连续出现\n");
        SpeedTracker tracker;
        std::vector<TrackResult> out;
        bool got = false;
        for (int f = 0; f < 60; ++f) {
            std::vector<YoloBox> dets{mk_box(500.f, 700.f)};
            tracker.update(dets, f * 0.04, cfg, SpeedCalib(), &out);
            if (!out.empty() && out[0].static_bg) { got = true; break; }
        }
        expect(got, "连续不动 -> 判为静止背景");
    }

    // ---- ② 慢慢移动 -> 不能判静止 -----------------------------------------
    {
        printf("② 慢速移动的框\n");
        SpeedTracker tracker;
        std::vector<TrackResult> out;
        bool wrongly_static = false;
        for (int f = 0; f < 60; ++f) {
            // 每帧挪 3 像素(30 帧窗口内最多 90px, 远超 5px 阈值)
            std::vector<YoloBox> dets{mk_box(500.f + f * 3.f, 700.f)};
            tracker.update(dets, f * 0.04, cfg, SpeedCalib(), &out);
            if (!out.empty() && out[0].static_bg) { wrongly_static = true; break; }
        }
        expect(!wrongly_static, "移动的框 -> 不判静止");
    }

    // ---- ③ 左右抖动 -> 不能判静止 -----------------------------------------
    // 关键: 用【最大位移】而不是【首尾位移】。
    //   这个框在 0 和 8 之间来回抖 —— 首尾位移可能是 0(它回到了原位),
    //   但它其实在动, 不该被判成广告牌。抖动幅度 8px > 阈值 5px。
    {
        printf("③ 左右抖动的框(首尾位移=0, 但其实在动)\n");
        SpeedTracker tracker;
        std::vector<TrackResult> out;
        bool wrongly_static = false;
        for (int f = 0; f < 60; ++f) {
            const float dx = (f % 2) ? 8.f : 0.f;   // 在 0 和 8 之间来回抖
            std::vector<YoloBox> dets{mk_box(500.f + dx, 700.f)};
            tracker.update(dets, f * 0.04, cfg, SpeedCalib(), &out);
            if (!out.empty() && out[0].static_bg) { wrongly_static = true; break; }
        }
        expect(!wrongly_static, "抖动的框 -> 不判静止(用最大位移而不是首尾位移)");
    }

    // ---- ③b 抖动幅度【小于】阈值 -> 按定义就是静止 -------------------------
    {
        printf("③b 抖动幅度小于阈值(4px < 5px)\n");
        SpeedTracker tracker;
        std::vector<TrackResult> out;
        bool got = false;
        for (int f = 0; f < 60; ++f) {
            const float dx = (f % 2) ? 4.f : 0.f;
            std::vector<YoloBox> dets{mk_box(500.f + dx, 700.f)};
            tracker.update(dets, f * 0.04, cfg, SpeedCalib(), &out);
            if (!out.empty() && out[0].static_bg) { got = true; break; }
        }
        expect(got, "位移 4px < 阈值 5px -> 按定义就是静止");
    }

    // ---- ④ 样本不够 -> 不下结论 -------------------------------------------
    {
        printf("④ 刚建 track, 样本不够\n");
        SpeedTracker tracker;
        std::vector<TrackResult> out;
        std::vector<YoloBox> dets{mk_box(500.f, 700.f)};
        tracker.update(dets, 0.0, cfg, SpeedCalib(), &out);
        expect(!out.empty() && !out[0].static_bg,
               "只有 1 帧历史 -> 不判静止(不误杀刚进画面的真目标)");
    }

    // ---- ⑤ 开关关掉 -> 永远不判 -------------------------------------------
    {
        printf("⑤ 开关关掉\n");
        SpeedCfg off = cfg;
        off.static_filter = false;
        SpeedTracker tracker;
        std::vector<TrackResult> out;
        bool any = false;
        for (int f = 0; f < 60; ++f) {
            std::vector<YoloBox> dets{mk_box(500.f, 700.f)};
            tracker.update(dets, f * 0.04, off, SpeedCalib(), &out);
            if (!out.empty() && out[0].static_bg) any = true;
        }
        expect(!any, "static_filter=false -> 永远不判静止");
    }

    // ---- ⑥ 不影响测速 ------------------------------------------------------
    {
        printf("⑥ 静止判定不影响测速\n");
        SpeedCfg with_filter = cfg;
        SpeedCfg no_filter = cfg;
        no_filter.static_filter = false;

        SpeedTracker t1, t2;
        std::vector<TrackResult> o1, o2;
        for (int f = 0; f < 40; ++f) {
            std::vector<YoloBox> dets{mk_box(500.f + f * 4.f, 700.f)};
            t1.update(dets, f * 0.04, with_filter, SpeedCalib(), &o1);
            t2.update(dets, f * 0.04, no_filter, SpeedCalib(), &o2);
        }
        const bool same = !o1.empty() && !o2.empty() &&
                          std::fabs(o1[0].speed_kmh - o2[0].speed_kmh) < 1e-6f &&
                          o1[0].speed_valid == o2[0].speed_valid;
        expect(same, "开/关过滤, 测速结果完全一致");
    }

    printf("\n==== %s (失败 %d 项) ====\n", g_fail ? "有失败项" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
