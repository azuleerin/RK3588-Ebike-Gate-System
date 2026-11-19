#include "mpp_decoder.h"

#if RK_HAVE_MPP

#include <string.h>
#include <unistd.h>

#include <dlfcn.h>
#include <sys/stat.h>

// MPP 头文件有两种装法, 这里两种都认:
//   1) /usr/include/rockchip/rk_mpi.h   ->  #include <rockchip/rk_mpi.h>
//   2) <prefix>/include/rk_mpi.h        ->  #include <rk_mpi.h>
// CMake 里 MPP_INC_DIR 被锁死成【板子系统自带】的那一份(见 CMakeLists.txt),
// 所以这里编出来的结构体布局和运行时加载的 .so 必然是同一版本 ——
// 头/库版本对不上时 mpp_create() 之后第一次调用就会按错偏移跳进野地址。
#if defined(__has_include)
#if __has_include(<rockchip/rk_mpi.h>)
#include <rockchip/rk_mpi.h>
#elif __has_include(<rk_mpi.h>)
#include <rk_mpi.h>
#else
#error "找不到 MPP 头文件(rk_mpi.h), 请安装板子系统自带的 librockchip-mpp-dev"
#endif
#else
#include <rockchip/rk_mpi.h>
#endif

// MPP 新版 API(板子系统自带的 1.5.0 才有的 rk_vdec_cfg.h / MppDecCfg)与旧版
// (20190626)在 MPP_DEC_SET_PARSER_SPLIT_MODE 的【参数语义】上不一样, 用这个
// 头文件的有无来区分 —— 详见 init() 步骤 2 的注释。
#if defined(__has_include)
#if __has_include(<rockchip/rk_vdec_cfg.h>)
#define RK_MPP_NEW_API 1
#endif
#endif
#ifndef RK_MPP_NEW_API
#define RK_MPP_NEW_API 0
#endif

namespace rk {
namespace {

// MPP 头文件里没有给出 parser split mode 的取值, 但 Rockchip 官方 MPP 里
//   0=NONE 1=FIRST 2=LAST 3=ALL
// ALL = 解码器自己处理带在码流里的 SPS/PPS/SEI (RTSP 场景必须)。
#ifndef MPP_PARSER_SPLIT_MODE_ALL
#define MPP_PARSER_SPLIT_MODE_ALL 3
#endif

// MpiCmd / MppParam 的类型
inline MppParam mpp_param_u64(uint64_t v) {
    // Rockchip 的老接口把整数值强转成 void* 传
    return reinterpret_cast<MppParam>(static_cast<uintptr_t>(v));
}

// ---------------------------------------------------------------------------
// 取 MppFrame 的 dma-buf fd —— 两种 MPP 版本都认
//
// · 新版 MPP(1.5.0 / BSP 版): 直接有 mpp_frame_get_fd(frame)
// · 旧版 MPP(20190626):       没有这个函数, 只能
//       mpp_buffer_get_fd(mpp_frame_get_buffer(frame))
//
// 新版里 mpp_frame_get_fd 是【宏】, 展开成
//     mpp_frame_get_fd_with_caller(frame, __FUNCTION__)
// 所以真正导出的动态符号是 "mpp_frame_get_fd_with_caller",
// 签名 int (*)(MppFrame, const char*)。
//
// 这里用 dlsym 探测, 不绑死任何一个版本:
//   探到 with_caller 就用它 -> 探到裸符号就用它 -> 都没有就退回 buffer 路径。
// ---------------------------------------------------------------------------
using mpp_frame_fd_caller_t = int (*)(MppFrame, const char*);
using mpp_frame_fd_plain_t = int (*)(MppFrame);

int mpp_frame_get_fd_compat(MppFrame frame) {
    static mpp_frame_fd_caller_t fn_caller = nullptr;
    static mpp_frame_fd_plain_t  fn_plain = nullptr;
    static bool tried = false;

    if (!tried) {
        tried = true;
        fn_caller = reinterpret_cast<mpp_frame_fd_caller_t>(
            dlsym(RTLD_DEFAULT, "mpp_frame_get_fd_with_caller"));
        if (!fn_caller) {
            fn_plain = reinterpret_cast<mpp_frame_fd_plain_t>(
                dlsym(RTLD_DEFAULT, "mpp_frame_get_fd"));
        }
    }

    if (fn_caller) {
        const int fd = fn_caller(frame, __FUNCTION__);
        if (fd >= 0) return fd;
    }
    if (fn_plain) {
        const int fd = fn_plain(frame);
        if (fd >= 0) return fd;
    }

    // 旧版 MPP: 从 frame 的 buffer 上拿
    MppBuffer buf = mpp_frame_get_buffer(frame);
    if (buf) return mpp_buffer_get_fd(buf);
    return -1;
}

}  // namespace

// ---------------------------------------------------------------------------
// 运行环境诊断
// ---------------------------------------------------------------------------
void diag_mpp_environment(const char* tag) {
    fprintf(stderr, "---- MPP 环境诊断 (%s) ----\n", tag ? tag : "");

    // (0) 编译期用的是哪份头文件 / 哪个库 —— 和下面(1)的运行时 .so 对比,
    //     一眼就能看出是不是"头文件和库不是同一个版本"。
#ifdef RK_MPP_HEADER_DIR
    fprintf(stderr, "[MPP] 编译期头文件   : %s\n", RK_MPP_HEADER_DIR);
#endif
#ifdef RK_MPP_LINK_LIB
    fprintf(stderr, "[MPP] 链接期库       : %s\n", RK_MPP_LINK_LIB);
#endif

    // (1) 运行时【实际加载】的是哪个 .so —— 最关键的一条。
    //     链接期用的是 CMake 里 -DMPP_LIB 指定的路径, 但运行时 ld.so 还会按
    //     LD_LIBRARY_PATH / ldconfig 缓存的顺序挑。板子自带 BSP 版 MPP 时,
    //     实际加载的可能根本不是我们装的那个。
    Dl_info info;
    memset(&info, 0, sizeof(info));
    void* sym = dlsym(RTLD_DEFAULT, "mpp_create");
    if (sym && dladdr(sym, &info) != 0 && info.dli_fname) {
        fprintf(stderr, "[MPP] 运行时加载的库 : %s\n", info.dli_fname);
    } else {
        fprintf(stderr, "[MPP] 运行时加载的库 : 未知(dlsym/dladdr 失败)\n");
    }

    // (2) 内核侧设备节点 —— 没有 /dev/mpp_service 基本不可能硬解
    const char* devs[] = {"/dev/mpp_service", "/dev/rkvdec", "/dev/dri",
                          "/dev/video0", "/dev/video1", "/dev/media0"};
    for (size_t i = 0; i < sizeof(devs) / sizeof(devs[0]); ++i) {
        struct stat st;
        fprintf(stderr, "[MPP] 设备节点 %-20s %s\n", devs[i],
                (stat(devs[i], &st) == 0) ? "存在" : "不存在");
    }

    // (3) 顺带看看 ldconfig 里到底有几个候选
    fprintf(stderr, "[MPP] 提示: ldconfig -p | grep rockchip_mpp   看有几个候选 .so\n");
    fprintf(stderr, "[MPP] 提示: dmesg | grep -iE 'mpp|rkvdec|vpu'  看内核侧\n");
    fprintf(stderr, "------------------------------------------\n");
    fflush(stderr);
}

// ---------------------------------------------------------------------------
// 初始化
//
// ⚠️ 这里刻意拆成 5 个独立步骤, 每一步的返回码【单独打印】。
//    以前这几步连在一起写, 一旦 mpp_create() 段错误, 日志上只剩一句
//    "MPP 初始化过程中崩溃", 根本看不出崩在哪一步。拆开之后:
//      崩在步骤 1 -> 上下文创建就炸了  -> 头/库版本对不上
//      崩在步骤 4 -> mpp_init 炸了       -> 内核驱动/格式不支持
//      崩在步骤 5 -> buffer group 炸了   -> dma-buf/ION 通路有问题
//    步骤顺序不能改: 解析器参数必须在 mpp_init 之前设。
// ---------------------------------------------------------------------------
bool MppDecoder::init(bool hevc, int width_hint, int height_hint) {
    destroy();
    hevc_ = hevc;

    diag_mpp_environment("init 之前");

    // ---- 步骤 1/5: 创建 MPP 上下文 --------------------------------------
    MppCtx ctx = nullptr;
    MppApi* mpi = nullptr;
    const MPP_RET cret = mpp_create(&ctx, &mpi);
    fprintf(stderr, "[MPP] 步骤 1/5 mpp_create (上下文创建)      返回码=%d ctx=%p mpi=%p\n",
            static_cast<int>(cret), static_cast<void*>(ctx), static_cast<void*>(mpi));
    if (cret != MPP_SUCCESS || !ctx || !mpi) {
        err_ = "mpp_create 失败";
        fprintf(stderr, "[MPP] ✗ 步骤 1 失败: mpp_create 返回 %d (ctx=%p mpi=%p)\n",
                static_cast<int>(cret), static_cast<void*>(ctx),
                static_cast<void*>(mpi));
        fprintf(stderr, "[MPP]   若这里就失败: 多半是【板子自带的新版 MPP 内核驱动】\n"
                        "         与本工程用的旧版用户态不匹配。\n"
                        "         绕过: --decoder ffmpeg 走软解, 硬解要换板子自带的 MPP 头文件/库重编。\n");
        return false;
    }

    // ---- 步骤 2/5: 解析器分割模式 (必须在 mpp_init 之前) ----------------
    // RTSP 码流里带 SPS/PPS/SEI, 必须让解码器自己完整解析并按 NAL 切帧,
    // 否则解不出画面。
    //
    // ⚠️⚠️ 参数语义在 MPP 1.5.0 变过, 这是本工程之前段错误的真正原因:
    //     旧版(20190626): 直接传整数
    //         mpi->control(ctx, cmd, (MppParam)3)
    //     新版(1.5.0): 要传【指向整数的指针】
    //         mpi->control(ctx, cmd, &mode)
    //     上游 mpp_dec_set_cfg_by_cmd() 里写的是
    //         mpp_dec_cfg_set_u32(cfg, "base:split_parse", *((RK_U32 *)param))
    //     也就是说它会【解引用 param】。新版还按旧写法传值的话,
    //     MPP 会去读地址 0x3 上的内存 -> 当场 SIGSEGV。
    //
    // 上游 mpp.c 明确支持"init 之前设置": 这时配置先记到 ctx 的 MppDecCfg 上,
    // mpp_init() 时再交给解码器(见上游 mpp_init() 里的 MppDecInitCfg)。
    //
    // 取值: 新版 base:split_parse 是个开关 —— 1 = 完整解析 + 按 NAL 切帧,
    //       0 = 快速路径(整包当一帧)。这正是老代码 MPP_PARSER_SPLIT_MODE_ALL
    //       想达到的效果, 也是官方 demo mpi_dec_test 设的值。
    MPP_RET sret = MPP_SUCCESS;
#if RK_MPP_NEW_API
    RK_U32 split_parse = 1;   // 1 = 完整解析并按 NAL 切分帧
    sret = mpi->control(ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, &split_parse);
#else
    sret = mpi->control(ctx, MPP_DEC_SET_PARSER_SPLIT_MODE,
                        mpp_param_u64(MPP_PARSER_SPLIT_MODE_ALL));
#endif
    fprintf(stderr, "[MPP] 步骤 2/5 control (解析器分割模式)     返回码=%d\n",
            static_cast<int>(sret));
    if (sret != MPP_SUCCESS) {
        // 和老代码一样: 这里只告警不中断(mpp_init 之后如果真解不出帧,
        // 日志里已经能看到这一步的返回码了)
        fprintf(stderr, "[MPP] ! 步骤 2 解析器分割模式没设成功, 码流里的 SPS/PPS 可能解不出来\n");
    }

    // ---- 步骤 3/5: 编码格式支持检查 ------------------------------------
    const MppCodingType coding = hevc ? MPP_VIDEO_CodingHEVC : MPP_VIDEO_CodingAVC;
    const int fmt_ret = static_cast<int>(mpp_check_support_format(MPP_CTX_DEC, coding));
    fprintf(stderr, "[MPP] 步骤 3/5 check_support_format (%s) 返回码=%d -> %s\n",
            hevc ? "H.265" : "H.264", fmt_ret,
            (fmt_ret == MPP_SUCCESS) ? "支持" : "不支持");
    if (fmt_ret != MPP_SUCCESS) {
        err_ = hevc ? "MPP 不支持 H.265" : "MPP 不支持 H.264";
        mpp_destroy(ctx);
        return false;
    }

    // ---- 步骤 4/5: 初始化解码器 (编码格式设置) -------------------------
    const MPP_RET iret = mpp_init(ctx, MPP_CTX_DEC, coding);
    fprintf(stderr, "[MPP] 步骤 4/5 mpp_init (编码格式设置)      返回码=%d\n",
            static_cast<int>(iret));
    if (iret != MPP_SUCCESS) {
        err_ = "mpp_init 失败";
        mpp_destroy(ctx);
        return false;
    }

    // ---- 步骤 5/5: 缓冲区组 ---------------------------------------------
    // ⚠️ 这一步是【能力探测】, 不是把外部 buffer group 挂到解码器上:
    //    MPP 解码器在 mpp_init() 里会自己建内部输出 buffer group, 我们不动它,
    //    只单独建一个同类型(ION)的 group, 验证 dma-buf/ION 通路可用, 打印
    //    返回码后立刻释放。这样万一 buffer 相关的调用出问题, 日志能指到第 5 步。
    MppBufferGroup group = nullptr;
    const MPP_RET gret = mpp_buffer_group_get_internal(&group, MPP_BUFFER_TYPE_ION, 0);
    fprintf(stderr, "[MPP] 步骤 5/5 buffer_group_get (缓冲区组)  返回码=%d group=%p\n",
            static_cast<int>(gret), static_cast<void*>(group));
    if (gret == MPP_SUCCESS && group) {
        mpp_buffer_group_put(group);
        group = nullptr;
    } else {
        fprintf(stderr, "[MPP] ! 步骤 5 缓冲区组探测失败(不影响解码, 解码器会用内部 group)\n");
    }

    ctx_ = ctx;
    api_ = mpi;
    err_.clear();
    (void)width_hint;
    (void)height_hint;
    fprintf(stderr, "[MPP] 初始化成功: %s 硬解上下文已就绪\n", hevc ? "H.265" : "H.264");
    fflush(stderr);
    return true;
}

void MppDecoder::destroy() {
    // 释放可能暂存着的帧(正常流程下不会有, 但异常退出时不能漏)
    if (held_frame_) {
        MppFrame hf = static_cast<MppFrame>(held_frame_);
        mpp_frame_deinit(&hf);
        held_frame_ = nullptr;
    }
    if (ctx_) {
        MppApi* mpi = static_cast<MppApi*>(api_);
        if (mpi) mpi->reset(static_cast<MppCtx>(ctx_));
        mpp_destroy(static_cast<MppCtx>(ctx_));
    }
    ctx_ = nullptr;
    api_ = nullptr;
    frames_ = 0;
}

// ---- 送包背压参数 ----
// 队列满时最多重试 500 次, 每次退避 2ms -> 最长等 1 秒。
// 1 秒足够解码器把队列排空(它按真实帧率在走); 超时了就当这一包坏了,
// 放弃它继续下一包, 不让整个流水线卡死。
static const int kPutRetryMax = 500;
static const int kPutRetrySleepUs = 2000;

bool MppDecoder::decode(const uint8_t* data, size_t size, int64_t pts, Nv12Frame* out,
                        void** out_frame) {
    if (!ctx_ || !api_ || !data || size == 0 || !out) return false;
    MppApi* mpi = static_cast<MppApi*>(api_);
    MppCtx ctx = static_cast<MppCtx>(ctx_);

    // ---- 送包: 队列满时必须【等】, 不能把包丢掉 ----
    //
    // ★ 这是"本地文件源一帧都解不出来"的真正原因(2026-11-13 定位):
    //   MPP 解码器的输入队列深度很浅。RTSP 源是 25fps【按真实时间】到达的,
    //   队列永远排得过来; 但读本地文件时, 包是"一瞬间"全到的, 队列马上就满,
    //   decode_put_packet 返回 MPP_ERR_BUFFER_FULL(-1012)。
    //   老代码把这个返回码一忽略就往下走 -> 绝大部分包被丢掉 ->
    //   "所有包都读完了, 却一帧都没解出来", 日志里表现为
    //   "读取结束: End of file"(很容易被误判成"视频文件坏了")。
    //
    //   正确做法是【背压】: 队列满就等它排空, 把这个包重投进去。
    //
    // ★★ 等待期间【必须顺手取帧】, 不能干等 —— 否则会死锁:
    //   MPP 在解出第一帧之前会先给一个"信息变更帧", 应用必须回
    //   MPP_DEC_SET_INFO_CHANGE_READY, 解码器才会继续往下解。
    //   如果队列满时我们只是傻等(不取帧), 就永远拿不到那个信息变更帧 ->
    //   解码器等 READY、应用等队列空, 谁也不动 -> 进程卡死,
    //   日志最后一行停在 "mpp_dec: reset at info change"。
    //   (第一版背压实现就栽在这里, 第二版才补上"等待时取帧"。)
    MppPacket packet = nullptr;
    MPP_RET ret = MPP_SUCCESS;
    for (int attempt = 0;; ++attempt) {
        if (mpp_packet_init(&packet, const_cast<void*>(static_cast<const void*>(data)), size)
            != MPP_SUCCESS) {
            err_ = "mpp_packet_init 失败";
            return false;
        }
        mpp_packet_set_pts(packet, static_cast<RK_S64>(pts));
        ret = mpi->decode_put_packet(ctx, packet);
        if (ret == MPP_SUCCESS) break;

        // 投失败: 先把手上的包释放掉, 再决定怎么办
        mpp_packet_deinit(&packet);
        packet = nullptr;
        if (ret != MPP_ERR_BUFFER_FULL) {
            // 码流异常时不必退出, 丢掉这一包继续
            return false;
        }
        if (attempt >= kPutRetryMax) {
            err_ = "MPP 输入队列一直是满的(等了 1 秒)";
            return false;
        }
        // 等待间隙顺手取帧:
        //   - 拿到"信息变更帧" -> take_one_frame() 内部会立刻回 READY, 解开死锁
        //   - 拿到真画面       -> 暂存到 held_frame_, 等这个包送进去之后再交出去
        //     (不能现在就交: 现在交了就等于把这个还没送进去的包丢了)
        Nv12Frame tmp{};
        void* tmp_raw = nullptr;
        bool got_real = false;
        if (take_one_frame(&tmp, &tmp_raw, &got_real) && got_real) {
            // ★★★ 这里原来是个 fd 泄漏点 ★★★
            //   held_frame_ 是"上一次攒到的帧"。直接覆盖它, 上一帧就再也没人
            //   mpp_frame_deinit 了 -> 它的 dma-buf fd 永远留在本进程里。
            //   症状: 跑几分钟后 fd 数撞到 1024 上限 ->
            //          "mpp_dma_heap: ioctl alloc failed for Too many open files"
            //          -> MPP 申请不到 dma-buf -> 【通道静默死掉】(进程还在跑)。
            //   (实测: 4 路并发时 5~8 分钟撞线; 改前改后两个版本都复现。)
            //
            //   触发条件: 上面 kPutRetryMax(500 次 x 2ms = 1 秒)用尽后直接
            //   return false, 此时 held_frame_ 还挂着; 下一轮 decode() 的重试
            //   循环又取到新帧 -> 覆盖 -> 旧帧泄漏。重试循环里每取一帧就漏一帧。
            if (held_frame_) {
                MppFrame old = static_cast<MppFrame>(held_frame_);
                mpp_frame_deinit(&old);
                held_frame_ = nullptr;
            }
            held_frame_ = tmp_raw;
            held_out_ = tmp;
        }
        usleep(kPutRetrySleepUs);   // 退避 2ms, 让解码器先排空队列
    }

    // ---- 包送进去了, 现在交帧 ----
    MppFrame frame = nullptr;
    const bool have_held = (held_frame_ != nullptr);
    if (have_held) {
        // 等待期间攒到的帧: 它比这个包更早解出来, 顺序正确, 直接交
        frame = static_cast<MppFrame>(held_frame_);
        held_frame_ = nullptr;
        *out = held_out_;
        held_out_ = Nv12Frame{};
    } else {
        ret = mpi->decode_get_frame(ctx, &frame);
    }
    // ★ 这里只能 deinit 一次: mpp_packet_deinit() 会把指针置空,
    //   再调一次就是 "MppPacket (nil) failed on check" —— 之前重复调用,
    //   结果每帧都往日志里打一条这种噪音。
    mpp_packet_deinit(&packet);
    if (!have_held && (ret != MPP_SUCCESS || !frame)) {
        // 正常情况: 送进去的包还没解出帧(需要多喂几包)
        return false;
    }

    // MPP(1.5.0 及以后)在解出第一帧之前会先给一个"信息变更帧"(宽高/格式
    // 还没定下来)。应用必须回一个 MPP_DEC_SET_INFO_CHANGE_READY, 解码器
    // 才会继续往下解 —— 不回的话会一直卡在信息变更这一步, 一帧都出不来。
    if (mpp_frame_get_info_change(frame)) {
        fprintf(stderr, "[MPP] 信息变更帧: 宽=%d 高=%d -> 已回 MPP_DEC_SET_INFO_CHANGE_READY\n",
                static_cast<int>(mpp_frame_get_width(frame)),
                static_cast<int>(mpp_frame_get_height(frame)));
        mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, mpp_param_u64(1));
        mpp_frame_deinit(&frame);
        return false;
    }

    // ---- 取 dma-buf fd ----
    // ★ 这里拿的是【物理内存的 fd】, 不是虚拟地址:
    //   fd 可以直接交给 RGA 的 importbuffer_fd(), 让 RGA 自己去 import,
    //   CPU 全程不碰像素 —— 这就是"零拷贝"的关键。
    //   ptr 只是顺带留着, 给 CPU 兜底路径(RGA 不可用时)用。
    out->fd = mpp_frame_get_fd_compat(frame);

    MppBuffer buf = mpp_frame_get_buffer(frame);
    if (buf) {
        out->ptr = mpp_buffer_get_ptr(buf);        // CPU 映射地址(兜底路径用)
        if (out->fd < 0) out->fd = mpp_buffer_get_fd(buf);
    }

    out->width = static_cast<int>(mpp_frame_get_width(frame));
    out->height = static_cast<int>(mpp_frame_get_height(frame));
    out->wstride = static_cast<int>(mpp_frame_get_hor_stride(frame));
    out->hstride = static_cast<int>(mpp_frame_get_ver_stride(frame));
    out->format = RK_FORMAT_YCbCr_420_SP;
    out->source = (out->fd >= 0) ? Nv12Source::kDmaBufFd : Nv12Source::kCpuPtr;

    // ★ 所有权处理: 传了 out_frame 就交给调用方(用 rk::MppFrameGuard 托管),
    //   让帧活到 fd/ptr 真正用完为止; 没传就保持原来的就地释放(兼容旧调用方)。
    //   以前这里是无条件 deinit -> 调用方拿着悬垂 fd/ptr 继续用 = P0-2。
    if (out_frame) {
        *out_frame = frame;
    } else {
        mpp_frame_deinit(&frame);
    }
    ++frames_;

    // 第一帧出成功后打一行, 方便确认"硬解真的在出帧"
    static bool first_frame_logged = false;
    if (!first_frame_logged) {
        first_frame_logged = true;
        fprintf(stderr, "[MPP] 硬解出帧: %dx%d stride=%dx%d dma-buf fd=%d\n",
                out->width, out->height, out->wstride, out->hstride,
                out->fd);
        fflush(stderr);
    }
    return true;
}

// ---------------------------------------------------------------------------
// 取一帧(内部处理"信息变更帧")
//
// 返回 true  = 拿到了一帧【真画面】(已经填进 out)
// 返回 false = 没拿到画面(要么队列空, 要么拿到的是信息变更帧, 已经回过 READY)
// ---------------------------------------------------------------------------
bool MppDecoder::take_one_frame(Nv12Frame* out, void** out_frame, bool* got_real) {
    if (got_real) *got_real = false;
    if (!ctx_ || !api_ || !out) return false;
    MppApi* mpi = static_cast<MppApi*>(api_);
    MppCtx ctx = static_cast<MppCtx>(ctx_);

    MppFrame frame = nullptr;
    if (mpi->decode_get_frame(ctx, &frame) != MPP_SUCCESS || !frame) return false;

    // 信息变更帧: 必须回 READY, 否则解码器一直卡着, 一帧都出不来
    if (mpp_frame_get_info_change(frame)) {
        fprintf(stderr, "[MPP] 信息变更帧: 宽=%d 高=%d -> 已回 MPP_DEC_SET_INFO_CHANGE_READY\n",
                static_cast<int>(mpp_frame_get_width(frame)),
                static_cast<int>(mpp_frame_get_height(frame)));
        mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, mpp_param_u64(1));
        mpp_frame_deinit(&frame);
        return false;
    }

    // ---- 取 dma-buf fd(零拷贝的关键) ----
    out->fd = mpp_frame_get_fd_compat(frame);
    MppBuffer buf = mpp_frame_get_buffer(frame);
    if (buf) {
        out->ptr = mpp_buffer_get_ptr(buf);
        if (out->fd < 0) out->fd = mpp_buffer_get_fd(buf);
    }
    out->width = static_cast<int>(mpp_frame_get_width(frame));
    out->height = static_cast<int>(mpp_frame_get_height(frame));
    out->wstride = static_cast<int>(mpp_frame_get_hor_stride(frame));
    out->hstride = static_cast<int>(mpp_frame_get_ver_stride(frame));
    out->format = RK_FORMAT_YCbCr_420_SP;
    out->source = (out->fd >= 0) ? Nv12Source::kDmaBufFd : Nv12Source::kCpuPtr;

    if (out_frame) {
        *out_frame = frame;          // 所有权交给调用方(由 MppFrameGuard 托管)
    } else {
        mpp_frame_deinit(&frame);
    }
    ++frames_;
    if (got_real) *got_real = true;

    // 第一帧出成功后打一行, 方便确认"硬解真的在出帧"
    static bool first_frame_logged = false;
    if (!first_frame_logged) {
        first_frame_logged = true;
        fprintf(stderr, "[MPP] 硬解出帧: %dx%d stride=%dx%d dma-buf fd=%d\n",
                out->width, out->height, out->wstride, out->hstride, out->fd);
        fflush(stderr);
    }
    return true;
}

}  // namespace rk

#else  // !RK_HAVE_MPP
// 没装 MPP 时的空实现(保证不装 MPP 也能编译链接)
namespace rk {

bool MppDecoder::init(bool, int, int) {
    err_ = "编译时没有 MPP 支持(RK_HAVE_MPP=0)";
    return false;
}

void MppDecoder::destroy() {}

// ★ 签名必须和头文件对齐: decode() 现在多一个 `void** out_frame` 出参
//   (把 mpp_buffer 指针交还给调用方, 由 MppFrameGuard 负责释放)。
//   这个桩是"编译时没有 MPP 支持(RK_HAVE_MPP=0)"时用的 —— 少一个参数就编译不过。
bool MppDecoder::decode(const uint8_t*, size_t, int64_t, Nv12Frame*, void**) { return false; }

void diag_mpp_environment(const char* tag) {
    fprintf(stderr, "---- MPP 环境诊断 (%s) ----\n", tag ? tag : "");
    fprintf(stderr, "[MPP] 本次编译【没有】MPP 支持(RK_HAVE_MPP=0)\n");
    fprintf(stderr, "------------------------------------------\n");
}

}  // namespace rk
#endif  // RK_HAVE_MPP
