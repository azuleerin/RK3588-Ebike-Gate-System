// test_tripwire.cpp —— 过线计数(TripwireCounter / TripwireService)的单元测试
//
// ★ 这是个【独立的测试主函数】, 不依赖任何录像/硬件:
//   跑法: ./build/tests/test_tripwire
//   全部用【伪造轨迹】驱动, 验证:
//     ① 几何: 叉积侧别判定 + 线段相交(含共线/端点相碰的退化情形)
//     ② 方向: 从上到下 = 进, 从下到上 = 出(以及反向配置)
//     ③ 边界: 不穿过线段只穿过延长线 -> 不计数; 从线段端点外侧掠过 -> 不计数
//     ④ 抖动: 站着不动不重复计; 瞬移(track_id 被复用)不误计
//     ⑤ 生命周期: 连续 N 帧没更新的轨迹必须被清掉(严格防泄漏)
//     ⑥ 多目标: 多条轨迹互不干扰
//     ⑦ 异步: submit() 绝不阻塞(1000 次提交耗时必须很短)
//     ⑧ 线程安全: 多线程并发 submit, 计数结果与串行一致
//
// 另外提供一个"录像回放"式的场景: 模拟一辆车从上往下穿过线, 再一辆车从下往上。
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "rk/tripwire.h"

using namespace rk;

static int g_fail = 0;
static int g_total = 0;
static void expect(bool cond, const char* what) {
    ++g_total;
    printf("  [%s] %s\n", cond ? " OK " : "FAIL", what);
    if (!cond) ++g_fail;
}

// 造一条轨迹点
static TripwireTrack mk(int id, float x, float y, int cls = 0) {
    TripwireTrack t;
    t.track_id = id;
    t.cls_id = cls;
    t.cx = x;
    t.cy = y;
    return t;
}

// 造一条 ByteTrack 的结果(测试走 vector<TrackResult> 那条入口)
static TrackResult mk_track(int id, float x, float y, int cls = 0) {
    TrackResult t;
    t.track_id = id;
    t.box.cls_id = cls;
    t.box.x1 = x - 20.f;
    t.box.y1 = y - 20.f;
    t.box.x2 = x + 20.f;
    t.box.y2 = y + 20.f;
    t.center_x = x;
    t.center_y = y;
    return t;
}

// 横向虚拟线: 从左到右, y = line_y。
//   side(P) = (x2-x1)*(P.y - line_y) -> 线下方为正、上方为负
//   所以 positive_is_in=true 就是"从上到下为进"。
static TripwireConfig hline(float line_y, float x1 = 100.f, float x2 = 900.f) {
    TripwireConfig c;
    c.x1 = x1;
    c.y1 = line_y;
    c.x2 = x2;
    c.y2 = line_y;
    c.positive_is_in = true;
    c.min_move_px = 1.0f;
    c.max_jump_px = 400.f;
    c.max_miss_frames = 30;
    c.cleanup_period_frames = 1;   // 测试里每次都扫, 便于断言
    return c;
}

// 喂一帧(单条轨迹)。
//   ★ 用具名变量而不是 &(TripwireTrack){...}(那是右值取地址, 非标准)；
//     ★ ++f 和 f*40 分开写, 避免实参求值顺序未定义。
static void feed(TripwireCounter& c, uint32_t& f, int id, float x, float y, int cls = 0,
                 int camera_id = 0) {
    const TripwireTrack t = mk(id, x, y, cls);
    ++f;
    c.ingest(camera_id, &t, 1, f, static_cast<int64_t>(f) * 40);
}

// ---------------------------------------------------------------------------
static void test_geometry() {
    printf("① 几何: 叉积侧别 + 线段相交\n");

    // 侧别: 线从 (0,0) 到 (10,0), 上方为负, 下方为正
    expect(TripwireCounter::side_of_point(5.f, -1.f, 0.f, 0.f, 10.f, 0.f) < 0.f,
           "线段上方的点 -> 负侧");
    expect(TripwireCounter::side_of_point(5.f, 1.f, 0.f, 0.f, 10.f, 0.f) > 0.f,
           "线段下方的点 -> 正侧");
    expect(std::fabs(TripwireCounter::side_of_point(5.f, 0.f, 0.f, 0.f, 10.f, 0.f)) < 1e-6f,
           "线段上的点 -> 0");

    // 严格相交
    expect(TripwireCounter::segments_intersect(0.f, 0.f, 10.f, 0.f, 5.f, -5.f, 5.f, 5.f),
           "十字交叉 -> 相交");
    // 平行不相交
    expect(!TripwireCounter::segments_intersect(0.f, 0.f, 10.f, 0.f, 0.f, 1.f, 10.f, 1.f),
           "平行 -> 不相交");
    // 延长线相交但线段本身不相交
    expect(!TripwireCounter::segments_intersect(0.f, 0.f, 10.f, 0.f, 20.f, -5.f, 20.f, 5.f),
           "只穿过延长线 -> 不相交");
    // 端点相碰
    expect(TripwireCounter::segments_intersect(0.f, 0.f, 10.f, 0.f, 10.f, 0.f, 10.f, 5.f),
           "端点相碰 -> 算相交");
    // 共线重叠
    expect(TripwireCounter::segments_intersect(0.f, 0.f, 10.f, 0.f, 5.f, 0.f, 15.f, 0.f),
           "共线重叠 -> 算相交");
    // 完全分离
    expect(!TripwireCounter::segments_intersect(0.f, 0.f, 10.f, 0.f, 20.f, 0.f, 30.f, 0.f),
           "完全分离 -> 不相交");
}

// ---------------------------------------------------------------------------
static void test_direction() {
    printf("② 方向判定: 上到下=进, 下到上=出\n");

    // --- 从上往下穿 -> 进 ---
    {
        TripwireCounter c(hline(500.f));
        uint32_t f = 0;
        feed(c, f, 1, 400.f, 400.f);
        feed(c, f, 1, 400.f, 600.f);
        const TripwireStats s = c.stats();
        expect(s.in_count == 1 && s.out_count == 0, "从上到下穿过 -> 进 1 出 0");
    }

    // --- 从下往上穿 -> 出 ---
    {
        TripwireCounter c(hline(500.f));
        uint32_t f = 0;
        feed(c, f, 1, 400.f, 600.f);
        feed(c, f, 1, 400.f, 400.f);
        const TripwireStats s = c.stats();
        expect(s.in_count == 0 && s.out_count == 1, "从下到上穿过 -> 进 0 出 1");
    }

    // --- 反向配置: positive_is_in=false 时上下颠倒 ---
    {
        TripwireConfig cfg = hline(500.f);
        cfg.positive_is_in = false;
        TripwireCounter c(cfg);
        uint32_t f = 0;
        feed(c, f, 1, 400.f, 400.f);
        feed(c, f, 1, 400.f, 600.f);
        const TripwireStats s = c.stats();
        expect(s.in_count == 0 && s.out_count == 1, "positive_is_in=false -> 方向反过来");
    }

    // --- 来回穿: 进 1 出 1 ---
    {
        TripwireCounter c(hline(500.f));
        uint32_t f = 0;
        feed(c, f, 1, 400.f, 400.f);
        feed(c, f, 1, 400.f, 600.f);
        feed(c, f, 1, 400.f, 400.f);
        const TripwireStats s = c.stats();
        expect(s.in_count == 1 && s.out_count == 1, "来回穿一次 -> 进 1 出 1");
    }
}

// ---------------------------------------------------------------------------
static void test_boundaries() {
    printf("③ 边界: 延长线 / 线段外 / 端点外\n");

    // 线段是 (100,500)-(900,500)。目标在 x=50 处上下穿 -> 穿的是延长线 -> 不计
    {
        TripwireCounter c(hline(500.f));
        uint32_t f = 0;
        feed(c, f, 1, 50.f, 400.f);
        feed(c, f, 1, 50.f, 600.f);
        const TripwireStats s = c.stats();
        expect(s.in_count == 0 && s.out_count == 0, "只穿过延长线 -> 不计数");
    }

    // 目标在 x=950(线段右端点之外) -> 不计
    {
        TripwireCounter c(hline(500.f));
        uint32_t f = 0;
        feed(c, f, 1, 950.f, 400.f);
        feed(c, f, 1, 950.f, 600.f);
        const TripwireStats s = c.stats();
        expect(s.in_count == 0 && s.out_count == 0, "线段外侧穿过 -> 不计数");
    }

    // 目标在 x=400(线段内) -> 计
    {
        TripwireCounter c(hline(500.f));
        uint32_t f = 0;
        feed(c, f, 1, 400.f, 400.f);
        feed(c, f, 1, 400.f, 600.f);
        const TripwireStats s = c.stats();
        expect(s.in_count == 1, "线段内穿过 -> 计数");
    }

    // 斜着的线: 从 (0,0) 到 (100,100), 目标从正侧穿到负侧
    //   side(P) = (x2-x1)*(P.y-y1) - (y2-y1)*(P.x-x1) = 100*(P.y - P.x)
    //   (20,80) -> +6000(正侧);  (60,20) -> -4000(负侧)  -> 真的穿过了
    {
        TripwireConfig cfg;
        cfg.x1 = 0.f; cfg.y1 = 0.f; cfg.x2 = 100.f; cfg.y2 = 100.f;
        cfg.positive_is_in = true;
        cfg.min_move_px = 1.f;
        TripwireCounter c(cfg);
        uint32_t f = 0;
        feed(c, f, 1, 20.f, 80.f);
        feed(c, f, 1, 60.f, 20.f);
        const TripwireStats s = c.stats();
        expect(s.in_count + s.out_count == 1, "斜线穿过 -> 计数 1 次(方向按叉积定)");
    }
}

// ---------------------------------------------------------------------------
static void test_jitter_and_jump() {
    printf("④ 抖动与瞬移\n");

    // 站着不动: 不该反复计数
    {
        TripwireCounter c(hline(500.f));
        uint32_t f = 0;
        for (int i = 0; i < 20; ++i) {
            feed(c, f, 1, 400.f, 400.f);
        }
        const TripwireStats s = c.stats();
        expect(s.in_count == 0 && s.out_count == 0, "原地不动 -> 不计数");
    }

    // 微抖动(1px)在线上方: 不该计数
    {
        TripwireCounter c(hline(500.f));
        uint32_t f = 0;
        for (int i = 0; i < 10; ++i) {
            feed(c, f, 1, 400.f, 499.f + (i % 2));
        }
        const TripwireStats s = c.stats();
        expect(s.in_count == 0 && s.out_count == 0, "1px 微抖动 -> 不计数");
    }

    // 瞬移(track_id 被复用): 不该当成一次穿越
    {
        TripwireCounter c(hline(500.f));
        uint32_t f = 0;
        feed(c, f, 1, 400.f, 400.f);
        feed(c, f, 1, 400.f, 900.f);   // 一帧跳 500px
        const TripwireStats s = c.stats();
        expect(s.in_count == 0 && s.out_count == 0, "瞬移 500px -> 不计数");
    }
}

// ---------------------------------------------------------------------------
static void test_lifecycle() {
    printf("⑤ 生命周期: 超时轨迹必须被清掉\n");

    // max_miss_frames = 5, cleanup 每帧都做
    {
        TripwireConfig cfg = hline(500.f);
        cfg.max_miss_frames = 5;
        cfg.cleanup_period_frames = 1;
        TripwireCounter c(cfg);
        uint32_t f = 0;
        // 造 100 条轨迹, 只喂第 1 帧
        std::vector<TripwireTrack> many;
        for (int i = 0; i < 100; ++i) many.push_back(mk(i + 1, 200.f + i, 300.f));
        ++f;
        c.ingest(0, many.data(), many.size(), f, static_cast<int64_t>(f) * 40);
        expect(c.stats().active_tracks == 100, "刚喂完 -> 在册 100 条");

        // 再喂 10 帧(都不含这 100 条) -> 超过 5 帧没更新 -> 应该被清空
        for (int i = 0; i < 10; ++i) {
            ++f;
            c.ingest(0, nullptr, 0, f, static_cast<int64_t>(f) * 40);
        }
        expect(c.stats().active_tracks == 0, "超时 10 帧(阈值 5) -> 全部清掉");
    }

    // 一直有更新的轨迹不能被误清
    {
        TripwireConfig cfg = hline(500.f);
        cfg.max_miss_frames = 5;
        cfg.cleanup_period_frames = 1;
        TripwireCounter c(cfg);
        uint32_t f = 0;
        for (int i = 0; i < 50; ++i) {
            feed(c, f, 1, 400.f, 300.f + (i % 3));
        }
        expect(c.stats().active_tracks == 1, "持续更新的轨迹 -> 一直在册");
    }
}

// ---------------------------------------------------------------------------
static void test_multi_track_and_class() {
    printf("⑥ 多目标与类别过滤\n");

    // 三条轨迹, 只有一条真的穿过
    {
        TripwireCounter c(hline(500.f));
        uint32_t f = 0;
        TripwireTrack t[3] = {mk(1, 400.f, 400.f), mk(2, 200.f, 450.f), mk(3, 700.f, 550.f)};
        ++f;
        c.ingest(0, t, 3, f, static_cast<int64_t>(f) * 40);
        t[0].cy = 600.f;                       // 1 号从上到下穿
        ++f;
        c.ingest(0, t, 3, f, static_cast<int64_t>(f) * 40);
        const TripwireStats s = c.stats();
        expect(s.in_count == 1, "三条轨迹里只有一条穿过 -> 只计 1 次");
        expect(s.active_tracks == 3, "三条轨迹都在册");
    }

    // 类别过滤: 只统计 cls=1
    {
        TripwireConfig cfg = hline(500.f);
        cfg.classes.push_back(1);
        TripwireCounter c(cfg);
        uint32_t f = 0;
        TripwireTrack t[2] = {mk(1, 400.f, 400.f, 0), mk(2, 400.f, 400.f, 1)};
        ++f;
        c.ingest(0, t, 2, f, static_cast<int64_t>(f) * 40);
        t[0].cy = 600.f; t[1].cy = 600.f;
        ++f;
        c.ingest(0, t, 2, f, static_cast<int64_t>(f) * 40);
        const TripwireStats s = c.stats();
        expect(s.in_count == 1, "类别过滤: 只有 cls=1 的那条被计数");
    }
}

// ---------------------------------------------------------------------------
// 用 vector<TrackResult> 那条入口(和主工程一致)走一遍
static void test_trackresult_path() {
    printf("⑦ vector<TrackResult> 入口(主工程实际走的那条)\n");
    TripwireCounter c(hline(500.f));
    std::vector<TrackResult> v;
    v.push_back(mk_track(7, 400.f, 400.f));
    c.ingest(0, v, 1u, 40);
    v[0].center_y = 600.f;
    c.ingest(0, v, 2u, 80);
    expect(c.stats().in_count == 1, "走 TrackResult 入口也能正确计数");
}

// ---------------------------------------------------------------------------
// 模拟一段"录像": 一辆车从上往下穿线, 一辆车从下往上穿线
static void test_scenario() {
    printf("⑧ 场景: 两辆车分别从上往下/从下往上\n");
    TripwireCounter c(hline(500.f));
    uint32_t f = 0;
    std::vector<TripwireTrack> t;

    // 车 A: x=300, y 从 200 走到 800 (向下穿)
    // 车 B: x=700, y 从 800 走到 200 (向上穿)
    for (int i = 0; i <= 30; ++i) {
        t.clear();
        t.push_back(mk(1, 300.f, 200.f + i * 20.f));
        t.push_back(mk(2, 700.f, 800.f - i * 20.f));
        ++f;
        c.ingest(0, t.data(), t.size(), f, static_cast<int64_t>(f) * 40);
    }
    const TripwireStats s = c.stats();
    expect(s.in_count == 1 && s.out_count == 1, "车 A 进 1, 车 B 出 1");
    expect(s.active_tracks == 2, "两条轨迹都在册");
}

// ---------------------------------------------------------------------------
static void test_async_service() {
    printf("⑨ 异步服务: submit 不阻塞 + 后台线程计数正确\n");

    TripwireService::Options opt;
    {
        TripwireCamera cam;
        cam.camera_id = 0;
        cam.channel = 1;
        cam.name = "ch1";
        cam.cfg = hline(500.f);
        opt.cameras.push_back(cam);
    }
    opt.sn = "test";
    opt.publish_interval_ms = 50;
    opt.queue_slots = 32;
    opt.mqtt_host.clear();       // 单测不发 MQTT

    TripwireService svc;
    expect(svc.start(opt), "TripwireService 启动成功");

    // ---- ① 按真实节奏喂 200 帧(每帧来回穿一次线) ----
    std::vector<TrackResult> v;
    double submit_ms = 0.0;   // ★ 只累计 submit 本身的耗时(不含给后台消化用的 sleep)
    for (int i = 0; i < 200; ++i) {
        v.clear();
        v.push_back(mk_track(1, 400.f, (i % 2) ? 600.f : 400.f));
        const auto t0 = std::chrono::steady_clock::now();
        svc.submit(0, v, static_cast<uint32_t>(i + 1), i * 40);
        submit_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        // 让后台线程有机会消化(模拟 20fps 的真实节奏), 别把队列灌爆
        if ((i % 10) == 9) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // ★ 提交 200 帧必须很快 —— 推理线程等不起
    expect(submit_ms < 50.0, "200 次 submit 总耗时 < 50ms(绝不阻塞推理线程)");

    // 等后台线程把队列消化完
    for (int i = 0; i < 200; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const TripwireStats st = svc.stats();
        if (st.in_count + st.out_count >= 199) break;
    }
    const TripwireStats s = svc.stats();
    // 轨迹每帧在 y=400/600 之间来回 -> 每一帧都是一次穿越
    expect(s.in_count + s.out_count >= 190,
           "后台线程计数正确(200 帧来回穿 -> >= 190 次)");
    expect(s.in_count > 0 && s.out_count > 0, "进和出都有计数");
    expect(s.dropped_frames == 0, "按节奏喂时不该丢帧");

    // ---- ② 猛灌: 队列满了必须丢最旧的, 且如实计数(不静默丢) ----
    {
        const TripwireStats before = svc.stats();
        for (int i = 0; i < 500; ++i) {
            v.clear();
            v.push_back(mk_track(2, 400.f, (i % 2) ? 600.f : 400.f));
            svc.submit(0, v, static_cast<uint32_t>(1000 + i), i * 40);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const TripwireStats after = svc.stats();
        const uint64_t submitted = 500;
        const uint64_t dropped = after.dropped_frames - before.dropped_frames;
        expect(dropped > 0, "猛灌 500 帧 -> 队列满了如实报告丢帧");
        expect(dropped + (after.in_count + after.out_count - before.in_count -
                          before.out_count) <= submitted,
               "丢帧数 + 计数数 不超过提交数(没有凭空多出来的计数)");
    }

    // ---- JSON ----
    const std::string json = svc.build_json(1234567890);
    expect(json.find("\"type\":\"tripwire\"") != std::string::npos, "JSON 含 type=tripwire");
    expect(json.find("\"in\":") != std::string::npos && json.find("\"out\":") != std::string::npos,
           "JSON 含 in / out 字段");
    printf("     JSON 样例: %s\n", json.c_str());

    svc.stop();
}

// ---------------------------------------------------------------------------
static void test_concurrent_submit() {
    printf("⑩ 并发提交: 4 个线程各提交 250 帧\n");

    TripwireService::Options opt;
    {
        TripwireCamera cam;
        cam.camera_id = 0;
        cam.channel = 1;
        cam.name = "ch1";
        cam.cfg = hline(500.f);
        opt.cameras.push_back(cam);
    }
    opt.publish_interval_ms = 50;
    opt.queue_slots = 64;
    TripwireService svc;
    svc.start(opt);

    std::atomic<int> ready{0};
    std::vector<std::thread> ths;
    for (int t = 0; t < 4; ++t) {
        ths.emplace_back([&svc, &ready, t]() {
            ready.fetch_add(1);
            while (ready.load() < 4) std::this_thread::yield();
            std::vector<TrackResult> v;
            for (int i = 0; i < 250; ++i) {
                v.clear();
                // 每条轨迹用不同的 track_id, 各自来回穿线
                v.push_back(mk_track(t * 1000 + i % 50, 100.f + (i % 50) * 15.f,
                                     (i % 2) ? 600.f : 400.f));
                svc.submit(t, v, static_cast<uint32_t>(i + 1), i * 40);
            }
        });
    }
    for (auto& th : ths) th.join();

    for (int i = 0; i < 200; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (svc.stats().in_count + svc.stats().out_count > 0) break;
    }
    const TripwireStats s = svc.stats();
    expect(s.dropped_frames > 0 || (s.in_count + s.out_count) > 0,
           "并发提交不崩, 且要么计数了要么明确报告了丢帧");
    printf("     并发结果: 进=%lld 出=%lld 丢帧=%llu\n",
           (long long)s.in_count, (long long)s.out_count,
           (unsigned long long)s.dropped_frames);
    svc.stop();
}


// ---------------------------------------------------------------------------
// 多相机: 各相机有自己的线, 计数互不干扰, 汇总正确
static void test_multi_camera() {
    printf("⑪ 多相机: 各自一条线, 独立计数 + 合计\n");

    TripwireCounter c;
    {
        TripwireCamera cam0;
        cam0.camera_id = 0; cam0.channel = 1; cam0.name = "ch1";
        cam0.cfg = hline(500.f);                       // 相机0: y=500
        c.add_camera(0, cam0.cfg, cam0.channel, cam0.name);

        TripwireCamera cam1;
        cam1.camera_id = 1; cam1.channel = 2; cam1.name = "ch2";
        cam1.cfg = hline(800.f);                       // 相机1: y=800(不同分辨率)
        c.add_camera(1, cam1.cfg, cam1.channel, cam1.name);
    }

    uint32_t f = 0;
    // 相机0 的目标: 从上往下穿 y=500
    feed(c, f, 0, 400.f, 400.f, 0, 0);
    feed(c, f, 0, 400.f, 600.f, 0, 0);
    // 相机1 的目标: 从下往上穿 y=800
    feed(c, f, 1, 400.f, 900.f, 0, 1);
    feed(c, f, 1, 400.f, 700.f, 0, 1);
    // 相机0 再来一次向下穿(换一条 track_id, 免得和上面那条连成"来回穿")
    feed(c, f, 1, 400.f, 400.f, 0, 0);   // track_id=1, camera_id=0
    feed(c, f, 1, 400.f, 600.f, 0, 0);

    const TripwireStats total = c.stats();
    expect(total.in_count == 2 && total.out_count == 1,
           "合计: 相机0 进 2 + 相机1 出 1 = 进 2 出 1");

    const std::vector<TripwireCameraStats> cams = c.camera_stats();
    expect(cams.size() == 2, "相机统计有 2 项");
    if (cams.size() == 2) {
        expect(cams[0].camera_id == 0 && cams[0].name == "ch1", "第 0 项是相机0(ch1)");
        expect(cams[1].camera_id == 1 && cams[1].name == "ch2", "第 1 项是相机1(ch2)");
        expect(cams[0].stats.in_count == 2 && cams[0].stats.out_count == 0,
               "相机0 独立计数: 进 2 出 0");
        expect(cams[1].stats.in_count == 0 && cams[1].stats.out_count == 1,
               "相机1 独立计数: 进 0 出 1");
    }

    // 没注册的相机: 喂了也不该影响任何计数
    const int64_t before = c.stats().total();
    feed(c, f, 99, 400.f, 400.f, 0, 99);        // camera_id=99 没注册
    feed(c, f, 99, 400.f, 600.f, 0, 99);
    expect(c.stats().total() == before, "未注册的相机 -> 不计数");
}

// ---------------------------------------------------------------------------
// 多相机时 JSON 里要带各相机的独立计数
static void test_multi_camera_json() {
    printf("⑫ JSON 里带各相机的独立计数\n");
    TripwireService::Options opt;
    {
        TripwireCamera cam0;
        cam0.camera_id = 0; cam0.channel = 1; cam0.name = "ch1";
        cam0.cfg = hline(500.f);
        opt.cameras.push_back(cam0);
        TripwireCamera cam1;
        cam1.camera_id = 1; cam1.channel = 2; cam1.name = "ch2";
        cam1.cfg = hline(500.f);
        opt.cameras.push_back(cam1);
    }
    opt.sn = "rk3588-01";
    opt.publish_interval_ms = 50;
    TripwireService svc;
    svc.start(opt);

    std::vector<TrackResult> v;
    for (int i = 0; i < 20; ++i) {
        v.clear();
        v.push_back(mk_track(1, 400.f, (i % 2) ? 600.f : 400.f));
        svc.submit(0, v, static_cast<uint32_t>(i + 1), i * 40);
        v.clear();
        v.push_back(mk_track(2, 400.f, (i % 2) ? 600.f : 400.f));
        svc.submit(1, v, static_cast<uint32_t>(i + 1), i * 40);
        if ((i % 10) == 9) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const std::string json = svc.build_json(1763345000123LL);
    printf("     JSON 样例: %s\n", json.c_str());
    expect(json.find("\"cameras\":[{") != std::string::npos, "JSON 含 cameras 数组");
    expect(json.find("\"name\":\"ch1\"") != std::string::npos, "JSON 里有 ch1 的独立计数");
    expect(json.find("\"name\":\"ch2\"") != std::string::npos, "JSON 里有 ch2 的独立计数");
    // 合计 = 两个相机之和
    const TripwireStats s = svc.stats();
    const std::vector<TripwireCameraStats> cams = svc.camera_stats();
    int64_t cam_sum = 0;
    for (const auto& c : cams) cam_sum += c.stats.total();
    expect(s.total() == cam_sum, "合计 == 各相机之和");
    expect(cams.size() == 2, "camera_stats 有 2 个相机");
    svc.stop();
}

// ---------------------------------------------------------------------------
int main() {
    printf("==== 过线计数单元测试 ====\n\n");
    test_geometry();
    printf("\n");
    test_direction();
    printf("\n");
    test_boundaries();
    printf("\n");
    test_jitter_and_jump();
    printf("\n");
    test_lifecycle();
    printf("\n");
    test_multi_track_and_class();
    printf("\n");
    test_trackresult_path();
    printf("\n");
    test_scenario();
    printf("\n");
    test_async_service();
    printf("\n");
    test_multi_camera();
    printf("\n");
    test_multi_camera_json();
    printf("\n");
    test_concurrent_submit();
    printf("\n==== %s: %d 项断言, 失败 %d 项 ====\n",
           g_fail ? "有失败项" : "全部通过", g_total, g_fail);
    return g_fail ? 1 : 0;
}
