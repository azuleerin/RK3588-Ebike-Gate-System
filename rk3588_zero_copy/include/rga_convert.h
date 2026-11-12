// rga_convert.h —— RGA 硬件格式转换/缩放封装
//
// 全链路的数据形态：
//   解码器 NV12(dma-buf 或 CPU 映射地址)
//        --RGA(improcess)--> RGB888(dma-buf)
//        --rkNN(fd import)--> NPU
// 这一步完全在 RGA 里做，CPU 不参与搬运，不存在 memcpy。
#ifndef RK_RGA_CONVERT_H_
#define RK_RGA_CONVERT_H_

// 带上 RGA 头, 使用方(RgaImage.format)才能拿到 RK_FORMAT_* 枚举。
// ⚠️ librga 的头文件里用了 NULL 却没有 include <cstddef>,
//    所以必须先引入 <cstddef>, 否则谁先包含 im2d.h 谁编译失败。
#include <cstddef>
#include <cstdint>
#include "rga.h"

namespace rk {

// RGA 的输入/输出描述。
//   fd        : dma-buf fd，>=0 时 RGA 直接 import 这块物理内存
//   vir_addr  : 同一块内存的用户态映射（只用于 CPU 兜底路径和 letterbox
//                填充 padding；fd>=0 时【不会】传给 librga —— 两者都填会让
//                librga 走"软件 MMU"那条会写坏堆的路）
//   wstride   : 每行【字节】数。
//                ⚠️ librga 的 rga_buffer_t 里那个 wstride 是【像素】, 两者在
//                   rga_convert.cc 的边界上换算(rga_wstride_pixels())。
//                   内核驱动的尺寸校验是 wstride * hstride * bpp(format),
//                   所以 RGB888 640 宽 -> wstride(像素)=640, 缓冲要 640*640*3。
//   hstride   : plane 行数。NV12 时 = luma plane 行数（MPP 会做 16 对齐），
//                UV plane 紧跟在 luma plane 之后，偏移 = wstride * hstride
//   format    : RK_FORMAT_* 枚举
struct RgaImage {
    int   fd = -1;
    void* vir_addr = nullptr;
    int   width = 0;
    int   height = 0;
    int   wstride = 0;
    int   hstride = 0;
    int   format = 0;

    // ---- YUV420 的色度信息 ----------------------------------------------
    // ★ 相机解码出来的帧【不一定】是 NV12: FFmpeg 软解出来的是
    //   yuvj420p —— 【平面】YUV420(U 平面 / V 平面分开), 而且是【全范围】
    //   (Y 用 0~255, 不是 16~235)。
    //   如果按 NV12(半平面, UV 交织) 去读, 颜色会整个错掉(实测整帧品红/绿),
    //   灰度结构对、颜色全错 —— 那种图上 LPRNet 永远认不出车牌。
    const uint8_t* u_plane = nullptr;  // 平面: U 平面; 半平面: UV 交织平面
    const uint8_t* v_plane = nullptr;  // 平面: V 平面; 半平面: 不用
    int   uv_wstride = 0;              // 色度行字节数(平面: w/2; 半平面: w)
    bool  planar_yuv = false;          // true = 平面 YUV420(U/V 分离)
    bool  full_range = false;          // yuvj* = 全范围, 需要先折成 limited

    // ---- 高位深(10/12bit)输入 --------------------------------------------
    // 高位深帧每个采样占 2 字节(小端 16bit 容器), 降到 8bit 要右移 bit_shift 位:
    //   10bit -> bit_shift = 2,  12bit -> bit_shift = 4
    // bit_shift = 0 表示普通 8bit(默认)。
    //
    // ★ 为什么需要它: DJI 那个 MP4 解出来是 yuv420p10le —— 每采样 2 字节,
    //   linesize[0]=3840(不是 1920)。当 8bit NV12 读的话颜色整个错掉
    //   (R 通道被压成 0, 画面发青), 而 Y 平面看着还挺正常, 极具迷惑性。
    // ★ RGA 硬件路径只认 8bit NV12, 高位深帧一律走 CPU 转换(见 rga_convert.cc)。
    int  bit_shift = 0;
};

// RGA 当前是否【已确认】可用。
//   · 探测通过(设备节点能打开 + librga 初始化成功) -> true
//   · 还没探测 / 探测失败                        -> false
// 失败原因见 rga_unavailable_reason()。
bool rga_available();

// RGA 不可用的原因(一句话, 给人看的), 没探测过或可用时返回空字符串。
// 例: "权限不足（open("/dev/rga") 返回 Permission denied，errno=13）"
const char* rga_unavailable_reason();

// RGA 源缓冲的注册方式(运行时用 RK_RGA_SRC 切换, 编译期用
// -DRK_ENABLE_RGA_DMA_BUF=0 整体关掉):
//
//   auto  (默认) : 有 dma-buf fd 就用 importbuffer_fd 注册, 否则走 CPU
//   import       : 强制 importbuffer_fd
//   legacy       : 强制老路(直接在 rga_buffer_t 里填 fd)
//   cpu          : 强制 OpenCV CPU 转换
//
// 实测 import 与 legacy 的输出【逐字节一致】(tests/test_rga_fd_import.cpp)。
//
// NV12 -> RGB888。
//
// letterbox = true : 等比缩放到 dst 内部并居中，四周用 114 灰填充
//                    （和 YOLOv8 官方 python demo 的 letterbox 保持一致）
// letterbox = false: 直接拉伸到 dst 尺寸
//
// out_scale / out_pad_x / out_pad_y 输出 letterbox 参数，用于把检测框
// 从模型输入坐标系映射回原始画面坐标。
//
// ★ RGA 打不开 /dev/rga 时 librga 会【自己段错误】, 所以这里先用 open() 预检
//   设备节点(打不开就给出明确原因并干净降级, 根本不给它崩的机会),
//   剩下的初始化阶段再用 SafeProbe 兜住 —— 保证不会再 core dump。
//   设备节点路径从环境变量 RGA_DEVICE 读, 默认 /dev/rga。
//
// 返回 0 成功，负数失败。
int rga_nv12_to_rgb888(const RgaImage& src, const RgaImage& dst,
                       bool letterbox,
                       float* out_scale, int* out_pad_x, int* out_pad_y);

// 从 src 里裁出 (sx, sy, sw, sh) 区域，等比/拉伸缩放到 dst 的整幅。
// 用于「检测框 -> 车牌小图」：一次 RGA 操作完成裁剪+缩放+格式转换，
// 整条路径没有 CPU 内存拷贝。
// 返回 0 成功，负数失败。
int rga_crop_to_rgb888(const RgaImage& src, int sx, int sy, int sw, int sh,
                       const RgaImage& dst);

// ---------------------------------------------------------------------------
// 把 CPU 上的 YUV420 帧打包成一份【连续的 NV12】。
//
// 用途(诊断模式): 机器上没有 MPP 硬解(拿不到 /dev/mpp_service 权限)时,
// 用它把软解出来的帧塞进一块 dma-buf, 再走 RGA 的 importbuffer_fd 路径,
// 用来单独验证 "DMA-BUF fd -> RGA" 这条路本身是通的。
//
// 支持两种源布局: 半平面 NV12 / 平面 YUV420(yuv420p, yuvj420p)。
// full_range 的帧会顺带折成 limited range(和 RGA 的 BT.601 limited 对齐)。
// 返回 0 成功, 负数失败。
int pack_yuv420_to_nv12(const RgaImage& src, uint8_t* dst, size_t dst_cap);

}  // namespace rk

#endif  // RK_RGA_CONVERT_H_
