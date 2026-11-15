#include "rga_convert.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "im2d.h"
#include "rga.h"
#include "safe_probe.h"

// 编译期总开关: 0 = 只保留 CPU 转换路径(给"没有 dma-buf 的机器"做回归)。
#ifndef RK_ENABLE_RGA_DMA_BUF
#define RK_ENABLE_RGA_DMA_BUF 1
#endif

// 编译期硬件开关: USE_RGA=0 -> 一律走 OpenCV CPU 转换(不探测 RGA 设备)。
// 默认 1: 没显式定义这个宏的构建单元(单元测试等)保持硬件路径。
#ifndef RK_USE_RGA
#define RK_USE_RGA 1
#endif

namespace {

// librga 里有两个"成功"值：imcheck_t 返回 IM_STATUS_NOERROR(2)，
// 而 improcess/imcvt 返回 IM_STATUS_SUCCESS(1)。两个都当作成功。
inline bool rga_ok(IM_STATUS s) {
    return s == IM_STATUS_NOERROR || s == IM_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// RGA 可用性状态机
//
//   Unknown : 还没探测过
//   Ready   : 设备节点能打开, 且 librga 的初始化探测也通过了
//   Broken  : 设备打不开 / librga 初始化崩溃 —— 之后一律走 CPU 兜底,
//              不再每帧都去撞同一堵墙。
// ---------------------------------------------------------------------------
enum class RgaState { Unknown, Ready, Broken };

RgaState g_rga_state = RgaState::Unknown;
std::string g_rga_reason;   // Broken 时说明到底为什么(给人看的)

// 设备节点(open 一次就知道)的状态, 单独记一份, 免得每帧都去 open。
enum class DeviceState { Unknown, Available, Unavailable };

// 运行时开关: RGA 硬件路径开/关(网页控制台点一下就切)。
//
// ★ 为什么不能用环境变量 RK_RGA_SRC: 那个是【启动时读一次的静态值】,
//   改了必须重启进程。控制台要的是"点一下立刻生效", 所以做成运行期原子开关。
//   环境变量仍然优先生效 —— 启动时设了 RK_RGA_SRC=cpu 就一直走 CPU。
std::atomic<bool> g_rga_hw_enabled{true};
DeviceState g_rga_device = DeviceState::Unknown;

// RK_FORCE_CPU_RGA=1 -> 强制走 CPU 转换。
// 用途: ① 在板子上 A/B 对比 RGA 与 CPU 两条路径的输出是否一致
//       ② 离线(没有 /dev/rga 的机器)验证 CPU 兜底逻辑
// 编译期 USE_RGA=0 -> 同样强制 CPU(这是"纯 CPU 基准"构建的入口)。
bool force_cpu_rga() {
#if !RK_USE_RGA
    return true;
#else
    static const bool v = (getenv("RK_FORCE_CPU_RGA") != nullptr);
    return v;
#endif
}

// 强制走 CPU 时提示一次 —— 否则日志里一条 RGA 相关输出都没有,
// 容易让人以为程序根本没走到 RGA 的判断。
bool rga_force_cpu_noted_once() {
    if (!force_cpu_rga()) return false;
    static bool noted = false;
    if (!noted) {
        noted = true;
#if !RK_USE_RGA
        fprintf(stderr,
                "[RGA] 编译期 USE_RGA=0 — 该构建一律走 OpenCV CPU 转换"
                "(不探测 RGA 设备)\n");
#else
        fprintf(stderr,
                "[RGA] RK_FORCE_CPU_RGA=1 — 按环境变量强制走 OpenCV CPU 转换"
                "(不探测 RGA 设备)\n");
#endif
    }
    return true;
}

// ---------------------------------------------------------------------------
// RGA 设备节点路径 —— 不写死。
//
// 优先读环境变量 RGA_DEVICE, 默认 /dev/rga。
//   · RGA_DEVICE 正好也是 librga ≥ 1.10 自己也认的环境变量(DRM 模式下用来
//     指定 /dev/dri/renderD128 之类), 用同一个名字, 我们的预检和 librga 的
//     真实行为就能对上。
//   · 工程里 third_party/lib/librga.so 是 1.9.1, 它还不懂这个变量, 固定
//     open("/dev/rga") —— 所以默认值必须填对。
// ---------------------------------------------------------------------------
const char* rga_device_path() {
    static const char* path = []() -> const char* {
        const char* env = getenv("RGA_DEVICE");
        return (env && env[0] != '\0') ? env : "/dev/rga";
    }();
    return path;
}

// 设备节点预检的结果。
struct DeviceProbe {
    bool        ok = false;
    std::string reason;   // 打不开时的原因
    std::string hint;     // 排查建议
};

// ---------------------------------------------------------------------------
// 设备节点预检 —— 这一步【完全不碰 librga】, 所以绝对不会崩。
//
// ★ 为什么必须先做这一步:
//   librga 1.9.1 的 rga_init() 是第一次调 imcheck/improcess 时才触发的,
//   它内部 open("/dev/rga")。open 失败时它只打印一行
//       "failed to open RGA: Permission denied."
//   然后【继续解引用没初始化的全局句柄】 -> SIGSEGV。
//   错误码根本传不回来 —— 只能在崩之前自己把原因查清楚:
//   open() 的 errno 就能区分「权限不足 / 节点不存在 / 驱动没加载」。
// ---------------------------------------------------------------------------
DeviceProbe probe_rga_device() {
    DeviceProbe out;
    const char* path = rga_device_path();

    int fd = ::open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0 && errno == EINVAL) {
        // 少数节点只允许只读打开
        fd = ::open(path, O_RDONLY | O_CLOEXEC);
    }
    if (fd >= 0) {
        ::close(fd);
        out.ok = true;
        return out;
    }

    const int err = errno;
    const char* kind = "打开失败";
    switch (err) {
        case EACCES:
        case EPERM:  kind = "权限不足"; break;
        case ENOENT: kind = "设备节点不存在"; break;
        case ENODEV: kind = "驱动未加载"; break;
        case ENXIO:  kind = "设备节点存在但没有对应驱动"; break;
        case EBUSY:  kind = "设备被占用"; break;
        case EINVAL: kind = "打开参数不被支持"; break;
        default:     break;
    }
    out.reason = std::string(kind) + "（open(\"" + path + "\") 返回 " +
                 strerror(err) + "，errno=" + std::to_string(err) + "）";

    switch (err) {
        case EACCES:
        case EPERM:
            out.hint = "当前用户对这个节点没有读写权限。"
                       "临时验证: sudo chmod 666 " + std::string(path) +
                       " ; 长期做法: 把运行用户加进节点所属的组(video/render),"
                       "或用 udev 规则固定权限。";
            break;
        case ENOENT:
            out.hint = "节点都不存在, 多半是内核没编 RGA 驱动。"
                       "查: ls /dev/rga* ; dmesg | grep -i rga ; "
                       "如果板子走的是 DRM 模式, 用 RGA_DEVICE=/dev/dri/renderD128 指定。";
            break;
        case ENODEV:
        case ENXIO:
            out.hint = "驱动没加载或没绑定这块设备。"
                       "查: lsmod | grep -i rga ; dmesg | grep -i rga ; modprobe rga";
            break;
        case EBUSY:
            out.hint = "设备被别的进程占着。查: fuser -v " + std::string(path);
            break;
        default:
            out.hint = "查: ls -l " + std::string(path) + " ; id ; dmesg | grep -i rga";
            break;
    }
    return out;
}

// 统一的"不可用"上报 —— 所有降级路径都走这里, 日志格式保持一致:
//     [RGA] RGA 不可用，原因：xxx
void report_rga_unavailable(const std::string& reason, const std::string& hint) {
    g_rga_state = RgaState::Broken;
    g_rga_reason = reason;
    fprintf(stderr, "[RGA] RGA 不可用，原因：%s\n", reason.c_str());
    if (!hint.empty()) {
        fprintf(stderr, "       %s\n", hint.c_str());
    }
    fprintf(stderr, "       已降级到 OpenCV CPU 转换（不崩，但慢一些）。\n");
}

// 缓冲层面的降级 —— 只影响【这一帧】, 不把 RGA 标记成永久不可用。
// (设备是好的, 只是这次的源/目标不是真 dma-buf; 后面换成 dma-buf 的帧
//  RGA 照样能用, 所以这里只提示一次, 不动 g_rga_state。)
void report_rga_skipped_once(const std::string& reason, const std::string& hint) {
    static bool warned = false;
    if (warned) return;
    warned = true;
    fprintf(stderr, "[RGA] RGA 不可用，原因：%s\n", reason.c_str());
    if (!hint.empty()) {
        fprintf(stderr, "       %s\n", hint.c_str());
    }
    fprintf(stderr, "       已降级到 OpenCV CPU 转换（不崩，但慢一些）。\n");
}

// ---------------------------------------------------------------------------
// 安全地调用 improcess
//
// ★ 为什么必须包 SafeProbe:
//   librga 在打不开 /dev/rga 时会【自己段错误】—— 它先打印
//       "failed to open RGA: Permission denied."
//   然后直接 SIGSEGV, 根本来不及把错误码返回给调用方。
//   所以 improcess 外层的 if (!rga_ok(st)) 判断【永远执行不到】,
//   整个进程 core dump。实测在 recomputer-rk3588-devkit 上就是这样。
//
//   SafeProbe 用 sigsetjmp/siglongjmp 把它兜住, 崩溃 -> 返回 false,
//   我们再降级到 CPU 转换, 流水线照常跑。
// ---------------------------------------------------------------------------
struct ImprocessCtx {
    rga_buffer_t s;
    rga_buffer_t d;
    rga_buffer_t pat;
    im_rect     srect;
    im_rect     drect;
    im_rect     prect;
    IM_STATUS   st = IM_STATUS_NOERROR;
};

bool do_improcess(void* p) {
    ImprocessCtx* c = static_cast<ImprocessCtx*>(p);
    // improcess 的 C++ 重载：
    //   src, dst, pat, srect, drect, prect,
    //   acquire_fence_fd, release_fence_fd, opt_ptr, usage
    c->st = improcess(c->s, c->d, c->pat, c->srect, c->drect, c->prect,
                       -1, nullptr, nullptr, 0);
    return true;
}

// ---------------------------------------------------------------------------
// librga 初始化探针
//
// ★ 这是踩过的最隐蔽的一个坑:
//   librga 的 rga_init() 是【第一次调用 imcheck/improcess 时才触发】的, 它会
//   open("/dev/rga") 并打印版本号( "rga_api version 1.9.1_[4]" )。
//   板子上打不开 /dev/rga 时, rga_init() 打印
//       "failed to open RGA: Permission denied."
//   之后【并没有干净地返回错误】—— 继续解引用没初始化的全局句柄, 直接 SIGSEGV。
//
//   所以: 只把 improcess 包进 SafeProbe 是不够的 —— imcheck 更早, 它才是
//   第一个触发 rga_init 的调用, 崩溃就发生在它里面, 根本走不到 improcess。
//   实测日志: "[NV12] 数据源: ..." 之后紧跟 "rga_api version 1.9.1_[4]",
//   然后 Segmentation fault —— 那两行之间就是 imcheck。
//
// 做法: 第一次用 RGA 之前, 先在 SafeProbe 里做一次探测性 imcheck,
//       把 rga_init 的崩溃一次性兜住。失败就永久标记 RGA 不可用, 全程走 CPU。
//       ★ 更前置的一层保护在 rga_device_available(): 调 librga 之前先自己
//         open() 一次设备节点 —— 打不开就【根本不让 librga 有机会崩】。
// ---------------------------------------------------------------------------
bool probe_rga_init(void* p) {
    (void)p;
    // 故意用小尺寸调一次 imcheck: 目的只是触发 rga_init(),
    // 返回值无所谓(初始化已经发生了)。
    rga_buffer_t s, d;
    memset(&s, 0, sizeof(s));
    memset(&d, 0, sizeof(d));
    im_rect r;
    memset(&r, 0, sizeof(r));
    r.width = 16;
    r.height = 16;
    imcheck(s, d, r, r);
    return true;
}

// imcheck 是纯校验, 但它同样是【会触发 rga_init 的调用】, 所以也包一层 SafeProbe。
struct ImcheckCtx {
    rga_buffer_t s;
    rga_buffer_t d;
    im_rect     srect;
    im_rect     drect;
    IM_STATUS   st = IM_STATUS_NOERROR;
};

bool do_imcheck(void* p) {
    ImcheckCtx* c = static_cast<ImcheckCtx*>(p);
    c->st = imcheck(c->s, c->d, c->srect, c->drect);
    return true;
}

// ---------------------------------------------------------------------------
// ★ 这块板子上 RGA 能不能用 —— 必须【源和目标都是 dma-buf】才行
//
// 踩过的坑(2026-11-11): 目标缓冲用普通 malloc(用户态 VA)时, librga 走的是
// 【软件 MMU 模式】: 它自己把用户态 VA 映射成物理地址去跑 DMA。
// 而这块板子的 dma-buf 根本分配不出来(日志: "[警告] dma-buf 不可用"),
// 于是每个缓冲都得靠软件 MMU 兜。软件 MMU 会缓存 VA->PA 映射:
// 缓冲 free 之后 VA 被 malloc 复用, 缓存里的 PA 还是老的, RGA 就把输出
// 写到了【别的分配】里 —— 堆被随机破坏, 崩溃点飘忽:
//     有时崩在 malloc("invalid next size"), 有时崩在 libavcodec 的
//     avcodec_send_packet, 有时崩在 cv::Mat(... m.cols)。
// 另有一个附带现象: RGA 在缩放比例不是整数倍时(1920x1080 -> 640x640)
// 会往目标缓冲【多写 1~2 行】(实测 +3840 字节), 目标缓冲按精确大小分配
// 就直接踩坏堆 —— 所以目标缓冲还额外留了 kRgaDstPadRows 行余量。
//
// 结论: 只有【源和目标都是真 dma-buf(fd >= 0)】时才走 RGA;
//       否则一律 CPU 兜底(CPU 路径本项目的证据跑分就是用它, 17.9fps)。
// ---------------------------------------------------------------------------
bool rga_buffers_are_dmabuf(const rk::RgaImage& src, const rk::RgaImage& dst) {
    // 平面 YUV420(U/V 分开) librga 的 rga_buffer_t 描述不了(它只有一组
    // wstride/hstride), 走 CPU 路径。
    if (src.planar_yuv) return false;
    // ★ 高位深(10/12bit): librga 只认 8bit NV12, 喂 16bit 采样进去只会花屏。
    //   一律走 CPU 转换(转换层按 bit_shift 降位深)。
    if (src.bit_shift > 0) return false;
    return src.fd >= 0 && dst.fd >= 0;
}

// ---------------------------------------------------------------------------
// 设备节点能不能打开 —— 只做 open(), 完全不碰 librga。
//
// 之所以把它单独拎出来, 是为了让调用顺序变成三件分得清的事:
//     ① 设备节点打不开   -> 权限/节点不存在/驱动没加载, 干净降级, 绝不碰 librga
//     ② 缓冲不是真 dma-buf -> librga 走软件 MMU 会写坏堆, 也不碰 librga
//     ③ 设备能开、缓冲也对, 但 librga 自己初始化时崩了 -> 才是 "init 内部崩溃"
// 三种情况在日志上是三行不同的文字, 不用猜。
// ---------------------------------------------------------------------------
bool rga_device_available() {
    if (g_rga_device == DeviceState::Unknown) {
        const DeviceProbe p = probe_rga_device();
        if (p.ok) {
            g_rga_device = DeviceState::Available;
            fprintf(stderr, "[RGA] 设备节点 %s 可以打开\n", rga_device_path());
        } else {
            g_rga_device = DeviceState::Unavailable;
            // 打不开 -> RGA 直接判死刑, 由外层降级, 不用再去撞 rga_init 的崩溃
            report_rga_unavailable(p.reason, p.hint);
        }
    }
    return g_rga_device == DeviceState::Available && g_rga_state != RgaState::Broken;
}

// RGA 能不能用。false = 不可用, 后面一律走 CPU。
bool ensure_rga_ready() {
    if (g_rga_state == RgaState::Broken) return false;
    if (g_rga_state == RgaState::Ready) return true;

    // ---------- ① 设备节点 ----------
    // 调用方一般已经查过一次, 这里再兜一道: 保证【任何】进入 librga 的路径
    // 都先过了这一关 —— 以后谁直接调 ensure_rga_ready() 也不会把进程带崩。
    if (!rga_device_available()) return false;

    // ---------- ② librga 初始化 ----------
    // 能走到这里, 说明设备节点是打得开的。所以万一还是崩, 那就不是"权限不够",
    // 而是 librga 自己和内核驱动对不上 —— 日志上要能一眼分出来。
    fprintf(stderr, "[RGA] 设备节点 %s 可打开, 进入 rga_init() 探测\n", rga_device_path());
    fflush(stderr);

    const bool survived = rk::SafeProbe::call(&probe_rga_init, nullptr);
    if (!survived) {
        report_rga_unavailable(
            "rga_init() 内部崩溃（设备节点可以打开, 但 librga 初始化时发生 SIGSEGV）",
            "设备权限没问题, 是 librga 和内核驱动对不上: 换与内核匹配的 librga"
            "(本工程是 1.9.1); 另外 librga 1.9.1 不认 RGA_DEVICE, 它固定 "
            "open(\"/dev/rga\"), 确认 RGA_DEVICE 指的就是它要开的那个节点。");
        return false;
    }

    fprintf(stderr, "[RGA] rga_init() 正常返回, RGA 硬件通道可用\n");
    g_rga_state = RgaState::Ready;
    return true;
}

// 返回 true = 转换成功; false = RGA 不可用(崩溃或返回错误)
bool safe_improcess(const rga_buffer_t& s, const rga_buffer_t& d,
                    const rga_buffer_t& pat,
                    const im_rect& srect, const im_rect& drect,
                    const im_rect& prect, const char* what) {
    ImprocessCtx c;
    c.s = s; c.d = d; c.pat = pat;
    c.srect = srect; c.drect = drect; c.prect = prect;

    const bool crashed = !rk::SafeProbe::call(&do_improcess, &c);
    if (crashed) {
        report_rga_unavailable(
            std::string(what) + " 期间 librga 崩溃（SIGSEGV 已捕获）",
            "查: dmesg | grep -i rga ; ls -l " + std::string(rga_device_path()));
        return false;
    }
    if (!rga_ok(c.st)) {
        report_rga_unavailable(
            std::string(what) + " 失败：" + imStrError(c.st),
            "这次不是崩溃, 是 librga 返回了错误码。"
            "查格式/stride: wstride 必须是【字节数】, hstride 是 plane 的行数。");
        return false;
    }
    return true;
}

// letterbox padding 的填充状态，按目标缓冲的 CPU 地址缓存。
// 稳态下几何不变 -> 不会重复填充。
struct FillSlot {
    void* base = nullptr;
    int   x = -1, y = -1, w = -1, h = -1;
};
FillSlot g_fill_cache[4];

// ---------------------------------------------------------------------------
// RGA 缓冲的 stride 单位换算
//
// ★★★ 这块板子的内核 RGA 驱动里, wstride 的单位是【像素】, 不是字节!
//     实测证据 (tests/probe_rga_stride.cc, RGB888 64x64 纯拷贝):
//       wstride=64  -> 与源【逐字节 100% 相同】  (正确)
//       wstride=192 -> 只有 34.6% 相同           (被横向压扁)
//     驱动的尺寸校验也是  size = wstride * hstride * bpp(format):
//       RGB888 bpp=3 -> 640x640 需要 640*640*3 = 1.2MB
//       NV12   bpp=1.5 -> 1920x1088 需要 1920*1088*1.5 = 3.1MB
//
// 而 RgaImage 里的 wstride 是【字节】(CPU 兜底路径按字节 memcpy 用的),
// 所以边界上换算一次:
//   · NV12: luma 一行本来就 1 字节/像素 -> 像素数 == 字节数
//   · RGB888/BGR888: 一行 3 字节/像素 -> 像素数 = 字节数 / 3
// ---------------------------------------------------------------------------
int rga_wstride_pixels(int format, int wstride_bytes) {
    int bpp = 1;
    switch (format) {
        case RK_FORMAT_RGB_888:
        case RK_FORMAT_BGR_888:
            bpp = 3; break;
        case RK_FORMAT_RGBA_8888:
        case RK_FORMAT_BGRA_8888:
            bpp = 4; break;
        default:
            bpp = 1; break;      // NV12 的 luma 行宽: 1 字节/像素
    }
    return (wstride_bytes > 0) ? (wstride_bytes / bpp) : 0;
}

// 驱动要求的缓冲大小 = wstride(像素) * hstride * bpp(format)
size_t rga_buffer_bytes(int format, int wstride_pixels, int hstride) {
    const size_t w = static_cast<size_t>(wstride_pixels);
    const size_t h = static_cast<size_t>(hstride);
    switch (format) {
        case RK_FORMAT_RGB_888:
        case RK_FORMAT_BGR_888:
            return w * h * 3;
        case RK_FORMAT_RGBA_8888:
        case RK_FORMAT_BGRA_8888:
            return w * h * 4;
        case RK_FORMAT_YCbCr_420_SP:
            // NV12: luma(w*h) + UV(w/2 * h/2 * 2) = w*h*1.5
            return w * h * 3 / 2;
        default:
            return w * h;
    }
}

// ---------------------------------------------------------------------------
// RGA 源缓冲的注册方式
//
//   kImportFd : importbuffer_fd(fd, size) -> wrapbuffer_handle_t(...)
//               把 dma-buf fd 【注册成 RGA 自己的 buffer handle】,
//               RGA 直接 import 这块物理内存 —— 这就是"零拷贝"的那一步。
//   kLegacyFd : 老路, 直接在 rga_buffer_t 里填 fd, 交给 improcess 内部去 import。
//               实测(importbuffer_fd vs 老路)输出【逐字节一致】, 保留当兜底。
//   kCpu      : 不走 RGA, 用 OpenCV 在 CPU 上转换(拷贝)。
//
// 运行时切换: 环境变量 RK_RGA_SRC = auto | import | legacy | cpu
// 编译期开关: -DRK_ENABLE_RGA_DMA_BUF=0 直接把 import/legacy 两条路编掉,
//              只留 CPU 路径(给"没有 dma-buf 的机器"做回归)。
// ---------------------------------------------------------------------------
enum class RgaSrcMode { kAuto, kImportFd, kLegacyFd, kCpu };

// 实际用上的注册方式(给日志如实上报; auto 模式下会变成实际选中的那条)
const char* g_src_reg_method = "importbuffer_fd";

RgaSrcMode rga_src_mode() {
#if !RK_ENABLE_RGA_DMA_BUF
    return RgaSrcMode::kCpu;
#else
    static const RgaSrcMode v = []() -> RgaSrcMode {
        const char* env = getenv("RK_RGA_SRC");
        if (!env || env[0] == '\0') return RgaSrcMode::kAuto;
        if (!strcmp(env, "import")) return RgaSrcMode::kImportFd;
        if (!strcmp(env, "legacy")) return RgaSrcMode::kLegacyFd;
        if (!strcmp(env, "cpu"))    return RgaSrcMode::kCpu;
        return RgaSrcMode::kAuto;
    }();
    return v;
#endif
}

// 一对 RGA 缓冲(源 + 目标)。用 importbuffer_fd 时需要持有 handle,
// 用完必须 releasebuffer_handle, 否则 handle 泄漏。
struct RgaBuffers {
    rga_buffer_t src = {};
    rga_buffer_t dst = {};
    rga_buffer_handle_t src_handle = 0;   // rga_buffer_handle_t 是 unsigned int
    rga_buffer_handle_t dst_handle = 0;
    bool imported = false;

    ~RgaBuffers() {
        if (src_handle) releasebuffer_handle(src_handle);
        if (dst_handle) releasebuffer_handle(dst_handle);
    }
};

// 传给 importbuffer_fd 的 size:
//   优先用 dma-buf 的【真实大小】(fstat), 至少覆盖驱动算出来的图像大小。
//   MPP 的输出缓冲不一定正好等于 width*height*1.5(它会按 stride 对齐, 有时还
//   带额外余量), 用真实大小最保险; 万一 fstat 拿不到, 就用算出来的图像大小。
size_t import_size_for(const rk::RgaImage& img, int wstride_px) {
    const size_t need = rga_buffer_bytes(img.format, wstride_px, img.hstride);
    size_t real = 0;
    struct stat st;
    if (img.fd >= 0 && ::fstat(img.fd, &st) == 0 && st.st_size > 0) {
        real = static_cast<size_t>(st.st_size);
    }
    return real > need ? real : need;
}

// 源缓冲注册: importbuffer_fd(把 fd 交给 RGA 当成它自己的 buffer)
bool register_src_import(const rk::RgaImage& img, RgaBuffers* out) {
    const int wstride_px = rga_wstride_pixels(img.format, img.wstride);
    if (wstride_px <= 0) return false;
    const size_t bytes = import_size_for(img, wstride_px);
    if (bytes == 0) return false;

    out->src_handle = importbuffer_fd(img.fd, static_cast<int>(bytes));
    if (!out->src_handle) return false;

    out->src = wrapbuffer_handle_t(out->src_handle, img.width, img.height,
                                    wstride_px, img.hstride, img.format);
    out->src.color_space_mode = IM_COLOR_SPACE_DEFAULT;
    out->imported = true;
    return true;
}

// 目标缓冲注册
bool register_dst_import(const rk::RgaImage& img, RgaBuffers* out) {
    const int wstride_px = rga_wstride_pixels(img.format, img.wstride);
    if (wstride_px <= 0) return false;
    const size_t bytes = import_size_for(img, wstride_px);
    if (bytes == 0) return false;

    out->dst_handle = importbuffer_fd(img.fd, static_cast<int>(bytes));
    if (!out->dst_handle) return false;

    out->dst = wrapbuffer_handle_t(out->dst_handle, img.width, img.height,
                                    wstride_px, img.hstride, img.format);
    out->dst.color_space_mode = IM_COLOR_SPACE_DEFAULT;
    return true;
}

// 老路: 直接在 rga_buffer_t 里填 fd
rga_buffer_t make_buffer(const rk::RgaImage& img) {
    rga_buffer_t b;
    memset(&b, 0, sizeof(b));
    b.fd = img.fd;
    // ★ fd 有效时【不要】再填 vir_addr:
    //   两者都填, librga 会走"软件 MMU"(用户态 VA 自己映射成物理地址)那条路,
    //   那条路会缓存 VA->PA 映射, 分配复用之后 PA 对不上, RGA 会把输出写进
    //   别的分配里 —— 堆被随机破坏(见本文件上面的说明)。
    b.vir_addr = (img.fd >= 0) ? nullptr : img.vir_addr;
    b.width = img.width;
    b.height = img.height;
    b.wstride = rga_wstride_pixels(img.format, img.wstride);   // ★ 单位是像素
    b.hstride = img.hstride;
    b.format = img.format;
    // NV12 源按 BT.601 limited range 解释。
    // ⚠️ 这块板子的 librga/驱动【不支持】BT.709:
    //     设 IM_YUV_BT709_LIMIT_RANGE 会报
    //     "Unsupported full csc mode! src_csm = 0x500, dst_csm = 0x500"
    b.color_space_mode = IM_COLOR_SPACE_DEFAULT;
    return b;
}

// 保证目标缓冲里 padding 区域是 114 灰（和 YOLOv8 官方 demo 一致）
void ensure_gray_border(const rk::RgaImage& dst, int x, int y, int w, int h) {
    if (!dst.vir_addr) return;

    FillSlot* slot = nullptr;
    for (auto& c : g_fill_cache) {
        if (c.base == dst.vir_addr) { slot = &c; break; }
    }
    if (!slot) {
        for (auto& c : g_fill_cache) {
            if (!c.base) { slot = &c; break; }
        }
        if (!slot) slot = &g_fill_cache[0];
    }
    if (slot->base == dst.vir_addr && slot->x == x && slot->y == y && slot->w == w &&
        slot->h == h) {
        return;  // 几何没变，不用重填
    }

    uint8_t* base = static_cast<uint8_t*>(dst.vir_addr);
    const int row = dst.wstride;
    for (int r = 0; r < dst.height; ++r) {
        memset(base + r * row, 114, row);
    }

    slot->base = dst.vir_addr;
    slot->x = x; slot->y = y; slot->w = w; slot->h = h;
}

// ---------------------------------------------------------------------------
// CPU 兜底: YUV420 -> RGB888
//
// RGA 不可用(/dev/rga 打不开、librga 崩溃、或者源/目标不是 dma-buf)时用
// OpenCV 走 CPU。性能比 RGA 差不少, 但能让整条链路继续跑, 不再 core dump。
//
// 内存布局(两种都要支持):
//   半平面 NV12:
//     Y  plane : vir_addr 起, 每行 wstride 字节, 有效 width 字节, 共 hstride 行
//     UV plane : u_plane 起, 每行 uv_wstride 字节(U/V 交织), 一行 UV 对应两行 Y
//   平面 YUV420(yuvj420p / yuv420p, FFmpeg 软解出来的就是这种):
//     Y  plane : vir_addr 起, 每行 wstride 字节
//     U  plane : u_plane 起, 每行 uv_wstride 字节(= w/2)
//     V  plane : v_plane 起, 每行 uv_wstride 字节(= w/2)
//
// 无论哪种, 这里都先打包成【一份连续的 NV12】, 再交给 OpenCV 一次性转换。
//
// ★ 全范围(yuvj*)的处理: OpenCV 的 COLOR_YUV2RGB_NV12 假定 limited range
//   (Y 16~235), 而 yuvj420p 是 full range(Y 0~255)。不折算的话颜色会偏暗
//   且饱和度不对。这里在打包的时候就把 Y/UV 从 full range 折成 limited:
//       Y_lim = Y * 219/255 + 16
//       C_lim = (C-128) * 224/255 + 128
//   折完之后 OpenCV 的 limited->RGB 正好还原出 full range 的颜色。
// ---------------------------------------------------------------------------

// full range -> limited range 的查表(256 项, 整数四舍五入)
struct RangeTables {
    uint8_t y[256];
    uint8_t c[256];
    RangeTables() {
        for (int i = 0; i < 256; ++i) {
            y[i] = static_cast<uint8_t>((i * 219 + 127) / 255 + 16);
            const int t = (i - 128) * 224;
            const int r = (t >= 0) ? ((t + 127) / 255) : -((-t + 127) / 255);
            c[i] = static_cast<uint8_t>(r + 128);
        }
    }
};
const RangeTables& range_tables() {
    static const RangeTables t;
    return t;
}

bool yuv420_region_to_nv12(const rk::RgaImage& src, int sx, int sy, int sw, int sh,
                           std::vector<uint8_t>* out) {
    if (!src.vir_addr || sw <= 0 || sh <= 0) return false;
    if (sx < 0) { sw += sx; sx = 0; }
    if (sy < 0) { sh += sy; sy = 0; }
    if (sx + sw > src.width) sw = src.width - sx;
    if (sy + sh > src.height) sh = src.height - sy;
    if (sw <= 0 || sh <= 0) return false;

    const uint8_t* yp = static_cast<const uint8_t*>(src.vir_addr);
    const uint8_t* up = src.u_plane;
    const uint8_t* vp = src.v_plane;
    const int uv_stride = src.uv_wstride > 0 ? src.uv_wstride : src.wstride;

    const size_t need = static_cast<size_t>(sw) * sh * 3 / 2;
    if (out->capacity() < need) out->reserve(need);
    out->resize(need);          // 复用时 size 不变 -> 不会重新清零
    uint8_t* dst = out->data();

    // ---- Y plane ----
    if (src.bit_shift > 0) {
        // ---- 高位深(10/12bit): 每采样 2 字节小端, 右移降位深 ----
        // Y 平面行宽是【字节数】(10bit 时 = 宽度*2), 所以像素偏移要乘 2。
        // 用 OpenCV 的 convertScaleAbs 一次性降位深(SIMD, 逐像素手写循环慢好几倍)。
        // ⚠️ 只在小端机上成立(ARM/x86 都是小端, FFmpeg 也只输出 *le)。
        const double k = 1.0 / static_cast<double>(1 << src.bit_shift);
        const uint16_t* y16_ptr = reinterpret_cast<const uint16_t*>(
            yp + static_cast<size_t>(sy) * src.wstride + static_cast<size_t>(sx) * 2);
        const cv::Mat y16(sh, sw, CV_16U, const_cast<uint16_t*>(y16_ptr), src.wstride);
        cv::Mat y8(sh, sw, CV_8UC1, dst);
        cv::convertScaleAbs(y16, y8, k, 0.0);
        if (src.full_range) {
            // 再 full -> limited: Y*219/255 + 16
            cv::Mat y_roi(sh, sw, CV_8UC1, dst);
            cv::Mat y_lim(sh, sw, CV_8UC1, dst);
            cv::convertScaleAbs(y_roi, y_lim, 219.0 / 255.0, 16.0);
        }
    } else if (src.full_range) {
        // full -> limited: Y*219/255 + 16
        // 用 OpenCV 的 SIMD 版本(逐像素查表要 7ms/帧, 这里 1ms 出头)
        const cv::Mat y_roi(sh, sw, CV_8UC1,
                            const_cast<uint8_t*>(yp + static_cast<size_t>(sy) * src.wstride + sx),
                            src.wstride);
        cv::Mat y_lim(sh, sw, CV_8UC1, dst);
        cv::convertScaleAbs(y_roi, y_lim, 219.0 / 255.0, 16.0);
    } else {
        for (int r = 0; r < sh; ++r) {
            memcpy(dst + static_cast<size_t>(r) * sw,
                   yp + static_cast<size_t>(sy + r) * src.wstride + sx,
                   static_cast<size_t>(sw));
        }
    }

    // ---- 色度 -> 交织成 NV12 的 UV ----
    // chroma 是 2x2 下采样, 坐标取偶数
    const int ux = sx & ~1, uy = sy & ~1;
    const int uw = sw & ~1, uh = sh & ~1;
    if (uw <= 0 || uh <= 0) return false;
    uint8_t* udst = dst + static_cast<size_t>(sw) * sh;

    if (!src.u_plane) {
        // 帧没有色度平面(比如灰度格式): 用中性 128 填, 保证灰度结构仍可用
        memset(udst, 128, static_cast<size_t>(uw) * (uh / 2));
        return true;
    }
    if (!src.planar_yuv) {
        // 半平面: UV 已经是交织的, 按行拷
        if (src.full_range) {
            const uint8_t* tbl = range_tables().c;
            for (int r = 0; r < uh / 2; ++r) {
                const uint8_t* s = up + static_cast<size_t>(uy / 2 + r) * uv_stride + ux;
                uint8_t* d = udst + static_cast<size_t>(r) * uw;
                for (int i = 0; i < uw; ++i) d[i] = tbl[s[i]];
            }
        } else {
            for (int r = 0; r < uh / 2; ++r) {
                memcpy(udst + static_cast<size_t>(r) * uw,
                       up + static_cast<size_t>(uy / 2 + r) * uv_stride + ux,
                       uw);
            }
        }
    } else {
        // 平面: U、V 各自一行, 交织成 U,V,U,V...
        // 用 OpenCV 做: merge 出来的 CV_8UC2 内存天然就是 U,V,U,V... 交织的行,
        // 不用逐像素手写交织循环(慢一倍多)。
        if (!vp) return false;
        const int cw = uw / 2;                 // 每行多少个 chroma 采样
        const int ch = uh / 2;                 // chroma 行数

        if (src.bit_shift > 0) {
            // ---- 高位深: 16bit 采样 -> 右移 -> 交织成 NV12 的 UV ----
            // chroma 也是 2 字节/采样, 像素偏移同样要乘 2。
            const double k = 1.0 / static_cast<double>(1 << src.bit_shift);
            const uint16_t* u16_ptr = reinterpret_cast<const uint16_t*>(
                up + static_cast<size_t>(uy / 2) * uv_stride + static_cast<size_t>(ux / 2) * 2);
            const uint16_t* v16_ptr = reinterpret_cast<const uint16_t*>(
                vp + static_cast<size_t>(uy / 2) * uv_stride + static_cast<size_t>(ux / 2) * 2);
            const cv::Mat u16(ch, cw, CV_16U, const_cast<uint16_t*>(u16_ptr), uv_stride);
            const cv::Mat v16(ch, cw, CV_16U, const_cast<uint16_t*>(v16_ptr), uv_stride);
            cv::Mat u8(ch, cw, CV_8UC1), v8(ch, cw, CV_8UC1);
            cv::convertScaleAbs(u16, u8, k, 0.0);
            cv::convertScaleAbs(v16, v8, k, 0.0);
            cv::Mat uv_roi;
            if (src.full_range) {
                // full -> limited: (C-128)*224/255 + 128
                const double a = 224.0 / 255.0;
                const double b = 128.0 * (1.0 - a);
                cv::Mat u_lim(ch, cw, CV_8UC1), v_lim(ch, cw, CV_8UC1);
                cv::convertScaleAbs(u8, u_lim, a, b);
                cv::convertScaleAbs(v8, v_lim, a, b);
                cv::merge(std::vector<cv::Mat>{u_lim, v_lim}, uv_roi);
            } else {
                cv::merge(std::vector<cv::Mat>{u8, v8}, uv_roi);
            }
            for (int r = 0; r < ch; ++r)
                memcpy(udst + static_cast<size_t>(r) * uw, uv_roi.ptr(r), uw);
            return true;
        }

        const cv::Mat u_roi(ch, cw, CV_8UC1,
                            const_cast<uint8_t*>(up + static_cast<size_t>(uy / 2) * uv_stride +
                                                 ux / 2),
                            uv_stride);
        const cv::Mat v_roi(ch, cw, CV_8UC1,
                            const_cast<uint8_t*>(vp + static_cast<size_t>(uy / 2) * uv_stride +
                                                 ux / 2),
                            uv_stride);
        if (src.full_range) {
            // full -> limited: (C-128)*224/255 + 128
            const double a = 224.0 / 255.0;
            const double b = 128.0 * (1.0 - a);
            cv::Mat u_lim(ch, cw, CV_8UC1), v_lim(ch, cw, CV_8UC1);
            cv::convertScaleAbs(u_roi, u_lim, a, b);
            cv::convertScaleAbs(v_roi, v_lim, a, b);
            cv::Mat uv_roi;
            cv::merge(std::vector<cv::Mat>{u_lim, v_lim}, uv_roi);
            for (int r = 0; r < ch; ++r)
                memcpy(udst + static_cast<size_t>(r) * uw, uv_roi.ptr(r), uw);
        } else {
            cv::Mat uv_roi;
            cv::merge(std::vector<cv::Mat>{u_roi, v_roi}, uv_roi);
            for (int r = 0; r < ch; ++r)
                memcpy(udst + static_cast<size_t>(r) * uw, uv_roi.ptr(r), uw);
        }
    }
    return true;
}

// 把 RGB 图写进目标缓冲(按 dst.wstride 逐行)
bool write_rgb_to_dst(const cv::Mat& rgb, const rk::RgaImage& dst, int dx, int dy) {
    if (!dst.vir_addr || rgb.empty()) return false;
    uint8_t* base = static_cast<uint8_t*>(dst.vir_addr);
    const int row = dst.wstride;
    for (int r = 0; r < rgb.rows; ++r) {
        const int yy = dy + r;
        if (yy < 0 || yy >= dst.height) continue;
        memcpy(base + static_cast<size_t>(yy) * row + static_cast<size_t>(dx) * 3,
               rgb.ptr(r), static_cast<size_t>(rgb.cols) * 3);
    }
    return true;
}

// CPU 版: NV12(src 的某一矩形) -> RGB888 -> 缩放 -> 写入 dst
//
// ★★ 这里以前是"跑了没几帧就 core dump"的元凶(2026-11-11)★★
//
// 原来的收尾是:
//     cv::Mat yuv(sh * 3 / 2, sw, CV_8UC1, nv12.data());
//     ...copyMakeBorder(补成偶数)...
//     cv::cvtColor(yuv_aligned, rgb, cv::COLOR_YUV2RGB_NV12);
//     rgb = rgb(cv::Rect(0, 0, yuv.cols, yuv.rows)).clone();   // ← 元凶
//
// 问题在于 OpenCV 的 NV12 转换:
//     输入是 (h*3/2, w) 的单通道 Mat, 输出是 (h, w) 的三通道图
//     ——【输出行数只有输入的 2/3】。
// 而那段 ROI 裁剪拿【NV12 缓冲的行数 1.5*h】当高度, 去裁【已经转成 RGB 的
// (h, w) 图】, 只要 w 或 h 是奇数(走过 copyMakeBorder 那条分支)就必然
//     h > rgb.rows
// -> OpenCV 抛 "-215: 0 <= roi.x && roi.x + roi.width <= m.cols" (function 'Mat')
// -> 没人 catch -> std::terminate -> Aborted (core dumped)
//
// 车牌 ROI 来自浮点几何计算, 宽高有一半概率是奇数 -> 所以是"跑几帧才崩"。
// RGA 能用的时候走 RGA 不会触发; RGA 挂掉(本机 /dev/rga 的 MMU 不支持
// 这些 buffer, dmesg 里是 "RGA_MMU unsupported memory larger than 4G")
// 落到 CPU 兜底就必崩。
//
// 修法: 【一开始】就把裁剪框对齐到偶数(NV12 的 chroma 本来就是 2x2 下采样,
// 奇数坐标/尺寸本来就不合法), 这样 OpenCV 转换不需要补边, 转换完的 RGB
// 就是精确的 (sh, sw), 根本不需要再裁一刀。
bool cpu_nv12_to_rgb888(const rk::RgaImage& src, const rk::RgaImage& dst,
                        int sx, int sy, int sw, int sh,
                        int dx, int dy, int dw, int dh) {
    // 偶数对齐(chroma 2x2 下采样, 奇数坐标/尺寸本来就没有意义)
    sx &= ~1;
    sy &= ~1;
    sw &= ~1;
    sh &= ~1;
    if (sw <= 0 || sh <= 0) return false;

    // 缓冲复用: 每帧重新分配 3MB 会触发大量缺页(实测 pack 要 4~12ms),
    // 复用之后几乎不要钱。
    static thread_local std::vector<uint8_t> nv12;
    if (!yuv420_region_to_nv12(src, sx, sy, sw, sh, &nv12)) return false;

    // OpenCV 的 NV12 转换要的是【原始 NV12 缓冲】: 单通道 CV_8UC1, 高 = h*3/2。
    //   (不是 reshape(2) —— 那个会报 "Invalid number of channels: scn is 2")
    //   w/h 已经保证是偶数, 所以不用 copyMakeBorder, 也不会有"输出行数是输入
    //   2/3"之后的错位问题。
    const cv::Mat yuv(sh * 3 / 2, sw, CV_8UC1, nv12.data());
    cv::Mat rgb;
    cv::cvtColor(yuv, rgb, cv::COLOR_YUV2RGB_NV12);   // rgb 是 (sh, sw)
    if (rgb.rows != sh || rgb.cols != sw) {
        // 理论上到不了这里; 真到了就说明 OpenCV 的 NV12 语义变了, 别往下写
        fprintf(stderr, "[RGA/CPU] NV12 转换尺寸异常: rgb=%dx%d, 期望 %dx%d\n",
                rgb.cols, rgb.rows, sw, sh);
        return false;
    }

    cv::Mat scaled;
    cv::resize(rgb, scaled, cv::Size(dw, dh), 0, 0, cv::INTER_LINEAR);
    return write_rgb_to_dst(scaled, dst, dx, dy);
}

// ---------------------------------------------------------------------------
// 源缓冲来源 —— 只打印一次。
//
// ★ 这是判断"到底走没走零拷贝"的那一行日志:
//     "DMA-BUF fd" = RGA 直接 import 硬件缓冲, CPU 一行像素都没碰
//     "CPU 映射地址" = 走 OpenCV 在 CPU 上转换, 有一整帧的拷贝
// ---------------------------------------------------------------------------
void log_rga_source(const rk::RgaImage& src, bool via_rga) {
    static bool logged = false;
    if (logged) return;
    logged = true;

    if (via_rga && src.fd >= 0) {
        fprintf(stderr, "[RGA] 输入来源: DMA-BUF fd (fd=%d, %s 注册为 RGA 源缓冲)\n",
                src.fd, g_src_reg_method);
    } else {
        fprintf(stderr, "[RGA] 输入来源: CPU 映射地址 (OpenCV CPU 转换, 有一整帧的 CPU 拷贝)\n");
    }
}

// ---------------------------------------------------------------------------
// 统一的 RGA 执行入口: 注册缓冲 -> imcheck -> improcess
//
// 源缓冲用哪种方式注册由 RK_RGA_SRC 决定:
//   import (默认): importbuffer_fd(fd, size) -> wrapbuffer_handle_t(...)
//   legacy       : 直接在 rga_buffer_t 里填 fd
//   cpu          : 不走 RGA
//
// 实测(tests/test_rga_fd_import.cpp): import 与 legacy 输出【逐字节一致】。
// ---------------------------------------------------------------------------
bool run_rga(const rk::RgaImage& src, const rk::RgaImage& dst,
             const im_rect& srect, const im_rect& drect, const char* what) {
    const RgaSrcMode mode = rga_src_mode();
    if (mode == RgaSrcMode::kCpu) return false;

    RgaBuffers bufs;
    const bool use_import = (mode != RgaSrcMode::kLegacyFd);
    g_src_reg_method = use_import ? "importbuffer_fd" : "rga_buffer_t.fd(老路)";
    if (use_import) {
        // importbuffer_fd: 把 dma-buf fd 注册成 RGA 自己的 buffer handle
        if (!register_src_import(src, &bufs)) {
            fprintf(stderr, "[RGA] importbuffer_fd(源 fd=%d) 失败, 退回老路\n", src.fd);
            g_src_reg_method = "rga_buffer_t.fd(老路)";
            bufs.src_handle = 0;
            bufs.src = make_buffer(src);
        } else if (!register_dst_import(dst, &bufs)) {
            fprintf(stderr, "[RGA] importbuffer_fd(目标 fd=%d) 失败, 退回老路\n", dst.fd);
            g_src_reg_method = "rga_buffer_t.fd(老路)";
            releasebuffer_handle(bufs.src_handle);
            bufs.src_handle = 0;
            bufs.dst_handle = 0;
            bufs.src = make_buffer(src);
            bufs.dst = make_buffer(dst);
        }
    } else {
        bufs.src = make_buffer(src);
        bufs.dst = make_buffer(dst);
    }

    rga_buffer_t pat;
    memset(&pat, 0, sizeof(pat));
    im_rect pat_rect;
    memset(&pat_rect, 0, sizeof(pat_rect));

    // librga 初始化探测(崩了能分清是"open 失败"还是"init 内部崩溃")
    if (!ensure_rga_ready()) return false;

    // imcheck 是纯校验, 但它同样会触发 rga_init, 所以也包一层 SafeProbe。
    ImcheckCtx ic;
    ic.s = bufs.src; ic.d = bufs.dst; ic.srect = srect; ic.drect = drect;
    if (!rk::SafeProbe::call(&do_imcheck, &ic)) {
        report_rga_unavailable(
            "imcheck() 内部崩溃（设备节点可以打开, 但 librga 校验阶段发生 SIGSEGV）",
            "设备权限没问题, 是 librga 自身的问题: 换与内核匹配的 librga 版本。");
        return false;
    }
    if (!rga_ok(ic.st)) {
        fprintf(stderr, "[RGA] imcheck: %s\n", imStrError(ic.st));
        // check 只是预检, 真正是否可行以 improcess 的返回值为准
    }
    return safe_improcess(bufs.src, bufs.dst, pat, srect, drect, pat_rect, what);
}

}  // namespace

namespace rk {

bool rga_available() { return g_rga_state == RgaState::Ready; }

// ---------------------------------------------------------------------------
// 运行时开关 RGA 硬件路径
// ---------------------------------------------------------------------------
void rga_set_hw_enabled(bool on) { g_rga_hw_enabled.store(on); }

bool rga_hw_enabled() {
    // 环境变量优先: 启动时就是 cpu 的构建, 运行期也【不许】被打开
    // (打开也没用 —— librga 压根没编进去/设备没节点)。
    if (rga_src_mode() == RgaSrcMode::kCpu) return false;
    return g_rga_hw_enabled.load(std::memory_order_relaxed);
}

const char* rga_unavailable_reason() { return g_rga_reason.c_str(); }

int rga_nv12_to_rgb888(const RgaImage& src, const RgaImage& dst,
                       bool letterbox,
                       float* out_scale, int* out_pad_x, int* out_pad_y) {
    if (src.width <= 0 || src.height <= 0 || dst.width <= 0 || dst.height <= 0) return -1;

    // ---------------------------------------------------------------- 目标区域
    float scale = 1.0f;
    int dx = 0, dy = 0, dw = dst.width, dh = dst.height;
    if (letterbox) {
        scale = std::min(static_cast<float>(dst.width) / src.width,
                         static_cast<float>(dst.height) / src.height);
        dw = static_cast<int>(src.width * scale + 0.5f);
        dh = static_cast<int>(src.height * scale + 0.5f);
        if (dw > dst.width) dw = dst.width;
        if (dh > dst.height) dh = dst.height;
        dx = (dst.width - dw) / 2;
        dy = (dst.height - dh) / 2;
    }

    // ---------------------------------------------------------------- 走 RGA
    bool ok = false;
    if (g_rga_state != RgaState::Broken && rga_hw_enabled() && !rga_force_cpu_noted_once()) {
        // ① 设备节点能不能打开 —— 这一步【不碰 librga】, 所以绝对不会崩。
        //    打不开就把原因说清楚(权限/不存在/驱动没加载), 由外层干净降级,
        //    根本不给 rga_init() 那个 SIGSEGV 机会。
        const bool dev_ok = rga_device_available();

        // ② 源/目标必须是真 dma-buf, 否则 librga 走软件 MMU 模式会写坏堆。
        const bool dmabuf_ok = rga_buffers_are_dmabuf(src, dst);

        if (dev_ok && dmabuf_ok) {
            if (letterbox) ensure_gray_border(dst, dx, dy, dw, dh);

            im_rect srect;
            srect.x = 0; srect.y = 0; srect.width = src.width; srect.height = src.height;
            im_rect drect;
            drect.x = dx; drect.y = dy; drect.width = dw; drect.height = dh;

            ok = run_rga(src, dst, srect, drect, "NV12->RGB888");
        }
        log_rga_source(src, ok);
    }

    // ---------------------------------------------------------------- CPU 兜底
    if (!ok) {
        if (g_rga_state != RgaState::Broken && !force_cpu_rga() &&
            !rga_buffers_are_dmabuf(src, dst)) {
            report_rga_skipped_once(
                "源/目标不是 dma-buf(fd<0)（librga 的软件 MMU 在 4G 以上的用户态地址上会失败, "
                "而且写坏堆）",
                "CPU 路径实测 17.9fps, 功能不丢。要走 RGA 硬件路径, 源和目标都必须是真 dma-buf。");
        }
        if (letterbox) ensure_gray_border(dst, dx, dy, dw, dh);
        if (!cpu_nv12_to_rgb888(src, dst, 0, 0, src.width, src.height, dx, dy, dw, dh)) {
            fprintf(stderr, "[RGA/CPU] CPU 兜底也失败了(源不是 CPU 可访问的内存?)\n");
            return -1;
        }
    }

    if (out_scale) *out_scale = scale;
    if (out_pad_x) *out_pad_x = dx;
    if (out_pad_y) *out_pad_y = dy;
    return 0;
}

// ---------------------------------------------------------------------------
// 裁剪 + 缩放 + 格式转换 一步到位(检测框 -> 160x48 车牌小图)

// ---------------------------------------------------------------------------
// RGA 缩放比例是否超出硬件承受范围
//
// 实测(2026-11-12, Step 07.2 期间):
//   LPR 的车牌 ROI 常常退化成极扁的矩形(例如 780x8 —— 检测框小, ROI 高度只有
//   框高的 16%, 向下取偶后剩 8px), 要缩放到 160x48 就需要【纵向放大 6 倍】。
//   RGA 的缩放器扛不住这种极端比例, 硬件直接报错:
//       rga: irq handler err! INTR[0x2099], HW_STATUS[0xa0000], CMD_STATUS[0x10000]
//       rga: RGA3_core0[0x1] soft reset complete.
//   对照实验: 同样的场景把 ROI 高度调到 --roi-height 0.6(比例正常) -> 900 帧 0 错误;
//             保持默认的扁 ROI -> 900 帧 3 错误(每 300 帧必现)。
//
// 处理: 这种极端比例【不走 RGA】, 直接落到 CPU 兜底。
//       (Step 07.2 之前 CPU 兜底读的是已释放的帧指针, 现在帧由 RAII 守卫托管,
//        所以 CPU 兜底是安全的 —— 这正是先修 RAII 的价值。)
static bool rga_ratio_too_extreme(int sw, int sh, int dw, int dh) {
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return true;
    if (sw < 16 || sh < 16) return true;            // 源太小, 放大必然极端
    const double sx = static_cast<double>(dw) / static_cast<double>(sw);
    const double sy = static_cast<double>(dh) / static_cast<double>(sh);
    if (sx > 4.0 || sy > 4.0) return true;           // 放大超过 4 倍
    if (sx < 0.125 || sy < 0.125) return true;       // 压缩超过 8 倍
    return false;
}

int rga_crop_to_rgb888(const RgaImage& src, int sx, int sy, int sw, int sh,
                       const RgaImage& dst) {
    if (sw <= 0 || sh <= 0 || dst.width <= 0 || dst.height <= 0) return -1;
    // 裁剪框超出画面就夹一下
    if (sx < 0) { sw += sx; sx = 0; }
    if (sy < 0) { sh += sy; sy = 0; }
    if (sx + sw > src.width) sw = src.width - sx;
    if (sy + sh > src.height) sh = src.height - sy;
    if (sw <= 0 || sh <= 0) return -1;

    bool ok = false;
    // ★ 极端比例不走 RGA(见 rga_ratio_too_extreme 的注释):
    //   扁 ROI(例如 780x8)要纵向放大 6 倍, RGA 缩放器扛不住, 硬件会报
    //   irq handler err! INTR[0x2099] 并被驱动软复位 core。
    const bool extreme = rga_ratio_too_extreme(sw, sh, dst.width, dst.height);
    if (extreme) {
        static bool noted = false;
        if (!noted) {
            noted = true;
            fprintf(stderr,
                    "[RGA] 裁剪比例 %dx%d -> %dx%d 超出 RGA 缩放器承受范围, 改走 CPU 兜底\n",
                    sw, sh, dst.width, dst.height);
        }
    }
    if (!extreme && g_rga_state != RgaState::Broken && rga_hw_enabled() &&
        !rga_force_cpu_noted_once()) {
        // 和主路径同一套判断顺序: 先看设备节点, 再看缓冲, 最后才碰 librga。
        const bool dev_ok = rga_device_available();
        const bool dmabuf_ok = rga_buffers_are_dmabuf(src, dst);
        if (dev_ok && dmabuf_ok) {
            im_rect srect;
            srect.x = sx; srect.y = sy; srect.width = sw; srect.height = sh;
            im_rect drect;
            drect.x = 0; drect.y = 0; drect.width = dst.width; drect.height = dst.height;

            ok = run_rga(src, dst, srect, drect, "裁剪缩放");
        }
    }

    if (!ok) {
        // CPU 兜底: 先在 NV12 域裁出这块, 再转 RGB, 再缩放
        if (!cpu_nv12_to_rgb888(src, dst, sx, sy, sw, sh, 0, 0, dst.width, dst.height)) {
            // 兜底也失败就放弃这个 track 的车牌(不影响检测/测速)
            return -1;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 从 NV12 帧裁一块区域, 用【CPU】转成 RGB888 并缩放到 dst 的尺寸。
//
// ★ 为什么要单独开这条路(车牌识别用):
//   RGA 硬件路径和 CPU 路径的 YUV->RGB 口径【不一样】。实测同一块 NV12:
//       RGA 硬件: (63,63,63)   <- 亮度按 BT.601 limited 展开, 色度被压掉
//       CPU/OpenCV: (0,133,0)  <- 保留色度
//   也就是 RGA 出来的图偏暗、偏灰。车牌上的小字("苏"这种笔画细的汉字)
//   对这点差别很敏感 —— 实测同一块绿牌:
//       RGA 路径  -> "AL"    (省份字没认出来)
//       CPU 路径  -> "苏A"   (认出来了)
//   车牌 ROI 很小(一百多像素宽), CPU 转换的代价是几十微秒, 相对一次
//   NPU 推理(20ms)可以忽略, 所以车牌这条路专门走 CPU, 保证和离线
//   验证(OpenCV 解码 + OpenCV 转换)是同一套色彩口径。
//
// ★ 通道序: dst.format 声明什么就产出什么(RGB_888 / BGR_888), 和 RGA
//   硬件路径保持一致 —— 否则同一条流水线上两种色彩口径, 极难排查。
// ★ 前提: src.vir_addr 必须是 CPU 可读的指针(MPP 解码器给的 ptr 就是)。
//   拿不到就返回 -1, 调用方自行退回 RGA 硬件路径。
// ---------------------------------------------------------------------------
int cpu_crop_to_rgb888(const RgaImage& src, int sx, int sy, int sw, int sh,
                       const RgaImage& dst) {
    if (sw <= 0 || sh <= 0 || dst.width <= 0 || dst.height <= 0) return -1;
    if (!dst.vir_addr) return -1;
    if (!src.vir_addr) return -1;       // 没有 CPU 可读的源 -> 走不了 CPU 路径
    if (sx < 0) { sw += sx; sx = 0; }
    if (sy < 0) { sh += sy; sy = 0; }
    if (sx + sw > src.width) sw = src.width - sx;
    if (sy + sh > src.height) sh = src.height - sy;
    if (sw <= 0 || sh <= 0) return -1;

    // ★★ 关键: YUV->RGB 的【范围口径】必须和 ffmpeg 一致 ★★
    //
    //   OpenCV 的 COLOR_YUV2RGB_NV12 用的是【全范围】口径:
    //       R = Y + 1.402*(V-128)          (Y 不做 16..235 -> 0..255 的展开)
    //   而 most 摄像机的 H.264 是【有限范围】(Y 16..235, C 16..240),
    //   正确的转换应该是:
    //       Y' = (Y-16)*255/219            (亮度展开)
    //       C' = (C-128)*255/224 + 128      (色度展开)
    //       R  = Y' + 1.402*(V'-128)
    //   ffmpeg/swscale 默认就是这么做的, 所以同一帧:
    //       ffmpeg 解出来的     -> 亮
    //       OpenCV NV12 直转的  -> 暗(少了亮度展开, 色度也偏)
    //   车牌上的小字对这点差别非常敏感: 同一块绿牌, 亮的那版读出 "苏A",
    //   暗的那版只读出 "A"。所以这里先把 NV12 区域展开成全范围,
    //   再交给 OpenCV 转 RGB。
    //
    //   RGA 硬件路径【已经】做了亮度展开(所以它的亮度是对的), 但色度被
    //   压掉了(绿牌变成灰的) —— 两条路径谁都不完全对, 见 README 12.4。
    if (!cpu_nv12_to_rgb888(src, dst, sx, sy, sw, sh, 0, 0, dst.width, dst.height)) {
        return -1;
    }

    // CPU 路径产出的是 RGB; dst 声明 BGR_888 时把 R/B 换回来, 和 RGA 硬件
    // 路径(BGR_888 -> BGR)对齐。
    if (dst.format == RK_FORMAT_BGR_888) {
        uint8_t* base = static_cast<uint8_t*>(dst.vir_addr);
        const int row = dst.wstride;
        for (int r = 0; r < dst.height; ++r) {
            uint8_t* p = base + static_cast<size_t>(r) * row;
            for (int c = 0; c < dst.width; ++c) {
                const uint8_t t = p[c * 3 + 0];
                p[c * 3 + 0] = p[c * 3 + 2];
                p[c * 3 + 2] = t;
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 把 CPU 上的 YUV420 帧打包成一份连续的 NV12(诊断模式用)
//
// 存在的理由: 机器上没有 MPP 硬解时(拿不到 /dev/mpp_service 权限),
// 软解出来的是 CPU 内存 + 平面 YUV420, RGA 既没有 fd 可以 import,
// 也描述不了平面格式。把帧重排成 NV12 塞进 dma-buf 之后, 就能把
// "DMA-BUF fd -> RGA" 这一段单独拎出来验证。
//
// ⚠️ 这里多了一次 CPU 拷贝, 所以它【不是】零拷贝, 只是诊断手段。
//    真正的零拷贝要靠 MPP 直接给 fd。
int pack_yuv420_to_nv12(const RgaImage& src, uint8_t* dst, size_t dst_cap) {
    if (!dst || dst_cap == 0) return -1;
    const size_t need = static_cast<size_t>(src.width) * src.height * 3 / 2;
    if (need > dst_cap) return -1;

    std::vector<uint8_t> tmp;
    if (!yuv420_region_to_nv12(src, 0, 0, src.width, src.height, &tmp)) return -1;
    if (tmp.size() < need) return -1;
    memcpy(dst, tmp.data(), need);
    return 0;
}

}  // namespace rk
