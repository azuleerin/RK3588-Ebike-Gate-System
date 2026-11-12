// src/minimal/unit_npu.cc —— npu-only 最小化单元
//
// 隔离边界（本单元**只允许**触碰 RKNN）:
//   ✅ 允许: RKNN(rknn_init / rknn_run / rknn_outputs_get)、dma-buf 分配、CPU 读文件
//   ❌ 禁止: MPP 解码、RGA 转换、任何 MQTT/串口/测速/画框
//
// 输入链路（与业务代码 main.cc:1405-1492 **保持一致**，这是"带病复现"的前提）:
//   预录 NV12/RGB 文件
//     -> CPU 转 RGB888(纯 CPU, 把 RGA 彻底摘出去)
//     -> memcpy 进 dma-buf(system-uncached)
//     -> rknn_create_mem_from_fd(fd, virt, size, 0)
//     -> rknn_set_io_mem(ctx, mem, attr{pass_through=0, type=UINT8, fmt=NHWC})
//     -> rknn_run / rknn_outputs_get / rknn_outputs_release
//
// 内存物理属性（规则第六节要求写清）:
//   - 输入缓冲: DMA-BUF, 来自 /dev/dma_heap/system-uncached(非 cache)
//     申请者: 本单元(dma_buf_alloc)  释放者: 本单元(dma_buf_unmap + close)
//     生命周期: 整个进程, 跨 NPU 推理调用, 只在主线程使用
//   - 帧数据: 普通堆内存(MemGuard 保护), 只在主线程使用
//
// 观测（rk_base）:
//   FdLedger: 每次把 fd 交给 RKNN 前校验有效性与尺寸; 定期与 /proc/self/fd 对账
//   MemGuard: 帧缓冲受控分配 + canary 巡检
//   StackGuard: 每帧 mark_stack + beat, 看门狗监控心跳与栈余量

#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include "dmabuf.h"
#include "rk/fd_ledger.h"
#include "rk/log.h"
#include "rk/mem_guard.h"
#include "rk/minimal/unit.h"
#include "rk/stack_guard.h"
#include "rknn_api.h"

namespace rk {
namespace minimal {
namespace {

double NowMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct SourceSpec {
    enum class Kind { kNv12, kRgb } kind = Kind::kNv12;
    std::string path;
};

// 解析 --src: "nv12:<path>" / "rgb:<path>" / "<path>"(按大小猜)
SourceSpec parse_src(const std::string& s) {
    SourceSpec spec;
    if (s.rfind("nv12:", 0) == 0) {
        spec.kind = SourceSpec::Kind::kNv12;
        spec.path = s.substr(5);
    } else if (s.rfind("rgb:", 0) == 0) {
        spec.kind = SourceSpec::Kind::kRgb;
        spec.path = s.substr(4);
    } else {
        spec.path = s;
    }
    return spec;
}

}  // namespace

int run_npu_unit(const UnitOptions& opt) {
    RK_LOG_INFO("npu", "start",
                "===== npu-only 单元启动 (只触碰 RKNN, 不含 MPP/RGA) =====");
    RK_LOG_INFO("npu", "start", "输入源=%s 模型=%s", opt.src.c_str(),
                opt.model.c_str());

    // ---------------- 1. 加载模型 ----------------
    rknn_context ctx = 0;
    uint32_t flags = RKNN_FLAG_PRIOR_HIGH;
    int ret = rknn_init(&ctx, const_cast<char*>(opt.model.c_str()), 0, flags, nullptr);
    if (ret != RKNN_SUCC) {
        RK_LOG_ERROR("npu", "init", "rknn_init 失败 ret=%d 模型=%s", ret,
                     opt.model.c_str());
        return 1;
    }

    rknn_sdk_version ver;
    if (rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver)) == RKNN_SUCC) {
        RK_LOG_INFO("npu", "init", "SDK=%s driver=%s", ver.api_version, ver.drv_version);
    }

    rknn_input_output_num io_num;
    if (rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num)) != RKNN_SUCC) {
        RK_LOG_ERROR("npu", "init", "rknn_query(IN_OUT_NUM) 失败");
        rknn_destroy(ctx);
        return 1;
    }

    rknn_tensor_attr in_attr;
    memset(&in_attr, 0, sizeof(in_attr));
    if (rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr)) != RKNN_SUCC) {
        RK_LOG_ERROR("npu", "init", "rknn_query(INPUT_ATTR) 失败");
        rknn_destroy(ctx);
        return 1;
    }

    // 输入属性: 从 dims 推出宽高
    //   RKNN_TENSOR_NHWC -> dims = [1, H, W, C]
    //   RKNN_TENSOR_NCHW -> dims = [1, C, H, W]
    int in_w = 0;
    int in_h = 0;
    int in_c = 3;
    if (in_attr.n_dims != 4) {
        RK_LOG_ERROR("npu", "init", "输入维度异常 n_dims=%u", in_attr.n_dims);
        rknn_destroy(ctx);
        return 1;
    }
    if (in_attr.fmt == RKNN_TENSOR_NHWC) {
        in_h = static_cast<int>(in_attr.dims[1]);
        in_w = static_cast<int>(in_attr.dims[2]);
        in_c = static_cast<int>(in_attr.dims[3]);
    } else {
        in_c = static_cast<int>(in_attr.dims[1]);
        in_h = static_cast<int>(in_attr.dims[2]);
        in_w = static_cast<int>(in_attr.dims[3]);
    }
    RK_LOG_INFO("npu", "init", "输入 %ux%ux%u fmt=%d type=%d 期望 %u 字节", in_w, in_h,
                in_c, in_attr.fmt, in_attr.type, in_attr.size);

    if (opt.core_mask > 0) {
        ret = rknn_set_core_mask(ctx, static_cast<rknn_core_mask>(opt.core_mask));
        RK_LOG_INFO("npu", "init", "rknn_set_core_mask(0x%x) = %d", opt.core_mask, ret);
    }

    // ---------------- 2. 准备输入帧 ----------------
    const SourceSpec spec = parse_src(opt.src);
    size_t src_size = 0;
    uint8_t* raw = load_file(spec.path, &src_size, "npu-src");
    if (!raw) {
        rknn_destroy(ctx);
        return 1;
    }

    const size_t rgb_bytes = static_cast<size_t>(in_w) * in_h * in_c;
    uint8_t* rgb = nullptr;
    if (spec.kind == SourceSpec::Kind::kNv12) {
        const size_t nv12_bytes = rgb_bytes / 2;
        if (src_size != nv12_bytes) {
            RK_LOG_ERROR("npu", "src", "NV12 尺寸不符: 文件 %zu 字节, 期望 %zu 字节",
                         src_size, nv12_bytes);
            MemGuard::free_ref(raw);
            rknn_destroy(ctx);
            return 1;
        }
        rgb = nv12_to_rgb888(raw, in_w, in_h, "npu-rgb");
        if (!rgb) {
            MemGuard::free_ref(raw);
            rknn_destroy(ctx);
            return 1;
        }
        RK_LOG_INFO("npu", "src", "NV12 %ux%u -> RGB888 (纯 CPU 转换)", in_w, in_h);
    } else {
        if (src_size != rgb_bytes) {
            RK_LOG_ERROR("npu", "src", "RGB 尺寸不符: 文件 %zu 字节, 期望 %zu 字节",
                         src_size, rgb_bytes);
            MemGuard::free_ref(raw);
            rknn_destroy(ctx);
            return 1;
        }
        rgb = raw;  // 直接复用
        raw = nullptr;
        RK_LOG_INFO("npu", "src", "RGB888 %ux%u 直接使用", in_w, in_h);
    }
    MemGuard::free_ref(raw);

    // ---------------- 3. DMA-BUF 输入缓冲（与业务代码同路径）----------------
    // 物理属性: DMA-BUF, /dev/dma_heap/system-uncached(非 cache)
    // 申请者/释放者: 本单元, 生命周期 = 整个进程
    const int model_fd = rk::dma_buf_alloc(rgb_bytes, /*uncached=*/true);
    if (model_fd < 0) {
        RK_LOG_ERROR("npu", "buf", "dma_buf_alloc(%zu) 失败", rgb_bytes);
        MemGuard::free_ref(rgb);
        rknn_destroy(ctx);
        return 1;
    }
    void* model_ptr = rk::dma_buf_map(model_fd, rgb_bytes);
    if (model_ptr == MAP_FAILED || model_ptr == nullptr) {
        RK_LOG_ERROR("npu", "buf", "dma_buf_map 失败");
        ::close(model_fd);
        MemGuard::free_ref(rgb);
        rknn_destroy(ctx);
        return 1;
    }
    FdLedger::track(model_fd, "npu-input", rgb_bytes);
    RK_LOG_INFO("npu", "buf", "DMA-BUF 输入缓冲 fd=%d size=%zu backend=%s", model_fd,
                rgb_bytes, rk::dma_buf_backend());

    // 把首帧先灌进去
    memcpy(model_ptr, rgb, rgb_bytes);

    // ---------------- 4. 绑定 NPU 输入（零拷贝 fd 路径）----------------
    rknn_tensor_mem* in_mem = rknn_create_mem_from_fd(ctx, model_fd, model_ptr,
                                                      rgb_bytes, 0);
    if (!in_mem) {
        RK_LOG_ERROR("npu", "bind", "rknn_create_mem_from_fd 失败");
        rk::dma_buf_unmap(model_ptr, rgb_bytes);
        FdLedger::close_tracked(model_fd, "npu-input");
        MemGuard::free_ref(rgb);
        rknn_destroy(ctx);
        return 1;
    }
    rknn_tensor_attr bind_attr;
    memset(&bind_attr, 0, sizeof(bind_attr));
    // ⚠️ 必须以查询到的输入属性为基础, 不能零填充后只填 type/fmt/dims。
    //    rknn_set_io_mem 内部要靠 attr 里的 size/n_elems/index 去校验这块
    //    内存是否与模型输入匹配 —— 零填充会让 size=0, 直接返回 -1。
    //    (与业务代码 main.cc:1457-1472 的 attr_logic 用法保持一致)
    bind_attr = in_attr;
    bind_attr.pass_through = 0;      // 告诉驱动"我给的是 NHWC/uint8"
    bind_attr.type = RKNN_TENSOR_UINT8;
    bind_attr.fmt = RKNN_TENSOR_NHWC;
    bind_attr.n_dims = 4;
    bind_attr.dims[0] = 1;
    bind_attr.dims[1] = static_cast<uint32_t>(in_h);
    bind_attr.dims[2] = static_cast<uint32_t>(in_w);
    bind_attr.dims[3] = static_cast<uint32_t>(in_c);
    ret = rknn_set_io_mem(ctx, in_mem, &bind_attr);
    if (ret != RKNN_SUCC) {
        RK_LOG_ERROR("npu", "bind", "rknn_set_io_mem 失败 ret=%d", ret);
        rknn_destroy_mem(ctx, in_mem);
        rk::dma_buf_unmap(model_ptr, rgb_bytes);
        FdLedger::close_tracked(model_fd, "npu-input");
        MemGuard::free_ref(rgb);
        rknn_destroy(ctx);
        return 1;
    }
    RK_LOG_INFO("npu", "bind", "输入绑定成功 (fd=%d, pass_through=0, NHWC/uint8)",
                model_fd);

    // 采集 fd 基线: 此时 stdio/日志文件/RKNN runtime 内部的
    // /dev/dri/cardN 与 /dmabuf: 都已打开。采集后, 后续对账只报
    // "基线之后新出现的未登记 fd" —— 那才是真正的嫌疑。
    FdLedger::capture_baseline();

    std::vector<rknn_output> outs(io_num.n_output);
    for (uint32_t i = 0; i < io_num.n_output; ++i) {
        memset(&outs[i], 0, sizeof(rknn_output));
        outs[i].index = i;
        outs[i].want_float = 0;
    }

    // ---------------- 5. 主循环 ----------------
    TimingStat t_run;
    TimingStat t_get;
    TimingStat t_total;
    uint64_t frames = 0;
    const auto t_begin = std::chrono::steady_clock::now();
    auto last_audit = std::chrono::steady_clock::now();
    bool stop = false;

    RK_LOG_INFO("npu", "loop", "开始推理循环: frames=%d duration=%ds loop=%d",
                opt.frames, opt.duration_sec, opt.loop ? 1 : 0);

    while (!stop) {
        StackGuard::mark_stack();

        const double t0 = NowMs();

        // 模拟"新一帧到达": 覆盖 DMA-BUF 内容。
        // 带病复现要点: 这就是业务链路每帧做的事(RGA 写进同一块 DMA-BUF)。
        // 这里额外把左上角一小块改掉, 避免 NPU 侧任何缓存导致结果恒定。
        memcpy(model_ptr, rgb, rgb_bytes);
        static_cast<uint8_t*>(model_ptr)[0] =
            static_cast<uint8_t>(frames & 0xFF);

        // 交给硬件之前先校验 fd(规则第二节要求)
        if (!FdLedger::validate(model_fd, rgb_bytes, "npu-input")) {
            RK_LOG_ERROR("npu", "run", "fd 校验未通过, 本帧跳过 (不改变流程, 仅记录)");
        }

        const double t1 = NowMs();
        ret = rknn_run(ctx, nullptr);
        const double t2 = NowMs();
        if (ret != RKNN_SUCC) {
            RK_LOG_ERROR("npu", "run", "rknn_run 失败 ret=%d frame=%llu", ret,
                         static_cast<unsigned long long>(frames));
            stop = true;
            break;
        }
        ret = rknn_outputs_get(ctx, io_num.n_output, outs.data(), nullptr);
        const double t3 = NowMs();
        if (ret != RKNN_SUCC) {
            RK_LOG_ERROR("npu", "run", "rknn_outputs_get 失败 ret=%d", ret);
            rknn_outputs_release(ctx, io_num.n_output, outs.data());
            stop = true;
            break;
        }
        rknn_outputs_release(ctx, io_num.n_output, outs.data());
        const double t4 = NowMs();

        t_run.push(t2 - t1);
        t_get.push(t3 - t2);
        t_total.push(t4 - t0);
        ++frames;
        StackGuard::beat();

        if (frames % 200 == 0) {
            RK_LOG_INFO("npu", "run",
                        "frame=%llu run=%.2fms 整轮=%.2fms 平均FPS=%.1f",
                        static_cast<unsigned long long>(frames), t_run.mean(),
                        t_total.mean(), 1000.0 / t_total.mean());
        }

        // 定期巡检
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_audit).count() >=
            opt.stats_interval) {
            last_audit = now;
            char why[64];
            snprintf(why, sizeof(why), "npu-only 第 %llu 帧",
                     static_cast<unsigned long long>(frames));
            audit_now(why);
        }

        // 退出条件
        if (opt.frames > 0 && frames >= static_cast<uint64_t>(opt.frames)) break;
        if (opt.duration_sec > 0 &&
            std::chrono::duration_cast<std::chrono::seconds>(now - t_begin).count() >=
                opt.duration_sec) {
            break;
        }
    }

    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t_begin).count();

    // ---------------- 6. 收尾 ----------------
    RK_LOG_INFO("npu", "done", "===== npu-only 结束 =====");
    RK_LOG_INFO("npu", "done", "帧数=%llu 用时=%.1fs 平均FPS=%.2f",
                static_cast<unsigned long long>(frames), elapsed,
                elapsed > 0 ? static_cast<double>(frames) / elapsed : 0.0);
    RK_LOG_INFO("npu", "done", "run 耗时: mean=%.3fms min=%.3f p50=%.3f p90=%.3f max=%.3f",
                t_run.mean(), t_run.min, t_run.pct(50), t_run.pct(90), t_run.max);
    RK_LOG_INFO("npu", "done", "整轮耗时: mean=%.3fms p50=%.3f max=%.3f",
                t_total.mean(), t_total.pct(50), t_total.max);

    audit_now("npu-only 结束前");

    // 清理顺序: 先解绑 NPU 映射 -> 再 unmap -> 再 close fd -> 最后销毁 ctx
    // (与 main.cc:2247-2254 的顺序保持一致, 顺序错了会 NPU 访问已释放内存)
    rknn_destroy_mem(ctx, in_mem);
    rk::dma_buf_unmap(model_ptr, rgb_bytes);
    FdLedger::close_tracked(model_fd, "npu-input");
    MemGuard::free_ref(rgb);
    rknn_destroy(ctx);

    RK_LOG_INFO("npu", "done", "清理完成, 未触发死机");
    return 0;
}

int run_decoder_unit(const UnitOptions& opt) {
    RK_LOG_ERROR("unit", "decoder",
                 "decoder-only 尚未实现 —— 见方案 step/03 (当前仅 npu-only 可用)");
    (void)opt;
    return 2;
}

int run_rga_unit(const UnitOptions& opt) {
    RK_LOG_ERROR("unit", "rga",
                 "rga-only 尚未实现 —— 见方案 step/04 (当前仅 npu-only 可用)");
    (void)opt;
    return 2;
}

}  // namespace minimal
}  // namespace rk
