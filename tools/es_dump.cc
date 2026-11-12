// tools/es_dump.cc —— 把 RTSP/MP4 的 H.264 码流录成 Annex-B 裸流
//
// 用途（step/05 引爆测试）:
//   我们需要一份"能反复改变分辨率"的 H.264 码流, 用来逼 MPP 做 info-change
//   重建 buffer group(这是 fd 被关闭/复用的唯一时机)。
//   相机不可控、板上没有 H.264 编码器, 所以走"码流层"路线:
//     1) 本工具把相机的码流录成 Annex-B 裸流(.h264)
//     2) sps_flip.py 就地改写 SPS 里的分辨率字段(比特长度不变, 其余不动)
//     3) 拼成"分辨率来回跳"的码流, 喂给业务二进制
//
// 用法: es_dump <输入(rtsp 或文件)> <输出.h264> [最大帧数]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
}


// 包内是否含 IDR(type 5) —— IDR 可能在包的中间, 不能只看首字节
static bool buf_has_idr(const uint8_t* d, int len) {
    for (int i = 0; i + 4 < len; ++i) {
        if (d[i] == 0 && d[i + 1] == 0) {
            int sl = 0, hdr = -1;
            if (d[i + 2] == 1) { sl = 3; hdr = i + 3; }
            else if (d[i + 2] == 0 && d[i + 3] == 1) { sl = 4; hdr = i + 4; }
            if (hdr > 0 && hdr < len && (d[hdr] & 0x1F) == 5) return true;
        }
    }
    return false;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "用法: %s <输入> <输出.h264> [最大帧数]\n", argv[0]);
        return 2;
    }
    const char* in_path = argv[1];
    const char* out_path = argv[2];
    const int max_packets = (argc > 3) ? atoi(argv[3]) : 2000;
    // --mp4: 输出 avc3 格式的 MP4(SPS/PPS 内嵌在码流里, 不是存在 extradata)
    //       avc3 的好处: ①每个 IDR 前都有 SPS/PPS, MPP 能直接吃;
    //                    ②SPS 在码流里, 就能逐 GOP 改写分辨率做 info-change。
    bool mp4_out = false;
    bool no_bsf = false;
    bool gop_group = false;
    bool ts_out = false;
    bool aus_out = false;
    for (int i = 4; i < argc; ++i) {
        if (!strcmp(argv[i], "--mp4")) mp4_out = true;
        else if (!strcmp(argv[i], "--no-bsf")) no_bsf = true;
        else if (!strcmp(argv[i], "--gop")) gop_group = true;
        else if (!strcmp(argv[i], "--ts")) ts_out = true;
        else if (!strcmp(argv[i], "--aus")) aus_out = true;
    }

    AVFormatContext* fmt = nullptr;
    int ret = avformat_open_input(&fmt, in_path, nullptr, nullptr);
    if (ret < 0) {
        char e[128];
        av_strerror(ret, e, sizeof(e));
        fprintf(stderr, "[es_dump] 打开失败: %s\n", e);
        return 1;
    }
    avformat_find_stream_info(fmt, nullptr);

    int vstream = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            vstream = static_cast<int>(i);
            break;
        }
    }
    if (vstream < 0) {
        fprintf(stderr, "[es_dump] 没有视频流\n");
        return 1;
    }
    const AVCodecParameters* cp = fmt->streams[vstream]->codecpar;
    fprintf(stderr, "[es_dump] 源: %s %dx%d 编码=%d\n", in_path, cp->width, cp->height,
            cp->codec_id);

    // Annex-B 转换
    const char* bsf_name = (cp->codec_id == AV_CODEC_ID_HEVC) ? "hevc_mp4toannexb"
                                                              : "h264_mp4toannexb";
    // ⚠️ 关键: RTSP 出来的包【本来就是 Annex-B】。
    //    h264_mp4toannexb 是给 AVCC(长度前缀) 输入用的, 喂 Annex-B 会把
    //    SPS 解析坏(FFmpeg 报 "sps_id 0 out of range"), MPP 就再也解不出帧。
    //    所以默认对 RTSP 源跳过 BSF。
    const AVBitStreamFilter* b = (mp4_out || no_bsf) ? nullptr : av_bsf_get_by_name(bsf_name);
    AVBSFContext* bsf_ctx = nullptr;
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
    fprintf(stderr, "[es_dump] Annex-B 转换: %s\n",
            bsf_ctx ? "启用" : (mp4_out ? "不转换(直接写原包, 已是 Annex-B)" : "直接写原包"));

    AVFormatContext* oc = nullptr;
    AVStream* ost = nullptr;
    FILE* out = nullptr;
    if (aus_out) {
        out = fopen(out_path, "wb");
        if (!out) { fprintf(stderr, "[es_dump] 打不开 %s\n", out_path); return 1; }
        fprintf(stderr, "[es_dump] 输出访问单元列表(4字节长度前缀)\n");
    } else if (ts_out) {
        // MPEG-TS: mpegts demuxer 不做 h264 解析, 原样吐 PES 负载,
        // 于是 MPP 能拿到带 SPS 的访问单元(裸 .h264 走 h264 parser 会被剥掉 SPS)。
        avformat_alloc_output_context2(&oc, nullptr, "mpegts", out_path);
        if (!oc) { fprintf(stderr, "[es_dump] 建 TS 容器失败\n"); return 1; }
        ost = avformat_new_stream(oc, nullptr);
        avcodec_parameters_copy(ost->codecpar, cp);
        ost->time_base = fmt->streams[vstream]->time_base;
        if (avio_open(&oc->pb, out_path, AVIO_FLAG_WRITE) < 0) {
            fprintf(stderr, "[es_dump] 打不开 %s\n", out_path);
            return 1;
        }
        if (avformat_write_header(oc, nullptr) < 0) {
            fprintf(stderr, "[es_dump] 写 TS 头失败\n");
            return 1;
        }
        fprintf(stderr, "[es_dump] 输出 MPEG-TS\n");
    } else if (mp4_out) {
        avformat_alloc_output_context2(&oc, nullptr, "mp4", out_path);
        if (!oc) {
            fprintf(stderr, "[es_dump] 建 MP4 容器失败\n");
            return 1;
        }
        ost = avformat_new_stream(oc, nullptr);
        avcodec_parameters_copy(ost->codecpar, cp);
        // ★ 必须用 avc3: 表示"码流内嵌 SPS/PPS"(Annex-B),
        //   而不是 avc1 的"extradata 模式"。我们写进去的就是 Annex-B 包,
        //   用 avc1 会被 muxer 当成 AVCC 处理, 写出来是坏的。
        ost->codecpar->codec_tag = MKTAG('a', 'v', 'c', '3');
        ost->time_base = fmt->streams[vstream]->time_base;
        if (avio_open(&oc->pb, out_path, AVIO_FLAG_WRITE) < 0) {
            fprintf(stderr, "[es_dump] 打不开 %s\n", out_path);
            return 1;
        }
        if (avformat_write_header(oc, nullptr) < 0) {
            fprintf(stderr, "[es_dump] 写 MP4 头失败\n");
            return 1;
        }
        fprintf(stderr, "[es_dump] 输出 MP4(avc3, SPS/PPS 内嵌)\n");
    } else {
        out = fopen(out_path, "wb");
        if (!out) {
            fprintf(stderr, "[es_dump] 打不开输出 %s\n", out_path);
            return 1;
        }
    }

    AVPacket* pkt = av_packet_alloc();
    AVPacket* bpkt = av_packet_alloc();
    int written = 0;
    int64_t bytes = 0;

    // GOP 归并缓冲: 相机逐 RTP 包发送, SPS/PPS/IDR 分散在不同包里;
    // FFmpeg 的 h264 demuxer 在这个码流上解析是坏的(misdetection), MPP 因此
    // 永远见不到 SPS -> 解不出帧。
    // 所以这里自己把 NAL 归并: 一个 GOP(SPS+PPS+IDR+slice...)攒成一个 sample,
    // FFmpeg 的 mp4 demuxer 原样吐出来, MPP 就能正常解。
    std::string gop_buf;
    bool gop_started = false;

    for (int i = 0; i < max_packets; ++i) {
        if (av_read_frame(fmt, pkt) < 0) break;
        if (pkt->stream_index != vstream) {
            av_packet_unref(pkt);
            continue;
        }
        AVPacket* src = pkt;
        int n = 0;
        if (bsf_ctx) {
            if (av_bsf_send_packet(bsf_ctx, pkt) < 0) {
                av_packet_unref(pkt);
                continue;
            }
            while (av_bsf_receive_packet(bsf_ctx, bpkt) == 0) {
                if (gop_group) {
                    const bool is_idr = buf_has_idr(bpkt->data, bpkt->size);
                    if (gop_started && is_idr) {
                        // 上一组结束, 冲出去
                        AVPacket* gp = av_packet_alloc();
                        av_new_packet(gp, gop_buf.size());
                        memcpy(gp->data, gop_buf.data(), gop_buf.size());
                        gp->stream_index = ost ? ost->index : 0;
                        if (mp4_out) {
                            av_packet_rescale_ts(gp, fmt->streams[vstream]->time_base,
                                                 ost->time_base);
                            av_interleaved_write_frame(oc, gp);
                        } else {
                            fwrite(gop_buf.data(), 1, gop_buf.size(), out);
                        }
                        bytes += gop_buf.size();
                        ++written;
                        gop_buf.clear();
                    }
                    gop_buf.append((const char*)bpkt->data, bpkt->size);
                    gop_started = true;
                    av_packet_unref(bpkt);
                    continue;
                }
                if (aus_out) {
                    uint32_t l = (uint32_t)bpkt->size;
                    fwrite(&l, 1, 4, out);
                    fwrite(bpkt->data, 1, bpkt->size, out);
                } else if (mp4_out || ts_out) {
                    bpkt->stream_index = ost->index;
                    av_packet_rescale_ts(bpkt, fmt->streams[vstream]->time_base,
                                         ost->time_base);
                    av_interleaved_write_frame(oc, bpkt);
                } else {
                    fwrite(bpkt->data, 1, bpkt->size, out);
                }
                bytes += bpkt->size;
                ++written;
                ++n;
                av_packet_unref(bpkt);
            }
            av_packet_unref(pkt);
        } else if (gop_group) {
            const bool is_idr = buf_has_idr(pkt->data, pkt->size);
            if (gop_started && is_idr) {
                AVPacket* gp = av_packet_alloc();
                av_new_packet(gp, gop_buf.size());
                memcpy(gp->data, gop_buf.data(), gop_buf.size());
                gp->stream_index = ost ? ost->index : 0;
                if (mp4_out) {
                    av_packet_rescale_ts(gp, fmt->streams[vstream]->time_base,
                                         ost->time_base);
                    av_interleaved_write_frame(oc, gp);
                } else {
                    fwrite(gop_buf.data(), 1, gop_buf.size(), out);
                }
                bytes += gop_buf.size();
                ++written;
                gop_buf.clear();
            }
            gop_buf.append((const char*)pkt->data, pkt->size);
            gop_started = true;
            av_packet_unref(pkt);
        } else {
            if (aus_out) {
                uint32_t l = (uint32_t)pkt->size;
                fwrite(&l, 1, 4, out);
                fwrite(pkt->data, 1, pkt->size, out);
            } else if (mp4_out || ts_out) {
                pkt->stream_index = ost->index;
                av_packet_rescale_ts(pkt, fmt->streams[vstream]->time_base,
                                     ost->time_base);
                av_interleaved_write_frame(oc, pkt);
            } else {
                fwrite(src->data, 1, src->size, out);
            }
            bytes += src->size;
            ++written;
            ++n;
            av_packet_unref(pkt);
        }
        if (n == 0) continue;
        if (written % 200 == 0) {
            fprintf(stderr, "[es_dump] 已写 %d 包 / %.1f MB\n", written,
                    bytes / 1048576.0);
        }
    }

    if (gop_group && !gop_buf.empty()) {
        AVPacket* gp = av_packet_alloc();
        av_new_packet(gp, gop_buf.size());
        memcpy(gp->data, gop_buf.data(), gop_buf.size());
        gp->stream_index = ost ? ost->index : 0;
        if (mp4_out) {
            av_packet_rescale_ts(gp, fmt->streams[vstream]->time_base, ost->time_base);
            av_interleaved_write_frame(oc, gp);
        } else {
            fwrite(gop_buf.data(), 1, gop_buf.size(), out);
        }
        bytes += gop_buf.size();
        ++written;
        gop_buf.clear();
    }

    if (mp4_out || ts_out) {
        av_write_trailer(oc);
        avio_closep(&oc->pb);
        avformat_free_context(oc);
    } else {
        fflush(out);
        fclose(out);
    }
    fprintf(stderr, "[es_dump] 完成: %d 包, %.2f MB -> %s\n", written,
            bytes / 1048576.0, out_path);

    av_packet_free(&pkt);
    av_packet_free(&bpkt);
    av_bsf_free(&bsf_ctx);
    avformat_close_input(&fmt);
    return 0;
}
