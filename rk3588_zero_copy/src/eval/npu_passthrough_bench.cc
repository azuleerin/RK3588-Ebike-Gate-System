// npu_passthrough_bench.cc —— pass_through=0 vs pass_through=1 的实测对比
//
// 问题(来自"RK3588 NPU 压榨"教程):
//   用 pass_through=1 直通模式 + 极致定频, 能把 NPU 吃满到 98%。
//   我们现在的检测模型是 INT8 量化模型, 走的是 pass_through=0 ——
//   驱动每次推理前要把 uint8 RGB 重量化成 int8 量化码。
//   这一步贵不贵? 能不能像教程那样改成 pass_through=1?
//
// 三条路径, 全部喂【同一块 dma-buf】(RGA 写进来的 uint8 RGB888):
//
//   A) passthrough=0  绑 uint8 NHWC, 让驱动自己重量化(现状)
//   B) passthrough=1  CPU 先把 uint8 重量化成 int8 量化码, 再绑 int8 NHWC 直通
//      (教程的做法 —— 但量化这一步搬到了 CPU 上, 要算它的账)
//   C) passthrough=1 + 模型输入本来就是 uint8 的情况(对照: 量化参数不适用)
//
// 每个路径都报:
//   · rknn_run 的墙钟耗时
//   · RKNN_QUERY_PERF_RUN 的 NPU 硬件耗时(差值 = 驱动在 NPU 之外干的活)
//   · B 路径额外报 CPU 重量化的耗时
//
// 用法:
//   npu_passthrough_bench --model model/ebike_int8_rk2332.rknn [--frames 200]
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#include <rknn_api.h>

#include "dmabuf.h"

namespace {

double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

bool read_file(const std::string& path, std::vector<uint8_t>* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }
    out->resize(static_cast<size_t>(sz));
    const size_t rd = fread(out->data(), 1, out->size(), f);
    fclose(f);
    return rd == out->size();
}

struct Stat {
    double wall_ms = 0;      // rknn_run 的墙钟
    double npu_ms  = 0;      // 驱动的 NPU 硬件计时
    double cpu_ms  = 0;      // CPU 侧的额外开销(重量化)
};

Stat run_path(rknn_context ctx, rknn_tensor_mem* mem, const rknn_tensor_attr* bind,
              uint8_t* buf, int frames, int warmup, bool quantize_on_cpu,
              float scale, int zp, uint8_t* qbuf) {
    Stat st;
    double wall_sum = 0, npu_sum = 0, cpu_sum = 0;
    const int n = warmup + frames;
    for (int i = 0; i < n; ++i) {
        double t_cpu = 0;
        if (quantize_on_cpu) {
            // CPU 重量化: q = clamp(round((x/255)/scale) + zp, -128, 127)
            const double inv = 1.0 / (255.0 * static_cast<double>(scale));
            const size_t n_elems = 640u * 640u * 3u;
            const double t0 = now_ms();
            for (size_t k = 0; k < n_elems; ++k) {
                int q = static_cast<int>(std::lround(buf[k] * inv)) + zp;
                if (q > 127) q = 127;
                if (q < -128) q = -128;
                qbuf[k] = static_cast<uint8_t>(static_cast<int8_t>(q));
            }
            t_cpu = now_ms() - t0;
        }
        const double t0 = now_ms();
        rknn_run(ctx, nullptr);
        const double t1 = now_ms();

        rknn_perf_run perf;
        memset(&perf, 0, sizeof(perf));
        rknn_query(ctx, RKNN_QUERY_PERF_RUN, &perf, sizeof(perf));
        const double t_npu = perf.run_duration / 1000.0;

        if (i >= warmup) {
            wall_sum += (t1 - t0);
            npu_sum += t_npu;
            cpu_sum += t_cpu;
        }
    }
    st.wall_ms = wall_sum / frames;
    st.npu_ms = npu_sum / frames;
    st.cpu_ms = cpu_sum / frames;
    return st;
}

}  // namespace

int main(int argc, char** argv) {
    std::string model;
    int frames = 200, warmup = 20;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc) model = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) warmup = atoi(argv[++i]);
    }
    if (model.empty()) {
        fprintf(stderr, "用法: %s --model <rknn> [--frames N] [--warmup N]\n", argv[0]);
        return 2;
    }

    std::vector<uint8_t> model_data;
    if (!read_file(model, &model_data)) {
        fprintf(stderr, "模型读不了: %s\n", model.c_str());
        return 1;
    }

    rknn_context ctx = 0;
    if (rknn_init(&ctx, model_data.data(), static_cast<uint32_t>(model_data.size()),
                  RKNN_FLAG_PRIOR_HIGH, nullptr) != 0) {
        fprintf(stderr, "rknn_init 失败\n");
        return 1;
    }

    // ---- 输入属性 ----
    rknn_tensor_attr ia;
    memset(&ia, 0, sizeof(ia));
    ia.index = 0;
    rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &ia, sizeof(ia));
    int H = 0, W = 0;
    if (ia.n_dims >= 4) {
        if (ia.fmt == RKNN_TENSOR_NCHW) { H = ia.dims[2]; W = ia.dims[3]; }
        else                            { H = ia.dims[1]; W = ia.dims[2]; }
    }
    const bool has_qnt = (ia.qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC) && ia.scale > 0.f;
    const float scale = has_qnt ? ia.scale : 1.0f;
    const int   zp    = has_qnt ? ia.zp : 0;
    printf("模型输入: %dx%dx%d type=%d fmt=%d scale=%.6f zp=%d 量化=%s\n",
           W, H, ia.dims[ia.n_dims - 1], ia.type, ia.fmt, scale, zp,
           has_qnt ? "AFFINE_ASYMMETRIC" : "无");

    rknn_tensor_attr nat;
    memset(&nat, 0, sizeof(nat));
    nat.index = 0;
    const bool has_native =
        (rknn_query(ctx, RKNN_QUERY_NATIVE_NHWC_INPUT_ATTR, &nat, sizeof(nat)) == 0);
    if (has_native) {
        printf("NATIVE_NHWC_INPUT_ATTR: type=%d fmt=%d dims=[", nat.type, nat.fmt);
        for (uint32_t i = 0; i < nat.n_dims; ++i) printf("%u,", nat.dims[i]);
        printf("]\n");
    }

    // ---- 一块 dma-buf 当输入 ----
    const size_t n_elems = static_cast<size_t>(W) * H * 3;
    const size_t buf_size = n_elems;
    int fd = rk::dma_buf_alloc(buf_size, true);
    if (fd < 0) { fprintf(stderr, "dma-buf 分配失败\n"); return 1; }
    uint8_t* ptr = static_cast<uint8_t*>(rk::dma_buf_map(fd, buf_size));
    if (!ptr || ptr == MAP_FAILED) { fprintf(stderr, "dma-buf 映射失败\n"); return 1; }
    // 填点有内容的像素(别全 0, 让驱动别走"空图快路径")
    for (size_t i = 0; i < n_elems; ++i) ptr[i] = static_cast<uint8_t>((i * 37) & 0xFF);

    // ================= 路径 A: pass_through=0(现状) =================
    {
        rknn_tensor_mem* mem = rknn_create_mem_from_fd(ctx, fd, ptr, buf_size, 0);
        rknn_tensor_attr bind = ia;
        bind.pass_through = 0;
        bind.type = RKNN_TENSOR_UINT8;
        bind.fmt = RKNN_TENSOR_NHWC;
        bind.n_dims = 4;
        bind.dims[0] = 1; bind.dims[1] = H; bind.dims[2] = W; bind.dims[3] = 3;
        const int br = rknn_set_io_mem(ctx, mem, &bind);
        printf("\n[A] pass_through=0 (uint8 NHWC, 驱动重量化)  set_io_mem=%d\n", br);
        if (br == 0) {
            Stat st = run_path(ctx, mem, &bind, ptr, frames, warmup, false, scale, zp, nullptr);
            printf("    rknn_run 墙钟 = %.3f ms | NPU 硬件 = %.3f ms | 差(驱动开销) = %.3f ms"
                   " | 折算上限 = %.1f FPS\n",
                   st.wall_ms, st.npu_ms, st.wall_ms - st.npu_ms, 1000.0 / st.wall_ms);
        }
        rknn_destroy_mem(ctx, mem);
    }

    // ================= 路径 B: pass_through=1 + CPU 重量化 =================
    if (has_qnt && ia.type == RKNN_TENSOR_INT8) {
        rknn_tensor_mem* mem = rknn_create_mem_from_fd(ctx, fd, ptr, buf_size, 0);
        rknn_tensor_attr bind = ia;
        bind.pass_through = 1;
        bind.type = RKNN_TENSOR_INT8;
        bind.fmt = RKNN_TENSOR_NHWC;
        bind.n_dims = 4;
        bind.dims[0] = 1; bind.dims[1] = H; bind.dims[2] = W; bind.dims[3] = 3;
        const int br = rknn_set_io_mem(ctx, mem, &bind);
        printf("\n[B] pass_through=1 (int8 NHWC 直通, CPU 先重量化)  set_io_mem=%d\n", br);
        if (br == 0) {
            std::vector<uint8_t> qbuf(n_elems);
            Stat st = run_path(ctx, mem, &bind, ptr, frames, warmup, true, scale, zp, qbuf.data());
            printf("    CPU 重量化 = %.3f ms | rknn_run 墙钟 = %.3f ms | NPU 硬件 = %.3f ms"
                   " | 合计 = %.3f ms | 折算上限 = %.1f FPS\n",
                   st.cpu_ms, st.wall_ms, st.npu_ms, st.cpu_ms + st.wall_ms,
                   1000.0 / (st.cpu_ms + st.wall_ms));
        }
        rknn_destroy_mem(ctx, mem);
    } else {
        printf("\n[B] 跳过: 模型输入不是 INT8 量化张量, 没有可直通的量化码\n");
    }

    rknn_destroy(ctx);
    rk::dma_buf_unmap(ptr, buf_size);
    ::close(fd);
    return 0;
}
