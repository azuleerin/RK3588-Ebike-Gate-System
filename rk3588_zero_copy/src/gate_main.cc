// gate_main.cc —— 单板 4 路大门: 单进程 + 全局 NPU 线程池
//
// ★ 为什么要有这个程序
//
//   rk_zero_copy 一次只跑一路, 4 路就是 4 个进程。进程之间共享不了 RKNN
//   上下文, 于是有两件事做不到:
//
//     1) 【识别优先】。PP-OCR 一次 ~24ms, YOLO 一次 ~30ms, 两者在 NPU
//        驱动队列里权重一样。近景车道的识别会被另一路的检测任务压住,
//        车牌上报延迟就抖 —— 这正是"识别通道饥饿"。
//     2) 【负载均衡】。同一个核上挤着两个进程时, 谁先谁后由驱动随手决定,
//        我们控制不了。
//
//   这个程序把 4 路收进【一个进程】, 共用一条按优先级排的任务队列
//   (rk::NpuPool): 识别任务优先级 > 检测任务; 每颗 NPU 核一个 worker,
//   worker 自己建上下文并绑核。
//
// ★ 跟单流版共用什么(不重复实现)
//   解码      rk::MppDecoder / FFmpeg
//   预处理    rk::rga_nv12_to_rgb888  (RGA 目标 = NPU 输入, 同一块 dma-buf)
//   后处理    rk::yolov8_postprocess / rk::filter_bad_boxes
//   跟踪测速  rk::SpeedTracker / rk::GroundPlane / rk::BevView
//   车牌识别  rk::LicensePlateRec
//   上报      rk::MqttSink / rk::StdoutSink
//   ROI 几何  rk/plate_roi.h
//
// 用法:
//   rk_gate --model M.rknn --labels labels.txt \
//           --det-core 1,2 --ocr-core 0 \
//           --ch ch1:/path/near.mp4:near \
//           --ch ch2:/path/high.mp4:high:"x,y;x,y;x,y;x,y"
//
//   --ch 格式: <名字>:<源>:<near|high>[:<IPM 四点>]
//     near = 近景: 跑车牌识别, 不跑测速
//     high = 高位: 跑 IPM 测速, 不跑车牌识别
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include <malloc.h>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ipm.h"
#include "mpp_decoder.h"
#include "rga_convert.h"
#include "result_sink.h"
#include "rk/av_frame_guard.h"
#include "rk/core_mask.h"
#include "rk/mpp_frame_guard.h"
#include "rk/npu_pool.h"
#include "rk/plate_roi.h"
#include "rk/tripwire.h"
#include "safe_probe.h"
#include "speed_track.h"
#include "stats.h"

// ---------------------------------------------------------------------------
// 读当前进程的 RSS(KB)。/proc/self/statm 比 /proc/self/status 便宜得多。
// ---------------------------------------------------------------------------
static size_t read_rss_kb() {
    static long page_kb = sysconf(_SC_PAGESIZE) / 1024;
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    long total = 0, resident = 0;
    if (fscanf(f, "%ld %ld", &total, &resident) != 2) resident = 0;
    fclose(f);
    return static_cast<size_t>(resident) * static_cast<size_t>(page_kb);
}

namespace {

// ---------------------------------------------------------------------------
std::atomic<bool> g_quit{false};
void on_signal(int) { g_quit = true; }

// 线程安全的 warn_once(单流版 main.cc 里那个用 static std::set, 多线程会炸)
std::mutex g_warn_m;
std::vector<std::string> g_warned;

// librga 的进程内串行锁 —— 4 个通道线程共用一份 librga, 必须排队用。
// (详细说明见 Channel::run() 里调用 rga_nv12_to_rgb888() 的地方。)
std::mutex g_rga_m;
void warn_once(const std::string& key, const char* fmt, ...) {
    {
        std::lock_guard<std::mutex> lk(g_warn_m);
        for (const auto& s : g_warned)
            if (s == key) return;
        g_warned.push_back(key);
    }
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[警告] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

// ---------------------------------------------------------------------------
// 一帧的 NV12 描述
struct Nv12Info {
    int   fd = -1;
    void* ptr = nullptr;
    int   width = 0, height = 0;
    int   stride = 0;      // Y 平面行距(字节)
    int   plane_h = 0;     // Y 平面真实行数
    bool  planar = false;  // true = YUV420P(平面), false = NV12(半平面)
    bool  full_range = false;
    int   uv_stride = 0;
    const uint8_t* u_plane = nullptr;
    const uint8_t* v_plane = nullptr;
    int   bit_shift = 0;
};

void extract_nv12(const AVFrame* f, Nv12Info* n) {
    n->ptr = f->data[0];
    n->stride = f->linesize[0];
    n->width = f->width;
    n->height = f->height;
    n->plane_h = f->height;
    n->fd = -1;
    const enum AVPixelFormat pf = (enum AVPixelFormat)f->format;
    if (pf == AV_PIX_FMT_NV12) {
        n->planar = false;
        n->u_plane = f->data[1];
        n->v_plane = nullptr;
    } else if (pf == AV_PIX_FMT_YUV420P || pf == AV_PIX_FMT_YUVJ420P) {
        n->planar = true;
        n->u_plane = f->data[1];
        n->v_plane = f->data[2];
        n->full_range = (pf == AV_PIX_FMT_YUVJ420P);
    }
    if (f->data[0] && f->data[1] && f->linesize[0] > 0) {
        const ptrdiff_t off = (const uint8_t*)f->data[1] - (const uint8_t*)f->data[0];
        if (off > 0 && off % f->linesize[0] == 0)
            n->plane_h = static_cast<int>(off / f->linesize[0]);
    }
}

// ---------------------------------------------------------------------------
// 配置
struct ChanCfg {
    std::string name;
    std::string url;
    bool        near = false;    // 近景: 跑车牌识别
    bool        high = false;    // 高位: 跑 IPM 测速
    std::string ipm_points;
};

struct Options {
    std::string model;
    std::string labels;
    std::string ppocr_model;
    std::string ppocr_dict;
    std::string ppocr_charset;
    std::vector<ChanCfg> chans;

    int   det_core_mask = 0;      // 检测可用核(掩码); 0x7 三核全开【禁用】
    int   ocr_core_mask = 0;      // 识别可用核(掩码); 0x7 三核全开【禁用】
    // ★ worker 数量: worker i 绑到 cores[i % 核数] —— 均匀轮询。
    //   默认 3 + 3: 每颗核上 1 个检测 + 1 个识别, 共 6 个 worker
    //   (独立压测台测出的最优: 每核 2 个 worker 吞吐封顶, 再多只涨延迟)。
    //   ★ 注意: 加【检测】worker 会同步增加 CPU 后处理(DFL+NMS)的压力,
    //     实测 3 个检测 worker 会把 CPU 吃到 78% 反而更慢 —— 所以能配, 用实测定。
    int   det_workers = 3;
    int   ocr_workers = 3;
    float conf = 0.15f;
    float nms  = 0.45f;
    int   max_fps = 20;
    int   stats_interval = 100;
    int   det_slots = 2;          // 每个检测 worker 几个 slot(双缓冲)
    int   det_w = 640, det_h = 640;   // 模型输入尺寸(从模型属性读)
    int   det_n_output = 1;
    bool  geo_filter = true;

    // 近景车牌 ROI 几何(与单流版 --roi-* 同义)
    float roi_ratio = 0.164f, roi_y = 1.0f, roi_height = 0.40f;
    float roi_shift = 0.126f, roi_below = 0.0f;
    float ppocr_pad = 0.10f;
    float ppocr_min_score = 0.30f;
    int   ppocr_ttl_ms = 5000;
    int   ppocr_denoise = 5;
    bool  ppocr_invert = false;
    int   ppocr_in_w = 320;
    int   ppocr_in_h = 48;

    // 测速
    double ipm_width_m = 3.5;
    double ipm_ppm = 100.0;
    double ipm_fov = 0.0;
    float  speed_limit = 25.0f;
    double speed_win = 0.80;
    float  match_thresh = 0.80f;

    int   plate_cls = 1;
    int   track_cls = 1;

    // 上报
    std::string sn = "rk3588-01";
    std::string device_name = "0001";
    std::string ipaddr = "192.168.8.77";
    std::string mqtt_host;
    int   mqtt_port = 1883;
    int   mqtt_qos = 0;
    bool  sink_stdout = false;

    int   duration = 0;           // 秒; 0 = 一直跑
    bool  loop_src = true;
    bool  use_mpp = true;

    // ---- 过线计数(二期 KPI) ----
    //   给了 --tripwire-line 才启用。坐标系是【原图像素】。
    //
    //   ★ 一条虚拟线规格:
    //     ch 为空  -> 适用于【所有】通道(只有分辨率相同的相机才能这么用)
    //     ch 非空  -> 只给这一个通道(如 "ch1")
    //   不同相机分辨率不同, 坐标不能共用 —— 所以线是【按相机】配的。
    struct TripwireLineSpec {
        std::string ch;                    // 空 = 所有通道
        float x1 = 0.f, y1 = 0.f, x2 = 0.f, y2 = 0.f;
    };
    std::vector<TripwireLineSpec> tripwire_lines;
    bool        tripwire_enabled = false;
    bool        tripwire_positive_is_in = true;   // true = 正方向穿越算"进"
    std::vector<int> tripwire_classes;          // 空 = 全部类别
    int         tripwire_miss_frames = 30;       // 超时多少帧清掉这条轨迹
    int64_t     tripwire_publish_ms = 1000;     // 统计上报周期
    std::string tripwire_topic;                 // 空 = 用默认 topic
    bool        tripwire_no_mqtt = false;       // 只本地计数, 不发 MQTT

    // ---- 调试: ROI 存盘(与单流版 rk_zero_copy --save-roi 同义) ----
    //   用途: 肉眼确认"模型到底裁到了什么"。门禁版以前没有这个能力,
    //   近景通道识别效果对不上单流版时无从查起 —— 现在两边都能存图对比。
    bool        save_roi = false;
    std::string save_roi_dir = "output_rois";
};

// ---------------------------------------------------------------------------
// ROI 存盘: 把【真正喂给 PP-OCR 的那张图】落盘
//
//   颜色说明: 内部缓冲是 BGR(PP-OCR 官方示例就是喂 BGR),
//   OpenCV 的 imwrite 也认 BGR —— 直接写, 不要再来回转色。
// ---------------------------------------------------------------------------
struct RoiDumper {
    bool        enabled = false;
    std::string dir = "output_rois";
    bool        ready = false;

    bool ensure_dir() {
        if (ready) return true;
        if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "[ROI] 建目录 %s 失败: %s\n", dir.c_str(), strerror(errno));
            return false;
        }
        ready = true;
        return true;
    }

    void dump(const std::string& chan, int track_id, int frame_idx,
              const uint8_t* roi_bgr, int roi_w, int roi_h, const char* suffix = "") {
        if (!enabled || !roi_bgr || roi_w <= 0 || roi_h <= 0) return;
        if (!ensure_dir()) return;
        char stem[192];
        snprintf(stem, sizeof(stem), "%s_track_%d_frame_%d%s",
                 chan.c_str(), track_id, frame_idx, suffix ? suffix : "");
        const bool png = (::getenv("RK_ROI_PNG") != nullptr);
        const std::string path = dir + "/" + stem + (png ? ".png" : ".jpg");
        cv::Mat bgr(roi_h, roi_w, CV_8UC3, const_cast<uint8_t*>(roi_bgr));
        if (!cv::imwrite(path, bgr))
            fprintf(stderr, "[ROI] 写 %s 失败\n", path.c_str());
    }
};

std::vector<std::string> load_labels(const std::string& path) {
    std::vector<std::string> v;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return v;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (!s.empty()) v.push_back(s);
    }
    fclose(f);
    return v;
}

bool parse_quad(const std::string& s, rk::Pt quad[4]) {
    int n = 0;
    size_t pos = 0;
    while (pos <= s.size() && n < 4) {
        const size_t sep = s.find(';', pos);
        const std::string item =
            (sep == std::string::npos) ? s.substr(pos) : s.substr(pos, sep - pos);
        double x = 0, y = 0;
        if (sscanf(item.c_str(), "%lf,%lf", &x, &y) != 2) return false;
        quad[n].x = x;
        quad[n].y = y;
        ++n;
        if (sep == std::string::npos) break;
        pos = sep + 1;
    }
    return n == 4;
}

// 解析 "名字:源:near|high[:IPM四点]"
bool parse_chan_spec(const std::string& spec, ChanCfg* out) {
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= spec.size()) {
        const size_t sep = spec.find(':', pos);
        parts.push_back((sep == std::string::npos) ? spec.substr(pos)
                                                   : spec.substr(pos, sep - pos));
        if (sep == std::string::npos) break;
        pos = sep + 1;
    }
    if (parts.size() < 3) return false;
    out->name = parts[0];
    out->url = parts[1];
    if (parts[2] == "near") out->near = true;
    else if (parts[2] == "high") out->high = true;
    else return false;
    if (parts.size() >= 4 && !parts[3].empty()) out->ipm_points = parts[3];
    return true;
}

// 从模型里读输入尺寸和输出个数(临时开一个上下文)
//
// ★ 尺寸怎么取: 和单流版 main.cc 完全一样的判据 ——
//   native NHWC 属性存在 -> dims[1]=高, dims[2]=宽
//   否则逻辑属性是 NHWC 4 维 -> dims[1]=高, dims[2]=宽
//   否则(NCHW) -> dims[2]=高, dims[3]=宽
//   直接取 dims[n_dims-2]/dims[n_dims-1] 是错的: 有的模型逻辑属性是
//   [1,3,640,640](NCHW), 那样会读出 3x640 —— 张量大小算下来只有 5760 字节,
//   rknn_set_io_mem 直接报 "input memory size(5760) < model input size(1228800)"。
bool probe_model(const std::string& path, int* w, int* h, int* n_out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(static_cast<size_t>(sz));
    const size_t n = fread(data.data(), 1, data.size(), f);
    fclose(f);
    if (n != data.size()) return false;

    rknn_context ctx = 0;
    if (rknn_init(&ctx, data.data(), static_cast<uint32_t>(data.size()),
                  RKNN_FLAG_PRIOR_HIGH, nullptr) != 0)
        return false;

    rknn_tensor_attr attr_logic;
    memset(&attr_logic, 0, sizeof(attr_logic));
    attr_logic.index = 0;
    rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &attr_logic, sizeof(attr_logic));
    rknn_tensor_attr attr_native;
    memset(&attr_native, 0, sizeof(attr_native));
    attr_native.index = 0;
    bool use_native = false;
    if (rknn_query(ctx, RKNN_QUERY_NATIVE_NHWC_INPUT_ATTR, &attr_native,
                   sizeof(attr_native)) == 0 &&
        attr_native.type == RKNN_TENSOR_UINT8 && attr_native.fmt == RKNN_TENSOR_NHWC &&
        attr_native.n_dims == 4 && attr_native.dims[3] == 3) {
        use_native = true;
    }

    if (use_native) {
        *h = static_cast<int>(attr_native.dims[1]);
        *w = static_cast<int>(attr_native.dims[2]);
    } else if (attr_logic.fmt == RKNN_TENSOR_NHWC && attr_logic.n_dims == 4) {
        *h = static_cast<int>(attr_logic.dims[1]);
        *w = static_cast<int>(attr_logic.dims[2]);
    } else if (attr_logic.n_dims == 4) {
        *h = static_cast<int>(attr_logic.dims[2]);
        *w = static_cast<int>(attr_logic.dims[3]);
    } else {
        rknn_destroy(ctx);
        return false;
    }

    // 输出张量个数 —— ★ 必须查, 不能写死 1:
    //   ebike_int8 是【形态C 双输出】(box + cls), 按单输出去解会一路错到底,
    //   结果是【一个框都检不出来】(踩过: objs 恒为 0)。
    rknn_input_output_num io_num;
    memset(&io_num, 0, sizeof(io_num));
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    *n_out = static_cast<int>(io_num.n_output);
    rknn_destroy(ctx);
    return (*w > 0 && *h > 0 && *n_out > 0);
}

// ---------------------------------------------------------------------------
// 每通道的上报队列
class ChanDispatcher {
public:
    ~ChanDispatcher() { stop(); }
    void add(std::unique_ptr<rk::ResultSink> s) { sinks_.push_back(std::move(s)); }
    bool empty_sinks() const { return sinks_.empty(); }
    void start() {
        if (th_.joinable()) return;
        th_ = std::thread([this]() {
            for (;;) {
                int idx = 0;
                std::vector<rk::DetectResult> rs;
                {
                    std::unique_lock<std::mutex> lk(m_);
                    cv_.wait(lk, [this]() { return quit_ || !q_.empty(); });
                    if (quit_ && q_.empty()) break;
                    idx = q_.front().first;
                    rs = std::move(q_.front().second);
                    q_.pop_front();
                }
                for (auto& s : sinks_) {
                    try { s->on_frame(idx, rs); }
                    catch (const std::exception& e) {
                        fprintf(stderr, "[sink] 回调异常: %s\n", e.what());
                    }
                }
            }
        });
    }
    void push(int idx, std::vector<rk::DetectResult> rs) {
        {
            std::lock_guard<std::mutex> lk(m_);
            while (q_.size() >= 32) q_.pop_front();
            q_.emplace_back(idx, std::move(rs));
        }
        cv_.notify_one();
    }
    void stop() {
        if (stopped_) return;
        stopped_ = true;
        {
            std::lock_guard<std::mutex> lk(m_);
            quit_ = true;
        }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
        for (auto& s : sinks_) s->flush();
    }
private:
    std::vector<std::unique_ptr<rk::ResultSink>> sinks_;
    std::deque<std::pair<int, std::vector<rk::DetectResult>>> q_;
    std::mutex m_;
    std::condition_variable cv_;
    std::thread th_;
    bool quit_ = false;
    bool stopped_ = false;
};

// ---------------------------------------------------------------------------
// MPP 初始化探针(SafeProbe 只能单线程调, 所以这些都在起线程之前跑)
struct MppInitProbe {
    rk::MppDecoder* dec;
    bool hevc;
    bool inited;
};
bool probe_mpp_init(void* p) {
    MppInitProbe* a = static_cast<MppInitProbe*>(p);
    a->inited = a->dec->init(a->hevc, 0, 0);
    return true;
}

// ---------------------------------------------------------------------------
// 一个视频通道
class Channel {
public:
    Channel(int index, const ChanCfg& cfg, const Options& opt, rk::NpuPool* pool)
        : index_(index), cfg_(cfg), opt_(opt), pool_(pool) {
        if (cfg_.near) plate_cls_ids_.push_back(opt_.plate_cls);
        track_cls_ids_.push_back(opt_.track_cls);
        tracker_.reset();
        tracker_.set_predict_enabled(true);
    }

    // ★ 必须在【所有线程起来之前】调用: MPP 的 init 要走 SafeProbe,
    //   而 SafeProbe 用的是进程级 sigaction + 静态 sigjmp_buf, 不能并发调。
    bool open_source();

    void run();
    void stop() { stop_ = true; }
    const std::string& name() const { return cfg_.name; }

    // 打开 ROI 存盘(调试用)
    void enable_roi_dump(bool on, const std::string& dir) {
        roi_dump_.enabled = on;
        roi_dump_.dir = dir;
    }

    // 挂上进程级共享的过线计数服务
    void set_tripwire(rk::TripwireService* svc) { tripwire_ = svc; }

private:
    bool rewind_source();
    bool decode_frame(Nv12Info* nv, rk::MppFrameGuard* mg, rk::AVFrameGuard* ag);
    // 裁一块车牌 ROI(紧凑 BGR, 行距 w*3), 成功返回宽度
    bool crop_plate_roi(const rk::RgaImage& src, int rx, int ry, int rw, int rh,
                        uint8_t* dst, size_t dst_cap, int* out_w);
    void report(const std::vector<rk::TrackResult>& tracks);
    void print_stats(size_t n_boxes, size_t n_tracks,
                     const std::vector<rk::TrackResult>* tracks = nullptr);

    int    index_;
    ChanCfg cfg_;
    const Options& opt_;
    rk::NpuPool* pool_;

    // ---- 解码 ----
    AVFormatContext* fmt_ = nullptr;
    AVCodecContext*  vctx_ = nullptr;
    AVBSFContext*    mp4_bsf_ = nullptr;
    AVPacket*         pkt_ = nullptr;
    int    vstream_ = 0;
    double src_nominal_fps_ = 0.0;
    bool   src_is_live_ = false;
    int64_t synth_pts_ = 0;
    rk::MppDecoder mpp_dec_;
    bool   use_mpp_ = false;
    std::atomic<bool> stop_{false};

    // ---- 状态 ----
    rk::PipelineStats stats_;
    rk::SpeedTracker  tracker_;
    rk::SpeedCalib   calib_;
    rk::BevView      bev_;
    bool calib_built_ = false;
    rk::PlateTrackCache plate_cache_;
    std::vector<int> plate_cls_ids_;
    std::vector<int> track_cls_ids_;
    std::chrono::steady_clock::time_point last_decode_ts_{};
    double next_analysis_due_ = 0.0;

    // ★ 过线计数服务(进程级共享, 可选)。有自己的后台线程:
    //   推理线程只调 submit()(一次定长拷贝, 绝不阻塞),
    //   计数 / JSON 组包 / MQTT 发送全在后台线程里做。
    //   一个进程一个服务 —— 各通道共用, 但每台相机有【自己的】虚拟线。
    rk::TripwireService* tripwire_ = nullptr;

    // 车牌 ROI 缓冲(紧凑行距: 每行 w*3)
    // ★ 现在 ROI 直接裁进【池里的 ROI 槽】, 这个缓冲只留作兜底/临时用。
    std::vector<uint8_t> roi_buf_;
    // cv::cvtColor 的灰度中间缓冲(只有 --ppocr-invert 才用)
    // ★ 提前开好: cv::Mat gray; 每次调用都会自己分配一块
    std::vector<uint8_t> roi_gray_buf_;

    // 阶段计时(每帧累加, 打印时取平均)
    double det_wait_acc_ = 0.0; int det_wait_n_ = 0;
    double ocr_wait_acc_ = 0.0; int ocr_wait_n_ = 0;
    // ROI 存盘(调试用)
    RoiDumper roi_dump_;

    ChanDispatcher dispatcher_;
};

// ---------------------------------------------------------------------------
bool Channel::open_source() {
    AVDictionary* dopts = nullptr;
    av_dict_set(&dopts, "rtsp_transport", "tcp", 0);
    av_dict_set(&dopts, "stimeout", "5000000", 0);
    av_dict_set(&dopts, "max_delay", "500000", 0);
    av_dict_set(&dopts, "fflags", "nobuffer", 0);
    av_dict_set(&dopts, "avioflags", "direct", 0);
    int ret = avformat_open_input(&fmt_, cfg_.url.c_str(), nullptr, &dopts);
    av_dict_free(&dopts);
    if (ret < 0) {
        fprintf(stderr, "[%s] 打开源失败: %s\n", cfg_.name.c_str(), cfg_.url.c_str());
        return false;
    }
    if (avformat_find_stream_info(fmt_, nullptr) < 0) {
        fprintf(stderr, "[%s] 找流失败\n", cfg_.name.c_str());
        return false;
    }
    vstream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vstream_ < 0) {
        fprintf(stderr, "[%s] 没有视频流\n", cfg_.name.c_str());
        return false;
    }
    AVStream* vst = fmt_->streams[vstream_];
    src_nominal_fps_ = av_q2d(vst->avg_frame_rate);
    if (!(src_nominal_fps_ > 0.1 && src_nominal_fps_ < 240.0))
        src_nominal_fps_ = av_q2d(vst->r_frame_rate);
    if (!(src_nominal_fps_ > 0.1 && src_nominal_fps_ < 240.0)) src_nominal_fps_ = 0.0;
    src_is_live_ = (cfg_.url.rfind("rtsp://", 0) == 0) ||
                   (cfg_.url.rfind("rtsps://", 0) == 0);

    const AVCodecID cid = vst->codecpar->codec_id;
    const AVCodec* dec = avcodec_find_decoder(cid);
    if (!dec) {
        fprintf(stderr, "[%s] 找不到解码器\n", cfg_.name.c_str());
        return false;
    }
    vctx_ = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(vctx_, vst->codecpar);
    vctx_->thread_count = 1;
    vctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (avcodec_open2(vctx_, dec, nullptr) < 0) {
        fprintf(stderr, "[%s] 解码器打开失败\n", cfg_.name.c_str());
        return false;
    }
    pkt_ = av_packet_alloc();

    // MP4(AVCC) 源要先过 mp4toannexb 再喂 MPP
    if (vst->codecpar->extradata_size >= 4) {
        const uint8_t* e = vst->codecpar->extradata;
        const bool src_is_annexb =
            (e[0] == 0 && e[1] == 0 && (e[2] == 1 || (e[2] == 0 && e[3] == 1)));
        if (!src_is_annexb) {
            const bool is_hevc = (cid == AV_CODEC_ID_HEVC);
            const AVBitStreamFilter* bsf =
                av_bsf_get_by_name(is_hevc ? "hevc_mp4toannexb" : "h264_mp4toannexb");
            if (bsf && av_bsf_alloc(bsf, &mp4_bsf_) == 0) {
                avcodec_parameters_copy(mp4_bsf_->par_in, vst->codecpar);
                if (av_bsf_init(mp4_bsf_) < 0) {
                    av_bsf_free(&mp4_bsf_);
                    mp4_bsf_ = nullptr;
                }
            }
        }
    }

    // ---- MPP 硬解 ----
    use_mpp_ = false;
    if (opt_.use_mpp && (cid == AV_CODEC_ID_H264 || cid == AV_CODEC_ID_HEVC)) {
        MppInitProbe probe;
        probe.dec = &mpp_dec_;
        probe.hevc = (cid == AV_CODEC_ID_HEVC);
        probe.inited = false;
        rk::SafeProbe::call(&probe_mpp_init, &probe);
        if (probe.inited) {
            use_mpp_ = true;
            if (mp4_bsf_ && mp4_bsf_->par_out->extradata_size > 0) {
                rk::Nv12Frame tmp;
                mpp_dec_.decode(mp4_bsf_->par_out->extradata,
                                mp4_bsf_->par_out->extradata_size, 0, &tmp, nullptr);
            }
        } else {
            fprintf(stderr, "[%s] MPP 初始化失败, 退回软解\n", cfg_.name.c_str());
        }
    }
    fprintf(stderr, "[%s] 解码后端 = %s (源 %.1f fps)\n", cfg_.name.c_str(),
            use_mpp_ ? "MPP 硬解" : "FFmpeg 软解", src_nominal_fps_);
    return true;
}

// ---------------------------------------------------------------------------
bool Channel::rewind_source() {
    if (!opt_.loop_src || src_is_live_) return false;
    if (av_seek_frame(fmt_, vstream_, 0, AVSEEK_FLAG_BACKWARD) < 0) return false;
    avcodec_flush_buffers(vctx_);
    if (mp4_bsf_) av_bsf_flush(mp4_bsf_);
    next_analysis_due_ = 0.0;
    return true;
}

// ---------------------------------------------------------------------------
bool Channel::decode_frame(Nv12Info* nv, rk::MppFrameGuard* mg, rk::AVFrameGuard* ag) {
    for (int i = 0; i < 32; ++i) {
        const int ret = av_read_frame(fmt_, pkt_);
        if (ret < 0) {
            if (ret != AVERROR(EAGAIN)) {
                if (rewind_source()) continue;
                warn_once(cfg_.name + ":eof", "%s 源读完了", cfg_.name.c_str());
            }
            return false;
        }
        if (pkt_->stream_index != vstream_) { av_packet_unref(pkt_); continue; }

        if (mp4_bsf_) {
            if (av_bsf_send_packet(mp4_bsf_, pkt_) < 0) { av_packet_unref(pkt_); return false; }
            if (av_bsf_receive_packet(mp4_bsf_, pkt_) < 0) {
                av_packet_unref(pkt_);
                continue;
            }
        }

        if (use_mpp_) {
            int64_t pts = pkt_->pts;
            if (pts == AV_NOPTS_VALUE) { pts = synth_pts_; synth_pts_ += 3600; }
            else synth_pts_ = pts + 3600;
            rk::Nv12Frame fi;
            void* raw = nullptr;
            if (mpp_dec_.decode(pkt_->data, pkt_->size, pts, &fi, &raw)) {
                mg->reset(raw, "mpp-src");
                nv->fd = fi.fd;
                nv->ptr = fi.ptr;
                nv->width = fi.width;
                nv->height = fi.height;
                nv->stride = fi.wstride;
                nv->plane_h = fi.hstride;
                nv->planar = false;
                nv->full_range = false;
                nv->uv_stride = fi.wstride;
                nv->u_plane = static_cast<const uint8_t*>(fi.ptr) +
                              static_cast<size_t>(fi.wstride) * fi.hstride;
                nv->v_plane = nullptr;
                av_packet_unref(pkt_);
                return true;
            }
            av_packet_unref(pkt_);
            continue;
        }

        // 软解
        if (avcodec_send_packet(vctx_, pkt_) < 0) {
            av_packet_unref(pkt_);
            continue;   // 解码出错不能退出, 跳过这一包
        }
        av_packet_unref(pkt_);
        ag->reset(av_frame_alloc(), "ffmpeg-src");
        const int r = avcodec_receive_frame(vctx_, ag->get());
        if (r == AVERROR(EAGAIN)) continue;
        if (r < 0) continue;
        extract_nv12(ag->get(), nv);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 裁一块车牌 ROI: 裁剪 + 按比例缩放到高 48 + 转 BGR888, 紧凑行距
//
// ★ 走 CPU 路径而不是 RGA 硬件路径:
//   两条路径的 YUV->RGB 口径不一样(RGA 把色度压掉了, 出来偏暗偏灰),
//   而车牌上的小字对这点差别非常敏感 —— 实测同一块绿牌:
//     RGA 路径 -> "AL"(省份字丢了), CPU 路径 -> "苏A"。
//   车牌 ROI 很小, CPU 转换几十微秒, 相对一次 NPU 推理(24ms)可以忽略。
// ★ 直接写进 dst(紧凑行距 w*3): 后面建张量要的本来就是紧凑行距,
//   以前这里还多搬一遍 46KB(16 对齐行距 -> 紧凑行距), 现在没有了。
//
// ★ dst 是【池里的 ROI 槽】, 裁好就直接被识别 worker 读 ——
//   中间不再有 ROI 的拷贝, 也不再有 ROI 的堆分配。
bool Channel::crop_plate_roi(const rk::RgaImage& src, int rx, int ry, int rw, int rh,
                              uint8_t* dst, size_t dst_cap, int* out_w) {
    // 目标尺寸: 按比例缩到高 48, 宽上限 320
    //
    // ★ 必须和 LicensePlateRec::calc_resized_w 用【同一个公式】:
    //     resized_w = ceil(img_h * src_w / src_h), 再夹到 [1, img_w]
    //   差一个像素, 字形就变形, 识别率就掉(车牌本来就只有几十像素宽)。
    //   这里是 ceil 不是 round —— 用 round 会和模型侧差 1 像素。
    int w = opt_.ppocr_in_w;
    if (rh > 0 && rw > 0) {
        const float ratio = static_cast<float>(rw) / static_cast<float>(rh);
        w = static_cast<int>(std::ceil(static_cast<float>(opt_.ppocr_in_h) * ratio));
        if (w > opt_.ppocr_in_w) w = opt_.ppocr_in_w;
        if (w < 1) w = 1;
    }
    const size_t need = static_cast<size_t>(w) * 3 * opt_.ppocr_in_h;
    if (!dst || dst_cap < need) return false;   // 槽不够大 —— 不裁(以前是扩容)

    rk::RgaImage ldst;
    ldst.fd = -1;
    ldst.vir_addr = dst;
    ldst.width = w;
    ldst.height = opt_.ppocr_in_h;
    ldst.wstride = w * 3;          // 紧凑行距
    ldst.hstride = opt_.ppocr_in_h;
    ldst.format = RK_FORMAT_BGR_888;

    if (rk::cpu_crop_to_rgb888(src, rx, ry, rw, rh, ldst) != 0) return false;

    if (opt_.ppocr_denoise > 0) {
        const int k = opt_.ppocr_denoise | 1;
        if (k >= 3) {
            cv::Mat roi(opt_.ppocr_in_h, w, CV_8UC3, dst);
            cv::GaussianBlur(roi, roi, cv::Size(k, k), 0);
        }
    }
    if (opt_.ppocr_invert) {
        // ★ 灰度中间缓冲提前开好 —— cv::Mat gray; 每次调用都会自己分配一块
        const size_t gray_need = static_cast<size_t>(w) * opt_.ppocr_in_h;
        if (roi_gray_buf_.size() < gray_need) roi_gray_buf_.resize(gray_need);
        cv::Mat roi(opt_.ppocr_in_h, w, CV_8UC3, dst);
        cv::Mat gray(opt_.ppocr_in_h, w, CV_8UC1, roi_gray_buf_.data());
        cv::cvtColor(roi, gray, cv::COLOR_BGR2GRAY);
        cv::bitwise_not(gray, gray);
        cv::cvtColor(gray, roi, cv::COLOR_GRAY2BGR);
    }
    *out_w = w;
    return true;
}

// ---------------------------------------------------------------------------
void Channel::run() {
    // ---- 上报通道 ----
    {
        rk::IvsPayloadCfg ivs;
        ivs.sn = opt_.sn;
        ivs.device_name = opt_.device_name;
        ivs.ipaddr = opt_.ipaddr;
        ivs.msg_id = opt_.sn;
        ivs.channel = index_;            // ★ 通道号进 JSON(字段本来就有)
        if (opt_.sink_stdout)
            dispatcher_.add(std::unique_ptr<rk::ResultSink>(new rk::StdoutSink()));
        if (!opt_.mqtt_host.empty()) {
            auto* s = new rk::MqttSink();
            // ★ 每路一个连接: client_id 和 topic 都带通道号 ——
            //   同一个 client_id 连两次会被 broker 踢掉。
            const std::string cid = opt_.sn + "-ch" + std::to_string(index_);
            const std::string topic = "device/" + opt_.sn + "/ch" + std::to_string(index_) +
                                      "/message/up/ivs_result";
            if (s->start(opt_.mqtt_host, opt_.mqtt_port, topic, cid, opt_.mqtt_qos, &ivs)) {
                dispatcher_.add(std::unique_ptr<rk::ResultSink>(s));
            } else {
                fprintf(stderr, "[%s] MQTT 连不上 %s\n", cfg_.name.c_str(),
                        opt_.mqtt_host.c_str());
                delete s;
            }
        }
        dispatcher_.start();
    }

    rk::SpeedCfg speed_cfg;
    speed_cfg.conf_thr = opt_.conf;
    speed_cfg.nms_iou = opt_.nms;
    speed_cfg.match_thresh = opt_.match_thresh;
    speed_cfg.speed_limit_kmh = opt_.speed_limit;
    speed_cfg.speed_win_sec = opt_.speed_win;
    speed_cfg.display_span = opt_.speed_win;

    rk::Pt ipm_quad[4];
    const bool have_quad = !cfg_.ipm_points.empty() && parse_quad(cfg_.ipm_points, ipm_quad);

    for (;;) {
        if (g_quit || stop_) break;

        rk::MppFrameGuard mpp_guard;
        rk::AVFrameGuard  av_guard;
        Nv12Info nv;
        if (!decode_frame(&nv, &mpp_guard, &av_guard)) {
            if (g_quit) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (nv.width <= 0 || nv.height <= 0) continue;

        const auto t_frame0 = std::chrono::steady_clock::now();
        if (last_decode_ts_.time_since_epoch().count() != 0) {
            stats_.on_source_gap(std::chrono::duration<double, std::milli>(
                t_frame0 - last_decode_ts_).count());
        }
        last_decode_ts_ = t_frame0;

        // ---- 分析帧率上限(--max-fps) ----
        if (opt_.max_fps > 0) {
            using clk = std::chrono::steady_clock;
            const double now_s = std::chrono::duration<double>(
                clk::now().time_since_epoch()).count();
            const double period = 1.0 / static_cast<double>(opt_.max_fps);
            if (next_analysis_due_ <= 0.0) {
                next_analysis_due_ = now_s + period;
            } else if (now_s < next_analysis_due_) {
                continue;               // 这一帧只解码, 不分析
            } else {
                next_analysis_due_ += period;
                if (next_analysis_due_ < now_s - 1.0) next_analysis_due_ = now_s + period;
            }
        }

        // ---- RGA: NV12 -> RGB888, 直接写进 NPU 的输入缓冲(同一块 dma-buf) ----
        const int slot = pool_->acquire_det_slot();
        if (slot < 0) break;

        rk::RgaImage src;
        src.fd = nv.fd;
        src.vir_addr = nv.ptr;
        src.width = nv.width;
        src.height = nv.height;
        src.wstride = nv.stride;
        src.hstride = nv.plane_h;
        src.format = RK_FORMAT_YCbCr_420_SP;
        src.u_plane = nv.u_plane;
        src.v_plane = nv.v_plane;
        src.uv_wstride = nv.uv_stride;
        src.planar_yuv = nv.planar;
        src.full_range = nv.full_range;
        src.bit_shift = nv.bit_shift;

        rk::RgaImage mdst;
        mdst.fd = pool_->slot_fd(slot);
        mdst.vir_addr = pool_->slot_ptr(slot);
        mdst.width = opt_.det_w;
        mdst.height = opt_.det_h;
        mdst.wstride = opt_.det_w * 3;
        mdst.hstride = opt_.det_h;
        mdst.format = RK_FORMAT_RGB_888;

        float scale = 1.f;
        int pad_x = 0, pad_y = 0;
        const auto t_rga0 = std::chrono::steady_clock::now();
        int rret;
        {
            // ★★ librga 在【一个进程里】是串行的:
            //   每次 rga_nv12_to_rgb888() 都要 importbuffer_fd() 注册一次目标缓冲,
            //   而那张全局的 handle 表是【进程内共享 + 加锁】的。
            //   4 个通道线程同时转, 就在这把锁上排队 —— 实测单次 RGA 从
            //   6.7ms(4K) 涨到 24.6ms, 近景那路直接被拖到 8fps。
            //   (4 进程版没这个问题: 每个进程各有一份 librga 实例。)
            //   自己先拿一把互斥量, 把"抢锁 + 排队"的抖动变成干净���排队:
            //   单次转换回到自然耗时, 4 路还是并行地做别的活。
            std::lock_guard<std::mutex> lk(g_rga_m);
            rret = rk::rga_nv12_to_rgb888(src, mdst, false, &scale, &pad_x, &pad_y);
        }
        const double t_rga = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_rga0).count();
        if (rret != 0) {
            fprintf(stderr, "[%s] RGA 转换失败\n", cfg_.name.c_str());
            pool_->release_det_slot(slot);   // 借了没用就得还, 否则这块 slot 永远回不来
            continue;
        }

        // ---- 提交检测 ----
        rk::DetTask dt;
        dt.chan = index_;
        dt.slot = slot;
        dt.img_w = nv.width;
        dt.img_h = nv.height;
        dt.scale = scale;
        dt.pad_x = pad_x;
        dt.pad_y = pad_y;
        dt.letterbox = false;
        dt.ts_ms = static_cast<int64_t>(
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        const auto t_det0 = std::chrono::steady_clock::now();
        pool_->submit_det(std::move(dt));

        rk::DetResult dr;
        if (!pool_->take_det(index_, &dr, 5000)) {
            fprintf(stderr, "[%s] 等检测结果超时\n", cfg_.name.c_str());
            break;
        }
        // 等检测结果的墙钟(含排队)。它比"推理本身"长, 说明 NPU 不够用;
        // 它跟"推理本身"差不多, 说明慢在别处(解码/RGA/后处理)。
        const double t_det_wait = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_det0).count();

        // ---- 后处理 + 几何过滤 + 跟踪 ----
        std::vector<YoloBox> boxes = std::move(dr.boxes);
        if (opt_.geo_filter && !boxes.empty())
            filter_bad_boxes(boxes, nv.width, nv.height, speed_cfg.geo);

        const double t_now = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const double t_track = static_cast<double>(dr.ts_ms) * 0.001;

        std::vector<rk::TrackResult> tracks;
        std::vector<YoloBox> track_boxes;
        if (track_cls_ids_.empty()) {
            track_boxes = boxes;
        } else {
            for (const auto& b : boxes)
                if (std::find(track_cls_ids_.begin(), track_cls_ids_.end(), b.cls_id) !=
                    track_cls_ids_.end())
                    track_boxes.push_back(b);
        }
        tracker_.update(track_boxes, t_track, speed_cfg, calib_, &tracks);

        // ---- 标定(首帧做一次) ----
        if (cfg_.high && !calib_built_) {
            calib_built_ = true;
            if (have_quad) {
                std::string err;
                rk::GroundPlane gp;
                double focal = 0.0;
                if (opt_.ipm_fov > 0.0)
                    focal = (nv.width / 2.0) / std::tan(opt_.ipm_fov * M_PI / 360.0);
                if (rk::GroundPlane::solve(ipm_quad, nv.width, nv.height, focal, &gp, &err)) {
                    const double width_m = opt_.ipm_width_m > 0.0 ? opt_.ipm_width_m : 3.5;
                    const double scale_c = width_m / gp.width_norm;
                    if (rk::BevView::build(ipm_quad, gp, scale_c, opt_.ipm_ppm, &bev_, &err)) {
                        calib_.bev = &bev_;
                        fprintf(stderr,
                                "[%s] IPM 就绪: f=%.1fpx 标定区 %.2fm x %.2fm 鸟瞰 %dx%d\n",
                                cfg_.name.c_str(), gp.f, bev_.width_m, bev_.length_m,
                                bev_.dst_w, bev_.dst_h);
                    } else {
                        fprintf(stderr, "[%s] BEV 构建失败: %s\n", cfg_.name.c_str(), err.c_str());
                    }
                } else {
                    fprintf(stderr, "[%s] 逆透视解算失败: %s\n", cfg_.name.c_str(), err.c_str());
                }
            }
        }

        // ---- 车牌识别 ----
        if (cfg_.near && !plate_cls_ids_.empty()) {
            const int64_t now_ms = static_cast<int64_t>(t_now * 1000.0);
            for (auto& tr : tracks) {
                if (std::find(plate_cls_ids_.begin(), plate_cls_ids_.end(), tr.box.cls_id) ==
                    plate_cls_ids_.end())
                    continue;
                if (tr.score < opt_.ppocr_min_score) continue;

                std::string plate;
                bool cached = false;
                plate_cache_.get(tr.track_id, now_ms, opt_.ppocr_ttl_ms, &plate, &cached);
                if (cached) { tr.plate = plate; continue; }

                int rx = 0, ry = 0, rw = 0, rh = 0;
                if (!rk::plate_roi_from_box(tr.box, nv.width, nv.height, opt_.roi_ratio,
                                            opt_.roi_y, opt_.roi_height, opt_.roi_shift,
                                            opt_.roi_below, &rx, &ry, &rw, &rh))
                    continue;
                if (opt_.ppocr_pad > 0.f) {
                    const int dx = static_cast<int>(rw * opt_.ppocr_pad);
                    const int dy = static_cast<int>(rh * opt_.ppocr_pad);
                    rx -= dx; ry -= dy; rw += dx * 2; rh += dy * 2;
                }
                if (!rk::clamp_roi(&rx, &ry, &rw, &rh, nv.width, nv.height, 8, 6)) continue;
                rx &= ~1; ry &= ~1; rw &= ~1; rh &= ~1;
                if (!rk::clamp_roi(&rx, &ry, &rw, &rh, nv.width, nv.height, 8, 6)) continue;

                // ROI 最小尺寸: 太扁的话缩放到高 48 要放大好几倍, RGA 扛不住
                //   (实测: 默认参数每 300 帧必现一次 RGA 驱动软复位)
                constexpr int kMinRoiH = 16, kMinRoiW = 32;
                if (rh < kMinRoiH) {
                    const int need = kMinRoiH - rh;
                    ry -= need; rh = kMinRoiH;
                    if (ry < 0) { rh += ry; ry = 0; }
                }
                if (rw < kMinRoiW) {
                    const int need = kMinRoiW - rw;
                    rx -= need / 2; rw = kMinRoiW;
                    if (rx < 0) { rw += rx; rx = 0; }
                }
                if (!rk::clamp_roi(&rx, &ry, &rw, &rh, nv.width, nv.height, 8, 6)) continue;

                // ---- 两行牌: 上行试 3 个切分点, 下行切 1 次 ----
                //   PP-OCR 只会读一行文字, 两行牌整块喂进去读出来是乱码。
                //   切分点差 5%, 上行那一小截字的位置就差好几个像素 ——
                //   所以不能只试一个点, 也不能按分数挑(认对的 "苏A" 分数
                //   反而比认错的 "A" 低), 判据优先级是 含汉字 > 字符多 > 分数高。
                const bool two_row = (static_cast<float>(rw) / static_cast<float>(rh)) <
                                     rk::kTwoRowAspect;

                // 先把候选全裁出来(裁剪要源帧活着, 所以必须在通道线程里做)
                //
                // ★ 定长数组, 不用 vector: 候选最多 4 个(上行 3 个切分点 + 下行 1 个)。
                // ★ ROI 直接裁进【池里的 ROI 槽】, 任务只带槽号 ——
                //   以前是: 裁进 roi_buf_ -> 拷进 Cand::roi(vector, 一次堆分配)
                //          -> 再拷进 OcrTask::roi(又一次堆分配) -> 随任务走。
                //   每个候选两次 malloc + 两次 46KB memcpy; 现在一次都没有。
                struct Cand { bool top; int slot; int roi_slot; int w; };
                constexpr int kMaxCands = 8;
                Cand cands[kMaxCands];
                int n_cands = 0;
                const size_t roi_cap = pool_->ocr_roi_capacity();
                auto make_cand = [&](bool top, int slot, int y0, int h) -> bool {
                    if (n_cands >= kMaxCands) return false;
                    const int roi_slot = pool_->acquire_ocr_roi();
                    if (roi_slot < 0) return false;          // 池在关停
                    uint8_t* dst = pool_->ocr_roi_ptr(roi_slot);
                    int w = 0;
                    if (!crop_plate_roi(src, rx, y0, rw, h, dst, roi_cap, &w)) {
                        pool_->release_ocr_roi(roi_slot);
                        return false;
                    }
                    cands[n_cands].top = top;
                    cands[n_cands].slot = slot;
                    cands[n_cands].roi_slot = roi_slot;
                    cands[n_cands].w = w;
                    ++n_cands;
                    return true;
                };

                if (two_row) {
                    for (int t = 0; t < rk::kRowSplitTrials; ++t) {
                        const float sp = rk::kRowSplitDefault +
                                         (t - (rk::kRowSplitTrials - 1) / 2) * rk::kRowSplitStep;
                        const int cut = ry + static_cast<int>(rh * sp);
                        if (cut <= ry || cut >= ry + rh) continue;
                        make_cand(true, t, ry, cut - ry);
                    }
                    const int mid = ry + static_cast<int>(rh * rk::kRowSplitDefault);
                    if (mid > ry && mid < ry + rh) make_cand(false, 99, mid, ry + rh - mid);
                } else {
                    make_cand(true, 0, ry, rh);
                }
                if (n_cands == 0) continue;

                // 提交识别(高优先级任务, 排在检测前面)
                const int group = tr.track_id;
                int submitted = 0;
                for (int ci = 0; ci < n_cands; ++ci) {
                    const Cand& c = cands[ci];
                    // ROI 存盘(调试): 落的就是真正喂给 PP-OCR 的那张图
                    char sfx[24];
                    snprintf(sfx, sizeof(sfx), c.top ? "_top%d" : "_bot", c.slot);
                    roi_dump_.dump(cfg_.name, tr.track_id, static_cast<int>(stats_.frames),
                                   pool_->ocr_roi_ptr(c.roi_slot), c.w, opt_.ppocr_in_h, sfx);
                    rk::OcrTask ot;
                    ot.chan = index_;
                    ot.group = group;
                    ot.slot = c.slot;
                    ot.is_top = c.top;
                    ot.roi_slot = c.roi_slot;      // ★ 只带槽号, 不带像素
                    ot.roi_w = c.w;
                    ot.roi_h = opt_.ppocr_in_h;
                    ot.ts_ms = now_ms;
                    if (!pool_->submit_ocr(std::move(ot))) {
                        // 提交失败(池在关停): ROI 槽没人会还了, 这里自己还
                        pool_->release_ocr_roi(c.roi_slot);
                        continue;
                    }
                    ++submitted;
                }

                // 等结果
                std::string best_top;
                float best_top_score = -1.f;
                std::string row_bot;
                float plate_score = 0.f;
                const auto t_ocr0 = std::chrono::steady_clock::now();
                for (int ci = 0; ci < n_cands; ++ci) {
                    const Cand& c = cands[ci];
                    rk::OcrResult orr;
                    if (!pool_->take_ocr(index_, group, c.slot, &orr, 3000) || !orr.ok) continue;
                    if (c.top) {
                        if (rk::better_top_row(orr.text, orr.score, best_top, best_top_score)) {
                            best_top = orr.text;
                            best_top_score = orr.score;
                        }
                    } else {
                        row_bot = orr.text;
                        plate_score = orr.score;
                    }
                }
                (void)submitted;
                // ★ plate 上面已经声明过了(取缓存用的那个), 这里直接复用:
                //   重复声明会编译不过, 而且语义上也是同一个变量。
                if (!best_top.empty() && !row_bot.empty()) plate = best_top + row_bot;
                else if (!best_top.empty()) plate = best_top;
                else if (!row_bot.empty()) plate = row_bot;
                // 裁剪宽度(日志用): 候选里最宽的那张
                int crop_w = 0;
                for (int ci = 0; ci < n_cands; ++ci) crop_w = std::max(crop_w, cands[ci].w);
                // 等识别结果的墙钟(含排队)
                const double t_ocr_wait = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t_ocr0).count();
                ocr_wait_acc_ += t_ocr_wait;
                ocr_wait_n_ += 1;
                // ★ 缓存【无条件】写入 —— 包括空结果。
                //   只缓存非空的话, 识别失败的那些 track 下一帧又会去识别一次,
                //   两行牌一次就是 4 次 PP-OCR(每次 ~24ms) -> 近景那路直接掉到 4fps。
                //   (单流版 main.cc 就是无条件 put 的。)
                plate_cache_.put(tr.track_id, now_ms, opt_.ppocr_ttl_ms, plate);
                if (!plate.empty()) {
                    tr.plate = plate;
                    fprintf(stderr,
                            "[%s] [车牌] track=%d cls=%d score=%.2f box=%dx%d@(%d,%d) "
                            "roi=%dx%d@(%d,%d) 裁剪=%dx%d%s -> \"%s\"\n",
                            cfg_.name.c_str(), tr.track_id, tr.box.cls_id, plate_score,
                            static_cast<int>(tr.box.x2 - tr.box.x1),
                            static_cast<int>(tr.box.y2 - tr.box.y1),
                            static_cast<int>(tr.box.x1), static_cast<int>(tr.box.y1),
                            rw, rh, rx, ry, crop_w, opt_.ppocr_in_h,
                            two_row ? " 两行" : "",
                            plate.c_str());
                }
            }
        }

        report(tracks);

        const double t_total = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_frame0).count();
        stats_.on_frame(t_total);
        stats_.rga.push(t_rga);
        stats_.npu_run.push(dr.infer_ms);
        det_wait_acc_ += t_det_wait;
        det_wait_n_ += 1;
        if (opt_.stats_interval > 0 && stats_.frames % opt_.stats_interval == 0)
            print_stats(boxes.size(), tracks.size(), &tracks);
    }

    dispatcher_.stop();
    if (vctx_) avcodec_free_context(&vctx_);
    if (fmt_) avformat_close_input(&fmt_);
    if (pkt_) av_packet_free(&pkt_);
    if (mp4_bsf_) av_bsf_free(&mp4_bsf_);
}

// ---------------------------------------------------------------------------
void Channel::report(const std::vector<rk::TrackResult>& tracks) {
    // ★ 过线计数: 【异步】提交。
    //   submit() 只做一次定长拷贝就返回 —— 推理线程不等待、不碰网络。
    //   真正的计数 / JSON 组包 / MQTT 发送都在 TripwireService 的后台线程里。
    if (tripwire_) {
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        tripwire_->submit(index_, tracks, static_cast<uint32_t>(stats_.frames), now);
    }
    if (dispatcher_.empty_sinks()) return;
    std::vector<rk::DetectResult> drs;
    drs.reserve(tracks.size());
    for (const auto& tr : tracks) {
        rk::DetectResult d;
        d.track_id = tr.track_id;
        d.cls_id = tr.box.cls_id;
        d.score = tr.score;
        d.speed_kmh = tr.speed_kmh;
        d.speed_valid = tr.speed_valid;
        d.over_speed = tr.over_speed;
        d.plate = tr.plate;
        d.ts_ms = static_cast<int64_t>(
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        drs.push_back(d);
    }
    dispatcher_.push(stats_.frames, std::move(drs));
}

// ---------------------------------------------------------------------------
// ★ 格式和单流版一致: 压测脚本用 grep 'deliv=[0-9.]+' 取帧率
void Channel::print_stats(size_t n_boxes, size_t n_tracks,
                          const std::vector<rk::TrackResult>* tracks) {
    // det_wait: 等检测结果的墙钟(含 NPU 排队) —— 和 npu(纯推理)一比就知道
    //           慢在 NPU 还是慢在别处。
    // ocr_wait: 等识别结果的墙钟(平均到每帧)。
    const double det_wait = det_wait_n_ ? det_wait_acc_ / det_wait_n_ : 0.0;
    const double ocr_wait = ocr_wait_n_ ? ocr_wait_acc_ / ocr_wait_n_ : 0.0;
    // ★ 测速: 本采样窗口里最快的目标速度 + 有几个目标速度有效。
    //   和单流版的 spd=/sv= 字段同名同义, 两边的日志能直接对比。
    float spd = 0.f;
    int   sv = 0;
    if (tracks) {
        for (const auto& tr : *tracks) {
            if (!tr.speed_valid) continue;
            ++sv;
            if (tr.speed_kmh > spd) spd = tr.speed_kmh;
        }
    }
    fprintf(stderr,
            "[%s][frame %d] fps=%.1f deliv=%.1f src=%.1f rga=%.2fms npu=%.2fms "
            "detwait=%.2fms ocrwait=%.2fms objs=%zu tracks=%zu spd=%.1f sv=%d "
            // ★ rss/heap: 长跑时的内存证据(和单流版 main.cc 同口径)。
            //   heap = mallinfo2().uordblks = 【正在使用】的堆字节, 只增不减才是真泄漏;
            //   RSS 会被 arena 保留/碎片干扰, 单看它会把碎片误判成泄漏。
            "rss=%zuKB heap=%zuKB\n",
            cfg_.name.c_str(), stats_.frames, stats_.fps(), stats_.delivered_fps(),
            src_nominal_fps_, stats_.rga.mean(), stats_.npu_run.mean(),
            det_wait, ocr_wait, n_boxes, n_tracks, spd, sv,
            read_rss_kb(), (size_t)mallinfo2().uordblks / 1024);
    det_wait_acc_ = 0.0; det_wait_n_ = 0;
    ocr_wait_acc_ = 0.0; ocr_wait_n_ = 0;
}

// ---------------------------------------------------------------------------
void usage(const char* prog) {
    fprintf(stderr,
            "用法: %s --model M.rknn [选项]\n"
            "  --ch 名字:源:near|high[:IPM四点]   加一路(可重复)\n"
            "  --labels labels.txt\n"
            "  --det-core 1,2    检测用哪些核(核号, 0/1/2; 三核全开 0,1,2 已禁用)\n"
            "  --ocr-core 0      识别用哪些核(核号)\n"
            "  --det-workers 3   检测 worker 数(均匀轮询绑到上面那些核)\n"
            "  --ocr-workers 3   识别 worker 数(均匀轮询绑到上面那些核)\n"
            "  --ppocr M.rknn    车牌识别模型\n"
            "  --ppocr-dict d.txt\n"
            "  --conf 0.15 --nms 0.45\n"
            "  --max-fps 20      每路分析帧率上限\n"
            "  --duration 600    跑多少秒(0=一直)\n"
            "  --mqtt-host 1.2.3.4 --mqtt-port 1883 --sn rk3588-01\n"
            "  --tripwire-line x1,y1,x2,y2   虚拟线(原图像素); 给了才启用过线计数\n"
            "  --tripwire-line ch1:x1,y1,x2,y2  只给某个通道配线(可重复)\n"
            "  --tripwire-in positive|negative 哪个方向算\"进\"(默认 positive)\n"
            "  --tripwire-classes 0,1          只统计这些类别(默认全部)\n"
            "  --tripwire-miss-frames 30       轨迹多少帧没更新就清掉\n"
            "  --tripwire-interval-ms 1000     统计上报周期\n"
            "  --tripwire-topic <topic>        覆盖默认上报 topic\n"
            "  --tripwire-no-mqtt              只本地计数, 不发 MQTT\n",
            prog);
}

}  // namespace

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "参数 %s 缺值\n", what);
                exit(1);
            }
            return argv[++i];
        };
        if (a == "--model") opt.model = next("--model");
        else if (a == "--labels") opt.labels = next("--labels");
        else if (a == "--ppocr") opt.ppocr_model = next("--ppocr");
        else if (a == "--ppocr-dict") opt.ppocr_dict = next("--ppocr-dict");
        else if (a == "--ppocr-charset") opt.ppocr_charset = next("--ppocr-charset");
        else if (a == "--det-core") opt.det_core_mask = rk::parse_core_mask(next("--det-core").c_str());
        else if (a == "--ocr-core") opt.ocr_core_mask = rk::parse_core_mask(next("--ocr-core").c_str());
        else if (a == "--det-workers") opt.det_workers = atoi(next("--det-workers").c_str());
        else if (a == "--ocr-workers") opt.ocr_workers = atoi(next("--ocr-workers").c_str());
        else if (a == "--conf") opt.conf = atof(next("--conf").c_str());
        else if (a == "--nms") opt.nms = atof(next("--nms").c_str());
        else if (a == "--max-fps") opt.max_fps = atoi(next("--max-fps").c_str());
        else if (a == "--stats-interval") opt.stats_interval = atoi(next("--stats-interval").c_str());
        else if (a == "--det-slots") opt.det_slots = atoi(next("--det-slots").c_str());
        else if (a == "--ipm-width-m") opt.ipm_width_m = atof(next("--ipm-width-m").c_str());
        else if (a == "--ipm-ppm") opt.ipm_ppm = atof(next("--ipm-ppm").c_str());
        else if (a == "--ipm-fov") opt.ipm_fov = atof(next("--ipm-fov").c_str());
        else if (a == "--speed-limit") opt.speed_limit = atof(next("--speed-limit").c_str());
        else if (a == "--plate-cls") opt.plate_cls = atoi(next("--plate-cls").c_str());
        else if (a == "--track-cls") opt.track_cls = atoi(next("--track-cls").c_str());
        else if (a == "--roi-ratio") opt.roi_ratio = atof(next("--roi-ratio").c_str());
        else if (a == "--roi-y") opt.roi_y = atof(next("--roi-y").c_str());
        else if (a == "--roi-height") opt.roi_height = atof(next("--roi-height").c_str());
        else if (a == "--roi-shift") opt.roi_shift = atof(next("--roi-shift").c_str());
        else if (a == "--roi-below") opt.roi_below = atof(next("--roi-below").c_str());
        else if (a == "--ppocr-ttl") opt.ppocr_ttl_ms = atoi(next("--ppocr-ttl").c_str());
        else if (a == "--ppocr-denoise") opt.ppocr_denoise = atoi(next("--ppocr-denoise").c_str());
        else if (a == "--ppocr-invert") opt.ppocr_invert = true;
        else if (a == "--tripwire-line") {
            // 格式: x1,y1,x2,y2              (适用于所有通道)
            //   或: ch1:x1,y1,x2,y2           (只给 ch1)
            const std::string spec = next("--tripwire-line");
            std::string ch;
            std::string coords = spec;
            const size_t colon = spec.find(':');
            if (colon != std::string::npos) {
                ch = spec.substr(0, colon);
                coords = spec.substr(colon + 1);
            }
            float v[4] = {0, 0, 0, 0};
            if (sscanf(coords.c_str(), "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]) != 4) {
                fprintf(stderr, "--tripwire-line 格式应为 x1,y1,x2,y2 或 ch1:x1,y1,x2,y2\n");
                return 1;
            }
            Options::TripwireLineSpec ls;
            ls.ch = ch;
            ls.x1 = v[0]; ls.y1 = v[1]; ls.x2 = v[2]; ls.y2 = v[3];
            opt.tripwire_lines.push_back(ls);
            opt.tripwire_enabled = true;
        }
        else if (a == "--tripwire-in") {
            const std::string v = next("--tripwire-in");
            if (v == "positive") opt.tripwire_positive_is_in = true;
            else if (v == "negative") opt.tripwire_positive_is_in = false;
            else {
                fprintf(stderr, "--tripwire-in 只能是 positive|negative\n");
                return 1;
            }
        }
        else if (a == "--tripwire-classes") {
            const std::string v = next("--tripwire-classes");
            const char* p2 = v.c_str();
            while (*p2) {
                char* end = nullptr;
                const long id = strtol(p2, &end, 10);
                if (end == p2) break;
                opt.tripwire_classes.push_back(static_cast<int>(id));
                p2 = (*end == ',') ? end + 1 : end;
            }
        }
        else if (a == "--tripwire-miss-frames") opt.tripwire_miss_frames = atoi(next("--tripwire-miss-frames").c_str());
        else if (a == "--tripwire-interval-ms") opt.tripwire_publish_ms = atoll(next("--tripwire-interval-ms").c_str());
        else if (a == "--tripwire-topic") opt.tripwire_topic = next("--tripwire-topic");
        else if (a == "--tripwire-no-mqtt") opt.tripwire_no_mqtt = true;
        else if (a == "--save-roi") opt.save_roi = true;
        else if (a == "--save-roi-dir") opt.save_roi_dir = next("--save-roi-dir");
        else if (a == "--sn") opt.sn = next("--sn");
        else if (a == "--device-name") opt.device_name = next("--device-name");
        else if (a == "--ipaddr") opt.ipaddr = next("--ipaddr");
        else if (a == "--mqtt-host") opt.mqtt_host = next("--mqtt-host");
        else if (a == "--mqtt-port") opt.mqtt_port = atoi(next("--mqtt-port").c_str());
        else if (a == "--mqtt-qos") opt.mqtt_qos = atoi(next("--mqtt-qos").c_str());
        else if (a == "--duration") opt.duration = atoi(next("--duration").c_str());
        else if (a == "--no-loop") opt.loop_src = false;
        else if (a == "--no-mpp") opt.use_mpp = false;
        else if (a == "--stdout") opt.sink_stdout = true;
        else if (a == "--ch") {
            ChanCfg c;
            if (!parse_chan_spec(next("--ch"), &c)) {
                fprintf(stderr, "--ch 格式应为 名字:源:near|high[:IPM四点]\n");
                return 1;
            }
            opt.chans.push_back(c);
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "未知参数: %s\n", a.c_str());
            usage(argv[0]);
            return 1;
        }
    }

    if (opt.model.empty() || opt.chans.empty()) {
        usage(argv[0]);
        return 1;
    }
    if (opt.det_core_mask == 0) {
        fprintf(stderr, "[错误] --det-core 必须给(每一路都要跑检测)\n");
        return 1;
    }
    if (opt.ocr_core_mask == 0 && !opt.ppocr_model.empty()) {
        fprintf(stderr, "[错误] 给了 --ppocr 就必须给 --ocr-core\n");
        return 1;
    }
    // ★ 核掩码 0x7 在这一层是【合法】的: 它只表示"可用核集合 = {0,1,2}",
    //   池子给每个 worker 绑的是【单核】掩码(1/2/4), 不会把 0x7 交给驱动。
    //   被禁的是"把 0x7 直接送给驱动"(RKNN_NPU_CORE_0_1_2 = 一次推理拆三核):
    //   独立压测台实测吞吐只有 1/2.6, e2e P99 高 25 倍。
    //   那条禁令落在驱动边界(rk::sanitize_core_mask), 不在这儿 ——
    //   在这儿拦会把"三颗核都用上"这种正确用法也拦掉。
    if (opt.det_workers <= 0 || opt.ocr_workers <= 0) {
        fprintf(stderr, "[错误] --det-workers / --ocr-workers 必须是正数\n");
        return 1;
    }
    if (!probe_model(opt.model, &opt.det_w, &opt.det_h, &opt.det_n_output)) {
        fprintf(stderr, "[错误] 模型打不开: %s\n", opt.model.c_str());
        return 1;
    }
    {
        // ★ 两个掩码要用【两个不同的缓冲】: 同一个 buf 会被后一次调用覆盖,
        //   打印出来就变成同一个字符串(踩过: mask 0x6 打成了 "Core0")。
        char cb_det[64], cb_ocr[64];
        fprintf(stderr, "[模型] %s 输入 %dx%d 输出 %d 个\n", opt.model.c_str(),
                opt.det_w, opt.det_h, opt.det_n_output);
        fprintf(stderr, "[NPU] 检测核 mask=0x%x (%s) x%d worker / 识别核 mask=0x%x (%s) x%d worker\n",
                opt.det_core_mask,
                rk::core_mask_to_string(opt.det_core_mask, cb_det, sizeof(cb_det)),
                opt.det_workers,
                opt.ocr_core_mask,
                rk::core_mask_to_string(opt.ocr_core_mask, cb_ocr, sizeof(cb_ocr)),
                opt.ocr_workers);
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    // ---- 起 NPU 池 ----
    rk::NpuPool pool;
    {
        rk::NpuPool::Config pc;
        pc.det_model = opt.model;
        pc.det_w = opt.det_w;
        pc.det_h = opt.det_h;
        pc.det_n_output = opt.det_n_output;
        pc.conf = opt.conf;
        pc.nms = opt.nms;
        pc.det_core_mask = opt.det_core_mask;
        pc.ocr_core_mask = opt.ocr_core_mask;
        pc.det_slots_per_worker = opt.det_slots;
        // ★ worker 数量(池内按 i % 核数 均匀轮询绑核)
        pc.det_workers = opt.det_workers;
        pc.ocr_workers = opt.ocr_workers;
        // ★ ROI 槽: 一个候选一块, 尺寸按 PP-OCR 的输入上限
        pc.ocr_roi_max_bytes = static_cast<size_t>(opt.ppocr_in_w) * 3 *
                                static_cast<size_t>(opt.ppocr_in_h);
        pc.ocr_roi_slots = 64;
        if (!opt.ppocr_model.empty()) {
            pc.ocr_cfg.model_path = opt.ppocr_model;
            // 字典: 没给就从模型同目录找(和单流版同一条规矩)
            std::string dict_path = opt.ppocr_dict;
            if (dict_path.empty()) {
                const size_t slash = opt.ppocr_model.find_last_of('/');
                const std::string dir = (slash == std::string::npos)
                                            ? std::string(".")
                                            : opt.ppocr_model.substr(0, slash);
                dict_path = dir + "/ppocr_keys_v1.txt";
            }
            pc.ocr_cfg.dict_path = dict_path;
            pc.ocr_cfg.norm_baked_in = true;
            pc.ocr_cfg.allowed_charset = opt.ppocr_charset;
            pc.ocr_cfg.swap_rb = false;
            // ★ 这里【不要】再写 npu_core_mask —— 池子里的每个识别 worker 会
            //   用【自己的】核掩码去初始化上下文(见 npu_pool.cc 的 ocr_worker_loop)。
            //   以前这里写 0(=AUTO), 而池子并没有真的去绑, 于是识别上下文
            //   一直是"驱动随便挑一颗核": 每核闸门管的不是硬件真在用的核,
            //   "识别优先"形同虚设, 每核统计也记错了核。
        }
        if (!pool.init(pc)) {
            fprintf(stderr, "[错误] NPU 池启动失败\n");
            return 1;
        }
    }

    // ---- 过线计数(二期 KPI): 一个进程一个服务, 各相机一条自己的线 ----
    //
    //   为什么不是"每路一个服务":
    //   · 各相机的虚拟线坐标不同(分辨率不同), 线必须按相机配;
    //   · 上报要带【各相机的独立计数】+ 合计 -> 一份 JSON 里全带上,
    //     Web 端订阅【一个】topic 就能拿到所有相机的数据。
    std::unique_ptr<rk::TripwireService> tripwire;
    if (opt.tripwire_enabled) {
        rk::TripwireService::Options tw;
        tw.sn = opt.sn;
        tw.publish_interval_ms = opt.tripwire_publish_ms;
        if (!opt.tripwire_no_mqtt) {
            tw.mqtt_host = opt.mqtt_host;
            tw.mqtt_port = opt.mqtt_port;
            tw.mqtt_qos = opt.mqtt_qos;
            // 设备级 topic: Web 端订阅一次就能拿到所有相机的计数
            tw.mqtt_topic = opt.tripwire_topic.empty()
                ? ("device/" + opt.sn + "/message/up/tripwire")
                : opt.tripwire_topic;
            tw.mqtt_client_id = opt.sn + "-tripwire";
        }
        // 每台相机: 先找【点名给它的】线, 找不到就用【通用线】
        for (size_t i = 0; i < opt.chans.size(); ++i) {
            const Options::TripwireLineSpec* spec = nullptr;
            for (const auto& ls : opt.tripwire_lines) {
                if (!ls.ch.empty() && ls.ch == opt.chans[i].name) { spec = &ls; break; }
            }
            if (!spec) {
                for (const auto& ls : opt.tripwire_lines) {
                    if (ls.ch.empty()) { spec = &ls; break; }
                }
            }
            if (!spec) {
                fprintf(stderr, "[Tripwire] 通道 %s 没配虚拟线, 不参与计数\n",
                        opt.chans[i].name.c_str());
                continue;
            }
            rk::TripwireCamera cam;
            cam.camera_id = static_cast<int>(i);
            cam.channel = static_cast<int>(i);
            cam.name = opt.chans[i].name;
            cam.cfg.x1 = spec->x1;
            cam.cfg.y1 = spec->y1;
            cam.cfg.x2 = spec->x2;
            cam.cfg.y2 = spec->y2;
            cam.cfg.positive_is_in = opt.tripwire_positive_is_in;
            cam.cfg.classes = opt.tripwire_classes;
            cam.cfg.max_miss_frames = opt.tripwire_miss_frames;
            tw.cameras.push_back(cam);
        }
        if (tw.cameras.empty()) {
            fprintf(stderr, "[错误] 配了 --tripwire-line 但没有一条能对上通道\n");
            return 1;
        }
        tripwire.reset(new rk::TripwireService());
        if (!tripwire->start(tw)) {
            fprintf(stderr, "[错误] 过线计数服务启动失败\n");
            return 1;
        }
    }

    // ---- 建通道 ----
    std::vector<std::unique_ptr<Channel>> chans;
    for (size_t i = 0; i < opt.chans.size(); ++i) {
        chans.emplace_back(new Channel(static_cast<int>(i), opt.chans[i], opt, &pool));
        if (opt.save_roi)
            chans.back()->enable_roi_dump(true, opt.save_roi_dir);
        if (tripwire) chans.back()->set_tripwire(tripwire.get());
    }
    // ★ 全部源先在【主线程】里打开: MPP 的 init 要走 SafeProbe(进程级信号处理),
    //   不能并发调。
    std::vector<std::thread> threads;
    for (size_t i = 0; i < chans.size(); ++i) {
        if (!chans[i]->open_source()) {
            fprintf(stderr, "[错误] 通道 %s 打开失败\n", chans[i]->name().c_str());
            return 1;
        }
    }
    for (size_t i = 0; i < chans.size(); ++i) {
        threads.emplace_back([&chans, i]() { chans[i]->run(); });
    }

    // ---- 主线程: 定时打印每核占用 + 等退出 ----
    const auto t_start = std::chrono::steady_clock::now();
    for (;;) {
        if (g_quit) break;
        if (opt.duration > 0) {
            const double sec = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t_start).count();
            if (sec >= opt.duration) break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    for (auto& c : chans) c->stop();
    for (auto& t : threads) if (t.joinable()) t.join();
    pool.stop();
    // 过线计数的后台线程(进程级共享服务)在这里统一停
    if (tripwire) {
        const rk::TripwireStats ts = tripwire->stats();
        fprintf(stderr, "\n===== 过线计数汇总 =====\n");
        fprintf(stderr, "  合计: 进 %lld / 出 %lld (共 %lld), 在册轨迹 %d, 丢帧 %llu\n",
                (long long)ts.in_count, (long long)ts.out_count,
                (long long)ts.total(), ts.active_tracks,
                (unsigned long long)ts.dropped_frames);
        for (const auto& cs : tripwire->camera_stats()) {
            fprintf(stderr, "  相机 %s(ch %d): 进 %lld / 出 %lld (共 %lld)\n",
                    cs.name.c_str(), cs.channel,
                    (long long)cs.stats.in_count, (long long)cs.stats.out_count,
                    (long long)cs.stats.total());
        }
        tripwire->stop();
    }

    // ---- 收尾: 每核占用(应用内自测口径) ----
    {
        const double up = pool.uptime_ms();
        fprintf(stderr, "\n===== 每核 NPU 占用(应用内自测: 该核上 rknn_run 耗时 / 墙钟) =====\n");
        for (const auto& st : pool.core_stats()) {
            char cb[64];
            fprintf(stderr,
                    "  %-8s mask=0x%-2x 任务 %6llu  NPU占用 %6.2f%%  含后处理 %6.2f%%\n",
                    st.kind == 1 ? "识别" : "检测", st.core_mask,
                    (unsigned long long)st.tasks,
                    up > 0 ? 100.0 * st.busy_ms / up : 0.0,
                    up > 0 ? 100.0 * st.total_ms / up : 0.0);
            (void)cb;
        }
        fprintf(stderr, "  (对照命令, 需 root: sudo cat /sys/kernel/debug/rknpu/load)\n");
    }
    return 0;
}
