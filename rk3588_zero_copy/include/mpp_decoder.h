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
    //
    // ★ out_frame (可选): 传非空指针时, 解码成功后把 MppFrame 的【所有权】
    //   交给调用方(值写入 *out_frame), 此时本函数【不会】再 mpp_frame_deinit。
    //   调用方必须用 rk::MppFrameGuard 接管, 让帧活到真正用完 fd/ptr 为止。
    //
    //   为什么需要这个出参: 以前本函数在返回前无条件 mpp_frame_deinit(),
    //   于是调用方拿到的 out->fd / out->ptr 已经是【归还给 MPP buffer group】
    //   的悬垂引用 —— 缓冲随时会被下一次解码重新分配。
    //   这就是 P0-2, 也是硬挂死前兆的真正持有者。
    bool decode(const uint8_t* data, size_t size, int64_t pts, Nv12Frame* out,
                void** out_frame = nullptr);
    void destroy();

    bool valid() const { return ctx_ != nullptr; }
    const char* last_error() const { return err_.c_str(); }
    unsigned long long frames() const { return frames_; }

private:
    // 取一帧: 内部会处理 MPP 的"信息变更帧"(自动回 READY)。
    // got_real=true 表示这次拿到的是【真画面】(不是信息变更帧)。
    // 为什么要单独抽出来: 送包遇到队列满时, 必须在等待间隙顺手取帧,
    // 否则拿不到信息变更帧 -> 解码器和应用互相等待 -> 死锁。
    bool take_one_frame(Nv12Frame* out, void** out_frame, bool* got_real);

    void* ctx_ = nullptr;      // MppCtx
    void* api_ = nullptr;      // MppApi*
    std::string err_;
    unsigned long long frames_ = 0;
    bool hevc_ = false;
    // 等待 MPP 输入队列腾空时【顺手取到】的一帧: 暂存在这里,
    // 等当前这个包送进解码器之后再交出去 —— 顺序才是对的。
    // (要是当场就交, 等于把还没送进去的包丢了。)
    void* held_frame_ = nullptr;   // MppFrame*
    Nv12Frame held_out_{};
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
