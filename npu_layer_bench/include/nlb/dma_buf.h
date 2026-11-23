// nlb/dma_buf.h —— 自包含的 dma-buf 分配器
//
// ★ 为什么需要它:
//   RGA 要求【源和目标都是真 dma-buf fd】(给的是 vir_addr 会走 librga 的
//   软件 MMU 路径, 既慢又会踩堆)。RKNN 的零拷贝输入也要求一块
//   "NPU 可直读" 的物理内存。所以第 3 层需要一个能拿到 fd 的分配器。
//
// ★ 本文件是【独立实现】, 不复用主工程的 src/dmabuf.cc —— 保证本测试台
//   与主工程零耦合。行为对齐主工程踩过的坑:
//     · Rockchip 的 dma-heap ioctl magic 是 'R' 不是 'H'
//     · 结构体字段顺序是 {len, fd, fd_flags, heap_flags} —— fd 在【第二个】
//     · UNCACHED 标志是 bit0, 且本板内核对 heap_flags != 0 一律回 EINVAL
//     · 优先用 /dev/dma_heap/system-uncached(本板内核没有 cache 同步 ioctl,
//       用 cached 堆会读到旧 cache)
#ifndef NLB_DMA_BUF_H_
#define NLB_DMA_BUF_H_

#include <cstddef>

namespace nlb {

// 分配一块 dma-buf。返回 fd(>=0) 或 -1。
int dma_buf_alloc(std::size_t size, bool uncached = true);

// 把 dma-buf 映射到用户态。失败返回 nullptr。
void* dma_buf_map(int fd, std::size_t size);
void dma_buf_unmap(void* addr, std::size_t size);

// 当前后端描述(用于打印), 指向静态缓冲, 不要跨调用缓存指针。
const char* dma_buf_backend();

// dma-buf 的字节大小(fstat)。
std::size_t dma_buf_size(int fd);

}  // namespace nlb

#endif  // NLB_DMA_BUF_H_
