// test_mem_trim.cpp —— 验证 malloc_trim(0) 真的能把 RSS 压下来
//
// 客户要求: 长跑时 RSS 必须是一条直线, 不能有 0.5 KB/h 的底噪上涨。
//
// ★ 这个测试的关键是【分配/释放模式】, 直接决定 malloc_trim 有没有用:
//
//   模式 A(容易误判的写法):
//     申请 64MB -> 全部 free -> RSS 立刻掉回来, malloc_trim 归还 0 字节。
//     原因: 全部按 LIFO 释放会合并成 top chunk, glibc 的 free() 直接
//           sbrk 收缩堆(automated trim), 内存当场就还给内核了 ——
//           所以这种模式下根本不需要 malloc_trim, 测不出任何东西。
//
//   模式 B(真实业务模式, 本测试采用):
//     一批【长寿】分配一直占着 + 一批【短寿】分配夹在中间被释放。
//     短寿块释放后在堆里留下【洞】, 没法合并回 top chunk, 于是它们的页
//     一直被进程占着 —— 这正是长跑 RSS 缓慢上涨的真正原因。
//     malloc_trim(0) 会把这些空闲块的页 madvise(DONTNEED) 掉, RSS 立刻降。
//
// 用法: ./build/test_mem_trim
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "rk/mem_guard.h"

namespace {

size_t rss_kb() {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    size_t kb = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            kb = static_cast<size_t>(atoll(line + 6));
            break;
        }
    }
    fclose(f);
    return kb;
}

int g_fail = 0;
void expect(bool cond, const char* what) {
    printf("  [%s] %s\n", cond ? " OK " : "FAIL", what);
    if (!cond) ++g_fail;
}

}  // namespace

int main() {
    // 16KB 一块: 小于 M_MMAP_THRESHOLD(128KB), 落在 arena 里, 不会被 munmap。
    const size_t kChunk = 16 * 1024;
    const int    kCount = 4096;              // 4096 * 16KB = 64MB

    printf("==== malloc_trim 机制自测 ====\n");
    printf("初始 RSS: %zu KB\n", rss_kb());

    // ---- 长寿块: 一直占着, 防止堆顶收缩 ----
    // (真实业务里对应: 模型输入缓冲 / dma-buf 映射 / 常驻的帧池)
    std::vector<void*> live;
    live.reserve(kCount / 2);
    for (int i = 0; i < kCount / 2; ++i) {
        void* p = malloc(kChunk);
        if (!p) { printf("  长寿块分配失败于第 %d\n", i); return 1; }
        memset(p, 0x11, kChunk);
        live.push_back(p);
    }
    const size_t rss_live = rss_kb();

    // ---- 短寿块: 夹在长寿块之间申请, 然后释放 -> 留下无法合并的洞 ----
    std::vector<void*> holes;
    holes.reserve(kCount / 2);
    for (int i = 0; i < kCount / 2; ++i) {
        void* p = malloc(kChunk);
        if (!p) break;
        memset(p, 0x22, kChunk);
        holes.push_back(p);
    }
    const size_t rss_full = rss_kb();

    for (void* p : holes) free(p);
    holes.clear();
    const size_t rss_holes = rss_kb();

    printf("\n长寿块常驻后 RSS      : %zu KB\n", rss_live);
    printf("短寿块也申请后 RSS    : %zu KB\n", rss_full);
    printf("短寿块释放后 RSS     : %zu KB (洞还在, %+zd KB)\n", rss_holes,
           static_cast<ssize_t>(rss_holes) - static_cast<ssize_t>(rss_full));

    // ---- 关键: malloc_trim 把洞里的页还给内核 ----
    const size_t freed = rk::mem_trim();
    const size_t rss_trimmed = rss_kb();
    printf("malloc_trim(0) 归还   : %zu KB\n", freed / 1024);
    printf("trim 之后 RSS        : %zu KB (相对释放后 %+zd KB)\n\n", rss_trimmed,
           static_cast<ssize_t>(rss_trimmed) - static_cast<ssize_t>(rss_holes));

    expect(freed > 0, "malloc_trim 报告归还了非零字节(洞里的空闲块确实在 arena 里)");
    expect(rss_trimmed < rss_holes, "malloc_trim 之后 RSS 下降(空闲内存真的还给内核了)");
    expect(rss_trimmed < rss_full, "trim 之后 RSS 低于峰值");

    // ---- 不能误释放活着的块 ----
    bool live_ok = true;
    for (void* p : live) {
        const unsigned char* c = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < kChunk; i += 4096) {
            if (c[i] != 0x11) { live_ok = false; break; }
        }
        if (!live_ok) break;
    }
    expect(live_ok, "trim 之后长寿块的内容完好(没有误释放)");
    for (void* p : live) free(p);

    printf("\n==== %s (失败 %d 项) ====\n", g_fail ? "有失败项" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
