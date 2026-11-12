// mpp_decoder.h —— 用 Rockchip MPP 直接做 H.264/H.265 硬解
//
// 为什么自己解而不是用 FFmpeg 的 h264_rkmpp:
//   Ubuntu/Debian 的 FFmpeg 【没有】rkmpp 解码器(不打包 Rockchip MPP 支持),
//   要用就得从 Rockchip SDK 拿 ffmpeg-rockchip, 离线环境下很难搞。
//   而 librockchip-mpp 的 .deb 是可以从 apt 仓库直接下载的 —— 于是我们
//   让 FFmpeg 只做 RTSP 拉流, 解码交给 MPP 自己来做。
//
// 好处:
//   * 真正的 VPU 硬解(H.264/H.265)
//   * 拿到的是 MPP 的 dma-buf, RGA 可以直接 import -> 整条链路零拷贝
//   * 不依赖任何打了补丁的 FFmpeg
//
// MPP API 版本说明:
//   本工程用的是 Rockchip MPP 1.0.x/1.3.x 的 MppApi 函数指针接口:
//       mpp_create(&ctx, &mpi);
//       mpi->control(ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, ALL);  // 必须在 init 之前
//       mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingAVC);
//       mpi->decode_put_packet(ctx, pkt);
//       mpi->decode_get_frame(ctx, &frame);
#ifndef RK_MPP_DECODER_H_
#define RK_MPP_DECODER_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "nv12_frame.h"

namespace rk {

class MppDecoder {
public:
    ~MppDecoder() { destroy(); }

    // hevc=true 解 H.265, 否则解 H.264
    bool init(bool hevc, int width_hint, int height_hint);
    // 喂一个 Annex-B 的 H.264/H.265 包, 出一帧; 没解出帧返回 false
    //
    // out 会被填成一张【轻量帧描述】: dma-buf fd + 尺寸 + stride + 格式。
    // fd >= 0 表示这帧是货真价实的 dma-buf, RGA 可以直接 import 它,
    // 整条链路不需要 CPU 碰一下像素。
    bool decode(const uint8_t* data, size_t size, int64_t pts, Nv12Frame* out);
    void destroy();

    bool valid() const { return ctx_ != nullptr; }
    const char* last_error() const { return err_.c_str(); }
    unsigned long long frames() const { return frames_; }

private:
    void* ctx_ = nullptr;      // MppCtx
    void* api_ = nullptr;      // MppApi*
    std::string err_;
    unsigned long long frames_ = 0;
    bool hevc_ = false;
};

// ---------------------------------------------------------------------------
// 运行环境诊断(不碰硬件, 随时可调用)
//
// 为什么要它:
//   本工程用的是 Radxa 的 librockchip-mpp **20190626**(旧 API)。而板子
//   (recomputer-rk3588-devkit, Ubuntu 26.04 / 新内核) 很可能【自带】一套
//   Rockchip BSP 版的 MPP —— 如果头文件、用户态 .so、内核驱动三者版本对不上,
//   mpp_create() 会直接【段错误】把整个进程带走, 表现就是:
//       打印到 "解码器: h264" 之后【一行都没有】, 连 "[解码] 后端 = ..." 都没有。
//
//   所以要先把三件事打清楚:
//     1) 运行时【实际加载】的是哪个 .so (dladdr, 不是链接期那个)
//     2) 内核 MPP/VPU 设备节点在不在
//     3) mpp_create 的返回码
// ---------------------------------------------------------------------------
void diag_mpp_environment(const char* tag);

}  // namespace rk

#endif  // RK_MPP_DECODER_H_
