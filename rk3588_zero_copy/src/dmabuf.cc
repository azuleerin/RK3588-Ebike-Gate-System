#include "dmabuf.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// dma-heap uapi（内核 drivers/dma-heap）。
//
// ⚠️⚠️ 这里有两个【不同的】dma-heap ioctl, 搞错就静默失败:
// ---------------------------------------------------------------------------
//
// (A) Rockchip 自带的 rk-dma-heap（本板子内核 6.1.172-vendor-seeed 用的就是这个）
//     头文件: /usr/src/linux-headers-*/include/uapi/linux/rk-dma-heap.h
//         #define RK_DMA_HEAP_IOC_MAGIC   'R'
//         #define RK_DMA_HEAP_UNCACHED     (1UL << 0)
//         struct rk_dma_heap_allocation_data {
//             __u64 len; __u32 fd; __u32 fd_flags; __u64 heap_flags;
//         };
//     ioctl: _IOWR('R', 0x0, struct rk_dma_heap_allocation_data)
//
// (B) 上游标准 dma-heap（<linux/dma-heap.h>）
//         #define DMA_HEAP_IOC_MAGIC      'H'
//         struct dma_heap_allocation_data {
//             __u64 len; __u32 fd; __u32 fd_flags; __u64 heap_flags;
//         };
//     ioctl: _IOWR('H', 0x0, struct dma_heap_allocation_data)
//
// ★★★ 坑 1: 字段顺序 —— 【fd 在第二个字段】, 不是最后一个!
//     之前这里写成 {len, fd_flags, heap_flags, fd}, 于是内核把我们的
//     fd_flags 当成 fd 读、把我们填的 fd(0) 当成 heap_flags 读 ->
//     heap_flags=0 之外的组合全被判非法 -> ioctl 返回 EINVAL,
//     dma-buf 分配【静默失败】, 于是日志里一直是
//         "[警告] dma-buf 不可用, 退化为 CPU 内存输入"
//     而 RGA 又要求源和目标都必须是真 dma-buf -> 整条 RGA 路径永远走不到。
//
// ★★★ 坑 2: magic 字母 —— Rockchip 版是 'R' 不是 'H'。
//     用错 magic, ioctl 号对不上, 内核直接返回 EINVAL/ENOTTY。
//     两种都试一下, 谁能用就用谁(标准版留给别的内核)。
//
// ★★★ 坑 3: heap_flags 的 UNCACHED 位是 (1<<0), 不是 (1<<1)。
//     而且本板子的内核对 heap_flags!=0 一律回 EINVAL ——
//     "system-uncached" 这个堆本身就是非 cache 的, 用 heap_flags=0 申请即可。
// ---------------------------------------------------------------------------
typedef uint64_t rk_u64;
typedef uint32_t rk_u32;

// 两种 ioctl 共用同一个结构体布局(只是 magic 不同)。
struct rk_dma_heap_alloc_data {
    rk_u64 len;
    rk_u32 fd;          // 内核回填: 分配出来的 dma-buf fd
    rk_u32 fd_flags;    // 打开这个 fd 时的 flags
    rk_u64 heap_flags;  // 传给 heap 的附加标志
};

#define RK_DMA_HEAP_IOCTL_ALLOC_R _IOWR('R', 0x0, struct rk_dma_heap_alloc_data)
#define RK_DMA_HEAP_IOCTL_ALLOC_H _IOWR('H', 0x0, struct rk_dma_heap_alloc_data)

#ifndef RK_DMA_HEAP_UNCACHED
#define RK_DMA_HEAP_UNCACHED (1ULL << 0)   // Rockchip 版是 bit0
#endif

// dma-buf cache 同步 uapi
struct rk_dma_buf_sync {
    rk_u32 flags;
    rk_u64 start;
    rk_u64 end;
};
#define RK_DMA_BUF_IOCTL_SYNC _IOWR('P', 2, struct rk_dma_buf_sync)
#define RK_DMA_BUF_SYNC_START 0x00000000
#define RK_DMA_BUF_SYNC_END   0x00000001
#define RK_DMA_BUF_SYNC_WRITE 0x00000002
#define RK_DMA_BUF_SYNC_READ  0x00000004

namespace {

// ★ 用固定 char 缓冲而不是 std::string:
//   dma_buf_backend() 返回的是"当前后端"的字符串, 调用方经常会先存住指针,
//   下一次 dma_buf_alloc() 又会改写它 —— 存下来的指针会变成悬垂指针。
char g_backend[128] = "none";

// 收集 /dev/dma_heap 下可用的 heap 节点，按【能不能直接给硬件用】排序。
//
// ★★★ 关键: 这块板子的内核【没有实现 dma-buf 的 cache 同步 ioctl】——
//     RK_DMA_BUF_IOCTL_SYNC (_IOWR('P', 2, ...)) 一律返回 ENOTTY。
//     也就是说:
//       · 从 cached 堆(system / reserved)拿到的内存, RGA/NPU 用 DMA 写完之后,
//         CPU 再去读, 读到的是【自己那份旧 cache】—— 数据看起来"没写进去"。
//         (实测: RGA 明明转换成功, CPU 读回来还是 memset 的填充值 0xA5)
//       · 只有【非 cache】的堆(system-uncached)才不需要同步。
//     所以优先用 system-uncached, 拿不到再退回别的堆。
//
// 排序: uncached(0) > cma(1) > 其它(2)
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
    std::stable_sort(heaps.begin(), heaps.end(), [](const std::string& a, const std::string& b) {
        auto rank = [](const std::string& p) {
            if (p.find("uncached") != std::string::npos) return 0;  // 非 cache, 首选
            if (p.find("cma") != std::string::npos) return 1;       // 连续物理内存
            return 2;
        };
        return rank(a) < rank(b);
    });
    return heaps;
}

}  // namespace

namespace rk {

const char* dma_buf_backend() { return g_backend; }

int dma_buf_alloc(std::size_t size, bool uncached) {
    if (size == 0) return -1;

    // 堆节点按优先级排列; 每个堆再依次尝试:
    //   ① Rockchip 版 ioctl ('R')  + uncached 标志
    //   ② Rockchip 版 ioctl ('R')  + heap_flags=0
    //   ③ 标准版 ioctl   ('H')  + heap_flags=0
    // 只要有一档成功就返回。
    //
    // ★ "system-uncached" 这个堆名本身就说明它是非 cache 的, 所以对它
    //   直接用 heap_flags=0; 对 "system"/"reserved" 先试 uncached 标志,
    //   内核不支持就退回 0(此时拿到的是 cached 内存, 需要 dma_buf_sync)。
    for (const auto& heap : probe_dma_heaps()) {
        int hfd = ::open(heap.c_str(), O_RDWR | O_CLOEXEC);
        if (hfd < 0) continue;

        const bool heap_is_uncached = heap.find("uncached") != std::string::npos;

        for (int attempt = 0; attempt < 4; ++attempt) {
            const bool want_uncached = uncached && !heap_is_uncached && (attempt == 0);
            const bool use_rk_ioctl = (attempt < 2);

            struct rk_dma_heap_alloc_data data;
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
    if (fd < 0 || size == 0) return MAP_FAILED;
    return ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
}

void dma_buf_unmap(void* addr, std::size_t size) {
    if (addr == MAP_FAILED || addr == nullptr) return;
    ::munmap(addr, size);
}

int dma_buf_sync(int fd, std::size_t offset, std::size_t len, bool cpu_write) {
    if (fd < 0) return -1;
    struct rk_dma_buf_sync sync;
    memset(&sync, 0, sizeof(sync));
    sync.flags = RK_DMA_BUF_SYNC_START | (cpu_write ? RK_DMA_BUF_SYNC_WRITE : RK_DMA_BUF_SYNC_READ);
    sync.start = offset;
    sync.end = offset + len;
    if (::ioctl(fd, RK_DMA_BUF_IOCTL_SYNC, &sync) != 0) return -1;

    sync.flags = RK_DMA_BUF_SYNC_END | (cpu_write ? RK_DMA_BUF_SYNC_WRITE : RK_DMA_BUF_SYNC_READ);
    return ::ioctl(fd, RK_DMA_BUF_IOCTL_SYNC, &sync);
}

// ---------------------------------------------------------------------------
// 通过 /proc 反查地址对应的 dma-buf fd
// ---------------------------------------------------------------------------
int find_dmabuf_fd_for_addr(const void* addr) {
    if (!addr) return -1;

    // --- 1) 在 /proc/self/maps 里找包含该地址的映射, 取出它的路径和 inode
    FILE* maps = fopen("/proc/self/maps", "r");
    if (!maps) return -1;

    char line[1024];
    char path[512] = {0};
    unsigned long ino = 0;
    bool found = false;
    const unsigned long target = reinterpret_cast<unsigned long>(addr);

    while (fgets(line, sizeof(line), maps)) {
        unsigned long start = 0, end = 0, offset = 0, in = 0;
        unsigned devmaj = 0, devmin = 0;
        char perms[16] = {0};
        char p[512] = {0};
        int n = sscanf(line, "%lx-%lx %15s %lx %x:%x %lu %511[^\n]",
                       &start, &end, perms, &offset, &devmaj, &devmin, &in, p);
        if (n < 7) continue;                       // 匿名映射没有路径
        if (target < start || target >= end) continue;
        ino = in;
        char* s = p;
        while (*s == ' ' || *s == '\t') s++;
        snprintf(path, sizeof(path), "%s", s);
        found = true;
        break;
    }
    fclose(maps);

    if (!found || ino == 0 || path[0] == '\0') return -1;   // 匿名映射 -> 不是 dma-buf

    // --- 2) 扫 /proc/self/fd, 用 readlink + fdinfo 的 ino 对上
    DIR* d = opendir("/proc/self/fd");
    if (!d) return -1;

    int result = -1;
    struct dirent* e = nullptr;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;

        char fdpath[320];
        snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%s", e->d_name);
        char link[512];
        ssize_t len = ::readlink(fdpath, link, sizeof(link) - 1);
        if (len <= 0) continue;
        link[len] = '\0';
        if (strcmp(link, path) != 0) continue;

        char infopath[320];
        snprintf(infopath, sizeof(infopath), "/proc/self/fdinfo/%s", e->d_name);
        FILE* fi = fopen(infopath, "r");
        if (!fi) continue;
        char infoline[256];
        while (fgets(infoline, sizeof(infoline), fi)) {
            unsigned long v = 0;
            if (sscanf(infoline, "ino: %lu", &v) == 1 && v == ino) {
                result = atoi(e->d_name);
                break;
            }
        }
        fclose(fi);
        if (result >= 0) break;
    }
    closedir(d);
    return result;
}

}  // namespace rk
