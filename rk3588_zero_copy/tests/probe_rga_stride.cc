// probe_rga_stride.cc —— 搞清楚 librga / 内核 RGA 驱动里 wstride 到底是什么单位
//
// 背景: 现有代码(dst.wstride = model_w * 3 = 1920)在 RGA 上直接被驱动拒了:
//   rga: Only get buffer 1232896 byte from dma_buf_ptr, but current image required 3686400 byte
//   rga: memory param: w = 1920, h = 640, f = RGB888(0x2)
// 驱动的尺寸校验是  size = W * H * bpp(format),  RGB888 的 bpp=3。
//
// 本程序用"同格式拷贝"这种最干净的实验来定夺:
//   源和目标都是 RGB888 WxH, 内容完全一样, RGA 只做搬运不做任何换算,
//   于是结果必然和源【逐字节相同】——
//   · 若 wstride 是【像素】: 目标行宽 = W*3 字节, 结果逐字节相同
//   · 若 wstride 是【字节】: 目标行宽 = wstride 字节, 装不下 W*3 个像素,
//     结果要么被拒, 要么被横向压扁 —— 结果不会逐字节相同
//
// 顺带把 NV12->NV12 也测一遍(确认色度平面的步长怎么算)。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include <vector>

#include "im2d.h"
#include "rga.h"
#include "dmabuf.h"

namespace {

inline bool rga_ok(IM_STATUS s) {
    return s == IM_STATUS_NOERROR || s == IM_STATUS_SUCCESS;
}

// 用 legacy 方式(fd 直接填进 rga_buffer_t)跑一次
bool run_legacy(int sfd, int dfd, int w, int h, int sws, int shs, int dws, int dhs,
                int sfmt, int dfmt) {
    rga_buffer_t s, d, pat;
    memset(&s, 0, sizeof(s));
    memset(&d, 0, sizeof(d));
    memset(&pat, 0, sizeof(pat));
    s.fd = sfd; s.width = w; s.height = h; s.wstride = sws; s.hstride = shs; s.format = sfmt;
    d.fd = dfd; d.width = w; d.height = h; d.wstride = dws; d.hstride = dhs; d.format = dfmt;
    im_rect sr, dr, pr;
    memset(&sr, 0, sizeof(sr));
    memset(&dr, 0, sizeof(dr));
    memset(&pr, 0, sizeof(pr));
    sr.x = 0; sr.y = 0; sr.width = w; sr.height = h;
    dr.x = 0; dr.y = 0; dr.width = w; dr.height = h;
    IM_STATUS st = improcess(s, d, pat, sr, dr, pr, -1, nullptr, nullptr, 0);
    return rga_ok(st);
}

// 用 importbuffer_fd 方式跑一次
bool run_import(int sfd, int ssize, int dfd, int dsize, int w, int h, int sws, int shs,
                int dws, int dhs, int sfmt, int dfmt) {
    rga_buffer_handle_t sh = importbuffer_fd(sfd, ssize);
    rga_buffer_handle_t dh = importbuffer_fd(dfd, dsize);
    if (!sh || !dh) return false;
    rga_buffer_t s = wrapbuffer_handle_t(sh, w, h, sws, shs, sfmt);
    rga_buffer_t d = wrapbuffer_handle_t(dh, w, h, dws, dhs, dfmt);
    rga_buffer_t pat;
    memset(&pat, 0, sizeof(pat));
    im_rect sr, dr, pr;
    memset(&sr, 0, sizeof(sr));
    memset(&dr, 0, sizeof(dr));
    memset(&pr, 0, sizeof(pr));
    sr.x = 0; sr.y = 0; sr.width = w; sr.height = h;
    dr.x = 0; dr.y = 0; dr.width = w; dr.height = h;
    IM_STATUS st = improcess(s, d, pat, sr, dr, pr, -1, nullptr, nullptr, 0);
    releasebuffer_handle(sh);
    releasebuffer_handle(dh);
    return rga_ok(st);
}

void report(const char* tag, bool ok, const uint8_t* dst, size_t dcap,
             const uint8_t* src, size_t scap, size_t logical) {
    if (!ok) {
        printf("  %-46s RGA 失败\n", tag);
        return;
    }
    size_t written = 0;
    for (size_t i = 0; i < dcap; ++i) if (dst[i] != 0xA5) ++written;
    size_t same = 0;
    for (size_t i = 0; i < logical && i < scap; ++i) if (dst[i] == src[i]) ++same;
    printf("  %-46s 写了 %zu/%zu 字节, 与源逐字节相同 %zu/%zu (%.1f%%)\n",
           tag, written, dcap, same, logical, 100.0 * same / logical);
}

}  // namespace

int main() {
    // ★ 尺寸不能太小: 实测 64x64 的 RGB888 纯拷贝会被 RGA 驱动拒掉
    //   ("no core match" / "RGA_MMU unsupported memory larger than 4G"),
    //   640x480 正常。所以这里用 640x480。
    const int W = 640, H = 480;
    const size_t rgb_bytes = static_cast<size_t>(W) * H * 3;

    printf("==== RGA wstride 单位探针 (RGB888 %dx%d 纯拷贝) ====\n", W, H);
    printf("后端: %s\n\n", rk::dma_buf_backend());

    // 源: RGB888, 逐像素递增
    std::vector<uint8_t> src_rgb(rgb_bytes);
    for (size_t i = 0; i < rgb_bytes; ++i) src_rgb[i] = static_cast<uint8_t>(i * 7 + 3);

    struct Case {
        const char* name;
        int sws, dws;
    };
    const Case cases[] = {
        {"wstride=像素(640)",          640,  640},
        {"wstride=字节(1920)",        1920, 1920},
        {"源像素(640)/目标字节(1920)", 640, 1920},
        {"源字节(1920)/目标像素(640)",1920,  640},
    };

    for (const auto& c : cases) {
        printf("--- %s ---\n", c.name);
        // 目标缓冲按"最宽松"的大小分配: max(像素解释, 字节解释) * H * 3
        const size_t dcap = static_cast<size_t>(c.dws) * H * 3 + 4096;
        const size_t scap = static_cast<size_t>(c.sws) * H * 3 + 4096;

        int sfd = rk::dma_buf_alloc(scap, true);
        char src_backend[128];
        snprintf(src_backend, sizeof(src_backend), "%s", rk::dma_buf_backend());
        int dfd = rk::dma_buf_alloc(dcap, true);
        if (sfd < 0 || dfd < 0) { printf("  dma-buf 分配失败\n"); return 1; }
        printf("  [分配] src fd=%d (%s, %zu 字节)  dst fd=%d (%s, %zu 字节)\n",
               sfd, src_backend, scap, dfd, rk::dma_buf_backend(), dcap);
        uint8_t* sp = static_cast<uint8_t*>(rk::dma_buf_map(sfd, scap));
        uint8_t* dp = static_cast<uint8_t*>(rk::dma_buf_map(dfd, dcap));
        memset(sp, 0, rgb_bytes);
        memcpy(sp, src_rgb.data(), rgb_bytes);
        rk::dma_buf_sync(sfd, 0, scap, true);

        memset(dp, 0xA5, dcap);
        const bool ok_l = run_legacy(sfd, dfd, W, H, c.sws, H, c.dws, H,
                                     RK_FORMAT_RGB_888, RK_FORMAT_RGB_888);
        rk::dma_buf_sync(dfd, 0, dcap, false);
        report("legacy(rga_buffer_t.fd)", ok_l, dp, dcap, sp, scap, rgb_bytes);

        memset(dp, 0xA5, dcap);
        const bool ok_i = run_import(sfd, static_cast<int>(scap), dfd, static_cast<int>(dcap),
                                     W, H, c.sws, H, c.dws, H,
                                     RK_FORMAT_RGB_888, RK_FORMAT_RGB_888);
        rk::dma_buf_sync(dfd, 0, dcap, false);
        report("importbuffer_fd", ok_i, dp, dcap, sp, scap, rgb_bytes);

        rk::dma_buf_unmap(sp, scap);
        rk::dma_buf_unmap(dp, dcap);
        ::close(sfd);
        ::close(dfd);
        printf("\n");
    }

    // ---------------- NV12 -> NV12 纯拷贝 ----------------
    {
        printf("==== NV12 %dx%d 纯拷贝 (确认色度平面步长) ====\n", W, H);
        const int shs = H + 8;                    // 高度按 8 对齐, 模拟 MPP
        const size_t sz = static_cast<size_t>(W) * shs * 3 / 2;
        std::vector<uint8_t> src_nv12(sz);
        for (size_t i = 0; i < sz; ++i) src_nv12[i] = static_cast<uint8_t>(i * 3 + 11);

        int sfd = rk::dma_buf_alloc(sz, true);
        int dfd = rk::dma_buf_alloc(sz, true);
        uint8_t* sp = static_cast<uint8_t*>(rk::dma_buf_map(sfd, sz));
        uint8_t* dp = static_cast<uint8_t*>(rk::dma_buf_map(dfd, sz));
        memcpy(sp, src_nv12.data(), sz);
        rk::dma_buf_sync(sfd, 0, sz, true);

        memset(dp, 0xA5, sz);
        const bool ok = run_legacy(sfd, dfd, W, H, W, shs, W, shs,
                                   RK_FORMAT_YCbCr_420_SP, RK_FORMAT_YCbCr_420_SP);
        rk::dma_buf_sync(dfd, 0, sz, false);
        size_t same = 0;
        for (size_t i = 0; i < sz; ++i) if (dp[i] == src_nv12[i]) ++same;
        size_t written = 0;
        for (size_t i = 0; i < sz; ++i) if (dp[i] != 0xA5) ++written;
        printf("  legacy NV12->NV12: %s, 写了 %zu/%zu, 相同 %zu/%zu (%.1f%%)\n",
               ok ? "成功" : "失败", written, sz, same, sz, 100.0 * same / sz);
        rk::dma_buf_unmap(sp, sz);
        rk::dma_buf_unmap(dp, sz);
        ::close(sfd);
        ::close(dfd);
    }
    return 0;
}
