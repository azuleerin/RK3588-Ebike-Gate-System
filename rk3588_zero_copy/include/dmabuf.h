// dmabuf.h —— dma-buf 分配 / 映射 / cache 同步
//
// 为什么需要自己分配 dma-buf：
//   RGA 的目标缓冲、NPU(rknn_set_io_mem) 的输入缓冲都要求是"可被硬件 import 的
//   dma-buf"，普通的 malloc 内存不行。dma-buf 可以同时被 RGA / NPU / CPU 访问，
//   这正是零拷贝链路里"一帧图像只存在一份物理内存"的载体。
#ifndef RK_DMABUF_H_
#define RK_DMABUF_H_

#include <cstddef>
#include <cstdint>

namespace rk {

// 分配一块 dma-buf，返回 dma-buf fd（>=0 成功，<0 失败）。
//
// uncached = true 时优先申请非 cache 属性（DMA_HEAP_UNCACHED）：
//   RGA / NPU 写完之后 CPU 直接读就能拿到正确数据，不需要额外做 cache 同步。
//   heap 不支持 uncached 时自动退回 cached，此时请调用 dma_buf_sync()。
//
// 后端优先级：dma-heap(/dev/dma_heap/*) -> 失败返回 -1
const char* dma_buf_backend();

// 分配 dma-buf，size 字节。失败返回 -1。
int dma_buf_alloc(std::size_t size, bool uncached = true);

// 把 dma-buf 映射到用户态虚拟地址（CPU 可读写）。
//   注意：这只建立映射，不会产生数据拷贝。
//   返回 MAP_FAILED 表示失败。
void* dma_buf_map(int fd, std::size_t size);
void  dma_buf_unmap(void* addr, std::size_t size);

// cache 同步：仅当使用 cached heap 且设备与 CPU 之间有数据交换时才需要。
//   cpu_write = true  : CPU 即将写入这块内存（END 时回刷 cache）
//   cpu_write = false : CPU 即将读取这块内存（START 时失效 cache）
//   返回 0 成功。
int dma_buf_sync(int fd, std::size_t offset, std::size_t len, bool cpu_write);

// 找出与某个用户态地址对应的 dma-buf fd。
//
// 原理：/proc/self/maps 里 dma-buf 的映射行带 inode；/proc/self/fd/<fd> 的
// readlink 指向同一个设备节点，/proc/self/fdinfo/<fd> 里的 "ino:" 就是这个
// dma-buf 的 inode。两者对上就是它。
//
// 用途：拿不到 MPP_BUFFER 的 fd 时，用解码帧的 CPU 映射地址反查出底下的
// dma-buf fd，从而继续走纯 fd 路径。找不到返回 -1，调用方退化成
// "RGA 直接 DMA 读 CPU 映射地址"（依然没有 CPU 内存拷贝）。
int find_dmabuf_fd_for_addr(const void* addr);

}  // namespace rk

#endif  // RK_DMABUF_H_
