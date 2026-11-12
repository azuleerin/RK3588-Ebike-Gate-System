// src/minimal/unit_rga.cc —— rga-only 最小化单元
//
// 隔离边界（本单元**只允许**触碰 RGA）:
//   ✅ 允许: dma-buf 分配、librga import/improcess/release
//   ❌ 禁止: MPP 解码、RKNN 推理、MQTT、串口、测速、画框
//
// 输入: 预录的 NV12 帧(由 decoder-only --dump 产出) + .meta 几何副文件。
//       刻意不接 MPP, 这样 RGA 被完全单独隔离出来测。
//
// ------------------------------------------------------------------
// 本单元要回答两个问题:
//
// 【问题一】P0-3: 每帧 import/release 是不是元凶?
//   业务代码 rga_convert.cc:515/532 每帧对同一 dma-buf 做 2~6 次
//   importbuffer_fd()/releasebuffer_handle()。在 RGA 驱动侧, 这就是
//   IOMMU 的 map/unmap(建页表/拆页表)。而我们已经抓到过一次
//   rk_iommu_enable_stall 的 external abort —— 同一个子系统。
//   => --import-mode per-frame (带病, 默认) | cached (修复后的对照)
//
// 【问题二】P0-4: wstride 单位错误的后果到底有多大?
//   license_plate_rec.cc:553 写的是 d.wstride = resized_w * 3,
//   而 librga 的 wstride 单位是【像素】不是字节, 行距因此大 3 倍。
//   => --wstride-mode pixels (正确, 默认) | bytes (复现该 bug)
//
//   ⚠️ 安全设计: 目标缓冲开 4 倍余量 + 尾部哨兵区。
//      "bytes" 模式下的越界写会落进我们自己控制的哨兵区, 而不是冲出去
//      打坏内核。证据照样拿得到(哨兵被踩 + 边界数学断言), 但不会把板子搞挂。
//      这正是隔离单元的意义: 在可控的前提下复现危险行为。
//
// 内存物理属性:
//   - 源/目标缓冲: DMA-BUF(/dev/dma_heap/system-uncached), RGA 直接 DMA 读写
//   - 申请者/释放者: 本单元, 生命周期 = 整个进程, 只在主线程使用
//   - 文件帧数据: 普通堆内存(MemGuard + canary)

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "dmabuf.h"
#include "rk/fd_ledger.h"
#include "rk/log.h"
#include "rk/mem_guard.h"
#include "rk/minimal/unit.h"
#include "rk/stack_guard.h"
#include "safe_probe.h"

// librga 的 im2d.h 是 C++ 头(带重载), 不能用 extern "C" 包, 否则与
// 头文件里自己的 C++ 声明冲突。业务代码 rga_convert.cc 也是直接包含。
#include "im2d.h"

namespace rk {
namespace minimal {
namespace {

double NowMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 读 .meta 副文件: width/height/wstride/hstride
struct Geometry {
    int width = 0;
    int height = 0;
    int wstride = 0;
    int hstride = 0;
};

bool read_geometry(const std::string& path, Geometry* g) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    char key[64] = {0};
    int val = 0;
    // ⚠️ 格式串开头的空格不能省: %[^=] 不跳前导空白, 否则第二次读到的
    //    key 会带上换行符("\nheight"), 导致字段匹配不上。
    while (fscanf(f, " %63[^=]=%d", key, &val) == 2) {
        if (!strcmp(key, "width")) g->width = val;
        else if (!strcmp(key, "height")) g->height = val;
        else if (!strcmp(key, "wstride")) g->wstride = val;
        else if (!strcmp(key, "hstride")) g->hstride = val;
    }
    fclose(f);
    return g->width > 0 && g->height > 0;
}

// improcess 的调用上下文(SafeProbe 需要一个可调用的入口)
struct ImprocessCtx {
    rga_buffer_t s;
    rga_buffer_t d;
    rga_buffer_t pat;
    im_rect srect;
    im_rect drect;
    im_rect prect;
    IM_STATUS st;
};

bool do_improcess(void* p) {
    ImprocessCtx* c = static_cast<ImprocessCtx*>(p);
    c->st = improcess(c->s, c->d, c->pat, c->srect, c->drect, c->prect,
                       -1, nullptr, nullptr, 0);
    return true;
}

constexpr uint8_t kGuardByte = 0x5A;

}  // namespace

int run_rga_unit(const UnitOptions& opt) {
    RK_LOG_INFO("rga", "start", "===== rga-only 单元启动 (只触碰 RGA, 不含 MPP/RKNN) =====");
    RK_LOG_INFO("rga", "start", "输入源=%s", opt.src.c_str());

    // ---- 参数 ----
    // --src nv12:<file>
    std::string src_path = opt.src;
    if (src_path.rfind("nv12:", 0) == 0) src_path = src_path.substr(5);

    // wstride 模式: pixels = 正确(默认); bytes = 复现 license_plate_rec.cc:553 的 bug
    const std::string wstride_mode = opt.wstride_mode;
    // import 模式: per-frame = 带病(默认); cached = 对照
    const std::string import_mode = opt.import_mode;

    const int dst_w = 640;
    const int dst_h = 640;
    const int dst_bpp = 3;
    const int dst_format = RK_FORMAT_RGB_888;

    RK_LOG_INFO("rga", "start",
                "wstride模式=%s import模式=%s 输出=%dx%d", wstride_mode.c_str(),
                import_mode.c_str(), dst_w, dst_h);

    // ---- 读取源帧 ----
    Geometry geo;
    if (!read_geometry(src_path + ".meta", &geo)) {
        RK_LOG_ERROR("rga", "src", "读不到几何副文件 %s.meta", src_path.c_str());
        return 1;
    }
    size_t src_size = 0;
    uint8_t* src_raw = load_file(src_path, &src_size, "rga-src");
    if (!src_raw) return 1;
    const size_t expect = static_cast<size_t>(geo.wstride) * geo.hstride * 3 / 2;
    if (src_size != expect) {
        RK_LOG_ERROR("rga", "src", "NV12 尺寸不符: 文件 %zu 字节, 期望 %zu 字节", src_size,
                     expect);
        MemGuard::free_ref(src_raw);
        return 1;
    }
    RK_LOG_INFO("rga", "src", "源帧 %s: 可见 %dx%d stride %dx%d (%zu 字节)",
                src_path.c_str(), geo.width, geo.height, geo.wstride, geo.hstride,
                src_size);

    // ---- 源缓冲: DMA-BUF (RGA 直接 DMA 读) ----
    const int src_fd = rk::dma_buf_alloc(src_size, /*uncached=*/true);
    if (src_fd < 0) {
        RK_LOG_ERROR("rga", "buf", "源 dma_buf_alloc 失败");
        MemGuard::free_ref(src_raw);
        return 1;
    }
    void* src_ptr = rk::dma_buf_map(src_fd, src_size);
    if (src_ptr == MAP_FAILED || !src_ptr) {
        RK_LOG_ERROR("rga", "buf", "源 dma_buf_map 失败");
        ::close(src_fd);
        MemGuard::free_ref(src_raw);
        return 1;
    }
    memcpy(src_ptr, src_raw, src_size);
    MemGuard::free_ref(src_raw);
    FdLedger::track(src_fd, "rga-src", src_size);
    RK_LOG_INFO("rga", "buf", "源 DMA-BUF fd=%d size=%zu", src_fd, src_size);

    // ---- 目标缓冲: DMA-BUF + 4 倍余量 + 哨兵区 ----
    //
    // 为什么要余量: "bytes" 模式下 RGA 算出的行距是正确值的 3 倍,
    // 写入量 = 640*3*3*640 = 3,686,400 字节, 而"应该"只用 1,228,800。
    // 缓冲开到 4 倍, 越界写就落在哨兵区里 —— 拿得到证据, 但不会冲出缓冲。
    const size_t intended = static_cast<size_t>(dst_w) * dst_h * dst_bpp;
    // "bytes" 错误模式下的最大写入量 = wstride(1920px)*3*640行 = 3,686,400。
    // 缓冲要开到能装下它, 这样错误模式的越界写落在我们自己的内存里,
    // 不会冲出去打坏内核 —— 既拿到证据, 又不把板子搞挂。
    const size_t max_bad_write = static_cast<size_t>(dst_w * dst_bpp) * dst_bpp * dst_h;
    const size_t scan_window = 64 * 1024;          // 每帧扫描窗口
    const size_t total = (max_bad_write > intended ? max_bad_write : intended) + scan_window;
    const size_t guard = total - intended;
    const int dst_fd = rk::dma_buf_alloc(total, /*uncached=*/true);
    if (dst_fd < 0) {
        RK_LOG_ERROR("rga", "buf", "目标 dma_buf_alloc 失败 (%zu 字节)", total);
        rk::dma_buf_unmap(src_ptr, src_size);
        FdLedger::close_tracked(src_fd, "rga-src");
        return 1;
    }
    void* dst_ptr = rk::dma_buf_map(dst_fd, total);
    if (dst_ptr == MAP_FAILED || !dst_ptr) {
        RK_LOG_ERROR("rga", "buf", "目标 dma_buf_map 失败");
        ::close(dst_fd);
        rk::dma_buf_unmap(src_ptr, src_size);
        FdLedger::close_tracked(src_fd, "rga-src");
        return 1;
    }
    memset(dst_ptr, kGuardByte, total);
    FdLedger::track(dst_fd, "rga-dst", total);
    RK_LOG_INFO("rga", "buf",
                "目标 DMA-BUF fd=%d 总 %zu 字节 (应有 %zu + 哨兵 %zu)",
                dst_fd, total, intended, guard);

    // ---- wstride 语义选择 ----
    // librga 的 wstride 是【像素】。pixels 模式给正确的 640;
    // bytes 模式故意给 640*3(=1920), 复现 license_plate_rec.cc:553 的单位错误。
    const int dst_wstride_px = (wstride_mode == "bytes") ? (dst_w * dst_bpp) : dst_w;
    const size_t implied_write =
        static_cast<size_t>(dst_wstride_px) * dst_bpp * dst_h;
    RK_LOG_INFO("rga", "wstride",
                "dst.wstride=%d 像素 (模式=%s) => RGA 预期写入 %zu 字节, "
                "业务实际需要 %zu 字节",
                dst_wstride_px, wstride_mode.c_str(), implied_write, intended);
    if (implied_write > intended) {
        RK_LOG_ERROR("rga", "wstride",
                     "★ 边界数学断言失败: 按 wstride=%d 像素算, RGA 会写 %zu 字节, "
                     "但缓冲只按 %zu 字节分配 —— 越界 %zu 字节! "
                     "(这就是 license_plate_rec.cc:553 的单位错误)",
                     dst_wstride_px, implied_write, intended, implied_write - intended);
    }

    // ---- 注册 RGA 缓冲 ----
    // 源: NV12, wstride 是像素(1 字节/像素)
    const int src_wstride_px = geo.wstride;   // NV12 luma 行宽 = 字节 = 像素
    rga_buffer_handle_t src_handle = importbuffer_fd(src_fd, static_cast<int>(src_size));
    rga_buffer_handle_t dst_handle = importbuffer_fd(dst_fd, static_cast<int>(total));
    if (!src_handle || !dst_handle) {
        RK_LOG_ERROR("rga", "import", "importbuffer_fd 失败 src=%u dst=%u", src_handle, dst_handle);
        if (src_handle) releasebuffer_handle(src_handle);
        if (dst_handle) releasebuffer_handle(dst_handle);
        rk::dma_buf_unmap(dst_ptr, total);
        rk::dma_buf_unmap(src_ptr, src_size);
        FdLedger::close_tracked(dst_fd, "rga-dst");
        FdLedger::close_tracked(src_fd, "rga-src");
        return 1;
    }
    RK_LOG_INFO("rga", "import", "importbuffer_fd 成功 src_handle=%u dst_handle=%u", src_handle,
                dst_handle);

    auto make_src_buf = [&](rga_buffer_handle_t h) {
        rga_buffer_t b = wrapbuffer_handle_t(h, geo.width, geo.height, src_wstride_px,
                                             geo.hstride, RK_FORMAT_YCbCr_420_SP);
        b.color_space_mode = IM_COLOR_SPACE_DEFAULT;
        return b;
    };
    auto make_dst_buf = [&](rga_buffer_handle_t h) {
        rga_buffer_t b = wrapbuffer_handle_t(h, dst_w, dst_h, dst_wstride_px, dst_h,
                                             dst_format);
        b.color_space_mode = IM_COLOR_SPACE_DEFAULT;
        return b;
    };

    // ---- 主循环 ----
    TimingStat t_rga;
    TimingStat t_memset;
    TimingStat t_scan;
    uint64_t frames = 0;
    uint64_t guard_hits = 0;
    uint64_t rga_fail = 0;
    const auto t_begin = std::chrono::steady_clock::now();
    auto last_audit = std::chrono::steady_clock::now();
    bool stop = false;

    RK_LOG_INFO("rga", "loop", "开始转换循环 (import模式=%s)", import_mode.c_str());

    while (!stop) {
        StackGuard::mark_stack();

        // 每次都从哨兵值重填目标区, 这样越界写能被立刻发现
        // ⚠️ 目标缓冲是 uncached 的 DMA-BUF(system-uncached), CPU 写它很慢:
        //    4.9MB 的 memset 每帧要 ~140ms。所以只重填"扫描窗口",
        //    其余部分按低频巡检。否则单元自身的开销会淹没 RGA 的行为。
        const double m0 = NowMs();
        memset(static_cast<uint8_t*>(dst_ptr) + intended, kGuardByte, scan_window);
        const double m1 = NowMs();

        rga_buffer_handle_t s_h = src_handle;
        rga_buffer_handle_t d_h = dst_handle;
        bool per_frame = (import_mode == "per-frame");

        if (per_frame) {
            // ★ 带病路径: 每帧重新 import/release (业务代码 rga_convert.cc:515/532)
            s_h = importbuffer_fd(src_fd, static_cast<int>(src_size));
            d_h = importbuffer_fd(dst_fd, static_cast<int>(total));
            if (!s_h || !d_h) {
                RK_LOG_ERROR("rga", "import", "每帧 import 失败");
                if (s_h) releasebuffer_handle(s_h);
                if (d_h) releasebuffer_handle(d_h);
                break;
            }
        }

        rga_buffer_t sb = make_src_buf(s_h);
        rga_buffer_t db = make_dst_buf(d_h);

        ImprocessCtx ctx;
        ctx.s = sb;
        ctx.d = db;
        ctx.pat = rga_buffer_t{};
        ctx.srect = im_rect{0, 0, geo.width, geo.height};
        ctx.drect = im_rect{0, 0, dst_w, dst_h};
        ctx.prect = im_rect{0, 0, dst_w, dst_h};

        const double t0 = NowMs();
        const bool crashed = !rk::SafeProbe::call(&do_improcess, &ctx);
        const double t1 = NowMs();
        if (crashed) {
            RK_LOG_ERROR("rga", "run", "librga 崩溃(SafeProbe 捕获 SIGSEGV), 本次不计入");
            rga_fail++;
        } else if (ctx.st != IM_STATUS_SUCCESS && ctx.st != 1) {
            RK_LOG_ERROR("rga", "run", "improcess 失败 status=%d", static_cast<int>(ctx.st));
            rga_fail++;
        }

        if (per_frame) {
            releasebuffer_handle(s_h);
            releasebuffer_handle(d_h);
        }

        // ---- 哨兵检查: 越界写的直接证据 ----
        // 每帧扫"窗口"(紧跟在应有数据之后的 64KB —— 越界写的第一字节必然落在这里),
        // 其余部分低频全扫。两者都能抓到越界, 但代价差两个数量级。
        const auto* bytes = static_cast<const uint8_t*>(dst_ptr);
        size_t bad = 0;
        size_t first_bad = 0;
        // 跨步采样: uncached 内存上逐字节读非常慢(实测 64KB 要 5.9ms),
        // 而越界写是"每行连续 1920 字节"的整块写, 跨步采样必然命中。
        // 每 100 帧做一次全量扫描, 兜底。
        const size_t scan_end = intended + scan_window;
        const size_t scan_to = (frames % 100 == 0) ? total : scan_end;
        const size_t scan_stride = (frames % 100 == 0) ? 1 : 16;
        for (size_t i = intended; i < scan_to; i += scan_stride) {
            if (bytes[i] != kGuardByte) {
                if (bad == 0) {
                    first_bad = i;
                    RK_LOG_ERROR("rga", "guard",
                                 "★ 哨兵区被写坏! 第 %zu 字节 (偏移 %zu, 应在 %zu 之后) "
                                 "值=0x%02x —— RGA 越界写了 %zu 字节以上",
                                 i, i, intended, bytes[i], i + 1 - intended);
                }
                ++bad;
            }
        }
        if (bad > 0) {
            ++guard_hits;
            if (first_bad > 0) {
                RK_LOG_ERROR("rga", "guard", "首个越界字节偏移=%zu (超出 %zu 字节)",
                             first_bad, first_bad - intended);
            }
        }
        const double m2 = NowMs();

        t_rga.push(t1 - t0);
        t_memset.push(m1 - m0);
        t_scan.push(m2 - m1);
        ++frames;
        StackGuard::beat();

        if (frames % 500 == 0) {
            RK_LOG_INFO("rga", "run",
                        "frame=%llu 转换=%.2fms 平均=%.1f次/s 哨兵损坏=%llu",
                        static_cast<unsigned long long>(frames), t_rga.mean(),
                        1000.0 / t_rga.mean(), static_cast<unsigned long long>(guard_hits));
        }
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_audit).count() >=
            opt.stats_interval) {
            last_audit = now;
            char why[64];
            snprintf(why, sizeof(why), "rga-only 第 %llu 帧",
                     static_cast<unsigned long long>(frames));
            audit_now(why);
        }
        if (opt.frames > 0 && frames >= static_cast<uint64_t>(opt.frames)) stop = true;
        if (opt.duration_sec > 0 &&
            std::chrono::duration_cast<std::chrono::seconds>(now - t_begin).count() >=
                opt.duration_sec) {
            stop = true;
        }
    }

    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t_begin).count();

    RK_LOG_INFO("rga", "done", "===== rga-only 结束 =====");
    RK_LOG_INFO("rga", "done", "帧数=%llu 用时=%.1fs 转换次数/秒=%.1f",
                static_cast<unsigned long long>(frames), elapsed,
                elapsed > 0 ? static_cast<double>(frames) / elapsed : 0.0);
    RK_LOG_INFO("rga", "done", "转换耗时 mean=%.3fms min=%.3f p50=%.3f p90=%.3f max=%.3f",
                t_rga.mean(), t_rga.min, t_rga.pct(50), t_rga.pct(90), t_rga.max);
    RK_LOG_INFO("rga", "done", "自身开销: 重填哨兵 mean=%.3fms 扫描 mean=%.3fms "
                             "(uncached DMA-BUF 上 CPU 访问很慢)",
                t_memset.mean(), t_scan.mean());
    RK_LOG_INFO("rga", "done", "哨兵损坏次数=%llu  RGA失败次数=%llu",
                static_cast<unsigned long long>(guard_hits),
                static_cast<unsigned long long>(rga_fail));
    RK_LOG_INFO("rga", "done", "模式: wstride=%s import=%s", wstride_mode.c_str(),
                import_mode.c_str());

    audit_now("rga-only 结束前");

    if (src_handle) releasebuffer_handle(src_handle);
    if (dst_handle) releasebuffer_handle(dst_handle);
    rk::dma_buf_unmap(dst_ptr, total);
    rk::dma_buf_unmap(src_ptr, src_size);
    FdLedger::close_tracked(dst_fd, "rga-dst");
    FdLedger::close_tracked(src_fd, "rga-src");

    RK_LOG_INFO("rga", "done", "清理完成, 未触发死机");
    return 0;
}

}  // namespace minimal
}  // namespace rk
