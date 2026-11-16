// rk/rga_pad.h —— RGA 目标缓冲的【余量行】常量(全工程唯一来源)
//
// ★ 为什么每个 RGA 目标缓冲都要多留几行
//
//   RK3588 的 RGA 在【缩放比例不是整数倍】时(例如 1920x1080 -> 640x640,
//   sy = 1080/640 = 1.6875), 行缓冲里没写完的 1~2 行也会被刷进目标缓冲,
//   也就是往目标缓冲末尾【越界写 1~2 行】。
//
//   实测(2026-11-11 那次 heap 崩溃): 目标缓冲后面挂 canary,
//   RGA 跑一次就发现 +3840 字节(= 2 行)被写了。
//   目标缓冲如果按精确大小分配(640*640*3 = 1228800 字节, 一字节余量都没有),
//   这 1~2 行就直接踩坏后面的堆 —— 表现为
//       malloc(): invalid next size (unsorted)
//   而且落点随机: 有时崩在 malloc, 有时崩在 libavcodec, 有时崩在 cv::Mat。
//
// ★ 为什么要抽成头文件
//
//   以前这个 16 只写在 main.cc 里, 于是【别的地方分配的 RGA 目标缓冲】没有余量:
//     - main.cc 的串行检测路径: 有余量 (model_buf_alloc)
//     - main.cc 的车牌路径    : 有余量 (ppocr_buf_alloc)
//     - rk_base/npu_pipe.cc   : ★【没有余量】-> 并行检测路径踩同一个坑
//   现在统一从���里取, 谁分配谁引用, 不会再漏。
#ifndef RK_RGA_PAD_H_
#define RK_RGA_PAD_H_

#include <cstddef>

namespace rk {

// RGA 目标缓冲多分配的余量行数。
// 16 行足够覆盖实测到的 2 行, 代价只有几十 KB。
constexpr int kRgaDstPadRows = 16;

// 按"行宽(字节) x 行数"算出带余量的分配大小。
inline size_t rga_padded_size(size_t row_bytes, size_t rows) {
    return row_bytes * rows + row_bytes * static_cast<size_t>(kRgaDstPadRows);
}

}  // namespace rk

#endif  // RK_RGA_PAD_H_
