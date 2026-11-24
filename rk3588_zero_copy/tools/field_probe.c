/*
 * field_probe.c —— 现场预检用的【流信息探针 + 单帧抓取器】
 *
 * 为什么要自己写一个:
 *   现场的板子上不一定装了 ffmpeg/ffprobe(本板就没装, 见 tools/build_field_probe.sh
 *   的说明), 但本工程【本来就依赖 libavformat/libavcodec/libswscale】(CMakeLists.txt:182-199),
 *   头文件和库都在。所以与其让现场工程师卡在"apt install ffmpeg 要 root"上,
 *   不如用工程自己已有的依赖做一个零额外依赖的探针。
 *
 * ★ 输出的 key=value 格式【故意与 ffprobe -of default=noprint_wrappers=1 对齐】,
 *   这样 scripts/field_preflight.sh 里只有【一套解析逻辑】, ffprobe 在场时用 ffprobe,
 *   不在场时用本程序, 两边行为完全一致 —— 不会出现"换了台板子解析结果就变了"。
 *
 * 用法:
 *   field_probe probe <url>              打印流信息(key=value, 一行一个)
 *   field_probe grab  <url> <out.jpg>    解码第一帧, 按【原始分辨率】写成 JPEG
 *   field_probe version                  打印版本
 *
 * 编译:
 *   tools/build_field_probe.sh            (自动用 pkg-config 找 libav*)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <inttypes.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>

#define FIELD_PROBE_VERSION "1.0"

/* ------------------------------------------------------------------ */
/* 超时: RTSP 打开时如果对方不响应, 不能无限期挂住现场预检          */
/* ------------------------------------------------------------------ */
typedef struct {
    int64_t deadline_us;   /* 绝对截止时间(微秒) */
} ProbeTimeout;

/*
 * ★ 为什么是【全局】而不是 open_input() 里的局部变量:
 *   interrupt_callback.opaque 指向的是一个结构体, 而 avformat_open_input()
 *   返回之后这个结构体必须【还活着】—— 之后每次 av_read_frame() 都会回调它。
 *   早先把它写成局部变量, open_input() 一返回就是悬垂指针, 回调读到垃圾数据
 *   会返回 1(要中断), 于是紧接着的 av_read_frame() 立刻失败退出。
 *   本工具是单线程的, 用全局一份最简单也最安全。
 */
static ProbeTimeout g_timeout;

static int interrupt_cb(void* opaque)
{
    ProbeTimeout* t = (ProbeTimeout*)opaque;
    if (!t) return 0;
    return (av_gettime_relative() >= t->deadline_us) ? 1 : 0;
}

/* 刷新一次超时窗口 —— 打开阶段和抓帧阶段各用各自的时间预算 */
static void timeout_refresh(AVFormatContext* ctx, int timeout_s)
{
    g_timeout.deadline_us = av_gettime_relative() + (int64_t)timeout_s * 1000000;
    if (ctx) {
        ctx->interrupt_callback.callback = &interrupt_cb;
        ctx->interrupt_callback.opaque = &g_timeout;
    }
}

/* ------------------------------------------------------------------ */
/* 把 FFmpeg 的 AVDictionary 填上 RTSP/超时相关选项                  */
/* ------------------------------------------------------------------ */
/*
 * ★ 关键: 这些选项【只对网络源】打开。
 *   实测踩过: 给本地 MP4 也加 "fflags=nobuffer" 时, mov 解复用器不再预读,
 *   解码器前 30 个包一个帧都吐不出来(recv 恒为 EAGAIN), 抓帧直接失败;
 *   去掉这个选项后第 5 个包就出帧了。
 *   所以: 网络源加超时/传输层选项, 本地文件一律用 demuxer 的默认行为。
 */
static int is_network_url(const char* url)
{
    static const char* schemes[] = {
        "rtsp://", "rtsps://", "rtmp://", "rtmps://", "http://", "https://",
        "tcp://", "udp://", "srt://", "srtp://", NULL
    };
    int i;
    if (!url) return 0;
    for (i = 0; schemes[i]; ++i) {
        size_t n = strlen(schemes[i]);
        if (strncasecmp(url, schemes[i], n) == 0) return 1;
    }
    return 0;
}

static void fill_opts(AVDictionary** opts, int timeout_s, const char* transport)
{
    char buf[64];
    av_dict_set(opts, "rtsp_transport", transport, 0);
    av_dict_set(opts, "stimeout",       "5000000", 0);          /* socket 超时 5s */
    av_dict_set(opts, "max_delay",      "500000",  0);
    snprintf(buf, sizeof(buf), "%d", timeout_s * 1000000);
    av_dict_set(opts, "timeout", buf, 0);
}

/* ------------------------------------------------------------------ */
/* rate 字符串: "30/1" -> 30.0 ; 取不到 -> 0.0                        */
/* ------------------------------------------------------------------ */
static double rate_to_double(AVRational r)
{
    if (r.num == 0 || r.den == 0) return 0.0;
    return (double)r.num / (double)r.den;
}

/*
 * 打开输入 + 找最佳视频流。
 * 返回 0 成功; 失败时 stderr 打印人类可读的原因。
 */
static int open_input(AVFormatContext** fmt_ctx, int* video_stream,
                      const char* url, int timeout_s, const char* transport)
{
    AVDictionary* opts = NULL;
    int ret;

    *fmt_ctx = NULL;
    *video_stream = -1;

    g_timeout.deadline_us = av_gettime_relative() + (int64_t)timeout_s * 1000000;

    /*
     * ★ 与生产 gate_main.cc:678-683 同一套写法: 【先】alloc 出 context 并挂上
     *   interrupt_callback, 【再】调 avformat_open_input ——
     *   这样连"打开过程本身"(要读头/探测流)也能被超时打断,
     *   不会在现场遇到一个不响应的 RTSP 源时把预检脚本挂死。
     */
    *fmt_ctx = avformat_alloc_context();
    if (!*fmt_ctx) {
        fprintf(stderr, "field_probe: 分配 AVFormatContext 失败\n");
        av_dict_free(&opts);
        return -1;
    }
    (*fmt_ctx)->interrupt_callback.callback = &interrupt_cb;
    (*fmt_ctx)->interrupt_callback.opaque = &g_timeout;

    if (is_network_url(url)) {
        fill_opts(&opts, timeout_s, transport);
    }
    ret = avformat_open_input(fmt_ctx, url, NULL, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, err, sizeof(err));
        fprintf(stderr, "field_probe: 打不开输入 %s: %s\n", url, err);
        avformat_close_input(fmt_ctx);
        return -1;
    }

    ret = avformat_find_stream_info(*fmt_ctx, NULL);
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, err, sizeof(err));
        fprintf(stderr, "field_probe: 读流信息失败 %s: %s\n", url, err);
        avformat_close_input(fmt_ctx);
        return -1;
    }

    ret = av_find_best_stream(*fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (ret < 0) {
        fprintf(stderr, "field_probe: %s 里没有视频流\n", url);
        avformat_close_input(fmt_ctx);
        return -1;
    }
    *video_stream = ret;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 打印 key=value —— 与 ffprobe 的字段名一一对应                     */
/* ------------------------------------------------------------------ */
/*
 * ★ FFmpeg 5.0 起 AVCodecParameters 不再带 pix_fmt / codec_name,
 *   profile 的取名函数也变成了两参数版本(要传 AVCodec*)。
 *   所以这里走 "解码器上下文" 这条路拿真实像素格式 ——
 *   比只看 codecpar 更准(有些封装会把格式藏在别处)。
 */
static void print_stream_info(AVFormatContext* fmt_ctx, int vs)
{
    AVCodecParameters* par = fmt_ctx->streams[vs]->codecpar;
    const AVCodec* dec_codec = avcodec_find_decoder(par->codec_id);
    const char* prof = NULL;
    const char* pix = NULL;
    const char* cname = avcodec_get_name(par->codec_id);
    int bits = par->bits_per_raw_sample;

    if (dec_codec) {
        AVCodecContext* tmp = avcodec_alloc_context3(dec_codec);
        if (tmp) {
            if (avcodec_parameters_to_context(tmp, par) >= 0) {
                if (tmp->pix_fmt != AV_PIX_FMT_NONE) {
                    pix = av_get_pix_fmt_name((enum AVPixelFormat)tmp->pix_fmt);
                }
                if (tmp->bits_per_raw_sample > 0) bits = tmp->bits_per_raw_sample;
            }
            avcodec_free_context(&tmp);
        }
        prof = av_get_profile_name(dec_codec, par->profile);
    }

    printf("codec_name=%s\n", cname ? cname : "unknown");
    printf("profile=%s\n", (prof && prof[0]) ? prof : "unknown");
    printf("width=%d\n", par->width);
    printf("height=%d\n", par->height);
    printf("pix_fmt=%s\n", (pix && pix[0]) ? pix : "unknown");
    printf("bits_per_raw_sample=%d\n", bits);
    printf("avg_frame_rate=%d/%d\n", fmt_ctx->streams[vs]->avg_frame_rate.num,
                                  fmt_ctx->streams[vs]->avg_frame_rate.den);
    printf("r_frame_rate=%d/%d\n", fmt_ctx->streams[vs]->r_frame_rate.num,
                                fmt_ctx->streams[vs]->r_frame_rate.den);
    printf("nb_streams=%d\n", (int)fmt_ctx->nb_streams);
    printf("format_name=%s\n", fmt_ctx->iformat->name ? fmt_ctx->iformat->name : "unknown");
    if (fmt_ctx->duration != AV_NOPTS_VALUE) {
        printf("duration=%.3f\n", (double)fmt_ctx->duration / AV_TIME_BASE);
    } else {
        printf("duration=N/A\n");
    }
}

/* ------------------------------------------------------------------ */
/* probe 子命令                                                       */
/* ------------------------------------------------------------------ */
static int cmd_probe(const char* url, int timeout_s, const char* transport)
{
    AVFormatContext* fmt_ctx = NULL;
    int vs = -1;

    if (open_input(&fmt_ctx, &vs, url, timeout_s, transport) != 0) return 1;

    print_stream_info(fmt_ctx, vs);
    /* 顺带把帧率算成浮点, 方便脚本直接判"这一路是不是只有几帧" */
    printf("avg_fps=%.3f\n", rate_to_double(fmt_ctx->streams[vs]->avg_frame_rate));
    printf("r_fps=%.3f\n",   rate_to_double(fmt_ctx->streams[vs]->r_frame_rate));

    avformat_close_input(&fmt_ctx);
    return 0;
}

/* ------------------------------------------------------------------ */
/* grab 子命令: 解第一帧 -> YUVJ420P -> MJPEG 落盘                    */
/* ------------------------------------------------------------------ */
/*
 * ★ 为什么直接转 YUVJ420P 而不是先转 RGB24 再给编码器:
 *   MJPEG 编码器只吃 YUVJ420P。把 RGB24 的帧直接喂给它, 编码器内部会
 *   按【自己的】格式解释这块内存 —— 轻则花屏, 重则直接段错误
 *   (本项目实测: 段错误, exit 139)。
 *   所以这里一步到位: 解码帧 -> sws 缩放成 YUVJ420P -> 送编码器。
 */
static int cmd_grab(const char* url, const char* out_path,
                    int timeout_s, const char* transport)
{
    AVFormatContext* fmt_ctx = NULL;
    AVCodecContext* dec = NULL;
    const AVCodec* dec_codec = NULL;
    AVCodecContext* enc = NULL;
    const AVCodec* enc_codec = NULL;
    AVFrame* frame = NULL;
    AVPacket* pkt = NULL;
    struct SwsContext* sws = NULL;
    AVFrame* yuv = NULL;
    int vs = -1;
    int ret = -1;
    int got_frame = 0;
    int wrote = 0;
    long grab_bytes = 0;

    if (open_input(&fmt_ctx, &vs, url, timeout_s, transport) != 0) return 1;

    /*
     * ★ 本地文件: avformat_find_stream_info() 已经消耗掉了开头若干个包,
     *   这里【seek 回第一个关键帧】, 保证抓到的是视频的第一帧 ——
     *   IPM 拾点需要一张可复现的基准图, 不能每次抓到的都不一样。
     *   网络源(RTSP)不支持随机访问, 跳过。
     */
    if (!is_network_url(url)) {
        av_seek_frame(fmt_ctx, vs, 0, AVSEEK_FLAG_BACKWARD);
    }

    print_stream_info(fmt_ctx, vs);
    printf("avg_fps=%.3f\n", rate_to_double(fmt_ctx->streams[vs]->avg_frame_rate));
    printf("r_fps=%.3f\n",   rate_to_double(fmt_ctx->streams[vs]->r_frame_rate));

    dec_codec = avcodec_find_decoder(fmt_ctx->streams[vs]->codecpar->codec_id);
    if (!dec_codec) {
        fprintf(stderr, "field_probe: 没有该格式的解码器\n");
        goto done;
    }
    dec = avcodec_alloc_context3(dec_codec);
    if (!dec) goto done;
    if (avcodec_parameters_to_context(dec, fmt_ctx->streams[vs]->codecpar) < 0) goto done;
    if (avcodec_open2(dec, dec_codec, NULL) < 0) {
        fprintf(stderr, "field_probe: 解码器打开失败\n");
        goto done;
    }

    frame = av_frame_alloc();
    pkt   = av_packet_alloc();
    yuv   = av_frame_alloc();
    if (!frame || !pkt || !yuv) goto done;

    /*
     * ---- 解出第一帧 ----
     * ★ 注意: 有些容器(RTSP 重连、MP4 的 B 帧重排)开头几帧会缺参考帧,
     *   解码器会报 "reference picture missing" 然后跳过。所以这里要
     *   【连续读多帧直到拿到一帧真的解出来的】, 而不是只试第一帧。
     */
    timeout_refresh(fmt_ctx, timeout_s);   /* 抓帧阶段用独立的超时预算 */
    {
        int tries = 0;
        const int max_tries = 120;   /* 最多读 120 个包, 够跨过几帧重排 */
        while (!got_frame && tries < max_tries) {
            int r = av_read_frame(fmt_ctx, pkt);
            if (r < 0) {
                fprintf(stderr, "field_probe: 读帧失败/超时(已读 %d 个包)\n", tries);
                goto done;
            }
            tries++;
            if (pkt->stream_index == vs) {
                if (avcodec_send_packet(dec, pkt) == 0) {
                    int rr = avcodec_receive_frame(dec, frame);
                    if (rr == 0) got_frame = 1;
                    else if (rr != AVERROR(EAGAIN) && rr != AVERROR_EOF) {
                        /* 这一帧解不出来(缺参考帧之类), 继续试下一帧 */
                    }
                }
            }
            av_packet_unref(pkt);
        }
    }
    if (!got_frame) {
        fprintf(stderr, "field_probe: 解码没有拿到任何一帧\n");
        goto done;
    }

    printf("grab_width=%d\n", frame->width);
    printf("grab_height=%d\n", frame->height);
    printf("grab_pix_fmt=%s\n", av_get_pix_fmt_name((enum AVPixelFormat)frame->format));

    /* ---- 转 YUVJ420P(原始分辨率, 不缩放) ---- */
    sws = sws_getContext(frame->width, frame->height, (enum AVPixelFormat)frame->format,
                         frame->width, frame->height, AV_PIX_FMT_YUVJ420P,
                         SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) {
        fprintf(stderr, "field_probe: sws 上下文创建失败\n");
        goto done;
    }
    yuv->format = AV_PIX_FMT_YUVJ420P;
    yuv->width  = frame->width;
    yuv->height = frame->height;
    if (av_frame_get_buffer(yuv, 32) < 0) goto done;
    sws_scale(sws, (const uint8_t* const*)frame->data, frame->linesize,
              0, frame->height, yuv->data, yuv->linesize);

    /* ---- 编码成 JPEG(原始分辨率, 不缩放) ---- */
    enc_codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!enc_codec) {
        fprintf(stderr, "field_probe: 找不到 MJPEG 编码器\n");
        goto done;
    }
    enc = avcodec_alloc_context3(enc_codec);
    if (!enc) goto done;
    enc->width      = yuv->width;
    enc->height     = yuv->height;
    enc->pix_fmt    = AV_PIX_FMT_YUVJ420P;
    enc->time_base  = (AVRational){1, 25};
    enc->color_range = AVCOL_RANGE_JPEG;
    enc->flags      = AV_CODEC_FLAG_QSCALE;
    {
        /* 画质优先 —— IPM 拾点要看清标线, 不能让 JPEG 压缩把标线糊掉 */
        AVDictionary* enc_opts = NULL;
        av_dict_set(&enc_opts, "q", "2", 0);
        int r = avcodec_open2(enc, enc_codec, &enc_opts);
        av_dict_free(&enc_opts);
        if (r < 0) {
            fprintf(stderr, "field_probe: MJPEG 编码器打开失败\n");
            goto done;
        }
    }
    if (avcodec_send_frame(enc, yuv) < 0) goto done;
    while (avcodec_receive_packet(enc, pkt) == 0) {
        FILE* fp = fopen(out_path, "wb");
        if (!fp) {
            fprintf(stderr, "field_probe: 写不了 %s\n", out_path);
            goto done;
        }
        fwrite(pkt->data, 1, pkt->size, fp);
        fclose(fp);
        grab_bytes = pkt->size;
        wrote = 1;
        av_packet_unref(pkt);
        break;
    }
    if (!wrote) {
        fprintf(stderr, "field_probe: 编码没有产出数据\n");
        goto done;
    }
    printf("grab_file=%s\n", out_path);
    printf("grab_bytes=%ld\n", (long)grab_bytes);
    ret = 0;

done:
    if (sws) sws_freeContext(sws);
    if (frame) av_frame_free(&frame);
    if (yuv)   av_frame_free(&yuv);
    if (pkt)   av_packet_free(&pkt);
    if (dec)   avcodec_free_context(&dec);
    if (enc)   avcodec_free_context(&enc);
    if (fmt_ctx) avformat_close_input(&fmt_ctx);
    return ret;
}

/* ------------------------------------------------------------------ */
int main(int argc, char** argv)
{
    const char* cmd;
    int timeout_s = 15;
    const char* transport = "tcp";

    if (argc < 2) {
        fprintf(stderr,
                "用法:\n"
                "  field_probe probe <url>\n"
                "  field_probe grab  <url> <out.jpg>\n"
                "  field_probe version\n");
        return 2;
    }
    cmd = argv[1];

    if (!strcmp(cmd, "version")) {
        printf("field_probe=%s\n", FIELD_PROBE_VERSION);
        printf("libavformat=%d.%d.%d\n", LIBAVFORMAT_VERSION_MAJOR,
               LIBAVFORMAT_VERSION_MINOR, LIBAVFORMAT_VERSION_MICRO);
        printf("libavcodec=%d.%d.%d\n", LIBAVCODEC_VERSION_MAJOR,
               LIBAVCODEC_VERSION_MINOR, LIBAVCODEC_VERSION_MICRO);
        return 0;
    }
    if (argc < 3) {
        fprintf(stderr, "field_probe: 缺少 URL\n");
        return 2;
    }
    if (argc >= 4 && !strcmp(cmd, "grab")) {
        return cmd_grab(argv[2], argv[3], timeout_s, transport) == 0 ? 0 : 1;
    }
    if (!strcmp(cmd, "probe")) {
        return cmd_probe(argv[2], timeout_s, transport) == 0 ? 0 : 1;
    }

    fprintf(stderr, "field_probe: 未知子命令 %s\n", cmd);
    return 2;
}
