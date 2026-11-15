// speed_track.h —— 跟踪 + 测速骨架
//
// 按架构组《测速稳定性优化 —— 板端 C++ 移植核心伪代码》(docs/speed_stabilization_cpp.md)
// 移植, 与 Python 侧 speed_demo.py 口径一致。
//
// 移植的三条硬约束(本工程的落实位置):
//   1) 几何过滤必须【前置】: 在跟踪之前就剔掉扁宽巨型假框, 否则它已经占了一个
//      track ID, 再过滤也来不及。见 yolo_post.h 的 is_bad_box()。
//   2) 【不用 EMA】: 显示值用滑动窗口中位数; 时间戳一律用真实单调时钟
//      (std::chrono::steady_clock), 不用 frame_idx / fps 推算。
//   3) 地面接地点用 5 帧滑动中值, 且 x/y 分开滤波(合在一起做二维中值会在
//      x 方向引入滞后)。
#ifndef RK_SPEED_TRACK_H_
#define RK_SPEED_TRACK_H_

#include <cstdint>
#include <vector>

#include "ipm.h"
#include "yolo_post.h"

namespace rk {

// ---------------------------------------------------------------------------
// 5 帧滑动中值（环形缓冲 + 插入排序, K=5 时 O(1), 无堆分配）
//
// 语义与 Python 的 `deque(maxlen=K)` + `sorted(...)[n//2]` 严格一致:
//   保留【最近 K 个】(按时间, 丢最旧的), 升序排列后取中位数。
//
// ⚠️ 两个容易写错的点:
//   1) 窗口没满时也必须保持有序, 否则取到的不是中位数;
//   2) 窗口满时丢的是【最旧的】, 不是【最小的】——
//      只维护一个有序数组会丢掉时间顺序, 必须用环形缓冲保留插入顺序。
// ---------------------------------------------------------------------------
class Med5 {
public:
    float push(float x) {
        ring_[head_] = x;
        head_ = (head_ + 1) % 5;
        if (n_ < 5) n_++;

        // 按插入顺序取出最近 n_ 个值, 插入排序(栈上数组, 无堆分配)
        float s[5];
        int m = 0;
        for (int i = 0; i < n_; ++i) {
            const float v = ring_[(head_ - n_ + i + 5) % 5];
            int j = m++;
            while (j > 0 && s[j - 1] > v) { s[j] = s[j - 1]; --j; }
            s[j] = v;
        }
        return s[m / 2];
    }
    void reset() { head_ = 0; n_ = 0; }
    int  count() const { return n_; }

private:
    float ring_[5] = {0.f, 0.f, 0.f, 0.f, 0.f};
    int   head_ = 0;
    int   n_ = 0;
};

// ---------------------------------------------------------------------------
// 参数（数值来自 Python 侧实测, 勿随意改）
// ---------------------------------------------------------------------------
struct SpeedCfg {
    // 检测
    float conf_thr = 0.25f;
    float nms_iou  = 0.60f;      // 注意：不是默认的 0.70

    // 模块 A: 几何先验
    GeoFilterCfg geo;

    // 模块 B/D/E: 跟踪 (沿用 ultralytics bytetrack.yaml)
    float track_high_thresh = 0.25f;
    float track_low_thresh  = 0.10f;
    float new_track_thresh  = 0.25f;
    float match_thresh      = 0.80f;
    int   track_buffer      = 30;
    // ★ 兜底关联(中心距离)的开关 + 阈值
    //   开着: IoU 配不上时, 对【过期轨迹】再按中心距离试一次
    //   (实测: 80px 的框、目标每帧走 6px 时, stride=2 的两次检测 IoU 只有 0.74,
    //    低于 match_thresh 0.80 -> 不开兜底就是一次都配不上 -> 1000 帧开 30 条轨迹)
    //   关掉: 回到老的纯 IoU 关联
    bool   match_fallback      = true;
    float  fallback_dist_ratio = 0.6f;   // 中心距离 < 框宽 * 这个比例

    // 模块 C: 地面接地点中值滤波
    int    ground_median_k = 5;      // 帧

    // 模块 E: 速度与显示
    double speed_win_sec     = 0.80; // 测速时间窗
    double display_span      = 0.80; // 显示/判定滑动时长
    int    min_speed_samples = 6;    // 起算所需样本
    double min_speed_dt      = 0.12; // 起算所需时间跨度
    int    min_track_samples = 8;    // 少于这个数不显示速度(防假警)
    float  speed_limit_kmh   = 25.f;
    bool   lsq = false;              // 估计器: false=chord(默认, 与 Python 一致), true=lsq

    // 模块 F: 静态背景过滤(广告牌之类的"鬼影")
    //   现场有一类固定在画面里的东西(广告牌/贴纸)被模型当成车/人, 而且它
    //   【永远不动】—— track ID 一直挂着, 每帧都送 LPRNet 推理, 白费算力,
    //   偶尔还会读出一串假车牌上报给业务层。
    bool   static_filter   = false;   // 总开关(默认关, 用 --static-filter 打开)
    int    static_window   = 30;      // 看最近多少【帧】的位移
    float  static_min_disp = 5.f;     // 窗口内最大位移小于它 -> 判为静止背景(像素)
    int    static_min_hits = 10;      // 窗口内至少要有这么多样本才敢判静止
};

// 标定: 二选一
//   IPM 模式  : 给 quad + 宽度 -> 鸟瞰图, 比例尺 = 1/ppm
//   单点比例尺: 给 p1,p2 + 真实长度 -> 比例尺 = real_meters / baseline_px
struct SpeedCalib {
    const BevView* bev = nullptr;
    Pt      p1, p2;
    double  real_meters = 0.0;
    double  zone_margin_px = 0.0;

    bool valid() const { return bev != nullptr || real_meters > 0.0; }

    // 地面点 -> 度量坐标。返回 false 表示"出界/无效", 这段历史要作废。
    bool to_metric(double gx, double gy, double* mx, double* my) const;

    // 米/像素
    double meters_per_pixel() const;
};

// 一条 track 的输出
struct TrackResult {
    int   track_id = -1;
    YoloBox box;            // 原始画面坐标
    float score = 0.f;
    float speed_kmh = 0.f;
    bool  speed_valid = false;
    bool  over_speed = false;
    bool  in_trap = false;  // 是否在标定区内
    bool  static_bg = false;// ★ 模块 F: 判定为静止背景(广告牌之类的鬼影)
    float static_disp = 0.f;// 模块 F: 判定用的"窗口内最大位移"(像素), 便于调阈值
    std::string plate;      // LPRNet 识别出的车牌(为空表示没识别)
};

// ---------------------------------------------------------------------------
class SpeedTracker {
public:
    // ★ 同时在册的轨迹数上限。
    //   为什么要有上限: 轨迹表是 std::vector<Track>, Track 里有三组环形缓冲
    //   (h_*/s_*/p_*), 单条约 4KB。轨迹数涨上去时 vector 翻倍扩容, 每一次
    //   扩容都是几百 KB 的重新分配 —— 活堆曲线因此变成一阶��上台阶
    //   (实测 4K 路口视频: 前 2600 帧累计涨 1.2MB, 每次上涨都对应轨迹数 +15)。
    //   一次 reserve 到位 + 这个上限 = 永不重分配 = 活堆绝对水平线。
    //   128 条并发轨迹 = 1.2 秒内出现 128 个不同目标, 真实路口远达不到
    //   (实测峰值 87 条), 所以这个上限不会误伤正常场景。
    static constexpr size_t kMaxTracks = 128;

    void reset();

    // 处理一帧: 关联 -> 地面点中值 -> IPM -> 速度 -> 输出
    // t 必须是【真实时间戳】(单调时钟秒), 不要用 frame_idx / fps
    void update(const std::vector<YoloBox>& dets, double t, const SpeedCfg& cfg,
                const SpeedCalib& calib, std::vector<TrackResult>* out);

    // ★ 只做运动预测(抽帧推理时用): 把每条轨迹的框按当前像素速度往前推。
    //
    //   为什么需要它:
    //     抽帧推理时, 相邻两次【真实检测】之间隔了 N 帧, 目标已经移动了。
    //     关联是拿新检测和轨迹的【上一个框】算 IoU(match_thresh=0.80),
    //     目标跑起来时那个 IoU 会掉到阈值以下 -> 关联失败 -> 旧轨迹 missed++、
    //     新检测另开一条 -> 【ID 乱跳】。
    //     把框按速度往前推, 下一个真实检测再跟【预测位置】比, IoU 就回来了。
    //
    //   它【不】做的事:
    //     · 不加检测样本(h_* 速度历史)—— 测速只用真实检测, 不掺假样本
    //     · 不动 hits/age/missed —— 这一帧不是"漏检", 是"没去看"
    void predict(double t);

    // 运动预测开关(默认开)。
    //   关掉 = 抽帧推理时【不】做运动预测 -> 下一个真实检测只能跟 N 帧前的
    //   旧框算 IoU -> 目标跑起来时 IoU 掉到 match_thresh 以下 -> ID 乱跳。
    //   留着这个开关是为了能把它和开启的效果做 A/B(验证预测到底值不值)。
    void set_predict_enabled(bool on) { predict_enabled_ = on; }
    bool predict_enabled() const { return predict_enabled_; }

    int track_count() const { return static_cast<int>(tracks_.size()); }

    // 累计开过多少条轨迹(next_id 单调递增)。
    // ★ 这是"ID 漂移"的量化指标: 场景里目标数量不变时, 这个数应该基本不变;
    //   一旦 ID 乱跳, 就会看到它飞快地涨(每乱跳一次就多开一条)。
    int total_tracks_created() const { return next_id_; }

    // ID 漂移次数(新开的轨迹和已有轨迹明显重叠)
    int id_drift_count() const { return dup_tracks_; }

    // 关联余量统计: near_miss = IoU 落在 (0.3, match_thresh) 的配对次数
    //   (这些就是"如果 IoU 再高一点就能配上"的框, 也就是换 ID 的边缘)
    int    near_miss() const { return near_miss_; }
    double near_miss_avg_iou() const {
        return near_miss_ ? iou_near_sum_ / near_miss_ : -1.0;
    }
    float  near_miss_min_iou() const { return near_miss_ ? iou_near_min_ : -1.0f; }
    // 兜底关联救回来的次数
    int    fallback_hits() const { return fallback_hits_; }

private:
    struct Track {
        int    id = -1;
        YoloBox box;
        float  score = 0.f;
        int    hits = 0;
        int    age = 0;
        int    missed = 0;

        // 模块 C: 地面点中值滤波(x/y 分开)
        Med5   med_x, med_y;

        // 模块 E: 速度历史(度量坐标 + 真实时间戳), 环形缓冲
        static constexpr int kMaxH = 128;
        double h_t[kMaxH] = {0};
        float  h_x[kMaxH] = {0.f};
        float  h_y[kMaxH] = {0.f};
        int    h_head = 0, h_cnt = 0;

        // 显示值样本(时间 + km/h)
        static constexpr int kMaxS = 96;
        double s_t[kMaxS] = {0};
        float  s_v[kMaxS] = {0.f};
        int    s_head = 0, s_cnt = 0;

        // ★ 模块 F: 像素级位置历史(静态背景判定用)
        //   存的是【框中心】的像素坐标, 每帧一个样本。
        //   和上面的 h_x/h_y 是两回事: h_* 是 IPM 之后的度量坐标,
        //   这里要的是画面里的像素位移, 不能拿度量坐标去比。
        static constexpr int kMaxP = 64;
        int    p_f[kMaxP] = {0};      // 帧号
        float  p_x[kMaxP] = {0.f};    // 框中心 x(像素)
        float  p_y[kMaxP] = {0.f};    // 框中心 y(像素)
        int    p_head = 0, p_cnt = 0;

        void p_push(int frame, float x, float y) {
            p_f[p_head] = frame; p_x[p_head] = x; p_y[p_head] = y;
            p_head = (p_head + 1) % kMaxP;
            if (p_cnt < kMaxP) p_cnt++;
        }
        int p_newest() const { return (p_head - 1 + kMaxP) % kMaxP; }

        bool   in_trap = false;

        // ★ 运动预测(抽帧推理用)
        //   det_t/det_cx/det_cy: 上一次【真实检测】的时刻和框中心
        //   vel_x/vel_y        : 像素速度(px/秒), 指数平滑
        //   vel_valid          : 有没有算出过速度(至少两次真实检测)
        double det_t = 0.0;
        float  det_cx = 0.f, det_cy = 0.f;
        float  vel_x = 0.f, vel_y = 0.f;
        bool   vel_valid = false;
        // 上次【做预测】的时刻。
        // ★ 必须有它: 否则每个跳过的帧都按 "vel × (now - det_t)" 推一次 ——
        //   而 (now - det_t) 是【累计】时间, 于是位移越叠越多(2 帧就变成 2 倍),
        //   框直接飞出去, 下一个真实检测再也关联不上(踩过, 2026-11-15)。
        double pred_t = 0.0;
        // 上一次【真实检测】的帧号 —— 用来判断这条轨迹是不是"过期"了
        // (过期 = 隔了不止一帧才有新检测, 也就是抽帧推理的情况)
        int    det_frame = -1;

        // 环形缓冲辅助
        int  h_oldest() const { return (h_head - h_cnt + kMaxH) % kMaxH; }
        int  h_newest() const { return (h_head - 1 + kMaxH) % kMaxH; }
        void h_push(double t, float x, float y) {
            h_t[h_head] = t; h_x[h_head] = x; h_y[h_head] = y;
            h_head = (h_head + 1) % kMaxH;
            if (h_cnt < kMaxH) h_cnt++;
        }
        void h_clear() { h_head = 0; h_cnt = 0; }

        void s_push(double t, float v) {
            s_t[s_head] = t; s_v[s_head] = v;
            s_head = (s_head + 1) % kMaxS;
            if (s_cnt < kMaxS) s_cnt++;
        }
        int  s_oldest() const { return (s_head - s_cnt + kMaxS) % kMaxS; }
    };

    // 窗口内首尾弦长 / 时间
    static float speed_from_hist_chord(const Track& tr, double mpp);
    // 窗口内最小二乘斜率
    static float speed_from_hist_lsq(const Track& tr, double mpp);
    // 显示值 = 滑动窗口内中位数(不是瞬时值, 也不是 EMA)
    static float display_median(const Track& tr, double t, double span);

    // ★ 模块 F: 静态背景判定
    //   看最近 cfg.static_window 帧里, 框中心相对当前位置的【最大】位移
    //   是否小于 cfg.static_min_disp 像素。
    //   · 用"最大位移"而不是"首尾位移": 万一它左右抖 4px 又抖回来,
    //     首尾位移是 0, 但它其实在动 —— 那种应该当"抖动的真目标"留着。
    //   · 要求窗口内至少 cfg.static_min_hits 个样本: 新 track 还没攒够历史
    //     就判静止, 会把刚进画面、还没来得及动的真目标误杀。
    // 返回窗口内最大位移(像素); 顺便判定是否静止背景。
    // out_disp 不为 nullptr 时会写出这个位移值。
    static bool is_static_bg(const Track& tr, const SpeedCfg& cfg, float* out_disp = nullptr);

    std::vector<Track> tracks_;
    int next_id_ = 0;
    // ★ ID 漂移探针: 新开轨迹时发现和已有轨迹明显重叠的次数
    //   (基本等于"同一个目标被换了个 ID"的次数)。只统计, 不改变行为。
    int dup_tracks_ = 0;
    bool predict_enabled_ = true;
    // ★ 关联余量: 有多少对"框对上了但 IoU 没过 match_thresh"(差一点就换 ID)
    int    near_miss_ = 0;
    double iou_near_sum_ = 0.0;
    float  iou_near_min_ = 1.0f;
    // 兜底关联救回来的次数(旧轨迹被重新接上, 没开新 ID)
    int    fallback_hits_ = 0;
    int frame_no_ = 0;      // 帧号(给模块 F 的"最近 N 帧"用)
    // 两次 update 的典型间隔(秒), 用来判断某条轨迹是不是"过期"了。
    //   抽帧推理时这个间隔 = N 帧的时间, 过期判据要按它来算。
    double frame_dt_ = 0.0;
    double last_update_t_ = 0.0;
};

}  // namespace rk

#endif  // RK_SPEED_TRACK_H_
