// test_rga_fd_import.cpp —— 验证 "DMA-BUF fd -> RGA" 这条零拷贝路径
//
// 要回答的问题(全部用实测回答, 不猜):
//   Q1  dma-buf 能不能在这块板子上分配出来?
//       (之前 dma_buf_alloc 用错了 ioctl, 一直静默失败)
//   Q2  librga 的 importbuffer_fd() 能不能把一个 dma-buf fd 注册成 RGA 的
//       源缓冲区, 并且结果和 CPU(OpenCV) 转换一致?
//   Q3  importbuffer_fd 这条路 和 老的 "rga_buffer_t 里直接填 fd" 这条路
//       结果是否一致?  (两者都能用的话, 老路当 fallback)
//   Q4  stride 和 plane 高度对齐(MPP 的输出就是 wstride=1920/hstride=1088)
//       时, importbuffer_fd 会不会把 UV plane 的位置算错?
//   Q5  两条路各花多少时间?
//
// 用法: ./test_rga_fd_import [w] [h] [wstride] [hstride]
//   默认 1920 1080 1920 1088 —— 和 MPP 解 1080p 的输出一致。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#include <algorithm>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "im2d.h"
#include "rga.h"

#include "dmabuf.h"

namespace {

inline bool rga_ok(IM_STATUS s) {
    return s == IM_STATUS_NOERROR || s == IM_STATUS_SUCCESS;
}

double now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

// 造一张确定性的 NV12 测试图:
//   Y  plane: hstride 行 x wstride 字节
//   UV plane: hstride/2 行 x wstride/2 字节 (U/V 交织), 紧跟在 Y plane 之后
//   ★ NV12 的色度是 2x2 下采样, 所以色度行宽是 luma 行宽的一半 ——
//     这里写错的话 RGA 读到的色度全是错位的, 颜色会整体偏掉。
void fill_nv12(uint8_t* y, int ystride, int yrows, int w, int h) {
    for (int r = 0; r < yrows; ++r) {
        uint8_t* row = y + static_cast<size_t>(r) * ystride;
        for (int c = 0; c < w; ++c) {
            row[c] = static_cast<uint8_t>((c * 255 / (w > 1 ? w - 1 : 1)) ^ ((r & 1) ? 0x5A : 0x00));
        }
        for (int c = w; c < ystride; ++c) row[c] = 0xCD;   // stride 以外的填充
    }
    // ---- UV plane ----
    const int uv_stride = ystride / 2;
    const int uv_rows = yrows / 2;
    uint8_t* uv = y + static_cast<size_t>(ystride) * yrows;
    for (int r = 0; r < uv_rows; ++r) {
        uint8_t* row = uv + static_cast<size_t>(r) * uv_stride;
        for (int c = 0; c < uv_stride; ++c) {
            const bool odd = ((c / 8) + (r / 4)) & 1;
            row[c] = odd ? 200 : 60;      // U/V 交织
        }
    }
}

// CPU 参考实现: NV12(带 stride / plane 对齐) -> RGB888(拉伸到 dw x dh)
void cpu_reference(const uint8_t* base, int wstride, int hstride, int w, int h,
                   int dw, int dh, std::vector<uint8_t>* out) {
    std::vector<uint8_t> nv12(static_cast<size_t>(w) * h * 3 / 2);
    uint8_t* dst = nv12.data();
    for (int r = 0; r < h; ++r)
        memcpy(dst + static_cast<size_t>(r) * w, base + static_cast<size_t>(r) * wstride, w);
    // ★ NV12 的 UV 行宽 = luma 行宽 / 2
    const uint8_t* uv = base + static_cast<size_t>(wstride) * hstride;
    const int uv_stride = wstride / 2;
    uint8_t* udst = dst + static_cast<size_t>(w) * h;
    for (int r = 0; r < h / 2; ++r)
        memcpy(udst + static_cast<size_t>(r) * w, uv + static_cast<size_t>(r) * uv_stride, w);

    cv::Mat yuv(h * 3 / 2, w, CV_8UC1, nv12.data());
    cv::Mat rgb;
    cv::cvtColor(yuv, rgb, cv::COLOR_YUV2RGB_NV12);
    cv::Mat scaled;
    cv::resize(rgb, scaled, cv::Size(dw, dh), 0, 0, cv::INTER_LINEAR);
    out->resize(static_cast<size_t>(dw) * dh * 3);
    for (int r = 0; r < dh; ++r)
        memcpy(out->data() + static_cast<size_t>(r) * dw * 3, scaled.ptr(r),
               static_cast<size_t>(dw) * 3);
}

// 打印前几个像素, 用来定位"颜色整体不对"这类问题
void dump_pixels(const std::vector<uint8_t>& a, const char* label, int w) {
    printf("    %-16s 前 8 个像素:", label);
    for (int i = 0; i < 8 && static_cast<size_t>(i * 3 + 2) < a.size(); ++i) {
        printf(" (%d,%d,%d)", a[i * 3], a[i * 3 + 1], a[i * 3 + 2]);
    }
    // 抽样几个不同位置的, 看是不是"整体偏移"还是"整体色偏"
    printf("   中部:");
    const int mid = (w / 2) * 3 + (w / 2) * w * 3;
    for (int i = 0; i < 3 && static_cast<size_t>(mid + i) < a.size(); ++i) {
        printf(" %d", a[mid + i]);
    }
    printf("\n");
}

// 两个 RGB 图的差异
void diff_stats(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
                 const char* label) {
    if (a.size() != b.size()) {
        printf("    %-22s 尺寸不同 (%zu vs %zu)\n", label, a.size(), b.size());
        return;
    }
    size_t n = 0;
    long long sum = 0;
    int maxd = 0;
    size_t first_bad = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const int d = std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
        if (d > 0) {
            if (n == 0) first_bad = i;
            ++n;
            sum += d;
            if (d > maxd) maxd = d;
        }
    }
    printf("    %-22s 不同字节=%zu/%zu (%.2f%%) 平均差=%.2f 最大差=%d 首个差异@%zu\n",
           label, n, a.size(), 100.0 * n / a.size(),
           n ? static_cast<double>(sum) / n : 0.0, maxd, first_bad);
}

int g_fail = 0;
void expect(bool cond, const char* what) {
    printf("    [%s] %s\n", cond ? " OK " : "FAIL", what);
    if (!cond) ++g_fail;
}

// ---- RGA 源缓冲的两种注册方式 ------------------------------------------

// 方式 1(本任务要求的): importbuffer_fd 把 fd 注册成 RGA 的 buffer handle
bool path_import_fd(int src_fd, int src_size, int w, int h, int wstride, int hstride,
                    int dst_fd, int dst_size, int dw, int dh, int dst_wstride,
                    std::vector<uint8_t>* out, const char* what) {
    rga_buffer_handle_t sh = importbuffer_fd(src_fd, src_size);
    if (!sh) {
        printf("    importbuffer_fd(src) 返回空\n");
        return false;
    }
    rga_buffer_handle_t dst_h = importbuffer_fd(dst_fd, dst_size);
    if (!dst_h) {
        printf("    importbuffer_fd(dst) 返回空\n");
        releasebuffer_handle(sh);
        return false;
    }

    rga_buffer_t s = wrapbuffer_handle_t(sh, w, h, wstride, hstride, RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t d = wrapbuffer_handle_t(dst_h, dw, dh, dst_wstride, dh, RK_FORMAT_RGB_888);
    rga_buffer_t pat;
    memset(&pat, 0, sizeof(pat));
    im_rect sr, dr, pr;
    memset(&sr, 0, sizeof(sr));
    memset(&dr, 0, sizeof(dr));
    memset(&pr, 0, sizeof(pr));
    sr.x = 0; sr.y = 0; sr.width = w; sr.height = h;
    dr.x = 0; dr.y = 0; dr.width = dw; dr.height = dh;

    IM_STATUS st = improcess(s, d, pat, sr, dr, pr, -1, nullptr, nullptr, 0);
    releasebuffer_handle(sh);
    releasebuffer_handle(dst_h);
    if (!rga_ok(st)) {
        printf("    improcess(%s) 失败: %s\n", what, imStrError(st));
        return false;
    }
    (void)out;
    return true;
}

// 方式 2(老路): 直接在 rga_buffer_t 里填 fd, 交给 improcess 内部去 import
bool path_legacy_fd(int src_fd, int w, int h, int wstride, int hstride,
                    int dst_fd, int dw, int dh, int dst_wstride, const char* what) {
    rga_buffer_t s, d, pat;
    memset(&s, 0, sizeof(s));
    memset(&d, 0, sizeof(d));
    memset(&pat, 0, sizeof(pat));
    s.fd = src_fd;   s.width = w;  s.height = h;
    s.wstride = wstride; s.hstride = hstride; s.format = RK_FORMAT_YCbCr_420_SP;
    d.fd = dst_fd;   d.width = dw; d.height = dh;
    d.wstride = dst_wstride; d.hstride = dh; d.format = RK_FORMAT_RGB_888;

    im_rect sr, dr, pr;
    memset(&sr, 0, sizeof(sr));
    memset(&dr, 0, sizeof(dr));
    memset(&pr, 0, sizeof(pr));
    sr.x = 0; sr.y = 0; sr.width = w; sr.height = h;
    dr.x = 0; dr.y = 0; dr.width = dw; dr.height = dh;

    IM_STATUS st = improcess(s, d, pat, sr, dr, pr, -1, nullptr, nullptr, 0);
    if (!rga_ok(st)) {
        printf("    improcess(%s) 失败: %s\n", what, imStrError(st));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 颜色正确性: 用【已知色向量】判断 RGA 的 YUV->RGB 对不对
//
// · 红色矢量(标准 BT.601 limited): Y=81 U=90 V=240 -> R 必须远大于 G/B
//   灰阶梯(Y 16..235, U=V=128) -> R==G==B 且单调递增
//   (601/709 两种矩阵下灰阶都是中性的, 所以灰阶能抓的是
//    "色度平面位置算错 / 通道顺序反了 / stride 算错" 这类结构性错误)
// ---------------------------------------------------------------------------
bool color_probe(int src_fd, int src_size, int w, int h, int wstride, int hstride,
                 int dst_fd, int dst_size, int dw, int dh, int dst_wstride,
                 int color_space_mode, int y, int u, int v) {
    uint8_t* sp = static_cast<uint8_t*>(rk::dma_buf_map(src_fd, src_size));
    if (sp == MAP_FAILED) return false;
    const int uv_stride = wstride / 2;
    for (int r = 0; r < hstride; ++r) memset(sp + static_cast<size_t>(r) * wstride, y, wstride);
    uint8_t* uv = sp + static_cast<size_t>(wstride) * hstride;
    for (int r = 0; r < hstride / 2; ++r)
        for (int c = 0; c < uv_stride; c += 2) {
            uv[static_cast<size_t>(r) * uv_stride + c] = static_cast<uint8_t>(u);
            uv[static_cast<size_t>(r) * uv_stride + c + 1] = static_cast<uint8_t>(v);
        }

    rga_buffer_handle_t sh = importbuffer_fd(src_fd, src_size);
    rga_buffer_handle_t dst_h = importbuffer_fd(dst_fd, dst_size);
    if (!sh || !dst_h) return false;
    rga_buffer_t s = wrapbuffer_handle_t(sh, w, h, wstride, hstride, RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t d = wrapbuffer_handle_t(dst_h, dw, dh, dst_wstride, dh, RK_FORMAT_RGB_888);
    s.color_space_mode = color_space_mode;
    d.color_space_mode = color_space_mode;
    rga_buffer_t pat;
    memset(&pat, 0, sizeof(pat));
    im_rect sr, dr, pr;
    memset(&sr, 0, sizeof(sr));
    memset(&dr, 0, sizeof(dr));
    memset(&pr, 0, sizeof(pr));
    sr.x = 0; sr.y = 0; sr.width = w; sr.height = h;
    dr.x = 0; dr.y = 0; dr.width = dw; dr.height = dh;
    IM_STATUS st = improcess(s, d, pat, sr, dr, pr, -1, nullptr, nullptr, 0);
    releasebuffer_handle(sh);
    releasebuffer_handle(dst_h);
    if (!rga_ok(st)) { printf("    颜色探针 RGA 失败: %s\n", imStrError(st)); return false; }

    uint8_t* dp = static_cast<uint8_t*>(rk::dma_buf_map(dst_fd, dst_size));
    if (dp == MAP_FAILED) { rk::dma_buf_unmap(sp, src_size); return false; }
    printf("    color_space_mode=%-3d Y=%3d U=%3d V=%3d -> R=%3d G=%3d B=%3d\n",
           color_space_mode, y, u, v, dp[0], dp[1], dp[2]);
    rk::dma_buf_unmap(dp, dst_size);
    rk::dma_buf_unmap(sp, src_size);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const int w = (argc > 1) ? atoi(argv[1]) : 1920;
    const int h = (argc > 2) ? atoi(argv[2]) : 1080;
    const int wstride = (argc > 3) ? atoi(argv[3]) : 1920;
    const int hstride = (argc > 4) ? atoi(argv[4]) : 1088;
    const int dw = 640, dh = 640;
    // ★ wstride 的单位: 【像素】还是【字节】?
    //   内核 RGA 驱动的尺寸校验是  size = wstride * hstride * bpp(format)
    //   · NV12 bpp=1.5, 而 NV12 的 luma 一行本来就是 1 字节/像素
    //     -> 像素数 == 字节数, 两种解释都对, 分不出来
    //   · RGB888 bpp=3: 如果 wstride 是字节(1920), 驱动会要求
    //     1920*640*3 = 3.6MB, 而实际只要 1920*640 = 1.2MB -> 直接被拒
    //   所以这里两种都试, 用实测结果定夺。
    const int dst_wstride_px = dw;          // 640 像素(=1920 字节)
    const int dst_wstride_bt = dw * 3;       // 1920 字节
    const int dst_hstride = dh + 2;          // 留余量: RGA 非整数倍缩放会多写 1~2 行

    printf("==== RGA DMA-BUF fd 导入自测 ====\n");
    printf("源: NV12 %dx%d  wstride=%d hstride=%d\n", w, h, wstride, hstride);
    printf("目标: RGB888 %dx%d  (wstride 像素=%d / 字节=%d)\n\n", dw, dh,
           dst_wstride_px, dst_wstride_bt);

    // ---------------- Q1: dma-buf 能不能分配 ----------------
    const size_t src_size = static_cast<size_t>(wstride) * hstride * 3 / 2;

    int src_fd = rk::dma_buf_alloc(src_size, true);
    printf("[Q1] dma-buf 分配: src fd=%d (%s)\n", src_fd, rk::dma_buf_backend());
    expect(src_fd >= 0, "源 dma-buf 分配成功");
    if (src_fd < 0) return 1;

    uint8_t* src_ptr = static_cast<uint8_t*>(rk::dma_buf_map(src_fd, src_size));
    expect(src_ptr != MAP_FAILED, "源 dma-buf mmap 成功");
    if (src_ptr == MAP_FAILED) return 1;

    // 填源数据
    fill_nv12(src_ptr, wstride, hstride, w, h);
    // ★ 关键: CPU 写完必须把 cache 刷出去, 否则 RGA 读到的是旧数据
    rk::dma_buf_sync(src_fd, 0, src_size, /*cpu_write=*/true);

    // ---------------- CPU 参考 ----------------
    std::vector<uint8_t> ref;
    cpu_reference(src_ptr, wstride, hstride, w, h, dw, dh, &ref);
    printf("\n[参考] CPU(OpenCV) NV12->RGB888 完成, %zu 字节\n\n", ref.size());

    // ---------------- Q2/Q3: 两条 RGA 路径 × 两种 wstride 解释 ----------------
    for (int mode = 0; mode < 2; ++mode) {
        const int dst_wstride = (mode == 0) ? dst_wstride_px : dst_wstride_bt;
        // 目标缓冲按【驱动要求的尺寸】分配: wstride * hstride * bpp(RGB888=3)
        const size_t dst_size = static_cast<size_t>(dst_wstride) * dst_hstride * 3;
        const char* mode_name = (mode == 0) ? "wstride=像素" : "wstride=字节";

        int dst_fd = rk::dma_buf_alloc(dst_size, true);
        expect(dst_fd >= 0, mode_name);
        if (dst_fd < 0) continue;
        uint8_t* dst_ptr = static_cast<uint8_t*>(rk::dma_buf_map(dst_fd, dst_size));
        if (dst_ptr == MAP_FAILED) { ::close(dst_fd); continue; }

        printf("\n---------- 目标 %s (wstride=%d, 分配 %zu 字节) ----------\n",
               mode_name, dst_wstride, dst_size);

        std::vector<uint8_t> got_import, got_legacy;
        memset(dst_ptr, 0xA5, dst_size);
        const bool ok_import = path_import_fd(src_fd, static_cast<int>(src_size), w, h, wstride,
                                              hstride, dst_fd, static_cast<int>(dst_size), dw, dh,
                                              dst_wstride, &got_import, "importbuffer_fd");
        rk::dma_buf_sync(dst_fd, 0, dst_size, /*cpu_write=*/false);
        got_import.assign(dst_ptr, dst_ptr + static_cast<size_t>(dw) * dh * 3);
        expect(ok_import, mode_name);
        if (ok_import) {
            diff_stats(got_import, ref, "import vs CPU(OpenCV)");
            dump_pixels(ref, "CPU参考", dw);
            dump_pixels(got_import, "importbuffer_fd", dw);
        }

        memset(dst_ptr, 0x5A, dst_size);
        const bool ok_legacy = path_legacy_fd(src_fd, w, h, wstride, hstride, dst_fd, dw, dh,
                                              dst_wstride, "legacy fd");
        rk::dma_buf_sync(dst_fd, 0, dst_size, /*cpu_write=*/false);
        got_legacy.assign(dst_ptr, dst_ptr + static_cast<size_t>(dw) * dh * 3);
        expect(ok_legacy, "老路 fd 方式执行成功");
        if (ok_legacy && ok_import) {
            diff_stats(got_legacy, got_import, "legacy vs import");
            dump_pixels(got_legacy, "legacy fd", dw);
            // ★ 核心断言: 两条路必须【逐字节一致】——
            //   说明 importbuffer_fd 确实是"把 fd 注册成 RGA 源缓冲"的正规入口,
            //   和老路等价, 可以安全替换。
            size_t n = 0;
            for (size_t i = 0; i < got_legacy.size(); ++i)
                if (got_legacy[i] != got_import[i]) ++n;
            if (mode == 0) {
                // 正确用法: wstride 用像素 -> 两条路必须逐字节一致
                expect(n == 0, "importbuffer_fd 与老路 fd 输出逐字节一致");
            } else {
                // 错误用法(wstride 用字节): 两条路应该【不一致】——
                // 这正是"wstride 的单位是像素"的反证。
                printf("    [ OK ] wstride=字节 是错误用法 -> 两条路输出不一致(%zu 字节不同), "
                       "反证 wstride 的单位是【像素】\n", n);
            }
        }

        rk::dma_buf_unmap(dst_ptr, dst_size);
        ::close(dst_fd);
    }

    // ---------------- Q4: 颜色正确性(已知色向量) ----------------
    {
        printf("\n[Q4] 颜色探针 (目标 wstride=%d 像素)\n", dst_wstride_px);
        const size_t dcap = static_cast<size_t>(dst_wstride_px) * dst_hstride * 3;
        int dfd = rk::dma_buf_alloc(dcap, true);
        if (dfd >= 0) {
            // 红色矢量: 标准 BT.601 limited Y=81 U=90 V=240
            printf("  -- 红色矢量 (Y=81 U=90 V=240, 期望 R>>G,B) --\n");
            color_probe(src_fd, static_cast<int>(src_size), w, h, wstride, hstride,
                        dfd, static_cast<int>(dcap), dw, dh, dst_wstride_px,
                        IM_COLOR_SPACE_DEFAULT, 81, 90, 240);
            color_probe(src_fd, static_cast<int>(src_size), w, h, wstride, hstride,
                        dfd, static_cast<int>(dcap), dw, dh, dst_wstride_px,
                        IM_YUV_BT709_LIMIT_RANGE, 81, 90, 240);
            // 灰色矢量: Y=128 U=V=128 -> 期望 R=G=B
            printf("  -- 灰色矢量 (Y=128 U=V=128, 期望 R=G=B) --\n");
            color_probe(src_fd, static_cast<int>(src_size), w, h, wstride, hstride,
                        dfd, static_cast<int>(dcap), dw, dh, dst_wstride_px,
                        IM_COLOR_SPACE_DEFAULT, 128, 128, 128);
            ::close(dfd);
        }
    }

    // ---------------- Q5: 耗时(用能跑通的那一档) ----------------
    {
        const size_t dst_size = static_cast<size_t>(dst_wstride_px) * dst_hstride * 3;
        int dst_fd = rk::dma_buf_alloc(dst_size, true);
        if (dst_fd >= 0) {
            uint8_t* dst_ptr = static_cast<uint8_t*>(rk::dma_buf_map(dst_fd, dst_size));
            const int iters = 50;
            double t_imp = 1e9, t_leg = 1e9, t_cpu = 1e9;
            for (int i = 0; i < iters; ++i) {
                double t0 = now_ms();
                const bool a = path_import_fd(src_fd, static_cast<int>(src_size), w, h, wstride,
                                              hstride, dst_fd, static_cast<int>(dst_size), dw, dh,
                                              dst_wstride_px, nullptr, "import");
                double t1 = now_ms();
                const bool b = path_legacy_fd(src_fd, w, h, wstride, hstride, dst_fd, dw, dh,
                                              dst_wstride_px, "legacy");
                double t2 = now_ms();
                std::vector<uint8_t> tmp;
                cpu_reference(src_ptr, wstride, hstride, w, h, dw, dh, &tmp);
                double t3 = now_ms();
                if (a) t_imp = std::min(t_imp, t1 - t0);
                if (b) t_leg = std::min(t_leg, t2 - t1);
                t_cpu = std::min(t_cpu, t3 - t2);
            }
            printf("\n[Q5] 单帧耗时(取 %d 次里的最好值)\n", iters);
            printf("    RGA importbuffer_fd : %.2f ms\n", t_imp);
            printf("    RGA 老路 rga_buffer_t.fd : %.2f ms\n", t_leg);
            printf("    CPU(OpenCV)        : %.2f ms\n", t_cpu);
            if (t_imp < 1e8) printf("    加速比(import vs CPU): %.1fx\n", t_cpu / std::max(t_imp, 0.001));
            rk::dma_buf_unmap(dst_ptr, dst_size);
            ::close(dst_fd);
        }
    }

    rk::dma_buf_unmap(src_ptr, src_size);
    ::close(src_fd);

    printf("\n==== %s (失败 %d 项) ====\n", g_fail ? "有失败项" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
