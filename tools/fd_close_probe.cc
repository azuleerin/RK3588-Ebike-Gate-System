// tools/fd_close_probe.cc —— 验证"销毁 MPP 解码器上下文"是否会关闭 dma-buf fd
//
// 背景(step/05 引爆测试):
//   目标是让 MPP 重建 buffer group -> 旧 buffer 的 dma-buf fd 被关闭 ->
//   业务代码里还握着的那个悬垂 fd 就会指向别的对象 ->
//   RGA 拿它去 importbuffer_fd = 把别人的内存交给 DMA = IOMMU fault = 硬挂死。
//
//   走"码流里切分辨率"这条路极其难(相机的 SPS 只有 16 字节且是截断的,
//   FFmpeg 的 h264 parser 解析不了它, 裸流 demuxer 会把 SPS 剥掉,
//   TS muxer 会插 AUD 并把 113KB 的 IDR 访问单元拆散)。
//
//   但"重建解码器上下文"能达到完全相同的效果: mpp_destroy 会销毁 buffer group,
//   里面的 buffer 全部释放, 导出的 fd 随之关闭。
//   本工具验证这一点: 解码一帧 -> 记下 fd -> 销毁解码器 -> 检查 fd 是否失效/被复用。
//
// 用法: fd_close_probe <rtsp_url>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
}

#include "mpp_decoder.h"

static bool fd_alive(int fd, uint64_t* ino_out) {
    struct stat st {};
    if (fd < 0 || ::fstat(fd, &st) != 0) return false;
    if (ino_out) *ino_out = (uint64_t)st.st_ino;
    return true;
}

int main(int argc, char** argv) {
    const char* url = (argc > 1) ? argv[1] : "rtsp://192.168.31.200:8557/h264";

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, url, nullptr, nullptr) < 0) {
        fprintf(stderr, "[probe] 打开 %s 失败\n", url);
        return 1;
    }
    avformat_find_stream_info(fmt, nullptr);
    int vs = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { vs = (int)i; break; }
    }
    if (vs < 0) { fprintf(stderr, "[probe] 没有视频流\n"); return 1; }

    rk::MppDecoder dec;
    if (!dec.init(false, 0, 0)) {
        fprintf(stderr, "[probe] MppDecoder 初始化失败: %s\n", dec.last_error());
        return 1;
    }

    AVPacket* pkt = av_packet_alloc();
    int fd = -1;
    uint64_t ino = 0;
    for (int i = 0; i < 400 && fd < 0; ++i) {
        if (av_read_frame(fmt, pkt) < 0) break;
        if (pkt->stream_index != vs) { av_packet_unref(pkt); continue; }
        rk::Nv12Frame nf;
        if (dec.decode(pkt->data, pkt->size, pkt->pts, &nf)) {
            fd = nf.fd;
            printf("[probe] 解出帧: fd=%d ptr=%p %dx%d stride=%dx%d\n", fd, nf.ptr,
                   nf.width, nf.height, nf.wstride, nf.hstride);
        }
        av_packet_unref(pkt);
    }
    if (fd < 0) { fprintf(stderr, "[probe] 没解出帧\n"); return 1; }

    fd_alive(fd, &ino);
    printf("[probe] 销毁前: fd=%d 有效=%d inode=%llu\n", fd, (int)fd_alive(fd, &ino),
           (unsigned long long)ino);

    // ★ 关键动作: 销毁解码器上下文(等价于 buffer group 销毁 + fd 关闭)
    dec.destroy();

    uint64_t ino2 = 0;
    const bool alive = fd_alive(fd, &ino2);
    printf("[probe] 销毁后: fd=%d 仍有效=%d inode=%llu\n", fd, (int)alive,
           (unsigned long long)ino2);
    if (!alive) {
        printf("[probe] ★ 结论: 解码器销毁后该 fd 立即失效 -> "
               "业务代码再拿它去 importbuffer_fd 就会踩到已回收的 fd 号\n");
    } else if (ino2 != ino) {
        printf("[probe] ★ 结论: fd 号仍在但已指向别的对象(inode 变了) -> "
               "更危险: RGA 会把别人的内存当成图像\n");
    } else {
        printf("[probe] 结论: fd 未被关闭也未复用(需要再查)\n");
    }

    // 再开一个 fd, 看这个 fd 号会不会被复用
    int probe = ::open("/dev/zero", O_RDONLY);
    printf("[probe] 新开一个 fd 拿到 %d (原来 %d)\n", probe, fd);
    av_packet_free(&pkt);
    avformat_close_input(&fmt);
    return 0;
}
