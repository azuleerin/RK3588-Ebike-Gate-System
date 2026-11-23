// nlb/rga_pre.h —— 第 3 层的 RGA 预处理(NV12 -> RGB888 + 缩放)
//
// ★ 零拷贝链路的形状:
//
//   [NV12 dma-buf] --RGA--> [RGB888 dma-buf] == [NPU 输入 dma-buf]
//                                            ^^^^^^^^^^^^^^^^
//                                     这两块是【同一块物理内存】
//                                     (rknn_create_mem_from_fd 绑定上去的)
//
//   所以中间没有任何 CPU memcpy: RGA 直接写进 NPU 要读的那块内存。
//
// ★ wstride 单位陷阱(踩过):
//   RgaImage::wstride 是【字节】, 而 librga 的 rga_buffer_t::wstride 是【像素】。
//   NV12 是 1 字节/像素 -> 两者数值相同; RGB888 是 3 字节/像素 ->
//   像素数 = 字节数 / 3。搞错会让 RGA 把图像横向压扁。
//
// ★ 目标缓冲要留余量:
//   RGA 在缩放比不是整数倍时会多写 1~2 行, 所以目标按
//   "行字节 × (行数 + 16 行)" 分配。
//
// ★ librga 在一个进程里是串行的:
//   每次调用都要 importbuffer_fd 注册缓冲, 而那张全局 handle 表是
//   进程内共享 + 加锁的。所以多线程场景下必须自己加一把互斥量。
//   本测试台第 3 层是单线程, 不涉及; 但第 4/5 层是驱动生产二进制, 由它自己管。
#ifndef NLB_RGA_PRE_H_
#define NLB_RGA_PRE_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace nlb {

// RGA 目标缓冲多留的行数(见文件头说明)。
constexpr int kRgaDstPadRows = 16;

class RgaConverter {
public:
    RgaConverter() = default;
    ~RgaConverter() { destroy(); }

    RgaConverter(const RgaConverter&) = delete;
    RgaConverter& operator=(const RgaConverter&) = delete;

    // 准备一块【NV12 源】 dma-buf, 尺寸 src_w × src_h。
    bool init_src(int src_w, int src_h, std::string* err);

    // 把源缓冲填成确定性的 NV12 图案(只在启动时写一次)。
    void fill_src_pattern();

    // 源 dma-buf 的 fd / 用户态地址 / 尺寸。
    int src_fd() const { return src_fd_; }
    void* src_ptr() const { return src_ptr_; }
    std::size_t src_size() const { return src_size_; }
    int src_w() const { return src_w_; }
    int src_h() const { return src_h_; }

    // 做一次 NV12 -> RGB888 + 缩放, 目标写进 dst_fd/dst_ptr。
    // dst 必须是一块【外部】dma-buf(通常是 NPU 的输入内存)。
    // 返回 0 成功, 负数失败。
    int convert(int dst_fd, void* dst_ptr, std::size_t dst_size,
                int dst_w, int dst_h);

    // RGA 设备是否可用(探测 /dev/rga + 一次 imcheck)。
    static bool available(std::string* reason);

    void destroy();

private:
    int src_fd_ = -1;
    void* src_ptr_ = nullptr;
    std::size_t src_size_ = 0;
    int src_w_ = 0, src_h_ = 0;
    int src_wstride_ = 0, src_hstride_ = 0;
    bool rga_ready_ = false;
};

}  // namespace nlb

#endif  // NLB_RGA_PRE_H_
