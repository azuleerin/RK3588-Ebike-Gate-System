// src/dma_buf.cpp —— 自包含的 dma-buf 分配器
//
// 实现要点(全部是本板子实测踩出来的坑, 详见 include/nlb/dma_buf.h):
//   · Rockchip dma-heap 的 ioctl magic 是 'R'(不是上游的 'H')
//   · 结构体字段顺序 {len, fd, fd_flags, heap_flags} —— fd 在第二个字段
//   · UNCACHED 标志是 bit0, 但本板内核对 heap_flags != 0 一律回 EINVAL
//   · 优先 /dev/dma_heap/system-uncached(本板内核没有 cache 同步 ioctl)
#include "nlb/dma_buf.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace nlb {
namespace {

typedef uint64_t rk_u64;
typedef uint32_t rk_u32;

struct rk_dma_heap_alloc_data {
    rk_u64 len;
    rk_u32 fd;          // 内核回填
    rk_u32 fd_flags;
    rk_u64 heap_flags;
};

#define RK_DMA_HEAP_IOCTL_ALLOC_R _IOWR('R', 0x0, struct rk_dma_heap_alloc_data)
#define RK_DMA_HEAP_IOCTL_ALLOC_H _IOWR('H', 0x0, struct rk_dma_heap_alloc_data)

#ifndef RK_DMA_HEAP_UNCACHED
#define RK_DMA_HEAP_UNCACHED (1ULL << 0)
#endif

// ★ 用固定 char 缓冲而不是 std::string:
//   返回值是"当前后端"的描述, 调用方经常会先存住指针,
//   下一次分配又改写它 —— 存下来的指针会变成悬垂指针。
char g_backend[160] = "none";

std::vector<std::string> probe_dma_heaps() {
    std::vector<std::string> heaps;
    DIR* d = opendir("/dev/dma_heap");
    if (!d) return heaps;
    struct dirent* e = nullptr;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        heaps.push_back(std::string("/dev/dma_heap/") + e->d_name);
    }
    closedir(d);
    // 排序: uncached(0) > cma(1) > 其它(2)
    std::stable_sort(heaps.begin(), heaps.end(), [](const std::string& a, const std::string& b) {
        auto rank = [](const std::string& p) {
            if (p.find("uncached") != std::string::npos) return 0;
            if (p.find("cma") != std::string::npos) return 1;
            return 2;
        };
        return rank(a) < rank(b);
    });
    return heaps;
}

}  // namespace

const char* dma_buf_backend() { return g_backend; }

int dma_buf_alloc(std::size_t size, bool uncached) {
    if (size == 0) return -1;
    for (const auto& heap : probe_dma_heaps()) {
        int hfd = ::open(heap.c_str(), O_RDWR | O_CLOEXEC);
        if (hfd < 0) continue;

        const bool heap_is_uncached = heap.find("uncached") != std::string::npos;

        // ① Rockchip ioctl + uncached 标志
        // ② Rockchip ioctl + heap_flags = 0
        // ③ 标准 ioctl   + heap_flags = 0
        for (int attempt = 0; attempt < 3; ++attempt) {
            const bool want_uncached = uncached && !heap_is_uncached && (attempt == 0);
            const bool use_rk_ioctl = (attempt < 2);

            rk_dma_heap_alloc_data data;
            memset(&data, 0, sizeof(data));
            data.len = size;
            data.fd_flags = O_RDWR | O_CLOEXEC;
            data.heap_flags = want_uncached ? RK_DMA_HEAP_UNCACHED : 0;

            const unsigned long req = use_rk_ioctl ? RK_DMA_HEAP_IOCTL_ALLOC_R
                                                   : RK_DMA_HEAP_IOCTL_ALLOC_H;
            if (::ioctl(hfd, req, &data) == 0 && data.fd != 0) {
                ::close(hfd);
                snprintf(g_backend, sizeof(g_backend), "dma_heap(%s,%s)",
                         heap.c_str(), data.heap_flags ? "uncached" : "cached");
                return static_cast<int>(data.fd);
            }
        }
        ::close(hfd);
    }
    snprintf(g_backend, sizeof(g_backend), "none");
    return -1;
}

void* dma_buf_map(int fd, std::size_t size) {
    if (fd < 0 || size == 0) return nullptr;
    void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) return nullptr;
    return p;
}

void dma_buf_unmap(void* addr, std::size_t size) {
    if (!addr || size == 0) return;
    ::munmap(addr, size);
}

std::size_t dma_buf_size(int fd) {
    if (fd < 0) return 0;
    struct stat st;
    if (::fstat(fd, &st) != 0) return 0;
    return static_cast<std::size_t>(st.st_size);
}

}  // namespace nlb
