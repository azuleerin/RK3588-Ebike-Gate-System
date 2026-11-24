// nv12_frame.h —— 解码帧的【轻量描述】
//
// 为什么单独抽一个结构体:
//   解码器(MPP) 和 RGA 之间只需要交接三样东西 —— "物理内存 + 尺寸 + 格式"。
//   把整个 AVFrame / cv::Mat 在中间转一手, 既慢又容易把"这帧到底是 CPU 内存
//   还是 dma-buf" 这件事搞丢。
//   Nv12Frame 就是这张交接单: MPP 填它, RGA 读它。
//
// ★ 全链路的数据形态(零拷贝):
//     MPP 解码 -> dma-buf fd ┐
//                           ├─> RGA(importbuffer_fd) -> RGB888 dma-buf
//     (软解)   -> CPU 指针 ──┘                          |
//                                                        v
//                                            RKNN(rknn_create_mem_from_fd) -> NPU
//   每一帧的像素从头到尾只存在【一份物理内存】里, CPU 不参与搬运。
#ifndef RK_NV12_FRAME_H_
#define RK_NV12_FRAME_H_

// 带上 RGA 头, 才有 RK_FORMAT_* 枚举。
// ⚠️ librga 的头文件里用了 NULL 却没有 include <cstddef>,
//    所以必须先引入 <cstddef>, 否则谁先包含 im2d.h 谁编译失败。
#include <cstddef>
#include "rga.h"

namespace rk {

// 这一帧的数据来自哪里 —— 决定 RGA 走哪条路。
enum class Nv12Source {
    kCpuPtr,    // CPU 可见的普通内存(软解, 或者拿不到 dma-buf 时)
    kDmaBufFd,  // 真 dma-buf fd —— RGA 可以直接 import, 零 CPU 拷贝
};

// 一帧 YUV420 的最小描述。
//
// ★ wstride 的单位是【字节】(一行 luma 占多少字节), 不是像素 ——
//   这是 RgaImage / CPU 兜底路径的约定。
//   librga 的 rga_buffer_t 里那个 wstride 则是【像素】, 两者在
//   rga_convert.cc 的边界上换算(见 rga_wstride_pixels())。
struct Nv12Frame {
    int   fd = -1;          // dma-buf fd; >=0 表示走 RGA import 路径
    void* ptr = nullptr;    // 同一块内存的 CPU 映射地址(CPU 兜底路径要用)
    int   width = 0;        // 显示宽(像素)
    int   height = 0;       // 显示高(像素)
    int   wstride = 0;      // luma plane 每行字节数
    int   hstride = 0;      // luma plane 行数(解码器会按 16/64 对齐)
    int   format = RK_FORMAT_YCbCr_420_SP;   // RK_FORMAT_*
    // ★ 每采样占几 bit(8/10/12/16)。
    //   MPP 给的 hor_stride 是【字节】, hor_stride_pixel 是【像素】,
    //   两者之比就是每个采样占几个字节:
    //       8bit -> 1.00 B/px   10bit -> 1.25 B/px(紧密打包)   16bit -> 2.00 B/px
    //   以前这里【不看】这个, 一律当 8bit NV12 用 —— 10bit 码流(HEVC Main10)
    //   于是整帧被按错误的位深解释 -> RGA 转出噪点 -> 检测器 cls_max≈0.0036 /
    //   objs=0, 而【全程零报错】。2026-11-24 加, 详见 mpp_decoder.cc。
    int   bit_depth = 8;
    Nv12Source source = Nv12Source::kCpuPtr;

    bool has_fd() const { return fd >= 0; }
    bool valid() const { return width > 0 && height > 0 && wstride > 0; }

    // 给人看的名字 —— 日志里出现的就是它。
    const char* source_name() const {
        return source == Nv12Source::kDmaBufFd ? "DMA-BUF fd" : "CPU 映射地址";
    }
};

}  // namespace rk

#endif  // RK_NV12_FRAME_H_
