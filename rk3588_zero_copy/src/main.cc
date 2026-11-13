// main.cc —— RK3588 零拷贝视频推理链路
//
//   RTSP(H.264)
//     └─ FFmpeg(avformat 拉流 + h264_rkmpp 硬解)
//          └─ AVFrame: NV12(MPP 的 dma-buf / CPU 映射地址)
//               └─ RGA(improcess)  缩放 + NV12→RGB888     ← 全程无 CPU memcpy
//                    └─ 自有 dma-buf(RGB888, NHWC)
//                         └─ rknn_create_mem_from_fd + rknn_set_io_mem
//                              └─ rknn_run                  ← NPU 直接读 dma-buf
//                                   └─ rknn_outputs_get → DFL 后处理 → 画框 → imshow
//
// 后处理部分移植自 AI 算法组的板端推理代码 (rknn_cpp/rknn_yolov8_infer.cc) :
//   * 9 输出 fork 格式 [box, cls, sum] * 3 尺度, DFL 在 CPU 上解
//   * 单输出标准格式 [1, 4+nc, num_anchors]
//   * OutTensor 统一访问器: float 直读 / int8 现场反量化
//
// 用法:
//   rk_zero_copy <rtsp_url> <model.rknn> [选项]
//     --size WxH      模型输入尺寸(默认从模型属性读取)
//     --conf 0.25     置信度阈值
//     --nms 0.45      NMS IoU 阈值
//     --labels f.txt  类别名文件(每行一个)
//     --letterbox     等比 letterbox(默认是拉伸缩放; 监控小目标用拉伸更好)
//     --stretch       显式指定拉伸缩放(默认)
//     --int8          取原始量化输出(int8), 不让驱动转 float
//     --no-show       不显示窗口(纯跑性能)
//     --disp-width N  显示窗口最大宽度(默认 1280)
//     --frames N      处理 N 帧后退出
//
// 环境变量:
//     RK_NO_MPP_OPAQUE=1   不去探测 frame->buf[0] 的 opaque(规避个别 FFmpeg 版本的段错误)
//     RK_NO_MPP_FD_LOOKUP=1  不做 /proc 反查 dma-buf fd

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <malloc.h>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <rknn_api.h>
}

#include <dirent.h>
#include <unistd.h>

#include <algorithm>

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "dmabuf.h"
#include "ipm.h"
#include "lprnet.h"
#include "mpp_compat.h"
#include "mpp_decoder.h"
#include "rk/av_frame_guard.h"
#include "rk/npu_pipe.h"
#include "rk/fd_ledger.h"
#include "rk/mem_guard.h"
#include "rk/mpp_frame_guard.h"
#include "rga_convert.h"
#include "result_sink.h"
#include "safe_probe.h"
#include "speed_track.h"
#include "stats.h"
#include "yolo_post.h"

static volatile sig_atomic_t g_quit = 0;
static void on_signal(int) { g_quit = 1; }

// ---------------------------------------------------------------------------
// 读当前进程的 RSS(KB)。/proc/self/statm 比 /proc/self/status 便宜得多,
// 可以每帧调。
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

// ---------------------------------------------------------------------------
// 日志落盘(可选): 自己开文件接管 stderr, 并定期 fdatasync
//
// ★ 为什么不能让外部直接重定向(stderr > file):
//   stderr 默认无缓冲, 每次 fprintf 都是一次 write() 系统调用, 数据先进
//   Page Cache 的【脏页】; 脏页要等内核的脏页回写周期(默认 30s)或写满
//   脏页比例阈值才落盘。长跑时日志文件可以涨到几百 MB, 脏页在 Page Cache
//   里堆着 —— 在 cgroup MemoryMax 限额下, 这会直接把内存吃满触发 OOM,
//   表现为"应用莫名被杀", 而不是真的内存泄漏。
//   这里接管 stderr 并每 5 秒 fdatasync 一次, 把脏页量级压到 KB 级。
//
// ★ 用法: --log-file /path/to/run.log
// ---------------------------------------------------------------------------
struct LogSink {
    int   fd = -1;
    FILE* fp = nullptr;

    bool open(const char* path) {
        fp = fopen(path, "a");
        if (!fp) {
            fprintf(stderr, "[日志] 打不开 %s: %s\n", path, strerror(errno));
            return false;
        }
        // 64KB 写缓冲: 太小会变成 write 系统调用风暴, 太大脏页又多
        static char buf[64 * 1024];
        setvbuf(fp, buf, _IOFBF, sizeof(buf));
        fflush(fp);
        fd = fileno(fp);
        // 接管 stderr: 之后所有 fprintf(stderr) 都进这个文件
        dup2(fd, STDERR_FILENO);
        // stderr 自身保持无缓冲, 由本文件统一控制落盘节奏
        setvbuf(stderr, nullptr, _IONBF, 0);
        fprintf(stderr, "[日志] 已接管 stderr -> %s (fd=%d, 64KB 缓冲, 每 5s fdatasync)\n",
                path, fd);
        return true;
    }

    // 定期把脏页刷到盘上(Page Cache 不膨胀)
    void sync() {
        if (fd >= 0) {
            fdatasync(fd);
        }
    }
};

// ---------------------------------------------------------------------------
// ★ RGA 目标缓冲必须留【余量行】—— 这是 2026-11-11 那次 heap 崩溃的真凶之一
//
// RK3588 的 RGA 在【缩放比例不是整数倍】时(例如 1920x1080 -> 640x640,
// sy = 1080/640 = 1.6875), 行缓冲里没写完的 1~2 行也会被刷进目标缓冲,
// 也就是往目标缓冲末尾【越界写 1~2 行】。目标缓冲如果按精确大小分配
// (比如 640*640*3 = 1228800 字节, 一字节余量都没有), 这 1~2 行就直接
// 踩坏后面的堆 —— 表现为
//     malloc(): invalid next size (unsorted)
// 而且落点随机: 有时崩在 malloc, 有时崩在 libavcodec, 有时崩在 cv::Mat。
//
// 实测: 目标缓冲后面挂 canary, RGA 跑一次就发现 +3840 字节(= 2 行)被写了。
// 修法: 所有【RGA 目标缓冲】都多分配 kRgaDstPadRows 行, 让溢出刷在余量里。
// (16 行足够覆盖实测到的 2 行, 代价只有几十 KB。)
constexpr int kRgaDstPadRows = 16;

static bool env_flag(const char* name) {
    const char* v = ::getenv(name);
    return v && v[0] && strcmp(v, "0") != 0;
}

// FFmpeg 的 av_err2str() 是宏, 返回 char[AV_ERROR_MAX_STRING_SIZE] 复合字面量,
// 不能直接作为变参传给 fprintf(会报 "taking address of temporary array")。
// 用 av_strerror() 写进本地 buffer 最保险, 各版本 FFmpeg 都兼容。
static void log_av_err(const char* what, int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(err, buf, sizeof(buf));
    fprintf(stderr, "%s: %s\n", what, buf);
}

static void warn_once(const char* key, const char* fmt, ...) {
    static std::set<std::string> warned;
    if (!warned.insert(key).second) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, fmt, ap);
    va_end(ap);
}

// ---------------------------------------------------------------------------
// 业务层分发器
//
// 推理线程只做"入队", 真正干活的(MQTT 发布 / 串口下发)在后台线程。
// 这是硬约束: 把阻塞 IO 放进推理线程会让 NPU 等 IO, 帧率直接崩。
// 队列满了就丢旧的帧 —— 宁可丢业务数据, 也不能拖慢视频流水线。
class ResultDispatcher {
public:
    ~ResultDispatcher() { stop(); }

    void add(std::unique_ptr<rk::ResultSink> s) { sinks_.push_back(std::move(s)); }
    bool empty_sinks() const { return sinks_.empty(); }

    void start() {
        if (th_.joinable()) return;
        th_ = std::thread([this]() {
            for (;;) {
                Frame f;
                {
                    std::unique_lock<std::mutex> lk(m_);
                    cv_.wait(lk, [this]() { return quit_ || !q_.empty(); });
                    if (quit_ && q_.empty()) break;
                    f = std::move(q_.front());
                    q_.pop_front();
                }
                for (auto& s : sinks_) {
                    try {
                        s->on_frame(f.idx, f.results);
                    } catch (const std::exception& e) {
                        fprintf(stderr, "[sink] 回调异常: %s\n", e.what());
                    }
                }
            }
        });
    }

    void push(int idx, std::vector<rk::DetectResult> rs) {
        {
            std::lock_guard<std::mutex> lk(m_);
            while (q_.size() >= 32) q_.pop_front();   // 消费不过来就丢旧的
            q_.push_back(Frame{idx, std::move(rs)});
        }
        cv_.notify_one();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(m_);
            quit_ = true;
        }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
        for (auto& s : sinks_) s->flush();
    }

private:
    struct Frame {
        int idx = 0;
        std::vector<rk::DetectResult> results;
    };
    std::vector<std::unique_ptr<rk::ResultSink>> sinks_;
    std::deque<Frame> q_;
    std::mutex m_;
    std::condition_variable cv_;
    std::thread th_;
    bool quit_ = false;
};

// ---------------------------------------------------------------------------
struct Options {
    std::string url;
    std::string model;
    std::string labels;
    int   model_w = 0, model_h = 0;
    float conf = 0.25f;
    float nms  = 0.60f;         // 模块 B: 注意不是默认的 0.70
    // 默认【拉伸缩放】(STRETCH), 与 AI 算法组板端参考实现一致。
    //
    // ★ 为什么不是 letterbox(算法组说"训练是 rect=false 所以必须 letterbox")?
    //   实测(RKNN 模拟器, 同一个 build, 9 张训练图):
    //       stretch  cls_max 均值 0.855
    //       letterbox cls_max 均值 0.855     —— 原图分辨率下两者等价
    //   但【监控画面里的小目标】就完全不是一回事(把训练图贴进 1920x1080 画布):
    //       目标占高 15%:  stretch 0.0076(0 框)  letterbox 0.0456(0 框)  —— 都检不出
    //       目标占高 25%:  stretch 0.7037(4 框)  letterbox 0.4488(2 框)  —— 拉伸更好
    //       目标占高 40%:  stretch 0.5895(7 框)  letterbox 0.2130(0 框)  —— 拉伸好得多
    //   原因: 1920x1080 做 letterbox 会先缩成 640x360 再上下补 140px 灰边,
    //   目标只占输入高度的 56%, 像素更少更难检出; 拉伸则让目标保住满幅高度。
    //   固定监控相机拍到的目标本来就小, 所以这里必须用拉伸。
    //
    //   --letterbox 保留, 供竖构图/目标很大的场景使用。
    bool  letterbox = false;
    bool  want_int8 = false;
    bool  show = true;
    int   max_frames = 0;
    // ---- 数据集采集 (--dump-frames N) ----
    int         npu_core = 0;                         // 检测模型的 NPU 核绑定(0=自动)
    int         npu_pipe = 1;                         // 检测并行度(1=串行单实例, 2=双实例双核并行)
    int         lpr_core = 0;                         // 车牌模型的 NPU 核绑定(0=自动)
    int         dump_frames = 0;                       // >0 -> 存 N 张原始帧(JPEG)
    std::string dump_dir = "dataset";                   // 输出目录
    // ---- 基准模式 (--benchmark N) ----
    int         benchmark_frames = 0;                    // >0 -> 跑基准
    std::string benchmark_images = "output_debug";        // 图片目录(默认整帧图)
    std::string benchmark_csv;                           // CSV 输出(追加)
    std::string log_file;      // --log-file: 自己开文件接管 stderr + 定期 fdatasync
    uint64_t    mem_trim_frames = 500;   // --mem-trim-frames: 每 N 帧 malloc_trim 一次(0=关)
    double      mem_trim_sec = 0.0;     // --mem-trim-sec: 每 N 秒 trim 一次(0=关)
    int   disp_max_w = 1280;

    // ---- 测速 ----
    bool        speed = true;          // 关掉就只做检测
    std::string ipm_points;            // "x,y;x,y;x,y;x,y"
    double      ipm_width_m = 0.0;     // 标定区横跨宽度(米)
    double      ipm_fov = 0.0;         // 水平 FOV(度), 0 = 由灭点反解焦距
    double      ipm_ppm = 100.0;       // 鸟瞰图 px/m
    std::string p1, p2;                // 单点比例尺模式
    double      real_m = 0.0;          // p1->p2 的真实长度(米)
    float       speed_limit = 25.0f;   // 超速阈值 km/h
    std::string estimator = "chord";   // chord | lsq
    float       bad_w_ratio = 0.70f;   // 模块 A: 框宽/画面宽 上限
    int         ground_median_k = 5;   // 模块 C: 地面点中值窗口(帧)
    double      speed_win = 0.80;      // 测速时间窗(秒)
    double      display_span = 0.80;   // 显示/判定滑动时长(秒)
    float       match_thresh = 0.80f;  // 关联 IoU 阈值(高速目标可能要调低)
    bool        geo_filter = true;    // 模块 A: 几何先验过滤开关
    std::string decoder = "auto";      // auto | mpp | ffmpeg

    // ---- LPRNet 车牌识别 ----
    std::string lprnet_model;          // 给了就启用
    std::string lprnet_charset;        // 34 个字符, 默认用内置
    std::string plate_cls;             // 哪些类别是车牌, 如 "0,2"; 空=不启用
    std::string track_cls;             // 哪些类参与跟踪/测速, 如 "1"(ebike); 空=全部
    float       lprnet_min_score = 0.30f;
    float       lprnet_pad = 0.10f;    // 车牌框外扩比例
    int         lprnet_ttl_ms = 2000;  // 同一条 track 的识别结果缓存时长
    std::string plate_prefix = "深圳";  // LPRNet 只输出数字/字母, Web 端要求带省份前缀
    bool        lprnet_feed_float = false; // false=int8量化码(默认), true=float32
    // 车牌图的两个可选预处理(默认关, 见下面 deskew_roi_rgb 的注释)
    bool        lprnet_invert = false; // 反白: 白字深色底(蓝牌/绿牌) -> 黑字白底
    bool        lprnet_deskew = false; // 纠偏: 把斜着的文字行摆正

    // ---- 车牌 ROI 几何先验(板端只有 ebike 车体框, 车牌在车尾=框的下部) ----
    float roi_ratio  = 0.72f;   // ROI 宽 / 车体框宽
    float roi_y      = 0.70f;   // ROI 顶边在框内的相对位置 (0=框顶, 1=框底)
    float roi_height = 0.16f;   // ROI 高 / 车体框高
    float roi_shift  = 0.00f;   // ROI 中心水平偏移 / 框宽 (正=偏右)
    float roi_below  = 0.05f;   // ROI 顶边在框底再往下延伸 / 框高

    // ---- 调试: 把送进 LPRNet 的 ROI 图存盘 ----
    // 目的: 肉眼确认模型到底裁到了什么(很多是废图), 以及 ROI 框画在哪。
    bool        save_roi = false;         // --save-roi 打开
    std::string save_roi_dir = "output_rois";  // ROI 图输出目录(默认 output_rois/)
    std::string save_roi_debug_dir = "output_debug";  // 整帧调试图目录
    int         save_roi_interval = 30;   // 整帧调试图每 N 帧存一张

    // ---- 静态背景过滤(广告牌之类的"鬼影") ----
    // 固定在画面里、永远不动的东西被模型当成车/人 -> track ID 一直挂着,
    // 每帧都送 LPRNet 白费算力, 偶尔还读出假车牌上报。
    bool        static_filter = false;    // 总开关
    int         static_window = 30;       // 看最近多少帧的位移
    float       static_px = 5.f;          // 窗口内最大位移小于它 -> 判为静止
    bool        static_discard = false;   // 判为静止后连检测结果都不上报

    // ---- 业务层输出 ----
    bool        sink_stdout = false;  // 打印每帧结果
    std::string mqtt_host;            // 非空则开 MQTT 上行
    int         mqtt_port = 1883;
    std::string mqtt_topic;           // 默认 device/<sn>/message/up/ivs_result
    std::string mqtt_client_id;
    int         mqtt_qos = 0;
    std::string serial_port;          // 非空则开串口(C5HE 控制卡)
    int         serial_baud = 9600;
    std::string speak_text;           // 固定播报文案(有车牌时用车牌代替)
    std::string display_text;         // 固定临显文案
    int         display_duration = 10;// 临显时长(秒)
    std::string sn = "rk3588-01";     // 设备序列号(拼 topic / 报文)
    std::string device_name = "0001";
    std::string ipaddr = "192.168.31.77";
};

static void usage(const char* prog) {
    fprintf(stderr,
            "用法: %s <rtsp_url> <model.rknn> [选项]\n"
            "  --size WxH      模型输入尺寸(默认从模型属性读取)\n"
            "  --conf 0.25     置信度阈值\n"
            "  --nms 0.60      NMS IoU 阈值(模块B: 不要用 0.70)\n"
            "  --labels f.txt  类别名文件(每行一个)\n"
            "  --letterbox     等比 letterbox(默认是拉伸缩放; 监控小目标用拉伸更好)\n"
            "  --stretch       显式指定拉伸缩放(默认)\n"
            "  --int8          取原始量化输出, 不让驱动转 float\n"
            "  --no-show       不显示窗口\n"
            "  --disp-width N  显示窗口最大宽度(默认 1280)\n"
            "  --frames N      处理 N 帧后退出\n"
            "\n"
            "  ── 测速(架构组 speed_stabilization_cpp.md) ──\n"
            "  --no-speed              只做检测, 不跟踪不测速\n"
            "  --ipm-points \"x,y;x,y;x,y;x,y\"  标定梯形 4 点(图像坐标)\n"
            "  --ipm-width-m 7.0       标定区横跨宽度(米)\n"
            "  --ipm-fov 84            水平 FOV(度); 不给则由两个灭点反解焦距\n"
            "  --ipm-ppm 100           鸟瞰图 px/m\n"
            "  --p1 \"x,y\" --p2 \"x,y\"   单点比例尺模式的标定线\n"
            "  --real-m 7.0            p1->p2 的真实长度(米)\n"
            "  --speed-limit 25        超速阈值 km/h\n"
            "  --estimator chord|lsq   速度估计器(默认 chord, 与 Python 一致)\n"
            "  --bad-w-ratio 0.70      模块A: 框宽/画面宽 上限(剔除巨型假框)\n"
            "  --ground-median 5       模块C: 地面点中值窗口(帧)\n"
            "  --match-thresh 0.80     关联 IoU 阈值(高速目标要调低, 否则会丢轨)\n"
            "  --track-cls 1           只有这些类参与跟踪/测速(逗号分隔的类别 id;\n"
            "                          空=全部都跟。person+ebike 模型务必填 1,\n"
            "                          否则人的脚会被当成地面点, 速度系统性偏高)\n"
            "  --no-geo-filter         关掉模块A的几何先验过滤(调试用)\n"
            "  --decoder auto|mpp|ffmpeg  解码后端: mpp=Rockchip MPP 硬解(推荐)\n"
            "  --mpp-diag              只打印 MPP 环境诊断然后退出(不碰 RTSP)\n"
            "\n"
            "  ── LPRNet 车牌识别 ──\n"
        "  --lprnet model.rknn     启用 LPRNet(160x48 uint8 RGB, 不要自己归一化)\n"
            "  --plate-cls 0,2         哪些类别当车牌(逗号分隔的类别 id)\n"
            "  --lprnet-charset \"0123...\"  字符集(默认内置 34 个)\n"
            "  --lprnet-min-score 0.30  车牌识别最低分\n"
            "  --lprnet-ttl 2000       同一条 track 的识别结果缓存时长(ms)\n"
            "  --lprnet-feed int8|float LPRNet 输入喂法(默认 int8 量化码; float 走 float32)\n"
            "  --lprnet-invert 0|1     车牌图反白(白字深色底 -> 黑字白底; 模型是按黑字白底训的)\n"
            "  --lprnet-deskew 0|1     车牌图纠偏(把斜着的文字行摆正; 近景侧拍时很关键)\n"
            "  --plate-prefix 深圳     车牌前缀(LPRNet 只出数字/字母, Web 端要带省份)\n"
            "  --save-roi              调试存图:\n"
            "                          output_rois/  = 送进 LPRNet 的 ROI 原图(每个 ROI 一张)\n"
            "                          output_debug/ = 原图 + 检测框 + ROI 框(每 30 帧一张)\n"
            "  --save-roi-dir DIR      换 ROI 输出目录(默认 output_rois)\n"
            "  --save-roi-debug-dir D  换调试图输出目录(默认 output_debug)\n"
            "  --save-roi-interval N   调试图每 N 帧存一张(默认 30)\n"
            "\n"
            "  ── 车牌 ROI 几何先验(板端只有 ebike 车体框) ──\n"
            "  --roi-ratio 0.72        ROI 宽 / 车体框宽\n"
            "  --roi-y 0.70            ROI 顶边在框内的位置(0=框顶, 1=框底)\n"
            "  --roi-height 0.16       ROI 高 / 车体框高\n"
            "  --roi-shift 0.00        ROI 水平偏移 / 框宽(正=偏右)\n"
            "  --roi-below 0.05        ROI 顶边在框底再往下延伸 / 框高\n"
            "  --lprnet-pad 0.0        ROI 再额外外扩的比例(一般用 --roi-* 调)\n"
            "\n"
            "  ── 业务层输出(默认全关) ──\n"
            "  --sink-stdout           每帧结果打到 stderr\n"
            "  --log-file PATH         日志落盘到文件(自己接管 stderr + 定期 fdatasync,\n"
            "                           避免 Page Cache 脏页堆积)\n"
            "  --mqtt-host 127.0.0.1  开启 MQTT 上行(臻识 ivs_result 协议)\n"
            "  --mqtt-port 1883\n"
            "  --mqtt-topic TOPIC      默认 device/<sn>/message/up/ivs_result\n"
            "  --mqtt-client-id ID     默认用 --sn\n"
            "  --mqtt-qos 0|1          默认 0\n"
            "  --serial-port /dev/ttyUSB0  开启串口控制卡(临显 + 语音播报)\n"
            "  --serial-baud 9600\n"
            "  --speak-text \"欢迎回家\"    固定播报文案(有车牌时用车牌代替)\n"
            "  --display-text \"业主车辆\"  固定临显文案\n"
            "  --display-duration 10    临显时长(秒)\n"
            "  --sn rk3588-01           设备序列号(拼 topic / 报文)\n"
            "  --device-name 0001\n"
            "  --ipaddr 192.168.31.77\n",
            prog);
}

static std::vector<std::string> load_labels(const std::string& path) {
    std::vector<std::string> out;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return out;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        if (!s.empty()) out.push_back(s);
    }
    fclose(f);
    return out;
}

// 解析 "x,y;x,y;x,y;x,y"
static bool parse_quad(const std::string& s, rk::Pt quad[4]) {
    int n = 0;
    size_t pos = 0;
    while (pos <= s.size() && n < 4) {
        size_t sep = s.find(';', pos);
        std::string item = (sep == std::string::npos) ? s.substr(pos) : s.substr(pos, sep - pos);
        double x = 0, y = 0;
        if (sscanf(item.c_str(), "%lf,%lf", &x, &y) != 2) return false;
        quad[n].x = x;
        quad[n].y = y;
        n++;
        if (sep == std::string::npos) break;
        pos = sep + 1;
    }
    return n == 4;
}

static bool parse_pt(const std::string& s, rk::Pt* p) {
    return sscanf(s.c_str(), "%lf,%lf", &p->x, &p->y) == 2;
}

static unsigned char* read_file(const char* path, size_t* out_len) {
    FILE* fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "打开模型失败: %s\n", path); return nullptr; }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    unsigned char* buf = (unsigned char*)malloc(size > 0 ? size : 1);
    if (!buf) { fclose(fp); return nullptr; }
    if (fread(buf, 1, size, fp) != (size_t)size) { free(buf); fclose(fp); return nullptr; }
    fclose(fp);
    *out_len = (size_t)size;
    return buf;
}

// ---------------------------------------------------------------------------
// NV12 源的形态
enum class Nv12Mode {
    kDmaBufFd,     // 拿到 dma-buf fd, RGA 直接 import
    kCpuPointer,   // 只有 CPU 映射地址, RGA 直接 DMA 读 (依然无 CPU 拷贝)
};

struct Nv12Info {
    int   fd = -1;
    void* ptr = nullptr;
    int   width = 0;
    int   height = 0;
    int   stride = 0;
    int   plane_h = 0;
    Nv12Mode mode = Nv12Mode::kCpuPointer;

    // ---- 色度信息(帧不一定��� NV12!) ----
    // FFmpeg 软解出来的是 yuvj420p: 【平面】YUV420(U 平面/V 平面分开)+
    // 【全范围】(Y 0~255)。按 NV12 去读颜色会整个错掉(整帧品红/绿),
    // 那种图上 LPRNet 永远认不出车牌。
    const uint8_t* u_plane = nullptr;   // 平面: U 平面; 半平面: UV 交织平面
    const uint8_t* v_plane = nullptr;   // 平面: V 平面
    int   uv_stride = 0;                // 色度行字节数
    bool  planar = false;               // true = 平面 YUV420
    bool  full_range = false;           // true = yuvj*(全范围)

    // ---- 高位深(10/12bit) ------------------------------------------------
    // 每采样 2 字节(小端), 降到 8bit 要右移 bit_shift 位(10bit->2, 12bit->4)。
    // 0 = 普通 8bit。
    int   bit_shift = 0;
};

// ---------------------------------------------------------------------------
// ★ 诊断模式(默认关闭): 把【软解出来的 CPU 帧】重排进一块 dma-buf,
//   再让 RGA 走 importbuffer_fd 路径。
//
//   为什么需要它:
//     MPP 硬解要打开 /dev/mpp_service, 那个节点默认 0600 root:root,
//     普通用户拿不到 -> MPP 起不来 -> 只能软解。
//     软解出来的是 "CPU 内存 + 平面 YUV420(yuvj420p)", RGA 既没有 fd 可以
//     import, 也描述不了平面格式, 于是只能退回 OpenCV 的 CPU 转换
//     (整帧拷贝, 而且慢)。
//     把帧重排成 NV12 塞进 dma-buf 之后, 就能把 "DMA-BUF fd -> RGA" 这一段
//     单独拎出来验证: 颜色对不对、耗时多少、比 CPU 路快多少。
//
//   ⚠ 这里【多了一次 CPU 重排拷贝】, 所以它不是零拷贝, 只是诊断手段。
//     真正的零拷贝必须靠 MPP 直接交出 fd(RK_RGA_STAGE 保持关闭即可)。
//
//   开关: 环境变量 RK_RGA_STAGE=1
// ---------------------------------------------------------------------------
struct StagingBuf {
    int      fd = -1;
    uint8_t* ptr = nullptr;
    size_t   cap = 0;
    bool ready() const { return fd >= 0 && ptr != nullptr; }
};

bool staging_enabled() {
    static const bool v = (getenv("RK_RGA_STAGE") != nullptr);
    return v;
}

// 把 nv 描述的帧重排成 NV12 写进 staging 缓冲;
// 成功后把 nv 改成 "dma-buf 源", 让 RGA 后面去 importbuffer_fd 它。
bool stage_frame_into_dmabuf(Nv12Info* nv, StagingBuf* sb) {
    if (!nv || !sb) return false;
    if (nv->width <= 0 || nv->height <= 0) return false;

    const size_t need = static_cast<size_t>(nv->width) * nv->height * 3 / 2;
    if (!sb->ready() || sb->cap < need) {
        if (sb->fd >= 0) { ::close(sb->fd); sb->fd = -1; }
        if (sb->ptr) { rk::dma_buf_unmap(sb->ptr, sb->cap); sb->ptr = nullptr; sb->cap = 0; }
        sb->fd = rk::dma_buf_alloc(need, true);
        if (sb->fd < 0) return false;
        sb->ptr = static_cast<uint8_t*>(rk::dma_buf_map(sb->fd, need));
        if (sb->ptr == MAP_FAILED) {
            ::close(sb->fd); sb->fd = -1; sb->ptr = nullptr; return false;
        }
        sb->cap = need;
        fprintf(stderr, "[RGA-Stage] 诊断模式: 软解帧重排进 dma-buf fd=%d (%s)\n",
                sb->fd, rk::dma_buf_backend());
    }

    rk::RgaImage src{};      // RgaImage 有默认成员初值, 不能 memset
    src.fd = -1;
    src.vir_addr = nv->ptr;
    src.width = nv->width;
    src.height = nv->height;
    src.wstride = nv->stride;
    src.hstride = nv->plane_h;
    src.format = RK_FORMAT_YCbCr_420_SP;
    src.u_plane = nv->u_plane;
    src.v_plane = nv->v_plane;
    src.uv_wstride = nv->uv_stride;
    src.planar_yuv = nv->planar;
    src.full_range = nv->full_range;

    if (rk::pack_yuv420_to_nv12(src, sb->ptr, sb->cap) != 0) return false;

    // 把 nv 改成 "dma-buf 源"
    nv->fd = sb->fd;
    nv->ptr = sb->ptr;
    nv->stride = nv->width;          // NV12 的 luma 一行 = width 字节
    nv->plane_h = nv->height;
    nv->planar = false;
    nv->full_range = false;          // pack 里已经把 full 折成 limited 了
    nv->uv_stride = nv->width;
    nv->u_plane = sb->ptr + static_cast<size_t>(nv->width) * nv->height;
    nv->v_plane = nullptr;
    nv->mode = Nv12Mode::kDmaBufFd;
    return true;
}

#if RK_HAVE_MPP
struct MppProbeCtx {
    const AVFrame* frame = nullptr;
    rk::rk_mpp_buffer_t buf = nullptr;
};

// 在 SafeProbe 的保护下运行: 这里如果把非 MPP_BUFFER 当成 MPP_BUFFER 解引用,
// 会段错误, 由 SafeProbe 捕获并返回 false, 调用方走降级路径。
static bool probe_mpp_opaque(void* p) {
    MppProbeCtx* c = static_cast<MppProbeCtx*>(p);
    const AVFrame* f = c->frame;
    if (!f || !f->buf[0]) return false;

    void* opaque = av_buffer_get_opaque(f->buf[0]);
    if (!rk::SafeProbe::plausible_ptr(opaque)) return false;

    rk::rk_mpp_buffer_t mpp_buf = reinterpret_cast<rk::rk_mpp_buffer_t>(opaque);
    // ⚠️ mpp_buffer_get_ptr 是【返回指针】的宏(只接受 1 个参数), 不是出参形式:
    //      #define mpp_buffer_get_ptr(buffer) mpp_buffer_get_ptr_with_caller(buffer, __FUNCTION__)
    //    写成 mpp_buffer_get_ptr(buf, &ptr) 会编译失败。
    //    这里解引用可能崩, 由 SafeProbe 兜住。
    void* ptr = mpp_buffer_get_ptr(mpp_buf);
    if (!ptr) return false;
    if (ptr != f->data[0]) return false;   // 不是这块缓冲 -> 放弃

    c->buf = mpp_buf;
    return true;
}

struct MppInitProbe {
    rk::MppDecoder* dec = nullptr;
    bool hevc = false;
    int  width = 0;
    int  height = 0;
    bool inited = false;   // init() 的返回值
};

// 在 SafeProbe 保护下调用 MppDecoder::init()。
//
// ★ 为什么必须包住:
//   本工程用的是旧版(20190626)用户态 MPP, 而板子(recomputer-rk3588-devkit,
//   Ubuntu 26.04 / 新内核)很可能自带一套 Rockchip BSP 版 MPP。三者版本对不上时
//   mpp_create() 会【直接段错误】把整个进程带走 —— 表现就是日志打到
//   "解码器: h264" 之后【一行都没有】, 连 "[解码] 后端 = ..." 都打不出来。
//   包住之后: 崩了也能退回软解, 并给出可读的结论, 至少软解链路能继续跑。
static bool probe_mpp_init(void* p) {
    MppInitProbe* a = static_cast<MppInitProbe*>(p);
    a->inited = a->dec->init(a->hevc, a->width, a->height);
    return true;   // 返回 true 只表示"没崩"; 真实结果看 a->inited
}
#endif  // RK_HAVE_MPP

static void extract_nv12(const AVFrame* f, bool try_mpp_opaque, Nv12Info& n) {
    n.ptr = f->data[0];
    n.stride = f->linesize[0];
    n.width = f->width;
    n.height = f->height;
    n.plane_h = n.height;
    n.fd = -1;
    // ---- 色度平面: 按真实的 pix_fmt 来, 不能一律当 NV12 ----
    const enum AVPixelFormat pf = (enum AVPixelFormat)f->format;
    if (pf == AV_PIX_FMT_NV12) {
        n.planar = false;
        n.u_plane = f->data[1];
        n.v_plane = nullptr;
        n.uv_stride = f->linesize[1] > 0 ? f->linesize[1] : f->linesize[0];
        n.full_range = false;
    } else if (pf == AV_PIX_FMT_YUV420P || pf == AV_PIX_FMT_YUVJ420P) {
        n.planar = true;
        n.u_plane = f->data[1];
        n.v_plane = f->data[2];
        n.uv_stride = f->linesize[1] > 0 ? f->linesize[1] : (f->width / 2);
        n.full_range = (pf == AV_PIX_FMT_YUVJ420P);
    } else if (pf == AV_PIX_FMT_YUV420P10LE || pf == AV_PIX_FMT_YUV420P12LE) {
        // ---- 高位深(10/12bit)平面 YUV420 --------------------------------
        // 每采样 2 字节小端, 右移 bit_shift 位就得到 8bit —— 转换层按这个读。
        //
        // ★ 实测踩过的坑(2026-11-11): DJI 那个 MP4 解出来就是 yuv420p10le。
        //   当成 8bit NV12 读的话颜色整个错掉(R 通道被压成 0, 画面发青),
        //   而 Y 平面看着"还挺正常" —— 极具迷惑性, 用 --save-roi 存图才看出来。
        n.planar = true;
        n.u_plane = f->data[1];
        n.v_plane = f->data[2];
        n.uv_stride = f->linesize[1] > 0 ? f->linesize[1] : (f->width);
        n.bit_shift = (pf == AV_PIX_FMT_YUV420P10LE) ? 2 : 4;
        // 全范围看 color_range(VUI 的 full_range 标志), 高位深没有独立的 yuvj 格式
        n.full_range = (f->color_range == AVCOL_RANGE_JPEG);
        static bool warned_hi = false;
        if (!warned_hi) {
            warned_hi = true;
            fprintf(stderr,
                    "[NV12] 高位深帧: %s (每采样 2 字节), 转 8bit 时右移 %d 位; "
                    "RGA 不支持这种布局, 走 CPU 转换\n",
                    av_get_pix_fmt_name(pf), n.bit_shift);
        }
    } else {
        // 不是 8bit / 高位深 YUV420 就没法按 NV12 处理(会整帧颜色错掉)
        //
        // ★ 实测踩过的坑(2026-11-11): DJI 那个 MP4 解出来是 yuv420p10le ——
        //   10bit, 每个采样占 2 字节(linesize[0]=3840 而不是 1920)。
        //   当成 8bit NV12 读的话, 颜色整个错掉(实测 R 通道被压成 0,
        //   画面发青), 而 Y 平面看着"还挺正常" —— 极具迷惑性。
        //   现在 yuv420p10le / yuv420p12le 已经在上面那个分支里正确处理了
        //   (每采样 2 字节, 右移 bit_shift 位降位深), 走到这里说明是
        //   真正没见过的格式(比如 yuv444p / rgb48)。用 --save-roi 存图
        //   能一眼看出来: 存出来的 ROI 是青绿色的废图。
        static bool warned_fmt = false;
        if (!warned_fmt) {
            warned_fmt = true;
            fprintf(stderr,
                    "[警告] 解码帧格式是 %s, 不是 8bit YUV420/NV12, 颜色可能不对\n"
                    "       帧 %dx%d linesize=[%d,%d,%d] (8bit 的话 linesize[0] 应该等于宽度)\n"
                    "       目前只支持 yuv420p / yuvj420p / nv12 / yuv420p10le / yuv420p12le\n",
                    av_get_pix_fmt_name(pf),
                    f->width, f->height, f->linesize[0], f->linesize[1], f->linesize[2]);
        }
        n.planar = false;
        n.u_plane = f->data[1];
        n.v_plane = nullptr;
        n.uv_stride = f->linesize[1] > 0 ? f->linesize[1] : f->linesize[0];
        n.full_range = false;
    }

    // NV12 的 UV plane 紧跟在 Y plane 之后。用两个 plane 的指针差反推 luma
    // plane 的真实行数, 比直接用 f->height 可靠 (解码器会把高度对齐到 16)。
    if (f->data[0] && f->data[1] && f->linesize[0] > 0) {
        ptrdiff_t off = static_cast<const uint8_t*>(f->data[1]) -
                        static_cast<const uint8_t*>(f->data[0]);
        if (off > 0 && off % f->linesize[0] == 0)
            n.plane_h = static_cast<int>(off / f->linesize[0]);
    }

    // ---- 拿 dma-buf fd, 三级降级, 任何一级失败都不会崩 ----
    //  1) opaque 里确实是 MPP_BUFFER -> mpp_buffer_get_fd()
    //  2) /proc/self/maps + fdinfo 反查地址对应的 dma-buf fd
    //  3) 都没有 -> 用 CPU 映射地址 (RGA 直接 DMA 读, 依然零 CPU 拷贝)
#if RK_HAVE_MPP
    if (try_mpp_opaque && f->buf[0] && !env_flag("RK_NO_MPP_OPAQUE")) {
        MppProbeCtx ctx;
        ctx.frame = f;
        if (rk::SafeProbe::call(&probe_mpp_opaque, &ctx)) {
            n.fd = rk::mpp_buffer_get_fd_compat(ctx.buf);
            if (n.fd < 0)
                warn_once("mpp_fd", "[NV12] MPP_BUFFER 有效但取不到 fd, 尝试 /proc 反查\n");
        } else {
            warn_once("mpp_opaque",
                      "[NV12] frame->buf[0] 的 opaque 不是 MPP_BUFFER(已安全探测, 未崩溃), "
                      "降级到 /proc 反查\n");
        }
    }
#endif
    if (n.fd < 0 && !env_flag("RK_NO_MPP_FD_LOOKUP")) {
        int fd = rk::find_dmabuf_fd_for_addr(f->data[0]);
        if (fd >= 0) {
            n.fd = fd;
            warn_once("fd_lookup", "[NV12] 通过 /proc 反查到 dma-buf fd=%d\n", fd);
        }
    }

    n.mode = (n.fd >= 0) ? Nv12Mode::kDmaBufFd : Nv12Mode::kCpuPointer;
    static bool mode_logged = false;
    if (!mode_logged) {
        mode_logged = true;
        fprintf(stderr, "[NV12] 数据源: %s\n",
                n.mode == Nv12Mode::kDmaBufFd ? "dma-buf fd (RGA import)" : "CPU 映射地址 (RGA DMA 直读)");
    }
}

// ---------------------------------------------------------------------------
// ROI 硬夹紧: 保证矩形完全落在 [0,fw) x [0,fh) 内。
//
// ★ 为什么必须多这一道:
//   1) plate_roi_from_box() 里用的是 lround() 取整, 取整误差会让
//      rx + rw 偶尔比 fw 大 1 像素;
//   2) --lprnet-pad 的外扩是在夹紧【之后】做的(外扩 10% 就能把
//      贴边的框顶出画面);
//   3) RGA 的 im_rect 是 int, 越界不会被检查, 只是画到画外。
//   任何一处越界 -> 裁剪读越界 -> 后面 cv::Mat 拿着垃圾 size -> OpenCV 抛
//   "-215: 0 <= roi.x && ... <= m.cols" -> terminate -> core dump。
//   所以这里做一次【最后一道】的整型硬夹紧, 夹完还太小就放弃这个目标。
// ---------------------------------------------------------------------------
// 车牌 ROI 的极性/倾角预处理(可选, 默认关)
//
// ★ 为什么需要这两个开关(2026-11-11 实测, 同一张绿牌 苏N A339058):
//
//   1) 【极性】lprnet_finetuned_int8 是在【黑字白底】的车牌图上训的
//      (合成图 test_plate.jpg 上能 100% 读对)。真实车牌是【白字深色底】
//      (蓝牌/绿牌), 直接喂进去基本读不出来:
//          原样裁剪   -> "H"      (conf 0.88)
//          反白之后   -> "A33909" (conf 0.87)
//      所以加了 --lprnet-invert。
//
//   2) 【倾角】车牌在画面里是斜的(相机侧拍/俯视), 文字行也是斜的,
//      直接拉成 160x48 会把字压扁, 首尾几个字符就废了:
//          不纠偏 -> "H4339"
//          纠 -3° -> "A33909"
//      所以加了 --lprnet-deskew。
//
// 两个开关默认都是关的 —— 保持算法组原来的喂法不变, 需要时再开。
// ---------------------------------------------------------------------------

// 在 160x48 的 RGB 缓冲上原地纠偏(把文字行摆正)。返回是否真的做了旋转。
static bool deskew_roi_rgb(uint8_t* rgb, int w, int h, float max_deg) {
    if (!rgb || w < 16 || h < 8) return false;
    cv::Mat src(h, w, CV_8UC3, rgb);
    cv::Mat gray, bin;
    cv::cvtColor(src, gray, cv::COLOR_RGB2GRAY);
    // 白字 -> 白前景(自适应阈值, 亮度变化大的地方才留)
    cv::adaptiveThreshold(gray, bin, 255, cv::ADAPTIVE_THRESH_MEAN_C,
                          cv::THRESH_BINARY, 15, -6);
    cv::Mat labels, stats, centroids;
    const int n = cv::connectedComponentsWithStats(bin, labels, stats, centroids, 8);
    int best = -1, best_area = 0;
    for (int i = 1; i < n; ++i) {
        const int a = stats.at<int>(i, cv::CC_STAT_AREA);
        if (a > best_area) { best_area = a; best = i; }
    }
    if (best < 0 || best_area < 25) return false;   // 没找到文字行, 不乱转
    std::vector<std::vector<cv::Point>> cnts;
    cv::findContours(labels == best, cnts, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    if (cnts.empty()) return false;
    cv::RotatedRect rr = cv::minAreaRect(cnts[0]);
    cv::Point2f p[4];
    rr.points(p);
    // 用相邻两点算边方向, 避免 minAreaRect 的 angle 在 OpenCV 各版本里语义不同
    float ang = std::atan2(p[1].y - p[0].y, p[1].x - p[0].x) * 180.f / static_cast<float>(CV_PI);
    if (ang > 45.f) ang -= 90.f;
    if (ang < -45.f) ang += 90.f;
    if (std::fabs(ang) < 0.7f || std::fabs(ang) > max_deg) return false;
    cv::Mat M = cv::getRotationMatrix2D(cv::Point2f(w * 0.5f, h * 0.5f), ang, 1.0);
    cv::Mat out;
    cv::warpAffine(src, out, M, cv::Size(w, h), cv::INTER_LINEAR, cv::BORDER_REPLICATE);
    out.copyTo(src);
    return true;
}

static bool clamp_roi(int* x, int* y, int* w, int* h, int fw, int fh,
                      int min_w = 1, int min_h = 1) {    if (!x || !y || !w || !h) return false;
    if (fw <= 0 || fh <= 0) return false;

    int rx = *x, ry = *y, rw = *w, rh = *h;
    if (rw < 0) rw = 0;
    if (rh < 0) rh = 0;

    // 左上角拉回画面内(同时把尺寸一起挪, 保持右边界)
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    // 右下角裁回画面内
    if (rx > fw) rx = fw;
    if (ry > fh) ry = fh;
    if (rx + rw > fw) rw = fw - rx;
    if (ry + rh > fh) rh = fh - ry;

    if (rw < min_w || rh < min_h) return false;   // 夹完没了 -> 放弃

    *x = rx; *y = ry; *w = rw; *h = rh;
    return true;
}

// ---------------------------------------------------------------------------
// 车牌 ROI 几何先验: 从"车体框(ebike)"切出"车牌区域"
//
// 板端只有电动车框, 没有车牌框 —— 车牌挂在车尾, 也就是框的下部。
// 所以 ROI 用【相对车体框】的比例来取, 而不是绝对像素:
//
//   水平: 宽度 = 框宽 * roi_ratio,  中心 = 框中心 + 框宽 * roi_shift
//   垂直: 顶边 = 框顶 + 框高 * roi_y,  再往下延伸 框高 * roi_below
//         高度 = 框高 * roi_height
//
// 全部可调(--roi-*), 不同机位/俯仰角只改比例, 不改代码。
// 返回 false 表示这个框切不出可用 ROI。
static bool plate_roi_from_box(const YoloBox& b, int fw, int fh,
                               float ratio, float roi_y, float height,
                               float shift, float below,
                               int* rx, int* ry, int* rw, int* rh) {
    const float bw = b.x2 - b.x1;
    const float bh = b.y2 - b.y1;
    if (bw < 4.f || bh < 4.f) return false;

    float w = bw * ratio;
    float h = bh * height;
    if (w < 8.f) w = 8.f;
    if (h < 6.f) h = 6.f;

    const float cx = 0.5f * (b.x1 + b.x2) + bw * shift;
    float left = cx - w * 0.5f;
    float top = b.y1 + bh * roi_y + bh * below;

    // 夹到画面内
    if (left < 0.f) { w += left; left = 0.f; }
    if (top < 0.f) { h += top; top = 0.f; }
    if (left + w > fw) w = static_cast<float>(fw) - left;
    if (top + h > fh) h = static_cast<float>(fh) - top;
    if (w < 8.f || h < 6.f) return false;

    *rx = static_cast<int>(lround(left));
    *ry = static_cast<int>(lround(top));
    *rw = static_cast<int>(lround(w));
    *rh = static_cast<int>(lround(h));
    // 最后一道整型硬夹紧(lround 的取整误差 + 贴边框都可能越界)
    return clamp_roi(rx, ry, rw, rh, fw, fh, 8, 6);
}

// ---------------------------------------------------------------------------
// --save-roi: 把送进 LPRNet 的 ROI 图存盘, 方便人眼看模型到底裁到了什么
//
// 存两类文件(都在 --save-roi-dir 目录下, 默认 output_rois/):
//   track_{id}_frame_{n}.jpg        —— ROI 原图(160x48, 就是喂给模型的那张)
//   track_{id}_frame_{n}_marked.jpg  —— 整帧原图 + 画上 ROI 矩形框, 方便定位
//
// ★ 颜色: 内部缓冲是 RGB(RGA/CPU 两条路都写 RGB), OpenCV 的 imwrite 认 BGR,
//   所以写盘前统一转一次, 否则看图会觉得红蓝反了。
// ★ 整帧缓存复用: 1920x1080x3 = 6MB, 每帧 malloc 一次会把缺页打满 ——
//   这里只申请一次, 之后反复覆盖。
// ---------------------------------------------------------------------------
struct RoiDumper {
    bool        enabled = false;
    std::string roi_dir = "output_rois";    // ROI 原图
    std::string debug_dir = "output_debug"; // 整帧 + 框
    int         debug_interval = 30;        // 整帧每 N 帧存一张
    bool        roi_dir_ready = false;
    bool        debug_dir_ready = false;

    std::vector<uint8_t> full_rgb;   // 整帧 RGB 缓存(调试那张用)
    int         full_w = 0;
    int         full_h = 0;

    static bool ensure_dir(const std::string& d, bool* ready) {
        if (*ready) return true;
        if (::mkdir(d.c_str(), 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "[ROI] 建目录 %s 失败: %s\n", d.c_str(), strerror(errno));
            return false;
        }
        *ready = true;
        return true;
    }

    // ---- ① ROI 原图 -> output_rois/ -------------------------------------
    // roi_rgb: 160*48 的 RGB 缓冲(就是喂给 LPRNet 的那张)
    void dump_roi(int track_id, int frame_idx,
                  const uint8_t* roi_rgb, int roi_w, int roi_h) {
        if (!enabled || !roi_rgb || roi_w <= 0 || roi_h <= 0) return;
        if (!ensure_dir(roi_dir, &roi_dir_ready)) return;

        char stem[128];
        snprintf(stem, sizeof(stem), "track_%d_frame_%d", track_id, frame_idx);

        // ★ 颜色: 内部缓冲是 RGB(RGA/CPU 两条路都写 RGB), OpenCV 的 imwrite
        //   认 BGR, 所以写盘前统一转一次, 否则看图会觉得红蓝反了。
        cv::Mat rgb(roi_h, roi_w, CV_8UC3, const_cast<uint8_t*>(roi_rgb));
        cv::Mat bgr;
        cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
        const std::string path = roi_dir + "/" + stem + ".jpg";
        if (!cv::imwrite(path, bgr)) {
            fprintf(stderr, "[ROI] 写 %s 失败\n", path.c_str());
        }
    }

    // ---- ② 整帧 + 检测框 + ROI 框 -> output_debug/ -----------------------
    // 每 debug_interval 帧存一张(整帧 1920x1080 的 RGB 拷贝很贵, 没必要每帧都存)。
    //
    // 检测框: 橙���, 带 类别/分数/track id
    // ROI 框: 绿色
    // 静止背景(模块 F 判出来的): 灰色 + STATIC 标注, 一眼能看出哪些是广告牌
    void dump_debug(int frame_idx, const rk::RgaImage& src,
                    const std::vector<rk::TrackResult>& tracks,
                    const std::vector<cv::Rect>& roi_rects,
                    const std::vector<std::string>& labels) {
        if (!enabled || debug_interval <= 0) return;
        if (frame_idx % debug_interval != 0) return;
        if (src.width <= 0 || src.height <= 0) return;
        if (!ensure_dir(debug_dir, &debug_dir_ready)) return;

        // 整帧用同一条转换链(NV12 -> RGB), 尺寸和源帧一致, 等于 1:1 拷贝。
        // 整帧缓存复用: 1920x1080x3 = 6MB, 每帧 malloc 一次会把缺页打满。
        const size_t need = static_cast<size_t>(src.width) * src.height * 3;
        if (full_w != src.width || full_h != src.height || full_rgb.size() < need) {
            full_rgb.assign(need, 0);
            full_w = src.width;
            full_h = src.height;
        }
        rk::RgaImage dst;
        dst.fd = -1;
        dst.vir_addr = full_rgb.data();
        dst.width = src.width;
        dst.height = src.height;
        dst.wstride = src.width * 3;
        dst.hstride = src.height;
        dst.format = RK_FORMAT_RGB_888;

        if (rk::rga_nv12_to_rgb888(src, dst, /*letterbox=*/false,
                                   nullptr, nullptr, nullptr) != 0) {
            fprintf(stderr, "[ROI] 整帧转 RGB 失败, 跳过调试图\n");
            return;
        }

        cv::Mat rgb(src.height, src.width, CV_8UC3, full_rgb.data());
        cv::Mat bgr;
        cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);

        // ---- 检测框 ----
        for (const auto& tr : tracks) {
            const cv::Rect r(static_cast<int>(tr.box.x1), static_cast<int>(tr.box.y1),
                             static_cast<int>(tr.box.x2 - tr.box.x1),
                             static_cast<int>(tr.box.y2 - tr.box.y1));
            if (r.width <= 0 || r.height <= 0) continue;
            const cv::Scalar color = tr.static_bg ? cv::Scalar(160, 160, 160)
                                                   : cv::Scalar(0, 165, 255);
            cv::rectangle(bgr, r, color, 2);
            std::string name = (static_cast<size_t>(tr.box.cls_id) < labels.size())
                                   ? labels[tr.box.cls_id]
                                   : std::to_string(tr.box.cls_id);
            char txt[160];
            snprintf(txt, sizeof(txt), "%s id=%d %.2f%s", name.c_str(), tr.track_id,
                     tr.score, tr.static_bg ? " STATIC" : "");
            cv::putText(bgr, txt, cv::Point(r.x, std::max(16, r.y - 6)),
                        cv::FONT_HERSHEY_SIMPLEX, 0.6, color, 2);
        }

        // ---- ROI 框 ----
        for (const auto& r : roi_rects) {
            if (r.width <= 0 || r.height <= 0) continue;
            cv::rectangle(bgr, r, cv::Scalar(0, 255, 0), 2);
        }

        char name[64];
        snprintf(name, sizeof(name), "frame_%06d.jpg", frame_idx);
        const std::string path = debug_dir + "/" + name;
        if (!cv::imwrite(path, bgr)) {
            fprintf(stderr, "[ROI] 写 %s 失败\n", path.c_str());
        } else {
            fprintf(stderr, "[ROI] 调试图 %s (检测 %zu 个, ROI %zu 个)\n",
                    path.c_str(), tracks.size(), roi_rects.size());
        }
    }
};

// ---------------------------------------------------------------------------
static const AVCodec* pick_decoder(AVCodecID id, bool* is_rkmpp) {
    const char* name = nullptr;
    switch (id) {
        case AV_CODEC_ID_H264: name = "h264_rkmpp"; break;
        case AV_CODEC_ID_HEVC: name = "h265_rkmpp"; break;
        default: break;
    }
    *is_rkmpp = false;
    if (name) {
        const AVCodec* c = avcodec_find_decoder_by_name(name);
        if (c) { *is_rkmpp = true; return c; }
        fprintf(stderr, "[警告] 没有找到 %s 解码器, 退回软件解码\n", name);
    }
    return avcodec_find_decoder(id);
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    // ★ --mpp-diag 可以单独用(不需要 <url> <model> 两个位置参数)。
    //   必须在碰 argv[1] / argv[2] 之前处理 —— 越界读 argv[2] 会直接段错误(这坑踩过)。
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--mpp-diag") == 0) {
            rk::diag_mpp_environment("手动诊断");
            fprintf(stderr, "\n[提示] 把上面这段贴回来即可定位 MPP 硬解问题。\n");
            fprintf(stderr, "[提示] 想先确认软解链路是否正常: 加 --decoder ffmpeg 跑。\n");
            return 0;
        }
    }
    if (argc < 3) { usage(argv[0]); return 1; }

    Options opt;
    opt.url = argv[1];
    opt.model = argv[2];
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--size" && i + 1 < argc) {
            sscanf(argv[++i], "%dx%d", &opt.model_w, &opt.model_h);
        } else if (a == "--conf" && i + 1 < argc) {
            opt.conf = atof(argv[++i]);
        } else if (a == "--nms" && i + 1 < argc) {
            opt.nms = atof(argv[++i]);
        } else if (a == "--labels" && i + 1 < argc) {
            opt.labels = argv[++i];
        } else if (a == "--letterbox") {
            opt.letterbox = true;
        } else if (a == "--stretch") {
            opt.letterbox = false;
        } else if (a == "--int8") {
            opt.want_int8 = true;
        } else if (a == "--no-show") {
            opt.show = false;
        } else if (a == "--disp-width" && i + 1 < argc) {
            opt.disp_max_w = atoi(argv[++i]);
        } else if (a == "--frames" && i + 1 < argc) {
            opt.max_frames = atoi(argv[++i]);
        } else if (a == "--no-speed") {
            opt.speed = false;
        } else if (a == "--ipm-points" && i + 1 < argc) {
            opt.ipm_points = argv[++i];
        } else if (a == "--ipm-width-m" && i + 1 < argc) {
            opt.ipm_width_m = atof(argv[++i]);
        } else if (a == "--ipm-fov" && i + 1 < argc) {
            opt.ipm_fov = atof(argv[++i]);
        } else if (a == "--ipm-ppm" && i + 1 < argc) {
            opt.ipm_ppm = atof(argv[++i]);
        } else if (a == "--p1" && i + 1 < argc) {
            opt.p1 = argv[++i];
        } else if (a == "--p2" && i + 1 < argc) {
            opt.p2 = argv[++i];
        } else if (a == "--real-m" && i + 1 < argc) {
            opt.real_m = atof(argv[++i]);
        } else if (a == "--speed-limit" && i + 1 < argc) {
            opt.speed_limit = atof(argv[++i]);
        } else if (a == "--estimator" && i + 1 < argc) {
            opt.estimator = argv[++i];
        } else if (a == "--bad-w-ratio" && i + 1 < argc) {
            opt.bad_w_ratio = atof(argv[++i]);
        } else if (a == "--ground-median" && i + 1 < argc) {
            opt.ground_median_k = atoi(argv[++i]);
        } else if (a == "--match-thresh" && i + 1 < argc) {
            opt.match_thresh = atof(argv[++i]);
        } else if (a == "--no-geo-filter") {
            opt.geo_filter = false;
        } else if (a == "--npu-pipe" && i + 1 < argc) {
            opt.npu_pipe = atoi(argv[++i]);
        } else if (a == "--npu-core" && i + 1 < argc) {
            opt.npu_core = atoi(argv[++i]);
        } else if (a == "--lpr-core" && i + 1 < argc) {
            opt.lpr_core = atoi(argv[++i]);
        } else if (a == "--dump-frames" && i + 1 < argc) {
            opt.dump_frames = atoi(argv[++i]);
        } else if (a == "--dump-dir" && i + 1 < argc) {
            opt.dump_dir = argv[++i];
        } else if (a == "--benchmark" && i + 1 < argc) {
            opt.benchmark_frames = atoi(argv[++i]);
            if (opt.benchmark_frames > 0 && opt.max_frames <= 0) {
                // 阶段 A(视频)先跑一小段, 够测解码耗时就行
                opt.max_frames = std::min(opt.benchmark_frames, 300);
            }
        } else if (a == "--benchmark-images" && i + 1 < argc) {
            opt.benchmark_images = argv[++i];
        } else if (a == "--benchmark-csv" && i + 1 < argc) {
            opt.benchmark_csv = argv[++i];
        } else if (a == "--decoder" && i + 1 < argc) {
            opt.decoder = argv[++i];
        } else if (a == "--lprnet" && i + 1 < argc) {
            opt.lprnet_model = argv[++i];
        } else if (a == "--plate-cls" && i + 1 < argc) {
            opt.plate_cls = argv[++i];
        } else if (a == "--track-cls" && i + 1 < argc) {
            opt.track_cls = argv[++i];
        } else if (a == "--lprnet-charset" && i + 1 < argc) {
            opt.lprnet_charset = argv[++i];
        } else if (a == "--lprnet-min-score" && i + 1 < argc) {
            opt.lprnet_min_score = atof(argv[++i]);
        } else if (a == "--lprnet-pad" && i + 1 < argc) {
            opt.lprnet_pad = atof(argv[++i]);
        } else if (a == "--lprnet-ttl" && i + 1 < argc) {
            opt.lprnet_ttl_ms = atoi(argv[++i]);
        } else if (a == "--lprnet-invert" && i + 1 < argc) {
            const char* m = argv[++i];
            opt.lprnet_invert = (strcmp(m, "1") == 0 || strcmp(m, "true") == 0 ||
                                strcmp(m, "on") == 0 || strcmp(m, "yes") == 0);
        } else if (a == "--lprnet-deskew" && i + 1 < argc) {
            const char* m = argv[++i];
            opt.lprnet_deskew = (strcmp(m, "1") == 0 || strcmp(m, "true") == 0 ||
                                strcmp(m, "on") == 0 || strcmp(m, "yes") == 0);
        } else if (a == "--lprnet-feed" && i + 1 < argc) {
            // 输入喂法: int8(默认, 按模型 zp/scale 生成量化码) | float(float32)
            const std::string m = argv[++i];
            opt.lprnet_feed_float = (m == "float" || m == "float32");
        } else if (a == "--plate-prefix" && i + 1 < argc) {
            opt.plate_prefix = argv[++i];
        } else if (a == "--save-roi") {
            opt.save_roi = true;
        } else if (a == "--save-roi-dir" && i + 1 < argc) {
            opt.save_roi_dir = argv[++i];
            opt.save_roi = true;      // 给了目录就默认打开
        } else if (a == "--save-roi-debug-dir" && i + 1 < argc) {
            opt.save_roi_debug_dir = argv[++i];
            opt.save_roi = true;
        } else if (a == "--save-roi-interval" && i + 1 < argc) {
            opt.save_roi_interval = atoi(argv[++i]);
            opt.save_roi = true;
        } else if (a == "--static-filter") {
            opt.static_filter = true;
        } else if (a == "--static-window" && i + 1 < argc) {
            opt.static_window = atoi(argv[++i]);
            opt.static_filter = true;
        } else if (a == "--static-px" && i + 1 < argc) {
            opt.static_px = static_cast<float>(atof(argv[++i]));
            opt.static_filter = true;
        } else if (a == "--static-discard") {
            opt.static_discard = true;
            opt.static_filter = true;
        } else if (a == "--roi-ratio" && i + 1 < argc) {
            opt.roi_ratio = atof(argv[++i]);
        } else if (a == "--roi-y" && i + 1 < argc) {
            opt.roi_y = atof(argv[++i]);
        } else if (a == "--roi-height" && i + 1 < argc) {
            opt.roi_height = atof(argv[++i]);
        } else if (a == "--roi-shift" && i + 1 < argc) {
            opt.roi_shift = atof(argv[++i]);
        } else if (a == "--roi-below" && i + 1 < argc) {
            opt.roi_below = atof(argv[++i]);
        } else if (a == "--sink-stdout") {
            opt.sink_stdout = true;
        } else if (a == "--log-file" && i + 1 < argc) {
            opt.log_file = argv[++i];
        } else if (a == "--mem-trim-frames" && i + 1 < argc) {
            opt.mem_trim_frames = strtoull(argv[++i], nullptr, 10);
        } else if (a == "--mem-trim-sec" && i + 1 < argc) {
            opt.mem_trim_sec = atof(argv[++i]);
        } else if (a == "--mqtt-host" && i + 1 < argc) {
            opt.mqtt_host = argv[++i];
        } else if (a == "--mqtt-port" && i + 1 < argc) {
            opt.mqtt_port = atoi(argv[++i]);
        } else if (a == "--mqtt-topic" && i + 1 < argc) {
            opt.mqtt_topic = argv[++i];
        } else if (a == "--mqtt-client-id" && i + 1 < argc) {
            opt.mqtt_client_id = argv[++i];
        } else if (a == "--mqtt-qos" && i + 1 < argc) {
            opt.mqtt_qos = atoi(argv[++i]);
        } else if (a == "--serial-port" && i + 1 < argc) {
            opt.serial_port = argv[++i];
        } else if (a == "--serial-baud" && i + 1 < argc) {
            opt.serial_baud = atoi(argv[++i]);
        } else if (a == "--speak-text" && i + 1 < argc) {
            opt.speak_text = argv[++i];
        } else if (a == "--display-text" && i + 1 < argc) {
            opt.display_text = argv[++i];
        } else if (a == "--display-duration" && i + 1 < argc) {
            opt.display_duration = atoi(argv[++i]);
        } else if (a == "--sn" && i + 1 < argc) {
            opt.sn = argv[++i];
        } else if (a == "--device-name" && i + 1 < argc) {
            opt.device_name = argv[++i];
        } else if (a == "--ipaddr" && i + 1 < argc) {
            opt.ipaddr = argv[++i];
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    // MQTT topic / client id 默认跟 sn 走
    if (opt.mqtt_topic.empty() && !opt.mqtt_host.empty())
        opt.mqtt_topic = "device/" + opt.sn + "/message/up/ivs_result";
    if (opt.mqtt_client_id.empty()) opt.mqtt_client_id = opt.sn;

    std::vector<std::string> labels = load_labels(opt.labels);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    av_log_set_level(AV_LOG_ERROR);
    avformat_network_init();

    // =====================================================================
    // 1. 拉 RTSP 流
    // =====================================================================
    AVFormatContext* fmt = nullptr;
    AVDictionary* dopts = nullptr;
    av_dict_set(&dopts, "rtsp_transport", "tcp", 0);   // TCP 避免 UDP 丢包
    av_dict_set(&dopts, "stimeout", "5000000", 0);      // socket 超时 5s (us)
    av_dict_set(&dopts, "max_delay", "500000", 0);       // 抖动缓冲 0.5s (us)
    av_dict_set(&dopts, "fflags", "nobuffer", 0);
    av_dict_set(&dopts, "avioflags", "direct", 0);
    int ret = avformat_open_input(&fmt, opt.url.c_str(), nullptr, &dopts);
    av_dict_free(&dopts);
    if (ret < 0) {
        log_av_err("打开 RTSP 失败", ret);
        return 1;
    }
    ret = avformat_find_stream_info(fmt, nullptr);
    if (ret < 0) {
        log_av_err("读取流信息失败", ret);
        return 1;
    }
    int vstream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vstream < 0) {
        fprintf(stderr, "没有找到视频流\n");
        return 1;
    }
    AVStream* vst = fmt->streams[vstream];
    fprintf(stderr, "视频流: %dx%d  fps=%.2f  codec=%s\n", vst->codecpar->width,
            vst->codecpar->height,
            (av_q2d(vst->avg_frame_rate) > 0.1 && av_q2d(vst->avg_frame_rate) < 240.0)
                ? av_q2d(vst->avg_frame_rate) : av_q2d(vst->r_frame_rate),
            avcodec_get_name(vst->codecpar->codec_id));
    // 标称源帧率(后面和实测取帧间隔对照用)
    // ⚠️ RTSP 的 avg_frame_rate 有时是 0/0 -> av_q2d 给 NaN, 打印出来是 "fps=nan"。
    //   这种情况退回 r_frame_rate, 再不行就报 0(表示"未知", 别拿 NaN 参与比较)。
    double src_nominal_fps = av_q2d(vst->avg_frame_rate);
    if (!(src_nominal_fps > 0.1 && src_nominal_fps < 240.0)) {
        src_nominal_fps = av_q2d(vst->r_frame_rate);
    }
    if (!(src_nominal_fps > 0.1 && src_nominal_fps < 240.0)) src_nominal_fps = 0.0;

    // =====================================================================
    // 2. 硬解码器
    // =====================================================================
    bool use_rkmpp = false;
    const AVCodec* dec = pick_decoder(vst->codecpar->codec_id, &use_rkmpp);
    if (!dec) {
        fprintf(stderr, "没有可用的解码器 (codec=%s)\n",
                avcodec_get_name(vst->codecpar->codec_id));
        return 1;
    }
    AVCodecContext* vctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(vctx, vst->codecpar);
    vctx->thread_count = 1;                    // 多线程解码会引入帧拷贝
    vctx->flags |= AV_CODEC_FLAG_LOW_DELAY;    // 低延迟
    ret = avcodec_open2(vctx, dec, nullptr);
    if (ret < 0) {
        fprintf(stderr, "打开解码器失败(%s): ", dec->name); log_av_err("", ret);
        return 1;
    }
    fprintf(stderr, "解码器: %s%s\n", dec->name, use_rkmpp ? "  (RKMPP 硬解)" : "");

    if (vctx->extradata_size >= 4) {
        const uint8_t* e = vctx->extradata;
        bool annexb = (e[0] == 0 && e[1] == 0 && (e[2] == 1 || (e[2] == 0 && e[3] == 1)));
        if (!annexb)
            fprintf(stderr, "[警告] extradata 不是 Annex-B, 部分 rkmpp 版本会解析不出 SPS\n");
    }

    // =====================================================================
    // 2.5 解码后端: MPP 硬解 / FFmpeg
    //
    // 系统自带的 FFmpeg 没有 rkmpp 解码器, 只能软解; 而 librockchip-mpp 的
    // .deb 是能离线装上的 —— 所以这里让 MPP 自己做解码, FFmpeg 只负责拉流。
    // =====================================================================
    rk::MppDecoder mpp_dec;
    bool use_mpp_decoder = false;
    int64_t last_mpp_recreate_ms = 0;   // 引爆开关用: 上次重建的时刻
    int decoy_fd = -1;                  // 引爆开关用: 占住释放 fd 号的小 dma-buf
    {
        const AVCodecID cid = vst->codecpar->codec_id;
        (void)cid;
#if RK_USE_MPP
        if (opt.decoder == "mpp" || opt.decoder == "auto") {
            if (cid == AV_CODEC_ID_H264 || cid == AV_CODEC_ID_HEVC) {
                const bool hevc = (cid == AV_CODEC_ID_HEVC);
                MppInitProbe pa;
                pa.dec = &mpp_dec;
                pa.hevc = hevc;
                pa.width = vst->codecpar->width;
                pa.height = vst->codecpar->height;

                // ★ 用 SafeProbe 包住: MPP 版本不匹配时 mpp_create() 会段错误,
                //   不包的话整个进程直接死, 后面什么都跑不了。
                const bool crashed = !rk::SafeProbe::call(&probe_mpp_init, &pa);
                if (crashed) {
                    fprintf(stderr,
                            "[警告] MPP 初始化过程中崩溃(SIGSEGV/SIGBUS 已捕获), 退回软解\n"
                            "       十有八九是【板子自带的新版 MPP】与工程里的旧版(20190626)\n"
                            "       头文件/库版本不匹配。硬解要改用板子自带的 MPP 重编。\n"
                            "       先跑 --mpp-diag 看清楚运行时加载的是哪个 .so。\n");
                    // ⚠️ 这里【不要】调 mpp_dec.destroy(): 崩溃后 MPP 内部状态不确定,
                    //    destroy() 可能二次崩溃或死锁。ctx_ 本来就是 nullptr
                    //    (init 只在完全成功时才赋值), 泄漏一点句柄可以接受。
                } else if (pa.inited) {
                    use_mpp_decoder = true;
                    fprintf(stderr, "[解码] 后端 = Rockchip MPP 硬解 (%s, VPU)\n",
                            hevc ? "H.265" : "H.264");
                } else {
                    fprintf(stderr, "[警告] MPP 初始化失败: %s\n", mpp_dec.last_error());
                }
            } else {
                fprintf(stderr, "[警告] MPP 只支持 H.264/H.265, 当前流是 %s\n",
                        avcodec_get_name(cid));
            }
        }
#else
        if (opt.decoder == "mpp")
            fprintf(stderr, "[警告] 编译时没有 MPP 支持(RK_USE_MPP=0), 无法用 MPP 硬解\n");
#endif
        if (!use_mpp_decoder) {
            fprintf(stderr, "[解码] 后端 = FFmpeg %s%s\n", dec->name,
                    use_rkmpp ? " (RKMPP)" : "  <== 软解(系统 ffmpeg 无 rkmpp 解码器)");
        }
    }

    // =====================================================================
    // 2.6 MP4(AVCC) 源: 挂 mp4toannexb 位流过滤器
    //
    // 为什么必须加这一步(踩过的坑, 值得写清楚):
    //   MPP 只认 Annex-B(00 00 00 01 起始码)。
    //   * RTSP 推的 H.264/H.265 天生是 Annex-B, SPS/PPS 带内跟着关键帧来
    //     -> MPP 自己就能解析, 所以生产链路一直没问题。
    //   * MP4 容器不一样: SPS/PPS 单独放在 extradata 里, 格式是 avcC/hvcC
    //     【配置记录】(不是起始码), 每个 packet 也是【长度前缀】格式。
    //     直接喂 MPP -> 找不到起始码 -> 解析不出 SPS -> 永远解不出帧。
    //     表现极具迷惑性: "视频流: 1920x1080 codec=hevc" 打印得完全正常,
    //     但一行 "[MPP] 硬解出帧" 都没有, 最后以 "读取结束: End of file"
    //     收场 —— 所有包都读完了, 却一帧都没出来, 很容易被误判成
    //     "视频文件坏了" 或者 "MPP 不支持这个编码"。
    //
    // 解法: 用 FFmpeg 自带的 mp4toannexb 位流过滤器, 它同时干两件事:
    //   1) 把 avcC/hvcC 里的 SPS/PPS 取出来, 转成 Annex-B 插到第一个关键帧前面
    //   2) 把每个 length-prefix 的 packet 转成 Annex-B
    //   —— 正好是 MPP 需要的形式。
    //
    // ★ RTSP 源【走不到这个分支】(extradata 已经是 Annex-B), 行为一个字节都不变,
    //   所以不影响已经通过 9.44 小时长跑的���产链路。
    // =====================================================================
    AVBSFContext* mp4_bsf = nullptr;
    if (use_mpp_decoder && vst->codecpar->extradata_size >= 4) {
        const uint8_t* e = vst->codecpar->extradata;
        const bool src_is_annexb = (e[0] == 0 && e[1] == 0 &&
                                    (e[2] == 1 || (e[2] == 0 && e[3] == 1)));
        if (!src_is_annexb) {
            const bool is_hevc = (vst->codecpar->codec_id == AV_CODEC_ID_HEVC);
            const char* bsf_name = is_hevc ? "hevc_mp4toannexb" : "h264_mp4toannexb";
            const AVBitStreamFilter* bsf = av_bsf_get_by_name(bsf_name);
            if (bsf) {
                // FFmpeg 5.0+ 的 bsf API: av_bsf_alloc() 返回错误码,
                // 参数要填进 ctx->par_in, 然后 av_bsf_init(ctx) 不再收参数。
                ret = av_bsf_alloc(bsf, &mp4_bsf);
                if (ret < 0) {
                    mp4_bsf = nullptr;
                } else {
                    // bsf 需要一份自己的 codecpar 副本(不能直接用 vst->codecpar 的指针)
                    avcodec_parameters_copy(mp4_bsf->par_in, vst->codecpar);
                    ret = av_bsf_init(mp4_bsf);
                }
                if (ret < 0) {
                    av_bsf_free(&mp4_bsf);
                    mp4_bsf = nullptr;
                    log_av_err("初始化 mp4toannexb 失败", ret);
                } else {
                    fprintf(stderr, "[解码] MP4(AVCC) 源: 已启用 %s 过滤器, 转成 Annex-B 再喂 MPP\n",
                            bsf_name);
                    // ★ 把参数集(SPS/PPS/VPS)单独喂一次:
                    //   mp4toannexb 把它们放在 par_out->extradata 里(已经是 Annex-B),
                    //   但【不会】自动内嵌到数据包中 —— 而 MPP 必须要 SPS 才知道
                    //   宽高, 没有它就永远停在"信息变更"之前, 一帧都出不来。
                    //   RTSP 源是带内传 SPS 的, 所以走不到这一步。
                    if (mp4_bsf->par_out->extradata_size > 0) {
                        rk::Nv12Frame tmp_fi;
                        // out_frame 传 nullptr: 这一帧由 decode() 内部释放
                        mpp_dec.decode(mp4_bsf->par_out->extradata,
                                       mp4_bsf->par_out->extradata_size, 0, &tmp_fi, nullptr);
                        fprintf(stderr, "[解码] 已把 SPS/PPS(%d 字节) 喂给 MPP\n",
                                mp4_bsf->par_out->extradata_size);
                    }
                }
            } else {
                fprintf(stderr, "[警告] 找不到 %s 过滤器, MP4 源可能解不出帧\n", bsf_name);
            }
        }
    }

    // =====================================================================
    // 3. RKNN 初始化 (与 AI 算法组 rknn_yolov8_infer.cc 保持一致)
    // =====================================================================
    size_t model_size = 0;
    unsigned char* model_data = read_file(opt.model.c_str(), &model_size);
    if (!model_data) return 1;

    rknn_context rctx = 0;
    ret = rknn_init(&rctx, model_data, static_cast<uint32_t>(model_size),
                    RKNN_FLAG_PRIOR_HIGH, nullptr);
    free(model_data);
    if (ret < 0) {
        fprintf(stderr, "rknn_init 失败: %d (%s)\n", ret, opt.model.c_str());
        return 1;
    }
    rknn_sdk_version ver;
    memset(&ver, 0, sizeof(ver));
    // ★ NPU 核绑定(检测模型): 1=CORE0 2=CORE1 4=CORE2 3=CORE0+1 7=三核, 0=自动
    if (opt.npu_core > 0) {
        const int cret = rknn_set_core_mask(rctx, static_cast<rknn_core_mask>(opt.npu_core));
        fprintf(stderr, "[NPU] 检测模型核绑定 mask=0x%x -> %d\n", opt.npu_core, cret);
    }
    rknn_query(rctx, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver));
    fprintf(stderr, "RKNN api=%s drv=%s\n", ver.api_version, ver.drv_version);

    rknn_input_output_num io_num;
    memset(&io_num, 0, sizeof(io_num));
    rknn_query(rctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (io_num.n_output == 0) {
        fprintf(stderr, "模型没有输出张量\n");
        rknn_destroy(rctx);
        return 1;
    }

    // 逻辑输入属性(对外 NCHW 视图)
    rknn_tensor_attr attr_logic;
    memset(&attr_logic, 0, sizeof(attr_logic));
    attr_logic.index = 0;
    rknn_query(rctx, RKNN_QUERY_INPUT_ATTR, &attr_logic, sizeof(attr_logic));

    // 原生 NHWC 输入属性(NPU 内部真正吃的布局) —— 真零拷贝要用它
    rknn_tensor_attr attr_native;
    memset(&attr_native, 0, sizeof(attr_native));
    attr_native.index = 0;
    bool use_native = false;
    if (rknn_query(rctx, RKNN_QUERY_NATIVE_NHWC_INPUT_ATTR, &attr_native,
                    sizeof(attr_native)) == 0 &&
        attr_native.type == RKNN_TENSOR_UINT8 && attr_native.fmt == RKNN_TENSOR_NHWC &&
        attr_native.n_dims == 4 && attr_native.dims[3] == 3) {
        use_native = true;
    }

    // 每个输出的属性: shape + 量化参数
    std::vector<rknn_tensor_attr> out_attrs(io_num.n_output);
    for (uint32_t i = 0; i < io_num.n_output; i++) {
        memset(&out_attrs[i], 0, sizeof(rknn_tensor_attr));
        out_attrs[i].index = i;
        rknn_query(rctx, RKNN_QUERY_OUTPUT_ATTR, &out_attrs[i], sizeof(out_attrs[i]));
    }

    int model_w = opt.model_w, model_h = opt.model_h;
    if (model_w <= 0 || model_h <= 0) {
        if (use_native) {
            model_h = static_cast<int>(attr_native.dims[1]);
            model_w = static_cast<int>(attr_native.dims[2]);
        } else if (attr_logic.fmt == RKNN_TENSOR_NHWC && attr_logic.n_dims == 4) {
            model_h = static_cast<int>(attr_logic.dims[1]);
            model_w = static_cast<int>(attr_logic.dims[2]);
        } else if (attr_logic.n_dims == 4) {
            model_h = static_cast<int>(attr_logic.dims[2]);
            model_w = static_cast<int>(attr_logic.dims[3]);
        }
    }
    fprintf(stderr, "模型输入: %dx%d type=%s fmt=%s  native_NHWC=%s  输入 %u / 输出 %u\n",
            model_w, model_h,
            attr_logic.type == RKNN_TENSOR_UINT8 ? "UINT8" :
            attr_logic.type == RKNN_TENSOR_INT8   ? "INT8"   :
            attr_logic.type == RKNN_TENSOR_FLOAT16 ? "FLOAT16" : "FLOAT32",
            attr_logic.fmt == RKNN_TENSOR_NHWC ? "NHWC" : "NCHW",
            use_native ? "是" : "否", io_num.n_input, io_num.n_output);

    const char* out_form = "形态B fork 多输出 [box, cls, sum] * 3 尺度";
    if (io_num.n_output == 1) {
        out_form = "形态A 单输出 [1, 4+nc, num_anchors]";
    } else if (io_num.n_output == 2) {
        out_form = "形态C 双输出 box[1,4,num_anchors] + cls[1,nc,num_anchors]";
    }
    fprintf(stderr, "输出形态: %s\n", out_form);
    for (uint32_t i = 0; i < io_num.n_output; i++) {
        fprintf(stderr, "    输出[%u] name=%s dims=[", i, out_attrs[i].name);
        for (uint32_t k = 0; k < out_attrs[i].n_dims; k++)
            fprintf(stderr, "%u%s", out_attrs[i].dims[k], k + 1 < out_attrs[i].n_dims ? "," : "");
        fprintf(stderr, "] type=%d qnt=%d zp=%d scale=%.8f\n", (int)out_attrs[i].type,
                (int)out_attrs[i].qnt_type, out_attrs[i].zp, out_attrs[i].scale);
    }

    // =====================================================================
    // 4. 分配缓冲
    // =====================================================================
    // ---- 输入到底该喂什么形式 -------------------------------------------
    //
    // ★★ 本工程所有模型都是用 config(mean=0, std=255) 转的 int8:
    //    归一化(除 255)已经【固化在模型图里】(有一个 Div255 节点)。
    //    所以板端必须喂【原始 0~255 像素】, 再除一次 255 就是双重归一化。
    //
    //    实测(RKNN 模拟器, 同一个 build 三路对拍):
    //        (a) uint8  NHWC 原始[0,255]   cls_max = 0.867202
    //        (b) float32     原始[0,255]   cls_max = 0.867202   (与 a 完全一致)
    //        (c) float32     已 /255        cls_max = 0.019018   ← 检测被毁
    //    也就是说 (a)/(b) 等价, 而 (c) 会让 cls_max 掉一个数量级 ->
    //    全部低于 conf 阈值 -> "绝大部分帧 objs=0"。
    //
    //    怎么区分"量化模型"和"真浮点模型"?
    //      量化模型的【逻辑】输入 attr 的 qnt_type 是 AFFINE_ASYMMETRIC,
    //      真浮点模型(FP16/FP32)的 qnt_type 是 NONE。
    //      只有后者才需要按 ONNX 契约喂 [0,1] 的 float。
    const bool model_input_quantized =
        (attr_logic.qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC);
    // ★ 判定"是不是浮点模型"必须同时看 FLOAT32 和 FLOAT16:
    //   rknn_tensor_type 里 FLOAT32=0, FLOAT16=1 —— FP16 模型的逻辑输入
    //   是 FLOAT16, 只判 FLOAT32 会把它当成量化模型, 喂 uint8 -> 输出全乱
    //   (实测: 每帧 6000+ 个假框)。判定依据是【逻辑输入的类型】,
    //   不是 qnt_type(FP16 模型的 qnt_type 也可能是 AFFINE_ASYMMETRIC)。
    const bool model_is_fp32 = (attr_logic.type == RKNN_TENSOR_FLOAT32 ||
                                attr_logic.type == RKNN_TENSOR_FLOAT16);
    const size_t model_buf_size = static_cast<size_t>(model_w) * model_h * 3;

    // ★ RGA 目标缓冲的【余量行】(见 kRgaDstPadRows 的说明)。
    //   张量本身还是 model_buf_size, 但底层分配要多给几行, 让 RGA 那 1~2 行
    //   的溢出刷在余量里, 而不是踩坏后面的堆。
    const size_t model_buf_alloc = model_buf_size +
                                   static_cast<size_t>(model_w) * 3 * kRgaDstPadRows;

    // 输入模式:
    //   kInUint8Raw  喂 uint8 RGB NHWC 原始像素[0,255]  —— 量化模型走这条
    //   kInFloatRaw  喂 float32 NCHW 原始[0,255]         —— uint8 被运行时拒绝时的兜底
    //                (模拟器实测与 uint8 完全等价: cls_max 都是 0.867202)
    //   kInFloatScaled 喂 float32 NCHW /255             —— 只有【真浮点】模型走这条
    //                (ONNX 契约是 [0,1]; 量化模型走这条就是双重归一化)
    enum InputMode { kInUint8Raw, kInFloatRaw, kInFloatScaled };
    InputMode in_mode = model_input_quantized ? kInUint8Raw
                        : (model_is_fp32 ? kInFloatScaled : kInUint8Raw);

    int model_fd = rk::dma_buf_alloc(model_buf_alloc, true);
    void* model_ptr = nullptr;
    bool zero_copy_input = false;
    if (model_fd >= 0) {
        model_ptr = rk::dma_buf_map(model_fd, model_buf_alloc);
        if (model_ptr == MAP_FAILED) {
            fprintf(stderr, "[警告] dma-buf mmap 失败\n");
            ::close(model_fd);
            model_fd = -1;
            model_ptr = nullptr;
        }
    }
    if (model_fd >= 0) {
        zero_copy_input = true;
        fprintf(stderr, "模型输入缓冲: dma-buf fd=%d (%s)\n", model_fd, rk::dma_buf_backend());
        // ★ NPU 侧的数据来源 —— 判断"零拷贝到底成没成"的第二个关键日志。
        //   DMA-BUF fd = NPU 直接读 RGA 写出来的那块物理内存, 中间零拷贝。
        //   CPU 内存   = rknn_inputs_set 要把整帧 RGB 拷进 NPU 的输入缓冲。
        fprintf(stderr, "[NPU] 输入来源: DMA-BUF fd (fd=%d, 与 RGA 输出同一块物理内存, 零 CPU 拷贝)\n",
                model_fd);
    } else {
        model_ptr = aligned_alloc(64, model_buf_alloc);
        memset(model_ptr, 114, model_buf_alloc);
        fprintf(stderr, "[警告] dma-buf 不可用, 退化为 CPU 内存输入\n");
        fprintf(stderr, "[NPU] 输入来源: CPU 内存 (rknn_inputs_set, 有一整帧 RGB 的 CPU 拷贝)\n");
    }

    // 只有需要 float32 输入时才分配转换缓冲; 量化模型直接喂 uint8, 一次都不转
    std::vector<float> fp32_buf;
    if (model_is_fp32) {
        fp32_buf.resize(static_cast<size_t>(model_w) * model_h * 3);
        if (in_mode == kInFloatScaled) {
            fprintf(stderr, "[警告] 模型输入是真浮点 FLOAT32 -> CPU 侧转 NCHW/255, 该段无法零拷贝\n");
            zero_copy_input = false;
        }
    }
    fprintf(stderr, "输入喂法: %s\n",
            in_mode == kInUint8Raw
                ? "uint8 RGB NHWC 原始像素[0,255] (归一化已固化在模型里, 不再除255)"
                : (in_mode == kInFloatRaw ? "float32 NCHW 原始[0,255]"
                                          : "float32 NCHW /255 (ONNX 契约 [0,1])"));

    // 把缓冲绑成 NPU 的输入张量内存
    rknn_tensor_mem* in_mem = nullptr;
    rknn_input rin;
    memset(&rin, 0, sizeof(rin));
    if (zero_copy_input) {
        in_mem = rknn_create_mem_from_fd(rctx, model_fd, model_ptr, model_buf_size, 0);
        if (!in_mem) {
            fprintf(stderr, "[警告] rknn_create_mem_from_fd 失败, 退回 rknn_inputs_set\n");
            zero_copy_input = false;
        } else {
            rknn_tensor_attr bind_attr;
            memset(&bind_attr, 0, sizeof(bind_attr));
            if (use_native) {
                bind_attr = attr_native;
                bind_attr.pass_through = 1;      // 布局已完全一致, 驱动不做转换
            } else {
                bind_attr = attr_logic;
                bind_attr.pass_through = 0;      // 告诉驱动"我给的是 NHWC/uint8"
                bind_attr.type = RKNN_TENSOR_UINT8;
                bind_attr.fmt = RKNN_TENSOR_NHWC;
                bind_attr.n_dims = 4;
                bind_attr.dims[0] = 1;
                bind_attr.dims[1] = static_cast<uint32_t>(model_h);
                bind_attr.dims[2] = static_cast<uint32_t>(model_w);
                bind_attr.dims[3] = 3;
            }
            ret = rknn_set_io_mem(rctx, in_mem, &bind_attr);
            if (ret != 0) {
                fprintf(stderr, "[警告] rknn_set_io_mem 失败(%d), 退回 rknn_inputs_set\n", ret);
                rknn_destroy_mem(rctx, in_mem);
                in_mem = nullptr;
                zero_copy_input = false;
            } else {
                fprintf(stderr, "NPU 输入: fd 绑定成功 (pass_through=%d) -> 零拷贝\n",
                        bind_attr.pass_through);
            }
        }
    }
    if (!zero_copy_input) {
        rin.index = 0;
        rin.buf = model_ptr;
        rin.size = static_cast<uint32_t>(model_buf_size);
        rin.pass_through = use_native ? 1 : 0;
        rin.type = RKNN_TENSOR_UINT8;
        rin.fmt = RKNN_TENSOR_NHWC;
    }

    // 显示缓冲(整幅画面, 输出 BGR888 供 OpenCV 直接用)
    int disp_fd = -1;
    void* disp_ptr = nullptr;
    size_t disp_size = 0;
    size_t disp_alloc = 0;   // 含 RGA 余量行, 清理时要用
    int disp_w = 0, disp_h = 0;

    // =====================================================================
    // 4.5 测速: 标定 + 跟踪器
    // =====================================================================
    rk::SpeedCfg speed_cfg;
    speed_cfg.conf_thr = opt.conf;
    speed_cfg.nms_iou = opt.nms;
    speed_cfg.geo.bad_w_ratio = opt.bad_w_ratio;
    speed_cfg.ground_median_k = opt.ground_median_k;
    speed_cfg.speed_win_sec = opt.speed_win;
    speed_cfg.display_span = opt.display_span;
    speed_cfg.speed_limit_kmh = opt.speed_limit;
    speed_cfg.match_thresh = opt.match_thresh;
    // 模块 F: 静态背景过滤
    speed_cfg.static_filter   = opt.static_filter;
    speed_cfg.static_window   = opt.static_window > 0 ? opt.static_window : 30;
    speed_cfg.static_min_disp = opt.static_px;

    // ---- 参与跟踪/测速的类别 ---------------------------------------------
    // 空 = 全部类都跟(老的 head/helmet 模型就是这样)。
    // ★ person+ebike 模型【必须】只跟 ebike: 跟人的话, 地面取点会落在骑手的脚上,
    //   而 IPM 假设 y2 是轮胎接地点 -> 算出来的速度会系统性偏高(历史踩过)。
    //   另外人和车的框互相重叠, 一起喂给跟踪器会让 track_id 在两者间来回跳。
    std::set<int> track_cls_ids;
    {
        size_t pos = 0;
        while (pos < opt.track_cls.size()) {
            const size_t comma = opt.track_cls.find(',', pos);
            const std::string item = (comma == std::string::npos)
                                        ? opt.track_cls.substr(pos)
                                        : opt.track_cls.substr(pos, comma - pos);
            if (!item.empty()) track_cls_ids.insert(atoi(item.c_str()));
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        if (!track_cls_ids.empty()) {
            std::string ids;
            for (int c : track_cls_ids) ids += (ids.empty() ? "" : ",") + std::to_string(c);
            fprintf(stderr, "[跟踪] 只跟踪类别 %s (其余类只画框, 不建轨不测速)\n", ids.c_str());
        }
    }
    speed_cfg.lsq = (opt.estimator == "lsq");

    rk::Pt ipm_quad[4];
    bool have_quad = false;
    if (!opt.ipm_points.empty()) {
        have_quad = parse_quad(opt.ipm_points, ipm_quad);
        if (!have_quad) {
            fprintf(stderr, "[错误] --ipm-points 格式应为 \"x,y;x,y;x,y;x,y\"\n");
            return 1;
        }
    }

    rk::BevView bev;
    rk::SpeedCalib calib;
    rk::SpeedTracker tracker;
    bool calib_built = false;
    int  n_bad_filtered = 0;

    if (opt.speed) {
        fprintf(stderr, "[测速] 估计器=%s 速度窗=%.2fs 显示窗=%.2fs 地面中值=%d帧 "
                        "超速阈值=%.1fkm/h\n",
                opt.estimator.c_str(), opt.speed_win, opt.display_span,
                opt.ground_median_k, opt.speed_limit);
        if (have_quad) {
            fprintf(stderr, "[测速] IPM 模式: 标定梯形 %s\n", opt.ipm_points.c_str());
        } else if (!opt.p1.empty() && !opt.p2.empty() && opt.real_m > 0) {
            fprintf(stderr, "[测速] 单点比例尺模式: %s -> %s = %.2f m\n",
                    opt.p1.c_str(), opt.p2.c_str(), opt.real_m);
        } else {
            fprintf(stderr, "[测速] 没有标定 -> 只跟踪不测速 "
                            "(加 --ipm-points/--ipm-width-m 或 --p1/--p2/--real-m)\n");
        }
    }

    // ---- 日志落盘(可选): 接管 stderr + 定期 fdatasync ----
    LogSink log_sink;
    if (!opt.log_file.empty()) {
        log_sink.open(opt.log_file.c_str());
    }

    // =====================================================================
    // 4.6 LPRNet 车牌识别(可选)
    //
    // 流程: YOLO 检出车牌框 -> RGA 裁剪+缩放到 160x48 RGB(uint8, 不归一化)
    //       -> LPRNet 推理 -> CTC 解码 -> 填进 DetectResult.plate
    //       -> MQTT 上行报文的 license 字段自动带上车牌
    // =====================================================================
    rk::LprNet lpr;
    std::set<int> plate_cls_ids;
    int   lpr_fd = -1;
    void* lpr_ptr = nullptr;
    const size_t lpr_buf_size = static_cast<size_t>(rk::kLprW) * rk::kLprH * 3;    // RGA 目标缓冲 + 余量行(见 kRgaDstPadRows)
    const size_t lpr_buf_alloc = lpr_buf_size +
                                 static_cast<size_t>(rk::kLprW) * 3 * kRgaDstPadRows;
    if (!opt.lprnet_model.empty()) {
        size_t pos = 0;
        while (pos < opt.plate_cls.size()) {
            const size_t comma = opt.plate_cls.find(',', pos);
            const std::string item = (comma == std::string::npos)
                                        ? opt.plate_cls.substr(pos)
                                        : opt.plate_cls.substr(pos, comma - pos);
            if (!item.empty()) plate_cls_ids.insert(atoi(item.c_str()));
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        if (plate_cls_ids.empty()) {
            fprintf(stderr, "[LPRNet] 给了 --lprnet 但没给 --plate-cls, 不启用\n");
        } else {
            const std::string cs = opt.lprnet_charset.empty()
                                       ? std::string(rk::lpr_default_charset())
                                       : opt.lprnet_charset;
            // 先定好输入喂法, 再 init —— init 里会校验并打进日志
            if (opt.lprnet_feed_float) lpr.set_feed(rk::LprFeed::kFloat32);
            if (lpr.init(opt.lprnet_model, cs)) {
                if (opt.lpr_core > 0) lpr.set_core_mask(opt.lpr_core);
                lpr_fd = rk::dma_buf_alloc(lpr_buf_alloc, true);
                if (lpr_fd >= 0) {
                    lpr_ptr = rk::dma_buf_map(lpr_fd, lpr_buf_alloc);
                    if (lpr_ptr == MAP_FAILED) {
                        lpr_ptr = nullptr;
                        ::close(lpr_fd);
                        lpr_fd = -1;
                    }
                }
                if (!lpr_ptr) {
                    lpr_ptr = aligned_alloc(64, lpr_buf_alloc);
                    memset(lpr_ptr, 0, lpr_buf_alloc);
                }
                fprintf(stderr, "[LPRNet] 已启用: 牌类=%s 缓冲=%s 最低分=%.2f 外扩=%.0f%% 输入=%s\n",
                        opt.plate_cls.c_str(), lpr_fd >= 0 ? "dma-buf(零拷贝)" : "CPU",
                        opt.lprnet_min_score, opt.lprnet_pad * 100.f,
                        lpr.feed() == rk::LprFeed::kFloat32
                            ? "float32 x/255 ([0,1])"
                            : "int8 量化码 (按模型 zp/scale)");
            }
        }
    }

    // ---- 调试: ROI 存盘(--save-roi) --------------------------------------
    // 目的: 肉眼确认"模型到底裁到了什么" —— 很多 ROI 是废图(框歪了、
    // 裁到车屁股而不是车牌、糊成一团), 光看日志里的车牌字符串看不出来。
    RoiDumper roi_dump;
    roi_dump.enabled = opt.save_roi;
    roi_dump.roi_dir = opt.save_roi_dir;
    roi_dump.debug_dir = opt.save_roi_debug_dir;
    roi_dump.debug_interval = opt.save_roi_interval > 0 ? opt.save_roi_interval : 30;
    if (roi_dump.enabled) {
        fprintf(stderr, "[ROI] 调试存盘已打开: %s/ (ROI 原图)  %s/ (整帧+框, 每 %d 帧一张)\n",
                roi_dump.roi_dir.c_str(), roi_dump.debug_dir.c_str(),
                roi_dump.debug_interval);
    }

    // =====================================================================
    // 4.6 业务层输出(MQTT 上行 / 串口控制卡)
    // =====================================================================
    ResultDispatcher dispatcher;
    if (opt.sink_stdout) dispatcher.add(std::unique_ptr<rk::ResultSink>(new rk::StdoutSink()));

    rk::IvsPayloadCfg ivs_cfg;
    ivs_cfg.sn = opt.sn;
    ivs_cfg.device_name = opt.device_name;
    ivs_cfg.ipaddr = opt.ipaddr;
    ivs_cfg.msg_id = opt.sn;

    if (!opt.mqtt_host.empty()) {
        auto* s = new rk::MqttSink();
        if (s->start(opt.mqtt_host, opt.mqtt_port, opt.mqtt_topic, opt.mqtt_client_id,
                     opt.mqtt_qos, &ivs_cfg)) {
            dispatcher.add(std::unique_ptr<rk::ResultSink>(s));
        } else {
            fprintf(stderr, "[MQTT] 启动失败, 本次不启用 MQTT 输出\n");
            delete s;
        }
    }
    if (!opt.serial_port.empty()) {
        auto* s = new rk::LedSink();
        if (s->start(opt.serial_port, opt.serial_baud, opt.speak_text, opt.display_text,
                     opt.display_duration)) {
            dispatcher.add(std::unique_ptr<rk::ResultSink>(s));
        } else {
            fprintf(stderr, "[串口] 启动失败, 本次不启用串口输出\n");
            delete s;
        }
    }
    dispatcher.start();

    // =====================================================================
    // 5. 主循环
    // =====================================================================
    AVPacket* pkt = av_packet_alloc();
    // 注意: AVFrame 不再在这里预分配 —— 改成每帧由 av_frame_guard 托管,
    // 生命周期与帧的引用计数统一交给 RAII(Step 07.2)。
    rk::PipelineStats stats;
    // ★ 各阶段耗时(毫秒)
    //   t_wait : 阻塞在"取帧"上的时间(网络/解码器等)—— 这不是我们的计算,
    //            是【源帧率】的直接体现, 判断"是不是源帧率卡住了"就靠它
    //   t_dec  : 真正的解码时间(软解=h264 CPU 解码; 硬解=VPU)
    //   t_rga  : 预处理   t_inf : NPU(检测+LPRNet)
    //   t_post : 后处理(DFL 解码 + NMS + 几何过滤 + 跟踪 + 业务组装)
    //   t_draw : 画框+显示   t_total : 整帧墙钟
    double t_wait = 0, t_dec = 0, t_rga = 0, t_inf = 0, t_post = 0;
    double t_draw = 0, t_total = 0, t_lpr = 0;
    double t_npu_run = 0, t_npu_out = 0;   // NPU 内部再拆: 跑 vs 取输出
    // 相邻两帧"解码出来"的时刻 -> 实测取帧间隔
    std::chrono::steady_clock::time_point last_decode_ts{};
    // 标称源帧率(FFmpeg 从流里读的) —— 用来和实测取帧间隔对照:
    //   实测间隔 ≈ 标称帧率的倒数 -> 我们跟得上源
    //   实测间隔 ≫ 标称帧率的倒数 -> 我们是瓶颈, 源在被我们拖慢
    bool display_broken = false;
    bool nv_src_logged = false;

    // ---- 强制内存池回收(客户要求: 长跑 RSS 必须是直线) ----
    //   glibc 的 malloc 把 free 掉的内存留在进程池里不还给内核, 于是 VmRSS
    //   看着在缓慢上涨(实测 +778 KB/h), 但受控块台账是 1:1 的 —— 那是分配器
    //   的 arena 保留/碎片, 不是泄漏。
    //   malloc_trim(0) 强制把池里空闲的内存还给内核, RSS 立刻掉下来。
    //   触发条件: 每 10000 帧 或 每 10 分钟, 取先到者。
    // ★ 间隔是【实测标定】出来的, 不是拍脑袋:
    //   整流水线(含 LPRNet + NPU 转换)的分配 churn 约 1.5 KB/帧, glibc 会把
    //   free 掉的内存留在 arena 里不还给内核 -> 10000 帧间隔时 RSS 斜率高达
    //   +43 MB/h(15 小时涨 640MB!); 500 帧间隔时锯齿波峰只有 ~0.75MB,
    //   满足客户"不超过基线 1MB"的要求。trim 本身是毫秒级, 开销可忽略。
    const uint64_t kTrimEveryFrames  = opt.mem_trim_frames;
    const double   kTrimEverySeconds = opt.mem_trim_sec;
    uint64_t last_trim_frame = 0;
    double   last_trim_time = 0.0;
    uint64_t trim_calls = 0;
    // 进程起点(第一帧的 t_now)。"每 10 分钟"这条必须从【进程启动】算,
    // 不能从"上一次 trim"算 —— 否则第一次 trim 要等满 10000 帧,
    // 帧率低的时候(多路共享 NPU)可能十几分钟都不回收一次。
    double   loop_start_time = -1.0;
    StagingBuf staging;        // 诊断模式用的暂存 dma-buf(RK_RGA_STAGE=1)

    // 合成 PTS: 源不给时间戳时按 90kHz 递增补(见下面 decode 处的注释)
    int64_t synth_pts = 0;

    // fd 台账基线: 把 stdio / 日志文件 / runtime 内部的 fd 记为已知,
    // 之后对账只报"基线之后新出现的未登记 fd"。
    rk::FdLedger::capture_baseline();

    // 取帧等待计时: av_read_frame 会阻塞在 RTSP/TCP 上, 这段时间属于"等源",
    // 不属于我们的计算。MPP 路径里可能要连续读多个包, 所以用累加。
    auto read_frame_timed = [&](AVPacket* p) -> int {
        const auto t0 = std::chrono::steady_clock::now();
        const int r = av_read_frame(fmt, p);
        t_wait += std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t0).count();
        return r;
    };

    // =====================================================================
    // 4.8 NPU 多实例并行检测(可选, --npu-pipe 2)
    //
    // ★ 背景: NPU 有三个核, 但单实例默认只在一个核上跑(Core0 ~50%, 其余空转)。
    //   把检测模型复制成 2 份, 每份绑一个核, 双缓冲让它们同时处理相邻两帧,
    //   检测阶段的吞吐可以【翻倍】(实测 33 -> 63 fps, 见 docs/NPU_SCHED_REPORT.md)。
    // ★ 注意: 现在的相机只有 25fps, 所以端到端 FPS 不会变(源限速),
    //   收益是【单帧延迟更低 + 为更高帧率的源留出余量】。
    // ★ 实验里 "双核模式(core0_1)" 反而更慢(26 fps), 所以没做成选项。
    rk::NpuPipe pipe;
    bool pipe_on = false;
    std::vector<YoloBox> pipe_boxes_all;
#if RK_USE_NPU
    if (opt.npu_pipe >= 2) {
        const int cmasks[2] = {1, 2};      // 实例0->core0, 实例1->core1
        pipe_on = pipe.init(opt.model, model_w, model_h, 2, cmasks,
                            opt.conf, opt.nms,
                            static_cast<int>(io_num.n_output > 0 ? io_num.n_output : 1));
        if (!pipe_on) {
            fprintf(stderr, "[NPU-Pipe] 初始化失败, 退回串行单实例\n");
        } else {
            fprintf(stderr, "[NPU-Pipe] 并行检测已开启: %d 个实例 x 双缓冲\n",
                    pipe.instance_count());
        }
    }
#endif

    fprintf(stderr, "\n开始处理...\n");

    while (!g_quit) {
        // ★ 整帧墙钟必须【手动】取时间戳: ScopedTimer 是在析构时才写值,
        //   而统计入队发生在析构之前 -> 读到的还是 0(以前 total=0.01ms 就是这么来的)。
        const auto t_frame_begin = std::chrono::steady_clock::now();
        t_wait = 0;

        // ★ RAII 帧守卫(Step 07.2, 修复 P0-1 / P0-2)
        //   两个守卫都是【每轮一个】, 作用域到本轮循环结束为止。
        //   于是 NPU 推理(1803 附近)、LPR 裁剪(2026 附近)、显示转换
        //   都发生在帧还活着的时候 —— 不再出现"帧已释放, 硬件还在用它的 fd"。
        //   任何 continue / return / 异常展开都会自动释放, 不会漏。
        rk::MppFrameGuard mpp_frame_guard;   // MPP 路径: 托管 MppFrame
        rk::AVFrameGuard  av_frame_guard;    // 软解路径: 托管 AVFrame
        void* raw_mpp_frame = nullptr;

        Nv12Info nv;
        bool have_frame = false;
        bool fatal = false;

        // ---------------- 解码 ----------------
        // MPP 路径: FFmpeg 只做拉流, 解码交给 Rockchip MPP(VPU 硬解)
        // FFmpeg 路径: 用 avcodec 解(系统 ffmpeg 没 rkmpp 时就是软解)
        {
            rk::ScopedTimer tm(&t_dec);
            if (use_mpp_decoder) {
                // ★ 轻量帧描述: 只带 dma-buf fd + 尺寸 + stride + 格式。
                //   MPP 把【物理内存的 fd】直接交出来, 后面 RGA 用
                //   importbuffer_fd() 注册它 —— CPU 全程不碰像素。
                rk::Nv12Frame fi;
                // 一个网络包通常解不出一帧, 需要连续喂几个包
                for (int i = 0; i < 32 && !have_frame && !fatal; ++i) {
                    ret = read_frame_timed(pkt);
                    if (ret < 0) {
                        if (ret != AVERROR(EAGAIN)) { log_av_err("读取结束", ret); }
                        fatal = (ret != AVERROR(EAGAIN));
                        break;
                    }
                    if (pkt->stream_index != vstream) { av_packet_unref(pkt); continue; }

                    // ★ MP4(AVCC) 源: 先过 mp4toannexb 过滤器再喂 MPP。
                    //   所有权要点: av_bsf_send_packet() 会【接管】这个包的数据,
                    //   所以发出去之后不能再 unref 它(否则二次释放);
                    //   收包用同一个 pkt 变量, 收到什么算什么。
                    if (mp4_bsf) {
                        ret = av_bsf_send_packet(mp4_bsf, pkt);
                        if (ret < 0) { av_packet_unref(pkt); fatal = true; break; }
                        ret = av_bsf_receive_packet(mp4_bsf, pkt);
                        if (ret < 0) {
                            av_packet_unref(pkt);
                            // EAGAIN = 过滤器内部还有缓存, 还没吐出包 -> 继续读下一个
                            if (ret == AVERROR(EAGAIN)) continue;
                            fatal = true;
                            break;
                        }
                    }

                    // out_frame: 把 MppFrame 的所有权交给我们, 由守卫托管。
                    //   帧会一直活到本轮循环结束, 期间的 fd/ptr 都是有效的。
                    //
                    // ★ PTS 兜底: 有些源【不给 PTS】(裸 .h264 裸流、部分 MP4),
                    //   av_read_frame 返回的是 AV_NOPTS_VALUE(INT64_MIN)。
                    //   原样喂给 MPP 会让它的时序判断失效 —— 解码器一直在等
                    //   一个"合法时间戳", 结果一帧都吐不出来(日志里表现为
                    //   "读取结束: End of file", 但其实包都读完了)。
                    //   这里补一个按 90kHz 递增的合成 PTS, 保证单调递增。
                    int64_t pts = pkt->pts;
                    if (pts == AV_NOPTS_VALUE) {
                        pts = synth_pts;
                        synth_pts += 3600;              // 90kHz 下 40ms 一帧
                    } else {
                        synth_pts = pts + 3600;
                    }
                    if (mpp_dec.decode(pkt->data, pkt->size, pts, &fi,
                                       &raw_mpp_frame)) {
                        mpp_frame_guard.reset(raw_mpp_frame, "mpp-src");
                        raw_mpp_frame = nullptr;
                        have_frame = true;
                        nv.fd = fi.fd;
                        nv.ptr = fi.ptr;
                        nv.width = fi.width;
                        nv.height = fi.height;
                        nv.stride = fi.wstride;
                        nv.plane_h = fi.hstride;
                        // MPP 解出来的是标准 NV12(半平面, UV 交织)
                        nv.planar = false;
                        nv.full_range = false;
                        nv.uv_stride = fi.wstride;
                        nv.u_plane = static_cast<const uint8_t*>(fi.ptr) +
                                     static_cast<size_t>(fi.wstride) * fi.hstride;
                        nv.v_plane = nullptr;
                    }
                    av_packet_unref(pkt);
                }
            } else {
                ret = read_frame_timed(pkt);
                if (ret < 0) {
                    if (ret == AVERROR(EAGAIN)) { continue; }
                    log_av_err("读取结束", ret);
                    break;
                }
                if (pkt->stream_index != vstream) { av_packet_unref(pkt); continue; }
                ret = avcodec_send_packet(vctx, pkt);
                // ★ 包所有权: 送包后【必须】释放我们这一份引用。
                //   FFmpeg 文档(avcodec_send_packet):
                //     "Ownership of the packet remains with the caller ...
                //      The decoder may create a reference to the packet data."
                //   即解码器会自己取一份引用(av_packet_ref), 我们这份照样要放。
                //   ⚠️ 之前怀疑"漏掉这里的 unref 才是泄漏源头"是错的:
                //     实测三种改法(立刻 unref / 延迟 unref / has_b_frames=0)
                //     泄漏斜率一模一样。真凶在 AVFrameGuard(见 av_frame_guard.cc)。
                av_packet_unref(pkt);
                if (ret < 0) { log_av_err("send_packet", ret); break; }
                // 每帧一个 AVFrame, 所有权交给守卫(离开作用域自动 unref)。
                av_frame_guard.reset(av_frame_alloc(), "ffmpeg-src");
                ret = avcodec_receive_frame(vctx, av_frame_guard.get());
                if (ret == AVERROR(EAGAIN)) continue;
                if (ret < 0) { log_av_err("receive_frame", ret); break; }
                extract_nv12(av_frame_guard.get(), use_rkmpp, nv);
                have_frame = true;
            }

            // 诊断模式: 没有 dma-buf 源时, 把软解帧重排进 dma-buf。
            //   故意留在【解码计时】里面 —— 这一步是实打实的一次 CPU 搬运,
            //   藏起来的话统计就骗人了。
            if (nv.fd < 0 && staging_enabled()) {
                stage_frame_into_dmabuf(&nv, &staging);
            }
        }
        if (fatal) break;
        if (!have_frame) continue;
        if (nv.width <= 0 || nv.height <= 0 || nv.stride <= 0) continue;

        // t_dec 里含了取帧等待, 这里扣掉 -> 只留真正的解码工作
        t_dec = std::max(0.0, t_dec - t_wait);
        // 相邻两帧解码出来的间隔 -> 相机实际推送节奏
        {
            const auto now_dec = std::chrono::steady_clock::now();
            if (last_decode_ts.time_since_epoch().count() != 0) {
                stats.on_source_gap(std::chrono::duration<double, std::milli>(
                                        now_dec - last_decode_ts).count());
            }
            last_decode_ts = now_dec;
        }

        // NV12 数据源只打印一次(这是判断"是否真走了硬解/零拷贝"的关键日志)
        if (!nv_src_logged) {
            nv_src_logged = true;
            fprintf(stderr, "[NV12] 数据源: %s\n",
                    nv.fd >= 0 ? "dma-buf fd (RGA import)" : "CPU 映射地址 (RGA DMA 直读)");
        }

        // ---------------- RGA: NV12 -> RGB888(模型输入) ----------------
        rk::RgaImage src;
        src.fd = nv.fd;
        src.vir_addr = nv.ptr;
        src.width = nv.width;
        src.height = nv.height;
        src.wstride = nv.stride;
        src.hstride = nv.plane_h;
        src.format = RK_FORMAT_YCbCr_420_SP;
        // 色度信息: 平面还是半平面、U/V 在哪、色度行宽、是不是全范围
        src.u_plane = nv.u_plane;
        src.v_plane = nv.v_plane;
        src.uv_wstride = nv.uv_stride;
        src.planar_yuv = nv.planar;
        src.full_range = nv.full_range;
        src.bit_shift = nv.bit_shift;   // 高位深: 10bit->2, 12bit->4, 8bit->0

        rk::RgaImage mdst;
        // ★ 并行模式: 目标缓冲跟着实例走(每个实例一块独立 dma-buf,
        //   推理期间不能被下一帧的 RGA 覆写)。
        const int pipe_slot = pipe_on ? (static_cast<int>(stats.frames) % pipe.instance_count()) : 0;
        mdst.fd = pipe_on ? pipe.fd(pipe_slot) : model_fd;
        mdst.vir_addr = pipe_on ? pipe.ptr(pipe_slot) : model_ptr;
        mdst.width = model_w;
        mdst.height = model_h;
        mdst.wstride = model_w * 3;
        mdst.hstride = model_h;
        mdst.format = RK_FORMAT_RGB_888;

        float scale = 1.f;
        int pad_x = 0, pad_y = 0;
        {
            rk::ScopedTimer tm(&t_rga);
            if (rk::rga_nv12_to_rgb888(src, mdst, opt.letterbox, &scale, &pad_x, &pad_y) != 0) {
                fprintf(stderr, "[RGA] 转换失败, 跳过该帧\n");
                continue;   // 守卫自动释放帧, 不再手动 unref
            }
        }


        // ---------------- 数据集采集: 存原始帧(不带任何绘制) ----------------
        // 用途: 给精度回归/量化对比准备【真实机位】的图集。
        // 存的是 NV12->RGB->BGR 的干净原图, 不画框、不写字。
        if (opt.dump_frames > 0 && static_cast<int>(stats.frames) <= opt.dump_frames) {
            const size_t need = static_cast<size_t>(nv.width) * nv.height * 3;
            static std::vector<uint8_t> dump_rgb;
            if (dump_rgb.size() < need) dump_rgb.assign(need, 0);
            rk::RgaImage ddst;
            ddst.fd = -1;
            ddst.vir_addr = dump_rgb.data();
            ddst.width = nv.width;
            ddst.height = nv.height;
            ddst.wstride = nv.width * 3;
            ddst.hstride = nv.height;
            ddst.format = RK_FORMAT_RGB_888;
            if (rk::rga_nv12_to_rgb888(src, ddst, false, nullptr, nullptr, nullptr) == 0) {
                cv::Mat rgb(nv.height, nv.width, CV_8UC3, dump_rgb.data());
                cv::Mat bgr;
                cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
                char name[512];
                snprintf(name, sizeof(name), "%s/frame_%06d.jpg", opt.dump_dir.c_str(),
                         static_cast<int>(stats.frames));
                cv::imwrite(name, bgr);
                if (stats.frames % 100 == 0) {
                    fprintf(stderr, "[dump] 已存 %d/%d 帧 -> %s\n",
                            static_cast<int>(stats.frames), opt.dump_frames,
                            opt.dump_dir.c_str());
                }
            }
        }

        // ---------------- NPU 推理 ----------------
        std::vector<rknn_output> outs(io_num.n_output > 0 ? io_num.n_output : 1);
        memset(outs.data(), 0, sizeof(rknn_output) * outs.size());
        for (size_t i = 0; i < outs.size(); i++) {
            outs[i].want_float = opt.want_int8 ? 0 : 1;
            outs[i].is_prealloc = 0;
            outs[i].index = static_cast<uint32_t>(i);
        }
        // ---- 预处理: 只有需要 float32 输入时才做(量化模型直接喂 uint8 原始像素) ----
        // ★ 这段 122 万次除法必须放在 t_inf 计时器【外面】, 否则会被算成
        //   "NPU 耗时", 让人误以为是模型变慢了(实测能虚增十几毫秒)。
        if (!zero_copy_input && in_mode != kInUint8Raw) {
            const uint8_t* s8 = static_cast<const uint8_t*>(model_ptr);
            const float scale = (in_mode == kInFloatScaled) ? (1.0f / 255.0f) : 1.0f;
            for (int y = 0; y < model_h; y++)
                for (int x = 0; x < model_w; x++)
                    for (int c = 0; c < 3; c++)
                        fp32_buf[(c * model_h + y) * model_w + x] =
                            s8[(y * model_w + x) * 3 + c] * scale;
            rin.buf = fp32_buf.data();
            rin.size = static_cast<uint32_t>(fp32_buf.size() * sizeof(float));
            rin.type = RKNN_TENSOR_FLOAT32;
            rin.fmt = RKNN_TENSOR_NCHW;
        }
#if RK_USE_NPU
        if (pipe_on) {
            // ★ 并行模式: 推理 + DFL/NMS 后处理都在工作线程里做
            //   (输出缓冲属于那个线程的上下文, 不能跨线程用)。
            //   主线程只负责: RGA 写缓冲 -> 提交 -> 取回上一帧的结果。
            rk::NpuPipeJob job;
            job.img_w = nv.width;
            job.img_h = nv.height;
            job.scale = scale;
            job.pad_x = pad_x;
            job.pad_y = pad_y;
            job.letterbox = opt.letterbox;
            const auto t_pipe0 = std::chrono::steady_clock::now();
            pipe.submit(pipe_slot, job);
            std::vector<YoloBox> pipe_boxes;
            double infer_ms = 0;
            pipe.acquire(&pipe_boxes, &infer_ms);
            t_inf = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t_pipe0).count();
            t_npu_run = infer_ms;
            pipe_boxes_all = std::move(pipe_boxes);
        } else
        {
            rk::ScopedTimer tm(&t_inf);
            const auto t_npu0 = std::chrono::steady_clock::now();
            if (zero_copy_input) {
                ret = rknn_run(rctx, nullptr);
            } else {
                ret = rknn_inputs_set(rctx, 1, &rin);
                if (ret != 0 && in_mode == kInUint8Raw) {
                    // 兜底: 运行时不接受 uint8 输入时, 改喂 float32 原始[0,255]
                    //       (模拟器实测与 uint8 完全等价), 并只切一次。
                    fprintf(stderr,
                            "[警告] rknn_inputs_set(uint8 NHWC) 失败(%d), "
                            "改用 float32 NCHW 原始[0,255]\n", ret);
                    in_mode = kInFloatRaw;
                }
                if (ret == 0) ret = rknn_run(rctx, nullptr);
            }
            t_npu_run = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t_npu0).count();
            if (ret == 0) {
                const auto t_out0 = std::chrono::steady_clock::now();
                ret = rknn_outputs_get(rctx, static_cast<uint32_t>(outs.size()), outs.data(),
                                       nullptr);
                t_npu_out = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - t_out0).count();
            }
        }
        if (ret != 0) {
            fprintf(stderr, "RKNN 推理失败: %d\n", ret);
            break;
        }
#else
        // ★ 编译期关掉了 NPU(USE_NPU=0): 不做推理, 也没有输出可释放。
        //   这个构建只用于"纯 CPU"性能基准 —— 它证明【去掉硬件还剩多少】。
        ret = 0;
#endif

#if RK_USE_NPU
        // ---------------- 后处理(移植自 AI 算法组代码) ----------------
        // ★ 后处理计时起点: DFL 解码 + NMS + 几何过滤 + 跟踪, 全是 CPU
        const auto t_post_begin = std::chrono::steady_clock::now();
        std::vector<YoloBox> boxes;
        if (pipe_on) {
            // 并行模式: DFL+NMS 已经在工作线程里做完了(输出缓冲属于那个线程的
            //   上下文, 不能跨线程用), 这里直接拿结果。
            boxes = std::move(pipe_boxes_all);
        } else {
        std::vector<OutTensor> ts(outs.size());
        for (size_t i = 0; i < outs.size(); i++) {
            ts[i].data    = outs[i].buf;
            ts[i].is_int8 = (out_attrs[i].type == RKNN_TENSOR_INT8) && opt.want_int8;
            ts[i].zp      = out_attrs[i].zp;
            ts[i].scale   = out_attrs[i].scale;
            ts[i].n_elems = out_attrs[i].n_elems;
            ts[i].n_dims  = out_attrs[i].n_dims;
            for (uint32_t k = 0; k < out_attrs[i].n_dims && k < 4; k++)
                ts[i].dims[k] = out_attrs[i].dims[k];
            if (ts[i].is_int8 && out_attrs[i].qnt_type != RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC) {
                fprintf(stderr, "[警告] 输出[%zu] 不是 AFFINE_ASYMMETRIC 量化, 改用 float 输出\n", i);
                ts[i].is_int8 = false;
            }
        }

        BoxMapper mapper;
        mapper.img_w = nv.width;
        mapper.img_h = nv.height;
        if (opt.letterbox) {
            mapper.mode = BoxMapper::LETTERBOX;
            mapper.inv_scale = scale;
            mapper.pad_x = pad_x;
            mapper.pad_y = pad_y;
        } else {
            mapper.mode = BoxMapper::STRETCH;
            mapper.scale_x = static_cast<float>(nv.width) / model_w;
            mapper.scale_y = static_cast<float>(nv.height) / model_h;
        }

        boxes = yolov8_postprocess(ts.data(), static_cast<int>(ts.size()), opt.conf, opt.nms, mapper);
        rknn_outputs_release(rctx, static_cast<uint32_t>(outs.size()), outs.data());
        }   // pipe_on / 串行 两条路在这里汇合

        // ---- 模块 A: 几何先验过滤 —— 必须【前置】在跟踪之前 ----------------
        // 模型会间歇性吐出几乎铺满画面的扁宽假框, 它和真框 IoU 只有 0.07~0.37,
        // NMS 压不掉, 置信度又高于 new_track_thresh, 会被当成新目标开一个 track
        // -> 画面上同一个车出现两个框。放到跟踪之后就来不及了。
        if (opt.geo_filter && !boxes.empty()) {
            const size_t before = boxes.size();
            filter_bad_boxes(boxes, nv.width, nv.height, speed_cfg.geo);
            n_bad_filtered += static_cast<int>(before - boxes.size());
        }

        // ---- 标定构建(需要真实帧尺寸, 首帧做一次) -------------------------
        if (opt.speed && !calib_built) {
            calib_built = true;
            if (have_quad) {
                std::string err;
                rk::GroundPlane gp;
                double focal = 0.0;
                if (opt.ipm_fov > 0.0)
                    focal = (nv.width / 2.0) / std::tan(opt.ipm_fov * M_PI / 360.0);
                if (rk::GroundPlane::solve(ipm_quad, nv.width, nv.height, focal, &gp, &err)) {
                    double width_m = opt.ipm_width_m > 0.0 ? opt.ipm_width_m : 3.5;
                    if (opt.ipm_width_m <= 0.0)
                        fprintf(stderr, "[测速] 未指定 --ipm-width-m, 默认假定标定区宽度 3.5 m\n");
                    const double scale_c = width_m / gp.width_norm;
                    if (rk::BevView::build(ipm_quad, gp, scale_c, opt.ipm_ppm, &bev, &err)) {
                        calib.bev = &bev;
                        fprintf(stderr,
                                "[测速] IPM 就绪: f=%.1fpx 标定区 %.2fm x %.2fm  "
                                "鸟瞰 %dx%d @%.0fpx/m  矩形误差=%.3f\n",
                                gp.f, bev.width_m, bev.length_m, bev.dst_w, bev.dst_h,
                                opt.ipm_ppm, gp.rect_err);
                    } else {
                        fprintf(stderr, "[测速] BEV 构建失败: %s\n", err.c_str());
                    }
                } else {
                    fprintf(stderr, "[测速] 逆透视解算失败: %s\n", err.c_str());
                }
            } else if (!opt.p1.empty() && !opt.p2.empty() && opt.real_m > 0) {
                parse_pt(opt.p1, &calib.p1);
                parse_pt(opt.p2, &calib.p2);
                calib.real_meters = opt.real_m;
            }
        }

        // ---- 跟踪 + 测速 -----------------------------------------------------
        // t 必须是【真实时间戳】(单调时钟秒), 不要用 frame_idx / fps:
        // 板子上解码抖动大, 用帧号推时间会把抖动放大成速度跳变。
        // 跟踪器一直跑(它给出稳定的 track_id, 业务层上报要用);
        // 没有标定时只是不出速度, 不影响跟踪。
        std::vector<rk::TrackResult> tracks;
        const double t_now = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        // 只有 --track-cls 指定的类进跟踪器; 其余类只画框, 不建轨不测速。
        // ★ person+ebike 模型必须只跟 ebike —— 跟人的话地面取点会落在骑手脚上,
        //   IPM 假设 y2 是轮胎接地点, 速度会系统性偏高; 且人框和车框互相重叠,
        //   一起喂跟踪器会让 track_id 在人/车之间来回跳。
        std::vector<YoloBox> track_boxes;
        if (track_cls_ids.empty()) {
            track_boxes = boxes;
        } else {
            track_boxes.reserve(boxes.size());
            for (const auto& b : boxes) {
                if (track_cls_ids.count(b.cls_id)) track_boxes.push_back(b);
            }
        }
        tracker.update(track_boxes, t_now, speed_cfg, calib, &tracks);
#else
        // ★ NPU 关闭: 没有检测结果 -> 没有后处理、没有跟踪。
        //   变量照常声明, 后面的画框/统计代码不用改。
        const auto t_post_begin = std::chrono::steady_clock::now();
        std::vector<YoloBox> boxes;
        std::vector<rk::TrackResult> tracks;
        const double t_now = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
        // ★ 这里原来有一句 av_frame_unref(frame) —— 它就是 P0-1 的现场:
        //   帧在这里被释放, 而 200 行之后的 LPR 裁剪还在用它的 fd/ptr。
        //   现在改成: 不再手动释放, 由本轮的 RAII 守卫在作用域结束时统一回收。

        // ---- LPRNet 车牌识别 -------------------------------------------------
        // RGA 一步完成: 原始帧 NV12 -> 裁出车牌框 -> 缩放到 160x48 -> RGB888
        // 整条路径没有 CPU 内存拷贝; 识别结果直接进 DetectResult.plate
        std::vector<cv::Rect> roi_rects;   // 调试图上要画的 ROI 框
#if RK_USE_NPU
        if (lpr.enabled() && !plate_cls_ids.empty() && !tracks.empty()) {
            // LPRNet 是一次独立的 NPU 调用, 计入 NPU 耗时(别让它变成"隐形开销")
            rk::ScopedTimer tm_lpr(&t_lpr);
            rk::RgaImage ldst;
            ldst.fd = lpr_fd;
            ldst.vir_addr = lpr_ptr;
            ldst.width = rk::kLprW;
            ldst.height = rk::kLprH;
            ldst.wstride = rk::kLprW * 3;
            ldst.hstride = rk::kLprH;
            ldst.format = RK_FORMAT_RGB_888;

            const int64_t now_ms = static_cast<int64_t>(t_now * 1000.0);
            for (auto& tr : tracks) {
                // ★ 模块 F: 静止背景(广告牌之类的鬼影)不做 LPRNet 推理 ——
                //   它永远不动, 每帧识别一次纯属浪费算力, 还可能读出假车牌
                //   上报给业务层。
                if (tr.static_bg) {
                    static int static_noted = 0;
                    if (static_noted < 20) {      // 最多提示 20 条, 免得刷屏
                        ++static_noted;
                        fprintf(stderr,
                                "[静态背景] track=%d 窗口内最大位移=%.1fpx(<%.1f) -> 跳过 LPRNet\n",
                                tr.track_id, tr.static_disp, opt.static_px);
                    }
                    continue;
                }
                if (plate_cls_ids.find(tr.box.cls_id) == plate_cls_ids.end()) continue;
                if (tr.score < opt.lprnet_min_score) continue;

                // 车牌 ROI: 从车体框按比例切(车牌在车尾 = 框的下部)
                int rx = 0, ry = 0, rw = 0, rh = 0;
                if (!plate_roi_from_box(tr.box, nv.width, nv.height,
                                        opt.roi_ratio, opt.roi_y, opt.roi_height,
                                        opt.roi_shift, opt.roi_below,
                                        &rx, &ry, &rw, &rh)) {
                    continue;
                }
                // 可选的额外外扩
                if (opt.lprnet_pad > 0.f) {
                    const int dx = static_cast<int>(rw * opt.lprnet_pad);
                    const int dy = static_cast<int>(rh * opt.lprnet_pad);
                    rx -= dx; ry -= dy; rw += dx * 2; rh += dy * 2;
                }
                // ★ 外扩之后一定要再夹一次: 贴边的车牌框外扩 10% 就会顶出画面。
                //   不夹的话 RGA 的 im_rect 越界, 后面裁剪出来的图是乱的,
                //   极端情况还会把堆写坏(2026-11-11 那次 core dump 就是这条路)。
                if (!clamp_roi(&rx, &ry, &rw, &rh, nv.width, nv.height, 8, 6)) {
                    continue;
                }
                // ★ RGA 裁 NV12 要求 x/y/w/h 都是偶数, 不对齐会报
                //   "Error yuv not align to 2" 然后整条 RGA 路失败(退回 CPU)。
                //   这里统一向下取偶, 再夹一次(宁可少裁 1 像素)。
                rx &= ~1; ry &= ~1; rw &= ~1; rh &= ~1;
                if (!clamp_roi(&rx, &ry, &rw, &rh, nv.width, nv.height, 8, 6)) {
                    continue;
                }
                // ★ ROI 最小尺寸约束(2026-11-12 修):
                //   检测框小的时候, ROI 会退化成极扁的矩形(如 780x8 —— ROI 高只有
                //   框高的 16%, 向下取偶后剩 8px), 缩放到 160x48 要【纵向放大 6 倍】。
                //   RGA 缩放器扛不住这种极端比例, 硬件直接报错并被驱动软复位 core:
                //       rga: irq handler err! INTR[0x2099], HW_STATUS[0xa0000]
                //       rga: RGA3_core0[0x1] soft reset complete.
                //   实测: 默认参数每 300 帧必现一次; ROI 加高后 900 帧 0 次。
                //   这里从【源头】保证 ROI 不退化: 高度/宽度不够就扩
                //   (纵向向上扩, 保住底边 —— 车牌在车尾), 再夹回画面。
                //   源高 >= 16 时, 放大到 48 只需 3 倍, 在 RGA 的舒适区内。
                constexpr int kMinRoiH = 16;
                constexpr int kMinRoiW = 32;
                if (rh < kMinRoiH) {
                    const int need = kMinRoiH - rh;
                    ry -= need;                 // 向上扩, 保住底边
                    rh = kMinRoiH;
                    if (ry < 0) { rh += ry; ry = 0; }   // 顶到画面上沿就压扁
                }
                if (rw < kMinRoiW) {
                    const int need = kMinRoiW - rw;
                    rx -= need / 2;              // 横向居中扩
                    rw = kMinRoiW;
                    if (rx < 0) { rw += rx; rx = 0; }
                }
                if (!clamp_roi(&rx, &ry, &rw, &rh, nv.width, nv.height, 8, 6)) {
                    continue;
                }
                if (rk::rga_crop_to_rgb888(src, rx, ry, rw, rh, ldst) != 0) continue;

                roi_rects.push_back(cv::Rect(rx, ry, rw, rh));

                // ---- 车牌图预处理(可选, 默认关) ------------------------------
                // 纠偏要在反白之前: 二值化找文字行, 反白后前景就变了
                if (opt.lprnet_deskew) {
                    deskew_roi_rgb(static_cast<uint8_t*>(lpr_ptr), rk::kLprW, rk::kLprH, 15.f);
                }
                if (opt.lprnet_invert) {
                    // ★ 先转【灰度】再反白, 不要逐通道反:
                    //   逐通道反白的话, 绿牌(0,150,80)会变成粉红色(255,105,175),
                    //   黑字粉底仍然不是模型见过的"黑字白底"(实测读 "9");
                    //   灰度反白 -> 浅灰底黑字, 模型读得更好(实测同一张 "8F33")。
                    cv::Mat roi(rk::kLprH, rk::kLprW, CV_8UC3, lpr_ptr);
                    cv::Mat gray;
                    cv::cvtColor(roi, gray, cv::COLOR_RGB2GRAY);
                    cv::bitwise_not(gray, gray);
                    cv::cvtColor(gray, roi, cv::COLOR_GRAY2RGB);
                }

                // 调试: 把这张 ROI 存下来(就是接下来要喂给 LPRNet 的那张)
                if (roi_dump.enabled) {
                    roi_dump.dump_roi(tr.track_id, static_cast<int>(stats.frames),
                                      static_cast<const uint8_t*>(lpr_ptr),
                                      rk::kLprW, rk::kLprH);
                }

                std::string plate;
                bool cached = false;
                if (!lpr.infer_track(tr.track_id, static_cast<const uint8_t*>(lpr_ptr),
                                     now_ms, opt.lprnet_ttl_ms, &plate, &cached)) {
                    continue;
                }
                // LPRNet 只输出数字/字母(如 0443M1), Web 端解析器要求带省份前缀
                // -> 这里强拼前缀(默认"深圳"); 报文走 UTF-8 Base64, 接收端先试 UTF-8
                if (!plate.empty() && !opt.plate_prefix.empty()) {
                    plate = opt.plate_prefix + plate;
                }
                tr.plate = plate;
                fprintf(stderr, "[车牌] track=%d cls=%d score=%.2f roi=%dx%d@(%d,%d) -> %s%s\n",
                        tr.track_id, tr.box.cls_id, tr.score, rw, rh, rx, ry, plate.c_str(),
                        cached ? "  (缓存)" : "");
            }
        }
#endif  // RK_USE_NPU

        // ---- 调试: 整帧 + 检测框 + ROI 框 -> output_debug/ --------------------
        if (roi_dump.enabled) {
            roi_dump.dump_debug(static_cast<int>(stats.frames), src, tracks, roi_rects, labels);
        }

        // ---- 业务层上报(只入队, 不阻塞推理线程) -----------------------------
        if (!dispatcher.empty_sinks()) {
            std::vector<rk::DetectResult> drs;
            drs.reserve(tracks.size());
            for (const auto& tr : tracks) {
                // ★ 模块 F: --static-discard 打开后, 静止背景连检测结果都不上报
                //   (默认只跳过 LPRNet, 框照画, 方便人眼确认过滤对不对)
                if (opt.static_discard && tr.static_bg) continue;
                rk::DetectResult d;
                d.track_id = tr.track_id;
                d.cls_id = tr.box.cls_id;
                d.score = tr.score;
                d.speed_kmh = tr.speed_kmh;
                d.speed_valid = tr.speed_valid;
                d.over_speed = tr.over_speed;
                d.plate = tr.plate;
                d.frame_w = nv.width;
                d.frame_h = nv.height;
                d.ts_ms = static_cast<int64_t>(t_now * 1000.0);
                drs.push_back(std::move(d));
            }
            dispatcher.push(stats.frames, std::move(drs));
        }

        // ---------------- 显示 ----------------
        if (opt.show && !display_broken) {
            rk::ScopedTimer tm(&t_draw);
            float ds = 1.0f;
            if (nv.width > opt.disp_max_w) ds = static_cast<float>(opt.disp_max_w) / nv.width;
            int nw = static_cast<int>(nv.width * ds) & ~1;
            int nh = static_cast<int>(nv.height * ds) & ~1;

            if (disp_fd < 0 || disp_w != nw || disp_h != nh) {
                if (disp_fd >= 0) {
                    rk::dma_buf_unmap(disp_ptr, disp_alloc);
                    ::close(disp_fd);
                } else {
                    free(disp_ptr);
                }
                // RGA 目标缓冲 + 余量行(见 kRgaDstPadRows)
                disp_size = static_cast<size_t>(nw) * nh * 3;
                disp_alloc = disp_size + static_cast<size_t>(nw) * 3 * kRgaDstPadRows;
                disp_fd = rk::dma_buf_alloc(disp_alloc, true);
                if (disp_fd >= 0) {
                    disp_ptr = rk::dma_buf_map(disp_fd, disp_alloc);
                } else {
                    disp_ptr = aligned_alloc(64, disp_alloc);
                    memset(disp_ptr, 0, disp_alloc);
                }
                disp_w = nw;
                disp_h = nh;
            }

            rk::RgaImage ddst;
            ddst.fd = disp_fd;
            ddst.vir_addr = disp_ptr;
            ddst.width = disp_w;
            ddst.height = disp_h;
            ddst.wstride = disp_w * 3;
            ddst.hstride = disp_h;
            ddst.format = RK_FORMAT_BGR_888;   // OpenCV 直接就是 BGR 顺序

            rk::rga_nv12_to_rgb888(src, ddst, false, nullptr, nullptr, nullptr);

            cv::Mat view(disp_h, disp_w, CV_8UC3, disp_ptr);
            if (opt.speed && !tracks.empty()) {
                // 先画【不参与跟踪】的类(比如 rider): 暗色细框, 不给 ID/速度,
                // 只作参考, 不进跟踪器也不测速。
                if (!track_cls_ids.empty()) {
                    for (const auto& b : boxes) {
                        if (track_cls_ids.count(b.cls_id)) continue;
                        cv::Rect r(static_cast<int>(b.x1 * ds), static_cast<int>(b.y1 * ds),
                                   static_cast<int>((b.x2 - b.x1) * ds),
                                   static_cast<int>((b.y2 - b.y1) * ds));
                        cv::rectangle(view, r, cv::Scalar(0, 165, 255), 1);
                        std::string name = (static_cast<size_t>(b.cls_id) < labels.size())
                                               ? labels[b.cls_id]
                                               : std::to_string(b.cls_id);
                        cv::putText(view, name, cv::Point(r.x, std::max(12, r.y - 4)),
                                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 165, 255), 1);
                    }
                }
                // 画 track: 带 ID 和速度, 超速变红
                for (const auto& tr : tracks) {
                    cv::Rect r(static_cast<int>(tr.box.x1 * ds), static_cast<int>(tr.box.y1 * ds),
                               static_cast<int>((tr.box.x2 - tr.box.x1) * ds),
                               static_cast<int>((tr.box.y2 - tr.box.y1) * ds));
                    // ★ 模块 F: 静止背景(广告牌之类的鬼影)画成灰色并标 STATIC,
                    //   一眼就能看出过滤掉的是哪些。
                    const cv::Scalar color = tr.static_bg ? cv::Scalar(160, 160, 160)
                                          : (tr.over_speed ? cv::Scalar(0, 0, 255)
                                                           : cv::Scalar(0, 220, 0));
                    cv::rectangle(view, r, color, 2);
                    std::string name = (static_cast<size_t>(tr.box.cls_id) < labels.size())
                                           ? labels[tr.box.cls_id]
                                           : std::to_string(tr.box.cls_id);
                    std::string txt = "ID:" + std::to_string(tr.track_id) + " " + name;
                    if (tr.speed_valid) {
                        char sp[48];
                        snprintf(sp, sizeof(sp), " %.1f km/h", tr.speed_kmh);
                        txt += sp;
                        if (tr.over_speed) txt += " 超速";
                    }
                    if (tr.static_bg) txt += " STATIC";
                    cv::putText(view, txt, cv::Point(r.x, std::max(12, r.y - 4)),
                                cv::FONT_HERSHEY_SIMPLEX, 0.5, color, 1);
                }
            } else {
                for (const auto& b : boxes) {
                    cv::Rect r(static_cast<int>(b.x1 * ds), static_cast<int>(b.y1 * ds),
                               static_cast<int>((b.x2 - b.x1) * ds),
                               static_cast<int>((b.y2 - b.y1) * ds));
                    const cv::Scalar color(0, 255, 0);
                    cv::rectangle(view, r, color, 2);
                    std::string name = (static_cast<size_t>(b.cls_id) < labels.size())
                                           ? labels[b.cls_id]
                                           : std::to_string(b.cls_id);
                    cv::putText(view, name + " " + std::to_string(static_cast<int>(b.score * 100)),
                                cv::Point(r.x, std::max(12, r.y - 4)),
                                cv::FONT_HERSHEY_SIMPLEX, 0.5, color, 1);
                }
            }

            char hud[256];
            snprintf(hud, sizeof(hud),
                     "FPS %.1f | wait %.1f | dec %.1f | rga %.1f | npu %.1f | post %.1f ms"
                     " | %dx%d | %s | tracks %d bad %d",
                     stats.fps(), stats.wait.mean(), stats.decode.mean(),
                     stats.rga.mean(), stats.infer.mean(), stats.post.mean(),
                     nv.width, nv.height,
                     nv.mode == Nv12Mode::kDmaBufFd ? "NV12:dma-buf" : "NV12:cpu-ptr",
                     tracker.track_count(), n_bad_filtered);
            cv::putText(view, hud, cv::Point(10, 24), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                        cv::Scalar(0, 0, 0), 3);
            cv::putText(view, hud, cv::Point(10, 24), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                        cv::Scalar(0, 255, 255), 1);

            try {
                cv::imshow("RK3588 zero-copy", view);
                cv::waitKey(1);
            } catch (const cv::Exception& e) {
                fprintf(stderr, "[显示] 不可用(%s), 已关闭显示\n", e.what());
                display_broken = true;
            }
        }

        // ---------------- 统计 ----------------
        // 后处理耗时 = 从 DFL 起点到跟踪结束(LPRNet 那段单独扣掉, 它算 NPU)
        t_post = std::max(0.0, std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - t_post_begin).count()
                                 - t_lpr);
        // LPRNet 的 NPU 时间并进 infer
        t_inf += t_lpr;
        // 整帧墙钟(手动取戳, 见循环开头)
        t_total = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t_frame_begin).count();

        stats.wait.push(t_wait);
        stats.decode.push(t_dec);
        stats.rga.push(t_rga);
        stats.infer.push(t_inf);
        stats.npu_run.push(t_npu_run);
        stats.npu_out.push(t_npu_out);
        stats.post.push(t_post);
        stats.draw.push(t_draw);
        stats.on_frame(t_total);
        if (stats.frames % 60 == 0) {
            // ★ 帧守卫的构造/析构必须严格 1:1(规则第二节: 释放内存后指针置空,
            //   异常路径也不能漏)。这里每 60 帧把计数打出来, 长跑时一眼能看出
            //   是否有泄漏(析构次数追不上构造次数)。
            fprintf(stderr,
                    "[frame %d] fps=%.1f src=%.1f gap=%.1fms wait=%.2fms dec=%.2fms "
                    "rga=%.2fms npu=%.2fms(run%.1f/out%.1f) post=%.2fms draw=%.2fms total=%.2fms "
                    "objs=%zu tracks=%d rss=%zuKB heap=%zuKB trim=%llu "
                    "guard[av 构%llu/析%llu/存%llu] guard[mpp 构%llu/析%llu/存%llu] "
                    "fd[登记%zu/未登记%zu]\n",
                    stats.frames, stats.fps(), src_nominal_fps, stats.src_gap.mean(),
                    stats.wait.mean(), stats.decode.mean(), stats.rga.mean(),
                    stats.infer.mean(), stats.npu_run.mean(), stats.npu_out.mean(),
                    stats.post.mean(), stats.draw.mean(), stats.total.mean(), boxes.size(),
                    tracker.track_count(),
                    read_rss_kb(), (size_t)mallinfo2().uordblks / 1024,
                    (unsigned long long)trim_calls,
                    (unsigned long long)rk::AVFrameGuard::total_constructed(),
                    (unsigned long long)rk::AVFrameGuard::total_destroyed(),
                    (unsigned long long)rk::AVFrameGuard::live_count(),
                    (unsigned long long)rk::MppFrameGuard::total_constructed(),
                    (unsigned long long)rk::MppFrameGuard::total_destroyed(),
                    (unsigned long long)rk::MppFrameGuard::live_count(),
                    rk::FdLedger::tracked_count(), rk::FdLedger::untracked_count());
        }
        // ★ 日志脏页定期刷盘(每 5 秒): 防止 Page Cache 脏页堆积把
        //   cgroup 内存限额吃满(表现是"应用被莫名 OOM kill", 不是真泄漏)。
        if (log_sink.fd >= 0) {
            static double last_sync = 0.0;
            if (t_now - last_sync >= 5.0) {
                last_sync = t_now;
                log_sink.sync();
            }
        }

        // ★ fd 台账定期对账(每 300 帧): 比对 /proc/self/fd,
        //   抓"用了没登记的 fd"和"登记了却已被关闭的 fd"。
        if (stats.frames % 300 == 0) {
            fprintf(stderr, "[fd 台账] 第 %d 帧对账:\n", stats.frames);
            rk::FdLedger::dump_open();
        }

        // ---------------- 强制内存池回收 ----------------
        //   把 glibc 池里空闲的内存还给内核, 让 RSS 曲线是直线而不是缓坡。
        //   每 10000 帧 或 每 10 分钟触发一次(取先到者)。
        if (t_now > 0.0) {
            if (loop_start_time < 0.0) loop_start_time = t_now;
            const bool by_frames = kTrimEveryFrames > 0 &&
                                  (stats.frames - last_trim_frame) >= kTrimEveryFrames;
            const bool by_time   = kTrimEverySeconds > 0 &&
                                   ((t_now - loop_start_time) >= kTrimEverySeconds ||
                                    (t_now - last_trim_time) >= kTrimEverySeconds);
            if ((by_frames || by_time) && stats.frames > 0) {
                const auto trim_t0 = std::chrono::steady_clock::now();
                const size_t freed = rk::mem_trim();
                const double trim_ms =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - trim_t0).count();
                last_trim_frame = stats.frames;
                last_trim_time = t_now;
                ++trim_calls;
                // ★ mallinfo2: 区分"活对象在涨"(真泄漏)和"碎片在涨"(arena 被撑大)
                //     uordblks = 正在使用的字节; fordblks = 空闲但没还给内核的字节
                //     arena     = arena 总大小
                struct mallinfo2 mi = mallinfo2();
                fprintf(stderr,
                        "[内存] malloc_trim @帧%llu: 归还空闲 %zu KB (累计 %llu 次) "
                        "rss=%zuKB 耗时=%.2fms 堆[在用%zuKB/空闲%zuKB/arena%zuKB]\n",
                        static_cast<unsigned long long>(stats.frames),
                        freed / 1024, static_cast<unsigned long long>(trim_calls),
                        read_rss_kb(), trim_ms,
                        (size_t)mi.uordblks / 1024, (size_t)mi.fordblks / 1024,
                        (size_t)mi.arena / 1024);
            }
        }
        if (opt.max_frames > 0 && stats.frames >= opt.max_frames) break;
    }

    stats.dump();

    // =====================================================================
    // 5.5 基准模式 (--benchmark N)
    //
    // 目的: 把"三种硬件配置"的性能摆在一张表里对比, 让"该不该上硬件"
    //       这种问题有数据支撑, 而不是靠感觉。
    //
    // 数据来自两段:
    //   阶段 A (视频流): 主循环跑 --benchmark-frames 帧, 拿到【解码】耗时
    //                     —— 解码只能测视频, 静态图片没有"解码"这一步。
    //   阶段 B (本地图片): 遍历 N 张本地图片(不够就循环读入), 测
    //                     【预处理/推理/后处理】—— 固定输入, 不受网络和
    //                     相机场景波动影响, 三种配置之间才可比。
    //
    // 输出 CSV: config,fps,decode_ms,rga_ms,npu_ms,post_ms,total_ms
    //   config 由【编译期宏】决定:
    //     MPP+RGA+NPU -> hw    仅NPU(CPU解码+CPU预处理) -> npu    全关 -> cpu
    //   三个构建各跑一次, 往同一个 CSV 里追加, 就是一张对比表。
    // =====================================================================
    if (opt.benchmark_frames > 0) {
        // ---------------- 阶段 B: 本地图片流水线 ----------------
        std::vector<cv::Mat> bench_imgs;
        {
            std::vector<std::string> paths;
            if (DIR* d = opendir(opt.benchmark_images.c_str())) {
                while (struct dirent* e = readdir(d)) {
                    std::string name = e->d_name;
                    if (name.size() > 4 &&
                        (name.compare(name.size() - 4, 4, ".jpg") == 0 ||
                         name.compare(name.size() - 4, 4, ".png") == 0)) {
                        paths.push_back(opt.benchmark_images + "/" + name);
                    }
                }
                closedir(d);
            }
            std::sort(paths.begin(), paths.end());
            for (const auto& p : paths) {
                cv::Mat m = cv::imread(p);
                if (!m.empty()) bench_imgs.push_back(std::move(m));
            }
        }
        if (bench_imgs.empty()) {
            fprintf(stderr, "[benchmark] 图片目录 %s 里没有可用图片, 跳过阶段 B\n",
                    opt.benchmark_images.c_str());
        } else {
            fprintf(stderr, "[benchmark] 载入 %zu 张图片, 跑 %d 帧(循环读入)\n",
                    bench_imgs.size(), opt.benchmark_frames);

            // NV12 暂存(每帧把 BGR 转成 NV12, 作为流水线的输入)
            const int bw = bench_imgs[0].cols, bh = bench_imgs[0].rows;
            const size_t nv12_size = static_cast<size_t>(bw) * bh * 3 / 2;
            std::vector<uint8_t> nv12_buf(nv12_size);

            // ★ 关键: 生产路径里 RGA 的【源】是 MPP 解码器给的 dma-buf fd,
            //   RGA 直接 DMA 读, 很快(实测 2.3ms)。如果喂 CPU 虚拟地址,
            //   RGA 反而比 OpenCV 还慢(实测 7.6ms vs 5.2ms) —— 那不是生产形态。
            //   所以这里也把输入放进 dma-buf, 保证测的是【真实的那条路】。
            int bsrc_fd = rk::dma_buf_alloc(nv12_size, true);
            void* bsrc_ptr = nullptr;
            if (bsrc_fd >= 0) {
                bsrc_ptr = rk::dma_buf_map(bsrc_fd, nv12_size);
                if (bsrc_ptr == MAP_FAILED) {
                    bsrc_ptr = nullptr;
                    ::close(bsrc_fd);
                    bsrc_fd = -1;
                }
            }
            fprintf(stderr, "[benchmark] 输入缓冲: %s\n",
                    bsrc_fd >= 0 ? "dma-buf (与生产路径一致)" : "CPU 内存(RGA 会走慢路径)");

            double sum_prep = 0, sum_rga = 0, sum_npu = 0, sum_post = 0;
            int    n_done = 0, n_objs = 0;
            for (int i = 0; i < opt.benchmark_frames; i++) {
                const cv::Mat& bgr = bench_imgs[static_cast<size_t>(i) % bench_imgs.size()];

                // ---- 准备: BGR -> NV12(静态图的"解码"等价物) ----
                auto t0 = std::chrono::steady_clock::now();
                cv::Mat yuv;
                cv::cvtColor(bgr, yuv, cv::COLOR_BGR2YUV_I420);
                {
                    const size_t y_size = static_cast<size_t>(bw) * bh;
                    const size_t c_size = y_size / 4;
                    memcpy(nv12_buf.data(), yuv.data, y_size);
                    uint8_t* uv = nv12_buf.data() + y_size;
                    const uint8_t* u = yuv.data + y_size;
                    const uint8_t* v = u + c_size;
                    for (size_t p = 0; p < c_size; p++) {   // I420 -> NV12(交织)
                        uv[p * 2 + 0] = u[p];
                        uv[p * 2 + 1] = v[p];
                    }
                }
                if (bsrc_ptr) memcpy(bsrc_ptr, nv12_buf.data(), nv12_size);
                sum_prep += std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - t0).count();

                // ---- 预处理: NV12 -> RGB888 (RGA 或 CPU, 由编译期宏决定) ----
                rk::RgaImage bsrc;
                bsrc.fd = bsrc_fd;
                bsrc.vir_addr = bsrc_ptr ? bsrc_ptr : nv12_buf.data();
                bsrc.width = bw;
                bsrc.height = bh;
                bsrc.wstride = bw;
                bsrc.hstride = bh;
                bsrc.format = RK_FORMAT_YCbCr_420_SP;
                bsrc.planar_yuv = false;
                bsrc.full_range = false;

                rk::RgaImage bdst;
                bdst.fd = model_fd;
                bdst.vir_addr = model_ptr;
                bdst.width = model_w;
                bdst.height = model_h;
                bdst.wstride = model_w * 3;
                bdst.hstride = model_h;
                bdst.format = RK_FORMAT_RGB_888;

                float bscale = 1.f;
                int bpad_x = 0, bpad_y = 0;
                t0 = std::chrono::steady_clock::now();
                if (rk::rga_nv12_to_rgb888(bsrc, bdst, opt.letterbox, &bscale,
                                           &bpad_x, &bpad_y) != 0) {
                    fprintf(stderr, "[benchmark] 第 %d 帧预处理失败, 跳过\n", i);
                    continue;
                }
                sum_rga += std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - t0).count();

#if RK_USE_NPU
                // ---- NPU 推理 ----
                std::vector<rknn_output> bouts(io_num.n_output > 0 ? io_num.n_output : 1);
                memset(bouts.data(), 0, sizeof(rknn_output) * bouts.size());
                for (size_t k = 0; k < bouts.size(); k++) {
                    bouts[k].want_float = opt.want_int8 ? 0 : 1;
                    bouts[k].is_prealloc = 0;
                    bouts[k].index = static_cast<uint32_t>(k);
                }
                t0 = std::chrono::steady_clock::now();
                int bret = 0;
                if (zero_copy_input) {
                    bret = rknn_run(rctx, nullptr);
                } else {
                    bret = rknn_inputs_set(rctx, 1, &rin);
                    if (bret == 0) bret = rknn_run(rctx, nullptr);
                }
                if (bret == 0) {
                    bret = rknn_outputs_get(rctx, static_cast<uint32_t>(bouts.size()),
                                            bouts.data(), nullptr);
                }
                sum_npu += std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - t0).count();
                if (bret != 0) {
                    fprintf(stderr, "[benchmark] 第 %d 帧推理失败(%d), 跳过\n", i, bret);
                    continue;
                }

                // ---- CPU 后处理 ----
                std::vector<OutTensor> bts(bouts.size());
                for (size_t k = 0; k < bouts.size(); k++) {
                    bts[k].data    = bouts[k].buf;
                    bts[k].is_int8 = (out_attrs[k].type == RKNN_TENSOR_INT8) && opt.want_int8;
                    bts[k].zp      = out_attrs[k].zp;
                    bts[k].scale   = out_attrs[k].scale;
                    bts[k].n_elems = out_attrs[k].n_elems;
                    bts[k].n_dims  = out_attrs[k].n_dims;
                    for (uint32_t d = 0; d < out_attrs[k].n_dims && d < 4; d++)
                        bts[k].dims[d] = out_attrs[k].dims[d];
                }
                BoxMapper bmapper;
                bmapper.img_w = bw;
                bmapper.img_h = bh;
                if (opt.letterbox) {
                    bmapper.mode = BoxMapper::LETTERBOX;
                    bmapper.inv_scale = bscale;
                    bmapper.pad_x = bpad_x;
                    bmapper.pad_y = bpad_y;
                } else {
                    bmapper.mode = BoxMapper::STRETCH;
                    bmapper.scale_x = static_cast<float>(bw) / model_w;
                    bmapper.scale_y = static_cast<float>(bh) / model_h;
                }
                t0 = std::chrono::steady_clock::now();
                std::vector<YoloBox> bboxes =
                    yolov8_postprocess(bts.data(), static_cast<int>(bts.size()),
                                       opt.conf, opt.nms, bmapper);
                sum_post += std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - t0).count();
                n_objs += static_cast<int>(bboxes.size());
                rknn_outputs_release(rctx, static_cast<uint32_t>(bouts.size()), bouts.data());
#else
                (void)n_objs;
#endif
                n_done++;
            }

            // ---------------- 汇总 + 输出 CSV ----------------
            const double dn = (n_done > 0) ? static_cast<double>(n_done) : 1.0;
            const double prep_ms = sum_prep / dn;
            const double rga_ms  = sum_rga / dn;
            const double npu_ms  = sum_npu / dn;
            const double post_ms = sum_post / dn;
            // 解码耗时来自阶段 A(视频流): 静态图没有解码这一步
            const double dec_ms = stats.decode.mean();

            const char* cfg =
#if RK_USE_MPP && RK_USE_RGA && RK_USE_NPU
                "hw";
#elif RK_USE_NPU
                "npu";
#else
                "cpu";
#endif
            const double total = dec_ms + rga_ms + npu_ms + post_ms;
            const double fps = (total > 0.001) ? (1000.0 / total) : 0.0;

            fprintf(stderr,
                    "\n===== 基准结果 (config=%s) =====\n"
                    "  图片帧数        : %d (实际完成 %d)\n"
                    "  图片准备(BGR->NV12): %.2f ms   (不计入流水线, 仅供参照)\n"
                    "  解码(视频流)     : %.2f ms\n"
                    "  预处理(RGA/CPU)  : %.2f ms\n"
                    "  NPU 推理         : %.2f ms\n"
                    "  CPU 后处理       : %.2f ms\n"
                    "  合计单帧         : %.2f ms   -> %.2f FPS\n"
                    "  平均检出目标数    : %.1f\n",
                    cfg, opt.benchmark_frames, n_done, prep_ms, dec_ms, rga_ms,
                    npu_ms, post_ms, total, fps, n_objs / dn);

            if (std::string(cfg) == "cpu") {
                fprintf(stderr,
                        "  ⚠️ 提醒: cpu 配置【不做检测】(NPU 已关闭), 它的 fps 只代表"
                        "\n     [取流+解码+预处理] 这条链的速度, 不能和 hw/npu 直接比\n"
                        "     (真要纯 CPU 检测, 得另有一个 CPU 版检测器, 本工程没有)。\n");
            }
            if (bsrc_fd >= 0) {
                if (bsrc_ptr) rk::dma_buf_unmap(bsrc_ptr, nv12_size);
                ::close(bsrc_fd);
            }
            if (!opt.benchmark_csv.empty()) {
                const bool need_header = (access(opt.benchmark_csv.c_str(), F_OK) != 0);
                FILE* f = fopen(opt.benchmark_csv.c_str(), "a");
                if (f) {
                    if (need_header)
                        fprintf(f, "config,fps,decode_ms,rga_ms,npu_ms,post_ms,total_ms\n");
                    fprintf(f, "%s,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                            cfg, fps, dec_ms, rga_ms, npu_ms, post_ms, total);
                    fclose(f);
                    fprintf(stderr, "  已追加到 CSV: %s\n", opt.benchmark_csv.c_str());
                }
            }
        }
    }

    // =====================================================================
    // 6. 清理
    // =====================================================================
    dispatcher.stop();          // 等后台线程把队列里的结果发完
    if (lpr_fd >= 0) {
        rk::dma_buf_unmap(lpr_ptr, lpr_buf_alloc);
        ::close(lpr_fd);
    } else if (lpr_ptr) {
        free(lpr_ptr);
    }
    if (in_mem) rknn_destroy_mem(rctx, in_mem);
    rknn_destroy(rctx);
    // AVFrame 不再需要在这里释放: 每一帧都由本轮的 AVFrameGuard 托管,
    // 离开作用域时自动 av_frame_unref (Step 07.2)。
    av_packet_free(&pkt);
    avformat_close_input(&fmt);
    if (model_fd >= 0) {
        rk::dma_buf_unmap(model_ptr, model_buf_alloc);
        ::close(model_fd);
    } else {
        free(model_ptr);
    }
    if (disp_fd >= 0) {
        rk::dma_buf_unmap(disp_ptr, disp_alloc);
        ::close(disp_fd);
    } else {
        free(disp_ptr);
    }
    if (staging.fd >= 0) {
        rk::dma_buf_unmap(staging.ptr, staging.cap);
        ::close(staging.fd);
    }
    avformat_network_deinit();
    return 0;
}
