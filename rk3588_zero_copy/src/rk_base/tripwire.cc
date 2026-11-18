// rk/tripwire.cc —— 过线计数的实现
//
// ★ 线程模型(两个线程, 互不阻塞):
//
//   推理线程(Channel::run)          后台线程(TripwireService::worker_loop)
//   ───────────────────────          ─────────────────────────────────────
//   submit()  ── 定长拷贝 ──┐        ┌──► 取快照 ──► 计数(叉积 O(1))
//   (不等待/不分配/不碰网络) └────────►│              └► 组 JSON ─► MQTT
//                                  └── 归还槽
//
// ★ 分配情况:
//   推理线程: 一次栈上/成员定长数组的拷贝, 【零堆分配】。
//   后台线程: 计数器内部的 unordered_map 在构造时 reserve 好, 之后只增删不扩容;
//              帧快照走定长环形缓冲, 满了丢最旧的。
#include "rk/tripwire.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "mqtt_pub.h"

namespace rk {

namespace {

// 单个快照槽里最多放多少条轨迹(定长数组的上限)
constexpr int kMaxTracksPerSlot = 128;

// 叉积的浮点容差: 小于它就算"在直线上"
constexpr float kEps = 1e-6f;

inline int sign_of(float v) {
    if (v > kEps) return 1;
    if (v < -kEps) return -1;
    return 0;
}

// 叉积 (B-A) x (C-A)
inline float cross3(float ax, float ay, float bx, float by,
                    float cx, float cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

// 点 P 是否落在线段 AB 上(共线时的退化情形)
inline bool on_segment(float ax, float ay, float bx, float by,
                        float px, float py) {
    const float min_x = std::min(ax, bx), max_x = std::max(ax, bx);
    const float min_y = std::min(ay, by), max_y = std::max(ay, by);
    return px >= min_x - kEps && px <= max_x + kEps &&
           py >= min_y - kEps && py <= max_y + kEps;
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

// ===========================================================================
// TripwireCounter —— 纯逻辑
// ===========================================================================
TripwireCounter::TripwireCounter(const TripwireConfig& cfg) : cfg_(cfg) {
    // 类别表排一下序, 之后用二分查(配置很短, 查得也快)
    std::sort(cfg_.classes.begin(), cfg_.classes.end());
    // ★ 预留: 运行期不再扩容。实测一个路口同时在册的轨迹只有几十条,
    //   留 2 倍余量就够; 超了以后靠 max_tracks 兜底拒绝新轨迹。
    states_.reserve(static_cast<size_t>(cfg_.max_tracks > 0 ? cfg_.max_tracks : 512) * 2);
}

float TripwireCounter::side_of_point(float px, float py,
                                     float x1, float y1, float x2, float y2) {
    return (x2 - x1) * (py - y1) - (y2 - y1) * (px - x1);
}

bool TripwireCounter::segments_intersect(float ax1, float ay1, float ax2, float ay2,
                                         float bx1, float by1, float bx2, float by2) {
    // ★ O(1): 4 个叉积 + 4 次符号判断, 没有循环、没有查表。
    const int d1 = sign_of(cross3(bx1, by1, bx2, by2, ax1, ay1));  // A1 在直线 B 的哪侧
    const int d2 = sign_of(cross3(bx1, by1, bx2, by2, ax2, ay2));  // A2
    const int d3 = sign_of(cross3(ax1, ay1, ax2, ay2, bx1, by1));  // B1 在直线 A 的哪侧
    const int d4 = sign_of(cross3(ax1, ay1, ax2, ay2, bx2, by2));  // B2

    // 严格相交(两侧异号)
    if (d1 * d2 < 0 && d3 * d4 < 0) return true;

    // 共线 / 端点相碰的退化情形
    if (d1 == 0 && on_segment(bx1, by1, bx2, by2, ax1, ay1)) return true;
    if (d2 == 0 && on_segment(bx1, by1, bx2, by2, ax2, ay2)) return true;
    if (d3 == 0 && on_segment(ax1, ay1, ax2, ay2, bx1, by1)) return true;
    if (d4 == 0 && on_segment(ax1, ay1, ax2, ay2, bx2, by2)) return true;
    return false;
}

bool TripwireCounter::class_allowed(int cls_id) const {
    if (cfg_.classes.empty()) return true;
    return std::binary_search(cfg_.classes.begin(), cfg_.classes.end(), cls_id);
}

void TripwireCounter::ingest(int camera_id, const TripwireTrack* tracks, size_t n,
                             uint32_t frame_idx, int64_t ts_ms) {
    if (!cfg_.valid()) return;
    cur_frame_ = frame_idx;

    const float max_jump2 = cfg_.max_jump_px * cfg_.max_jump_px;
    const float min_move2 = cfg_.min_move_px * cfg_.min_move_px;

    for (size_t i = 0; i < n; ++i) {
        const TripwireTrack& t = tracks[i];
        if (t.track_id < 0) continue;
        if (!class_allowed(t.cls_id)) continue;

        const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(camera_id)) << 32) |
                             static_cast<uint32_t>(t.track_id);

        // 找不到就是新轨迹
        auto it = states_.find(key);
        if (it == states_.end()) {
            if (static_cast<int>(states_.size()) >= cfg_.max_tracks) {
                ++stats_.rejected;      // 在册数到顶了, 拒绝新轨迹(防内存失控)
                continue;
            }
            TrackState st;
            st.px = t.cx;
            st.py = t.cy;
            st.has_prev = true;
            st.side = sign_of(side_of_point(t.cx, t.cy, cfg_.x1, cfg_.y1, cfg_.x2, cfg_.y2));
            st.last_frame = frame_idx;
            st.last_ts_ms = ts_ms;
            states_.emplace(key, st);
            continue;
        }

        TrackState& st = it->second;
        const float dx = t.cx - st.px;
        const float dy = t.cy - st.py;
        const float dist2 = dx * dx + dy * dy;

        if (dist2 > max_jump2) {
            // ★ 位移太大: 多半是 track_id 被复用(换了个目标)。
            //   重置历史, 【不】计数 —— 否则一次瞬移会被当成一次穿越。
            st.px = t.cx;
            st.py = t.cy;
            st.side = sign_of(side_of_point(t.cx, t.cy, cfg_.x1, cfg_.y1, cfg_.x2, cfg_.y2));
            st.last_frame = frame_idx;
            st.last_ts_ms = ts_ms;
            continue;
        }

        if (dist2 >= min_move2) {
            const int cur_side =
                sign_of(side_of_point(t.cx, t.cy, cfg_.x1, cfg_.y1, cfg_.x2, cfg_.y2));

            // ★ 方向判定: 上一次的【非零】侧 vs 这一次的侧, 异号才算穿越。
            //   (目标正好停在线上时 side==0, 这时保留上一次的非零侧,
            //    等它离开线的那一帧再判定 —— 不会漏, 也不会重复计。)
            if (cur_side != 0 && st.side != 0 && cur_side != st.side) {
                // ★ 用线段相交确认: 上一次中心 -> 这一次中心 的线段
                //   真的穿过了虚拟线段(而不是穿过了它的延长线)。
                if (segments_intersect(cfg_.x1, cfg_.y1, cfg_.x2, cfg_.y2,
                                       st.px, st.py, t.cx, t.cy)) {
                    const bool positive_dir = (st.side < 0 && cur_side > 0);
                    const bool is_in = (positive_dir == cfg_.positive_is_in);
                    if (is_in) ++stats_.in_count; else ++stats_.out_count;
                    stats_.last_event_ts_ms = ts_ms;

                    last_event_.ts_ms = ts_ms;
                    last_event_.frame = frame_idx;
                    last_event_.camera_id = camera_id;
                    last_event_.track_id = t.track_id;
                    last_event_.cls_id = t.cls_id;
                    last_event_.is_in = is_in;
                    last_event_.x = t.cx;
                    last_event_.y = t.cy;
                    if (on_event_) on_event_(last_event_);
                }
            }
            // 更新位置; 侧符号只在非零时更新(保住"上一个非零侧")
            st.px = t.cx;
            st.py = t.cy;
            if (cur_side != 0) st.side = cur_side;
        }
        st.last_frame = frame_idx;
        st.last_ts_ms = ts_ms;
    }

    cleanup_expired(frame_idx);
    stats_.active_tracks = static_cast<int>(states_.size());
}

void TripwireCounter::ingest(int camera_id, const std::vector<TrackResult>& tracks,
                             uint32_t frame_idx, int64_t ts_ms) {
    // 栈上定长缓冲: 不碰堆
    TripwireTrack buf[kMaxTracksPerSlot];
    const size_t n = std::min<size_t>(tracks.size(), kMaxTracksPerSlot);
    for (size_t i = 0; i < n; ++i) {
        buf[i].track_id = tracks[i].track_id;
        buf[i].cls_id = tracks[i].box.cls_id;
        buf[i].cx = tracks[i].center_x;
        buf[i].cy = tracks[i].center_y;
    }
    if (tracks.size() > kMaxTracksPerSlot) {
        stats_.truncated += tracks.size() - kMaxTracksPerSlot;
    }
    ingest(camera_id, buf, n, frame_idx, ts_ms);
}

void TripwireCounter::cleanup_expired(uint32_t cur_frame) {
    const int period = cfg_.cleanup_period_frames > 0 ? cfg_.cleanup_period_frames : 1;
    if (cur_frame - last_cleanup_frame_ < static_cast<uint32_t>(period)) return;
    last_cleanup_frame_ = cur_frame;

    const uint32_t max_miss = static_cast<uint32_t>(cfg_.max_miss_frames > 0
                                                     ? cfg_.max_miss_frames : 1);
    // ★ 严格清理: 连续 max_miss_frames 帧没出现的轨迹, 从缓存里删掉。
    //   没有这一步, 轨迹表只增不减 —— 跑一晚上就是一次慢性泄漏。
    for (auto it = states_.begin(); it != states_.end();) {
        if (cur_frame - it->second.last_frame > max_miss) {
            it = states_.erase(it);       // erase 不分配
        } else {
            ++it;
        }
    }
}

void TripwireCounter::reset() {
    states_.clear();
    stats_ = TripwireStats();
    last_event_ = TripwireEvent();
    last_cleanup_frame_ = cur_frame_;
}

// ===========================================================================
// TripwireService —— 异步服务
// ===========================================================================
TripwireService::TripwireService() = default;

TripwireService::~TripwireService() { stop(); }

bool TripwireService::start(const Options& opt) {
    if (started_) return true;
    if (!opt.cfg.valid()) {
        fprintf(stderr, "[Tripwire] 虚拟线无效(两个端点重合), 不启动\n");
        return false;
    }
    opt_ = opt;

    counter_.reset(new TripwireCounter(opt.cfg));
    counter_->set_event_cb([this](const TripwireEvent& e) {
        last_event_ts_ms_.store(e.ts_ms, std::memory_order_relaxed);
        // 事件可能很密(一秒几十个), 打日志要限流, 否则把 stderr 刷爆、
        // 还会拖慢磁盘 IO。
        const int64_t now = now_ms();
        if (now - last_event_log_ms_ < 1000) return;
        last_event_log_ms_ = now;
        fprintf(stderr, "[Tripwire] %s track=%d cls=%d @(%d,%d) -> %s\n",
                e.is_in ? "进" : "出", e.track_id, e.cls_id,
                static_cast<int>(e.x), static_cast<int>(e.y),
                e.is_in ? "IN" : "OUT");
    });

    const size_t n = opt_.queue_slots > 0 ? opt_.queue_slots : 1;
    slots_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        slots_[i].reset(new Slot());
        free_slots_.push_back(i);
    }

    started_ = true;
    th_ = std::thread(&TripwireService::worker_loop, this);
    fprintf(stderr, "[Tripwire] 启动: 线 (%.0f,%.0f)-(%.0f,%.0f) %s, camera=%d, "
                    "槽位=%zu, 每 %d ms 上报一次\n",
            opt.cfg.x1, opt.cfg.y1, opt.cfg.x2, opt.cfg.y2,
            opt.cfg.positive_is_in ? "正方向=进" : "负方向=进",
            opt.camera_id, n, static_cast<int>(opt.publish_interval_ms));
    return true;
}

void TripwireService::stop() {
    if (!started_) return;
    {
        std::lock_guard<std::mutex> lk(m_);
        quit_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
    started_ = false;
}

void TripwireService::submit(int camera_id, const std::vector<TrackResult>& tracks,
                             uint32_t frame_idx, int64_t ts_ms) {
    if (!started_) return;

    // ---- 抢一个槽(极短的临界区, 不等网络、不等结果) ----
    size_t idx = 0;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (quit_) return;
        if (free_slots_.empty()) {
            // ★ 队列满: 丢【最旧】的一帧, 绝不阻塞推理线程。
            //   (和 ChanDispatcher 一个路子: 宁可丢旧数据, 也不能让 NPU 等业务。)
            if (ready_.empty()) {
                dropped_frames_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            idx = ready_.front();
            ready_.pop_front();
            dropped_frames_.fetch_add(1, std::memory_order_relaxed);
        } else {
            idx = free_slots_.back();
            free_slots_.pop_back();
        }
    }

    // ---- 填快照(锁外做, 槽子此刻归我们独占) ----
    Slot* s = slots_[idx].get();
    s->camera_id = camera_id;
    s->frame = frame_idx;
    s->ts_ms = ts_ms;
    const size_t cap = std::min<size_t>(tracks.size(), static_cast<size_t>(kMaxTracksPerSlot));
    size_t n = 0;
    for (size_t i = 0; i < cap; ++i) {
        const TrackResult& t = tracks[i];
        if (t.track_id < 0) continue;
        s->tracks[n].track_id = t.track_id;
        s->tracks[n].cls_id = t.box.cls_id;
        s->tracks[n].cx = t.center_x;
        s->tracks[n].cy = t.center_y;
        ++n;
    }
    s->n = static_cast<int>(n);
    if (tracks.size() > static_cast<size_t>(kMaxTracksPerSlot)) {
        truncated_.fetch_add(tracks.size() - kMaxTracksPerSlot, std::memory_order_relaxed);
    }

    {
        std::lock_guard<std::mutex> lk(m_);
        ready_.push_back(idx);
    }
    cv_.notify_one();
}

TripwireStats TripwireService::stats() const {
    TripwireStats s;
    s.in_count = static_cast<int64_t>(in_count_.load(std::memory_order_relaxed));
    s.out_count = static_cast<int64_t>(out_count_.load(std::memory_order_relaxed));
    s.dropped_frames = dropped_frames_.load(std::memory_order_relaxed);
    s.truncated = truncated_.load(std::memory_order_relaxed);
    s.rejected = rejected_.load(std::memory_order_relaxed);
    s.last_event_ts_ms = last_event_ts_ms_.load(std::memory_order_relaxed);
    s.last_publish_ts_ms = last_publish_ts_ms_.load(std::memory_order_relaxed);
    return s;
}

std::string TripwireService::build_json(int64_t now_ms_value) const {
    const TripwireStats s = stats();
    // ★ 只读原子快照 —— 不碰 counter_ 的内部状态:
    //   那些状态只有 worker 线程能碰, 这里从别的线程读就是数据竞争。
    const int active = active_tracks_.load(std::memory_order_relaxed);
    char buf[640];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"tripwire\",\"sn\":\"%s\",\"ch\":%d,\"camera_id\":%d,"
             "\"line\":{\"x1\":%.1f,\"y1\":%.1f,\"x2\":%.1f,\"y2\":%.1f},"
             "\"in\":%lld,\"out\":%lld,\"total\":%lld,"
             "\"active_tracks\":%d,\"dropped_frames\":%llu,\"ts\":%lld}",
             opt_.sn.c_str(), opt_.channel, opt_.camera_id,
             opt_.cfg.x1, opt_.cfg.y1, opt_.cfg.x2, opt_.cfg.y2,
             static_cast<long long>(s.in_count), static_cast<long long>(s.out_count),
             static_cast<long long>(s.in_count + s.out_count),
             active, static_cast<unsigned long long>(s.dropped_frames),
             static_cast<long long>(now_ms_value));
    return std::string(buf);
}

void TripwireService::worker_loop() {
    // ★ MQTT 客户端只在这个线程里用 —— 推理线程永远碰不到 socket。
    MqttPublisher pub;
    const bool want_mqtt = !opt_.mqtt_host.empty() && !opt_.mqtt_topic.empty();
    int64_t last_publish = 0;

    for (;;) {
        size_t idx = 0;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [this] { return quit_ || !ready_.empty(); });
            if (quit_ && ready_.empty()) break;
            idx = ready_.front();
            ready_.pop_front();
        }

        Slot* s = slots_[idx].get();
        counter_->ingest(s->camera_id, s->tracks, static_cast<size_t>(s->n), s->frame, s->ts_ms);

        // 把计数器的统计搬到原子里(外部只读这些)
        const TripwireStats cs = counter_->stats();
        in_count_.store(static_cast<uint64_t>(cs.in_count), std::memory_order_relaxed);
        out_count_.store(static_cast<uint64_t>(cs.out_count), std::memory_order_relaxed);
        rejected_.store(cs.rejected, std::memory_order_relaxed);
        active_tracks_.store(cs.active_tracks, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lk(m_);
            free_slots_.push_back(idx);
        }

        // ---- 周期性上报 ----
        const int64_t now = now_ms();
        if (opt_.publish_interval_ms > 0 && now - last_publish >= opt_.publish_interval_ms) {
            last_publish = now;
            if (want_mqtt) {
                // 断线自动重连(和 MqttSink 一个套路); 连不上就跳过这一轮,
                // 绝不阻塞计数线程以外的东西。
                if (!pub.connected() && !pub.ensure_connected(
                        opt_.mqtt_host, opt_.mqtt_port,
                        opt_.mqtt_client_id.empty() ? (opt_.sn + "-tripwire")
                                                    : opt_.mqtt_client_id,
                        3000)) {
                    last_publish_ts_ms_.store(now, std::memory_order_relaxed);
                    continue;
                }
                const std::string payload = build_json(now);
                if (!pub.publish(opt_.mqtt_topic, payload, opt_.mqtt_qos)) {
                    fprintf(stderr, "[Tripwire] MQTT 发送失败: %s\n", pub.last_error().c_str());
                }
            }
            last_publish_ts_ms_.store(now, std::memory_order_relaxed);
        }
    }
}

}  // namespace rk
