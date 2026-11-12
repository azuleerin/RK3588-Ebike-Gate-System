// src/minimal/unit_decoder.cc —— decoder-only 最小化单元
//
// 隔离边界（本单元**只允许**触碰 MPP 解码）:
//   ✅ 允许: FFmpeg 只做 demux(拉流/拆包, 不解码)、MPP 硬解
//   ❌ 禁止: RGA 转换、RKNN 推理、MQTT、串口、测速、画框
//
// 带病复现（D7 批复）—— 本单元**故意不修** P0-2:
//   直接复用业务代码的 src/mpp_decoder.cc。它的 decode() 内部是这样的:
//       out->fd  = mpp_frame_get_fd_compat(frame);
//       out->ptr = mpp_buffer_get_ptr(buf);
//       ...
//       mpp_frame_deinit(&frame);      // ← 帧引用在这里就被丢掉了
//   而调用方(mpp_decoder.h 的契约)拿到 fd/ptr 后还要继续用。
//   也就是说: 我们拿到的是"已经被归还给 MPP buffer group 的内存"的 fd 与指针。
//   本单元照抄这个语义, 不做任何修正 —— 修正了就复现不出死机。
//
// 本单元额外做的观测(只观测, 不改变时序):
//   1) fd 有效性校验: 每帧把 fd 交给 FdLedger::validate
//   2) fd 号复用探测: 记录每帧 fd 的 inode(/proc/self/fdinfo/<fd>), 隔几帧复查
//      —— 如果同一个 fd 号现在指向了**另一个 inode**, 说明这个 fd 号被内核
//      回收并复用给别人了。此时继续拿它去 importbuffer_fd, 就会把别人的内存
//      映射进来并 DMA 写入 -> IOMMU fault -> 内核 oops -> 整机挂死。
//      这正是 P0-1(av_frame_unref 提前释放)的机理, 这里能直接观测到。
//   3) CPU 指针抽样读: 读几个字节, 确认 ptr 仍可访问(不崩)。
//
// 内存物理属性:
//   - 解码帧: MPP 内部 buffer group 分配的 ION/dma-buf(物理连续)
//     申请者: MPP(解码器内部)   释放者: MPP(下一次 decode 时回收复用)
//     生命周期: 跨线程? 否, 只在主线程使用
//     注意: 本单元**不持有** MPP 的帧引用, 这正是被测的"病"
//   - 本单元自己的缓冲: 全部走 MemGuard(canary 保护)

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
}

#include "mpp_decoder.h"
#include "rk/fd_ledger.h"
#include "rk/log.h"
#include "rk/mem_guard.h"
#include "rk/minimal/unit.h"
#include "rk/stack_guard.h"

namespace rk {
namespace minimal {
namespace {

double NowMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 读 /proc/self/fdinfo/<fd> 里的 inode。
// 同一个 fd 号在不同时刻指向不同 inode => 这个 fd 号被关闭并复用了。
// 这就是 P0-1 类 bug 的直接指纹。
uint64_t fd_inode(int fd) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    uint64_t ino = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "ino:", 4) == 0) {
            ino = strtoull(line + 4, nullptr, 10);
            break;
        }
    }
    fclose(f);
    return ino;
}

struct FrameMark {
    int fd = -1;
    uint64_t ino = 0;
    int64_t frame_no = 0;
};

// 帧历史环形队列: 用来隔帧复查"fd 号是否被复用"
struct FrameHistory {
    static constexpr size_t kDepth = 8;
    FrameMark ring[kDepth];
    size_t head = 0;
    size_t count = 0;

    void push(int fd, uint64_t ino, int64_t frame_no) {
        ring[head] = FrameMark{fd, ino, frame_no};
        head = (head + 1) % kDepth;
        if (count < kDepth) ++count;
    }

    // 复查所有还留存的帧: fd 号是否被复用 / fd 是否已消失
    void recheck(int64_t cur_frame) {
        for (size_t i = 0; i < kDepth; ++i) {
            if (ring[i].fd < 0) continue;
            const uint64_t now_ino = fd_inode(ring[i].fd);
            if (now_ino == 0) {
                RK_LOG_ERROR("mpp", "reuse",
                             "帧#%lld 的 fd=%d 已从 /proc 消失 —— 该 fd 被提前关闭了!",
                             static_cast<long long>(ring[i].frame_no), ring[i].fd);
                ring[i].fd = -1;
                continue;
            }
            if (now_ino != ring[i].ino) {
                RK_LOG_ERROR("mpp", "reuse",
                             "★ fd 号被复用! 帧#%lld 的 fd=%d 原 inode=%llu 现在=%llu"
                             " —— 这个 fd 号已被内核回收给别的对象, "
                             "继续 import 它 = 把别人的内存交给 RGA/NPU = 总线挂死",
                             static_cast<long long>(ring[i].frame_no), ring[i].fd,
                             static_cast<unsigned long long>(ring[i].ino),
                             static_cast<unsigned long long>(now_ino));
                ring[i].ino = now_ino;
            }
        }
        (void)cur_frame;
    }
};

// 一个可(重)打开的解码源。
// 为什么要重连: 24h 长跑对着的是一台真实相机, 流会断、会重连。
// 断流后如果直接退出, 长跑就白跑了; 重连后继续才是长跑该有的行为。
// 注意: 重连要重建 MPP 上下文, 因为解码器状态在断流后已不可信。
struct DecSource {
    AVFormatContext* fmt = nullptr;
    AVPacket* pkt = nullptr;
    AVPacket* bpkt = nullptr;
    AVBSFContext* bsf_ctx = nullptr;
    rk::MppDecoder dec;
    int vstream = -1;
    bool hevc = false;
    std::string src;

    void close() {
        av_packet_free(&pkt);
        av_packet_free(&bpkt);
        av_bsf_free(&bsf_ctx);
        bsf_ctx = nullptr;
        pkt = nullptr;
        bpkt = nullptr;
        dec.destroy();
        if (fmt) {
            avformat_close_input(&fmt);
            fmt = nullptr;
        }
    }

    bool open(const std::string& s, std::string& err) {
        src = s;
        int ret = avformat_open_input(&fmt, src.c_str(), nullptr, nullptr);
        if (ret < 0) {
            char e[128];
            av_strerror(ret, e, sizeof(e));
            err = std::string("avformat_open_input: ") + e;
            return false;
        }
        // RTSP 需要超时设置, 否则断线时会一直阻塞在读包上
        fmt->interrupt_callback.callback = nullptr;
        ret = avformat_find_stream_info(fmt, nullptr);
        if (ret < 0) {
            err = "avformat_find_stream_info 失败";
            close();
            return false;
        }
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            const AVCodecParameters* cp = fmt->streams[i]->codecpar;
            if (cp->codec_type == AVMEDIA_TYPE_VIDEO) {
                vstream = static_cast<int>(i);
                hevc = (cp->codec_id == AV_CODEC_ID_HEVC);
                break;
            }
        }
        if (vstream < 0) {
            err = "没有视频流";
            close();
            return false;
        }

        // Annex-B 自适应: MP4 是 AVCC 要转; RTSP 拆包后本来就是 Annex-B。
        // 这只是"让解码能跑起来"的格式适配, 与被测的帧生命周期语义无关。
        bool need_bsf = false;
        const AVCodecParameters* cp = fmt->streams[vstream]->codecpar;
        if (cp->extradata_size >= 4) {
            const uint8_t* e = cp->extradata;
            const bool annexb =
                (e[0] == 0 && e[1] == 0 && (e[2] == 1 || (e[2] == 0 && e[3] == 1)));
            need_bsf = !annexb;
        }
        if (need_bsf) {
            const char* bsf_name = hevc ? "hevc_mp4toannexb" : "h264_mp4toannexb";
            const AVBitStreamFilter* b = av_bsf_get_by_name(bsf_name);
            if (b) {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 0, 100)
                if (av_bsf_alloc(b, &bsf_ctx) < 0) bsf_ctx = nullptr;
#else
                bsf_ctx = av_bsf_alloc(b);
#endif
                if (bsf_ctx) {
                    avcodec_parameters_copy(bsf_ctx->par_in, cp);
                    bsf_ctx->time_base_in = fmt->streams[vstream]->time_base;
                    if (av_bsf_init(bsf_ctx) < 0) {
                        av_bsf_free(&bsf_ctx);
                        bsf_ctx = nullptr;
                    }
                }
            }
        }
        RK_LOG_INFO("dec", "open", "视频流 #%d 编码=%s %dx%d Annex-B转换=%s", vstream,
                    hevc ? "HEVC/H.265" : "H.264", cp->width, cp->height,
                    bsf_ctx ? "启用" : "跳过");

        if (!dec.init(hevc, cp->width, cp->height)) {
            err = std::string("MppDecoder 初始化失败: ") + dec.last_error();
            close();
            return false;
        }
        RK_LOG_INFO("dec", "init", "MppDecoder 就绪 (HEVC=%d)", hevc ? 1 : 0);

        pkt = av_packet_alloc();
        bpkt = av_packet_alloc();
        if (!pkt || !bpkt) {
            err = "av_packet_alloc 失败";
            close();
            return false;
        }
        return true;
    }
};

}  // namespace

int run_decoder_unit(const UnitOptions& opt) {
    RK_LOG_INFO("dec", "start",
                "===== decoder-only 单元启动 (只触碰 MPP, 不含 RGA/RKNN) =====");
    RK_LOG_INFO("dec", "start", "输入源=%s", opt.src.c_str());

    DecSource src;
    std::string err;
    if (!src.open(opt.src, err)) {
        RK_LOG_ERROR("dec", "open", "打开输入失败: %s", err.c_str());
        return 1;
    }
    // 采集 fd 基线: 相机连接 fd / /dev/mpp_service / MPP 的 dmabuf 都已打开,
    // 采集后它们不再被报成"新增未登记"。
    FdLedger::capture_baseline();

    // ---------------- 解码主循环 ----------------
    TimingStat t_dec;
    uint64_t frames = 0;
    uint64_t packets = 0;
    uint64_t reconnects = 0;
    uint64_t reuse_events = 0;
    const auto t_begin = std::chrono::steady_clock::now();
    auto last_audit = std::chrono::steady_clock::now();
    FrameHistory hist;
    bool stop = false;

    RK_LOG_INFO("dec", "loop", "开始解码循环");

    while (!stop) {
        StackGuard::mark_stack();

        const int ret = av_read_frame(src.fmt, src.pkt);
        if (ret < 0) {
            if (!opt.loop) {
                RK_LOG_INFO("dec", "loop", "输入结束");
                break;
            }
            // 断流/文件末尾 -> 重连
            ++reconnects;
            RK_LOG_WARN("dec", "loop", "输入中断(第 %llu 次), 重新连接 %s",
                        static_cast<unsigned long long>(reconnects), opt.src.c_str());
            src.close();
            std::string e2;
            int tries = 0;
            while (!src.open(opt.src, e2)) {
                ++tries;
                RK_LOG_ERROR("dec", "loop", "重连失败(%d): %s", tries, e2.c_str());
                if (tries >= 10) {
                    RK_LOG_ERROR("dec", "loop", "连续重连失败 10 次, 放弃");
                    stop = true;
                    break;
                }
                usleep(3 * 1000 * 1000);
            }
            if (stop) break;
            continue;
        }
        if (src.pkt->stream_index != src.vstream) {
            av_packet_unref(src.pkt);
            continue;
        }
        ++packets;

        // Annex-B 转换(仅 MP4 这类需要)
        if (src.bsf_ctx) {
            if (av_bsf_send_packet(src.bsf_ctx, src.pkt) < 0) {
                av_packet_unref(src.pkt);
                continue;
            }
            while (av_bsf_receive_packet(src.bsf_ctx, src.bpkt) == 0) {
                const double d0 = NowMs();
                rk::Nv12Frame fi;
                if (src.dec.decode(src.bpkt->data, src.bpkt->size, src.bpkt->pts, &fi)) {
                    const double d1 = NowMs();

                    // ---- 观测 1: fd 有效性(只记录不阻断)----
                    if (fi.fd >= 0) {
                        const size_t need =
                            static_cast<size_t>(fi.wstride) * fi.hstride * 3 / 2;
                        if (!FdLedger::validate(fi.fd, need, "mpp-frame")) {
                            RK_LOG_ERROR("dec", "use", "帧 fd 校验未通过 (仅记录, 不阻断)");
                        }
                    }

                    // ---- 观测 2: CPU 指针抽样读(模拟 CPU 兜底路径)----
                    if (fi.ptr) {
                        volatile uint8_t s0 = static_cast<const uint8_t*>(fi.ptr)[0];
                        volatile uint8_t s1 =
                            static_cast<const uint8_t*>(fi.ptr)[fi.wstride * fi.hstride];
                        (void)s0;
                        (void)s1;
                    }

                    // ---- 观测 3: fd 号复用探测 ----
                    const uint64_t ino = (fi.fd >= 0) ? fd_inode(fi.fd) : 0;
                    hist.push(fi.fd, ino, static_cast<int64_t>(frames));
                    hist.recheck(static_cast<int64_t>(frames));

                    t_dec.push(d1 - d0);
                    ++frames;
                    StackGuard::beat();

                    if (frames % 500 == 0) {
                        RK_LOG_INFO("dec", "run",
                                    "frame=%llu packets=%llu 解码=%.2fms 平均FPS=%.1f",
                                    static_cast<unsigned long long>(frames),
                                    static_cast<unsigned long long>(packets), t_dec.mean(),
                                    1000.0 / t_dec.mean());
                    }
                    const auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_audit)
                            .count() >= opt.stats_interval) {
                        last_audit = now;
                        char why[64];
                        snprintf(why, sizeof(why), "decoder-only 第 %llu 帧",
                                 static_cast<unsigned long long>(frames));
                        audit_now(why);
                    }
                    if (opt.frames > 0 && frames >= static_cast<uint64_t>(opt.frames)) stop = true;
                    if (opt.duration_sec > 0 &&
                        std::chrono::duration_cast<std::chrono::seconds>(now - t_begin).count() >=
                            opt.duration_sec) {
                        stop = true;
                    }
                }
                av_packet_unref(src.bpkt);
            }
            av_packet_unref(src.pkt);
        } else {
            const double d0 = NowMs();
            rk::Nv12Frame fi;
            if (src.dec.decode(src.pkt->data, src.pkt->size, src.pkt->pts, &fi)) {
                const double d1 = NowMs();

                if (fi.fd >= 0) {
                    const size_t need = static_cast<size_t>(fi.wstride) * fi.hstride * 3 / 2;
                    if (!FdLedger::validate(fi.fd, need, "mpp-frame")) {
                        RK_LOG_ERROR("dec", "use", "帧 fd 校验未通过 (仅记录, 不阻断)");
                    }
                }
                if (fi.ptr) {
                    volatile uint8_t s0 = static_cast<const uint8_t*>(fi.ptr)[0];
                    volatile uint8_t s1 =
                        static_cast<const uint8_t*>(fi.ptr)[fi.wstride * fi.hstride];
                    (void)s0;
                    (void)s1;
                }
                const uint64_t ino = (fi.fd >= 0) ? fd_inode(fi.fd) : 0;
                hist.push(fi.fd, ino, static_cast<int64_t>(frames));
                hist.recheck(static_cast<int64_t>(frames));

                t_dec.push(d1 - d0);
                ++frames;
                StackGuard::beat();

                if (frames % 500 == 0) {
                    RK_LOG_INFO("dec", "run",
                                "frame=%llu packets=%llu 解码=%.2fms 平均FPS=%.1f",
                                static_cast<unsigned long long>(frames),
                                static_cast<unsigned long long>(packets), t_dec.mean(),
                                1000.0 / t_dec.mean());
                }
                const auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - last_audit).count() >=
                    opt.stats_interval) {
                    last_audit = now;
                    char why[64];
                    snprintf(why, sizeof(why), "decoder-only 第 %llu 帧",
                             static_cast<unsigned long long>(frames));
                    audit_now(why);
                }
                if (opt.frames > 0 && frames >= static_cast<uint64_t>(opt.frames)) stop = true;
                if (opt.duration_sec > 0 &&
                    std::chrono::duration_cast<std::chrono::seconds>(now - t_begin).count() >=
                        opt.duration_sec) {
                    stop = true;
                }
            }
            av_packet_unref(src.pkt);
        }
    }

    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t_begin).count();

    RK_LOG_INFO("dec", "done", "===== decoder-only 结束 =====");
    RK_LOG_INFO("dec", "done", "帧数=%llu 包数=%llu 重连=%llu 用时=%.1fs 平均FPS=%.2f",
                static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(packets),
                static_cast<unsigned long long>(reconnects), elapsed,
                elapsed > 0 ? static_cast<double>(frames) / elapsed : 0.0);
    RK_LOG_INFO("dec", "done", "解码耗时 mean=%.3fms min=%.3f p50=%.3f p90=%.3f max=%.3f",
                t_dec.mean(), t_dec.min, t_dec.pct(50), t_dec.pct(90), t_dec.max);
    RK_LOG_INFO("dec", "done", "fd 号复用事件=%llu",
                static_cast<unsigned long long>(reuse_events));

    audit_now("decoder-only 结束前");

    src.close();
    RK_LOG_INFO("dec", "done", "清理完成, 未触发死机");
    return 0;
}


}  // namespace minimal
}  // namespace rk
