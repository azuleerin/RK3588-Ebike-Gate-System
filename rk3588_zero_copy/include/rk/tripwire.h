// rk/tripwire.h —— 过线计数(虚拟线)
//
// ★ 二期 KPI: 统计"从虚拟线上方驶入/驶出"的车辆数。
//
// ★ 设计红线(全部已在实现里落地):
//   1) 线段相交判定 O(1) —— 纯向量叉积, 没有任何轮询/遍历。
//   2) 不碰 std::map —— 轨迹状态用 std::unordered_map(初始化时 reserve),
//      且【只在 worker 线程里】读写; 帧快照走定长环形缓冲, 提交路径零分配。
//   3) 完全异步 —— 推理线程只调 TripwireService::submit(), 它只做一次
//      定长拷贝就返回; 计数、JSON 组包、MQTT 发送全在后台线程里。
//      推理线程【绝不】碰网络, 绝不等待。
//
// ★ 多相机: 一个【进程一个】TripwireService, 管所有相机。
//   每台相机有自己的虚拟线(分辨率不一样, 坐标不能共用), 但统计汇总成一份
//   JSON 上报: 既有合计(in/out/total), 也有各相机的独立计数(cameras[])。
//
// ★ 坐标系: 全部用【原图像素坐标】(不是模型输入坐标)。
//   图像坐标系 y 轴向下, 所以"从上到下"是 y 增大。
//
// ★ 方向定义(用叉积, 一句话讲清):
//   side(P) = (x2-x1)*(P.y-y1) - (y2-y1)*(P.x-x1)
//     · side > 0: P 在有向直线 P1->P2 的正侧
//     · side < 0: P 在负侧
//   一条轨迹从负侧穿到正侧 = "正方向穿越"; 反过来 = "负方向穿越"。
//   cfg.positive_is_in = true  -> 正方向穿越算"进", 负方向算"出"
//   直觉对照: 线从左画到右(x1<x2, y1==y2)时 side(P) = (x2-x1)*(P.y-y1),
//   线下方为正、上方为负, 所以 positive_is_in=true 就是"从上到下为进"。
#ifndef RK_TRIPWIRE_H_
#define RK_TRIPWIRE_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "speed_track.h"   // rk::TrackResult

namespace rk {

// ---------------------------------------------------------------------------
// 配置(一台相机一条线)
// ---------------------------------------------------------------------------
struct TripwireConfig {
    // ---- 虚拟线的两个端点(原图像素坐标) ----
    float x1 = 0.f, y1 = 0.f;
    float x2 = 0.f, y2 = 0.f;

    // ---- 进/出方向 ----
    //   true : 正方向穿越(负侧->正侧)算"进"
    //   线从左画到右时, true == "从上到下为进"(见头文件说明)
    bool   positive_is_in = true;

    // ---- 只统计这些类别; 空 = 全部类别都算 ----
    std::vector<int> classes;

    // ---- 轨迹生命周期 ----
    //   连续多少帧没更新就把它从缓存里清掉(严格防泄漏)
    int    max_miss_frames = 30;
    //   每隔多少帧扫一次缓存做清理(摊销成 O(在册数/周期), 默认在册只有几十条)
    int    cleanup_period_frames = 10;
    //   在册轨迹数硬上限(兜底; 超了就拒绝新轨迹, 不无限涨)
    int    max_tracks = 512;

    // ---- 抖动/误判防护 ----
    //   一帧内位移小于这个值就不判定穿越(目标站着不动时别反复计)
    float  min_move_px = 2.0f;
    //   一帧内位移大于这个值就认为"换了目标"(track_id 被复用),
    //   重置历史但不计数 —— 否则一次瞬移会被当成一次穿越
    float  max_jump_px = 400.0f;

    // 线是否有效(两个端点不能重合)
    bool valid() const {
        const float dx = x2 - x1, dy = y2 - y1;
        return (dx * dx + dy * dy) > 1e-6f;
    }
};

// ---------------------------------------------------------------------------
// 一台相机的注册信息
// ---------------------------------------------------------------------------
struct TripwireCamera {
    int         camera_id = 0;      // 内部编号(通道序号)
    int         channel = 0;        // 对外通道号
    std::string name;               // 通道名(ch1)
    TripwireConfig cfg;             // 这台相机的虚拟线
};

// ---------------------------------------------------------------------------
// 一条轨迹的快照(定长 POD —— 提交路径上零分配)
// ---------------------------------------------------------------------------
struct TripwireTrack {
    int   track_id = -1;
    int   cls_id = 0;
    float cx = 0.f;    // 中心点 x(原图坐标)
    float cy = 0.f;    // 中心点 y(原图坐标)
};

// ---------------------------------------------------------------------------
// 一次穿越事件
// ---------------------------------------------------------------------------
struct TripwireEvent {
    int64_t  ts_ms = 0;
    uint32_t frame = 0;
    int      camera_id = 0;
    int      track_id = -1;
    int      cls_id = 0;
    bool     is_in = false;      // true=进, false=出
    float    x = 0.f, y = 0.f;   // 穿越时的目标中心
};

// ---------------------------------------------------------------------------
// 统计快照
// ---------------------------------------------------------------------------
struct TripwireStats {
    int64_t  in_count = 0;
    int64_t  out_count = 0;
    int      active_tracks = 0;    // 在册(缓存里)的轨迹数
    uint64_t dropped_frames = 0;    // 因队列满被丢掉的帧
    uint64_t truncated = 0;        // 因单帧轨迹数超限被截断的条数
    uint64_t rejected = 0;         // 因在册数超上限被拒的新轨迹
    int64_t  last_event_ts_ms = 0;
    int64_t  last_publish_ts_ms = 0;

    int64_t total() const { return in_count + out_count; }
};

// 单台相机的统计(上报 JSON 的 cameras[] 用)
struct TripwireCameraStats {
    int         camera_id = 0;
    int         channel = 0;
    std::string name;
    TripwireStats stats;
};

// ---------------------------------------------------------------------------
// ★ 过线计数器 —— 纯逻辑, 不含任何线程
//
//   线程安全: 本类【不是】线程安全的, 由调用方保证只在【一个】线程里用。
//   TripwireService 内部就是: 只有它的 worker 线程碰这个对象。
// ---------------------------------------------------------------------------
class TripwireCounter {
public:
    TripwireCounter() = default;
    explicit TripwireCounter(const TripwireConfig& cfg) { add_camera(0, cfg); }

    // ---- 几何: 全部 O(1) ----

    // 点 P 在有向直线 P1->P2 的哪一侧: >0 正侧, <0 负侧, 0 线上
    static float side_of_point(float px, float py,
                               float x1, float y1, float x2, float y2);

    // 线段 A1A2 与线段 B1B2 是否相交(含共线重叠与端点相碰)
    static bool segments_intersect(float ax1, float ay1, float ax2, float ay2,
                                   float bx1, float by1, float bx2, float by2);

    // ---- 计数 ----

    // 注册一台相机及其虚拟线。必须在 ingest 之前调用。
    //   (注册之后 cams_ 的【配置】只读, 运行期不再增删 -> 不分配)
    void add_camera(int camera_id, const TripwireConfig& cfg,
                    int channel = 0, const std::string& name = "");

    // 喂一帧的轨迹快照。tracks 是定长数组的一部分, 本函数【不分配内存】。
    //   camera_id : 摄像头编号(和 track_id 一起构成缓存的键)
    //   frame_idx : 帧号(用来算"多少帧没更新")
    //   ts_ms     : 单调时钟毫秒
    void ingest(int camera_id, const TripwireTrack* tracks, size_t n,
                uint32_t frame_idx, int64_t ts_ms);

    // 便捷重载: 直接吃 ByteTrack 的结果(内部用栈上定长缓冲, 仍然零堆分配)
    void ingest(int camera_id, const std::vector<TrackResult>& tracks,
                uint32_t frame_idx, int64_t ts_ms);

    // 清空所有状态(换班/重置用)
    void reset();

    TripwireStats stats() const { return total_; }
    // 各相机的独立统计(按 camera_id 升序)
    std::vector<TripwireCameraStats> camera_stats() const;

    // 事件回调(可选)。在 ingest 里同步触发 —— 回调里不要做阻塞操作。
    void set_event_cb(std::function<void(const TripwireEvent&)> cb) { on_event_ = std::move(cb); }

    // 最近一次穿越事件(调试用)
    const TripwireEvent& last_event() const { return last_event_; }

private:
    // 一个 (camera_id, track_id) 的历史状态
    struct TrackState {
        float    px = 0.f, py = 0.f;      // 上一次的中心点
        bool     has_prev = false;
        // ★ 上一次所在【非零】侧的符号(+1/-1)。
        //   目标正好停在线上时 side 是 0, 这时保留上一个非零侧,
        //   等它离开线的那一帧再判定方向 —— 不会漏计也不会重复计。
        int      side = 0;
        uint32_t last_frame = 0;
        int64_t  last_ts_ms = 0;
    };

    // 一台相机的线 + 计数
    struct CameraState {
        TripwireConfig cfg;
        TripwireStats  stats;
        int            channel = 0;
        std::string    name;
    };

    void cleanup_expired(uint32_t cur_frame);
    static bool class_allowed(const TripwireConfig& cfg, int cls_id);

    // 相机表: 注册之后【配置】只读(运行期只查配置、不改)
    std::unordered_map<int, CameraState> cams_;
    // ★ 轨迹状态: unordered_map + 初始化时 reserve, 运行期只增删不扩容。
    //   键 = (camera_id << 32) | track_id, 一个 map 装所有摄像头。
    std::unordered_map<uint64_t, TrackState> states_;

    TripwireStats total_;
    TripwireEvent last_event_;
    std::function<void(const TripwireEvent&)> on_event_;
    uint32_t last_cleanup_frame_ = 0;
    uint32_t cur_frame_ = 0;
};

// ---------------------------------------------------------------------------
// ★ 异步服务 —— 推理线程只 submit(), 计数和 MQTT 全在后台线程
//
//   一个进程一个服务, 管所有相机(每台相机一条虚拟线)。
//   推理线程调用 submit() 的开销: 一次定长数组拷贝 + 一次 mutex(微秒级),
//   【不】做任何网络 IO、不等待结果、不分配堆内存。
//   队列满了就丢【最旧】的一帧(和 ChanDispatcher 一个路子), 并计数上报。
// ---------------------------------------------------------------------------
class TripwireService {
public:
    struct Options {
        // 各相机的虚拟线(一台相机一条; 分辨率不同所以不能共用坐标)
        std::vector<TripwireCamera> cameras;

        std::string sn = "rk3588-01";

        // ---- MQTT ----
        std::string mqtt_host;
        std::string mqtt_topic;          // 空 = 不发 MQTT(只本地计数)
        std::string mqtt_client_id;
        int      mqtt_port = 1883;
        int      mqtt_qos = 0;

        // ---- 节奏 ----
        int64_t  publish_interval_ms = 1000;   // 统计上报周期
        size_t   queue_slots = 16;              // 帧快照环形缓冲深度
    };

    TripwireService();
    ~TripwireService();

    TripwireService(const TripwireService&) = delete;
    TripwireService& operator=(const TripwireService&) = delete;

    bool start(const Options& opt);
    void stop();

    // ★ 推理线程入口: 【绝不阻塞】
    void submit(int camera_id, const std::vector<TrackResult>& tracks,
                uint32_t frame_idx, int64_t ts_ms);

    // 只读统计(给上层查询用)
    TripwireStats stats() const;
    // 各相机的独立统计
    std::vector<TripwireCameraStats> camera_stats() const;

    // 组一份 JSON(和 MQTT 发出去的内容完全一样), 方便单测和调试
    std::string build_json(int64_t now_ms) const;

private:
    // 一帧的快照槽(定长, 提交时不分配)
    struct Slot {
        int      camera_id = 0;
        uint32_t frame = 0;
        int64_t  ts_ms = 0;
        int      n = 0;
        TripwireTrack tracks[128];
    };

    void worker_loop();

    Options opt_;
    std::unique_ptr<TripwireCounter> counter_;
    bool started_ = false;

    // ---- 帧快照环形缓冲(固定容量, 满了丢最旧) ----
    std::vector<std::unique_ptr<Slot>> slots_;
    std::deque<size_t> free_slots_;   // 空闲槽(生产者拿)
    std::deque<size_t> ready_;        // 就绪槽(消费者拿)
    std::mutex m_;
    std::condition_variable cv_;
    bool quit_ = false;

    // ---- 统计(worker 线程写, 外部读 —— 用原子快照) ----
    std::atomic<uint64_t> in_count_{0};
    std::atomic<uint64_t> out_count_{0};
    std::atomic<uint64_t> dropped_frames_{0};
    std::atomic<uint64_t> truncated_{0};
    std::atomic<uint64_t> rejected_{0};
    std::atomic<int>      active_tracks_{0};
    std::atomic<int64_t>  last_event_ts_ms_{0};
    std::atomic<int64_t>  last_publish_ts_ms_{0};
    int64_t               last_event_log_ms_ = 0;   // 事件日志限流(1 秒一条)

    // 各相机的独立计数(worker 线程写)。读的时候用一把【独立的】锁,
    // 不和提交路径抢同一把锁 —— 外部查询不该把推理线程卡住。
    mutable std::mutex cam_m_;
    std::vector<TripwireCameraStats> cam_stats_;

    std::thread th_;
};

}  // namespace rk

#endif  // RK_TRIPWIRE_H_
