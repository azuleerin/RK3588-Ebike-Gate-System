// src/npu_model.cpp —— RKNN C API 的零拷贝封装实现
#include "nlb/npu_model.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace nlb {
namespace {

// 模型字节缓存: 同一个路径只读一次, 多个线程共享。
std::mutex g_blob_mu;
std::unordered_map<std::string, std::vector<uint8_t>> g_blobs;

// 输入缓冲的合成图案。
//
// ★ 为什么要图案而不是全 0:
//   全 0 的输入在某些量化模型上会触发异常的中间值(全 0 的 ReLU 会让整张
//   特征图塌成 0), 跑出来的耗时可能不代表真实负载。
//   用一个斜向渐变 + 周期方块: 内容非平凡, 但取值温和, 不产生极端值。
//
// ★ 热循环里绝不再调它 —— 循环里对输入缓冲的写次数必须是 0。
void fill_pattern(uint8_t* p, size_t n, uint32_t seed) {
    const uint32_t s = seed ? seed : 1u;
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        const uint32_t k = static_cast<uint32_t>(i / 3);
        p[i + 0] = static_cast<uint8_t>((k + s) & 0xFF);
        p[i + 1] = static_cast<uint8_t>((k / 2 + s * 3) & 0xFF);
        p[i + 2] = static_cast<uint8_t>(((k / 64 + s) % 200) + 20);
    }
    for (; i < n; ++i) p[i] = static_cast<uint8_t>((i + s) & 0xFF);
}

// 从输入属性推出 H/W/C。
//
// ★ RKNN 报的 INPUT_ATTR 两种布局都见过:
//     NCHW: dims = [1, C, H, W]
//     NHWC: dims = [1, H, W, C]
//   万一布局不认识, 退一步: 找出等于 3 的那一维当通道, 剩下两维按大/小当 H/W。
void derive_shape(const rknn_tensor_attr& a, uint32_t* h, uint32_t* w, uint32_t* c) {
    uint32_t d[4] = {0, 0, 0, 0};
    const uint32_t nd = a.n_dims < 4 ? a.n_dims : 4;
    for (uint32_t i = 0; i < nd; ++i) d[i] = a.dims[i];

    uint32_t ch = 0, hh = 0, ww = 0;
    if (a.fmt == RKNN_TENSOR_NHWC) {
        hh = d[1]; ww = d[2]; ch = d[3];
    } else {
        ch = d[1]; hh = d[2]; ww = d[3];
    }

    if (ch != 3) {
        int cidx = -1;
        for (uint32_t i = 0; i < nd; ++i) {
            if (d[i] == 3) { cidx = static_cast<int>(i); break; }
        }
        if (cidx >= 0) {
            ch = 3;
            uint32_t others[4];
            int no = 0;
            for (uint32_t i = 0; i < nd; ++i) {
                if (static_cast<int>(i) == cidx) continue;
                if (no < 4) others[no++] = d[i];
            }
            if (no >= 2) {
                hh = others[0] > others[1] ? others[0] : others[1];
                ww = others[0] > others[1] ? others[1] : others[0];
            }
        }
    }
    *h = hh; *w = ww; *c = ch ? ch : 3;
}

}  // namespace

bool ModelBlob::load(const std::string& p, std::string* err) {
    std::lock_guard<std::mutex> lk(g_blob_mu);
    auto it = g_blobs.find(p);
    if (it != g_blobs.end()) {
        bytes = it->second;
        path = p;
        return true;
    }
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        if (err) *err = "打不开模型文件: " + p;
        return false;
    }
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    if (b.empty()) {
        if (err) *err = "模型文件是空的: " + p;
        return false;
    }
    g_blobs[p] = b;
    bytes = std::move(b);
    path = p;
    return true;
}

bool NpuModel::init(const std::string& path, int core_mask, int input_fd, void* input_ptr,
                    std::size_t input_size, bool collect_perf, bool want_float,
                    std::string* err) {
    destroy();

    ModelBlob blob;
    if (!blob.load(path, err)) return false;

    uint32_t flags = RKNN_FLAG_PRIOR_HIGH;
    if (collect_perf) flags |= RKNN_FLAG_COLLECT_PERF_MASK;

    if (rknn_init(&ctx_, blob.bytes.data(), static_cast<uint32_t>(blob.bytes.size()),
                  flags, nullptr) != 0) {
        if (err) *err = "rknn_init 失败: " + path;
        ctx_ = 0;
        return false;
    }

    // ---- 绑核 ----
    core_mask_ = core_mask;
    if (core_mask > 0) {
        const int cret = rknn_set_core_mask(ctx_, static_cast<rknn_core_mask>(core_mask));
        if (cret != 0) {
            fprintf(stderr, "[nlb] rknn_set_core_mask(0x%x) 返回 %d\n", core_mask, cret);
        }
    }

    // ---- 版本 ----
    rknn_sdk_version ver;
    memset(&ver, 0, sizeof(ver));
    if (rknn_query(ctx_, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver)) == 0) {
        sdk_version_ = ver.api_version;
        drv_version_ = ver.drv_version;
    }

    // ---- 内存占用 ----
    rknn_mem_size ms;
    memset(&ms, 0, sizeof(ms));
    if (rknn_query(ctx_, RKNN_QUERY_MEM_SIZE, &ms, sizeof(ms)) == 0) {
        weight_bytes_ = ms.total_weight_size;
        internal_bytes_ = ms.total_internal_size;
        dma_bytes_ = ms.total_dma_allocated_size;
    }

    // ---- 输入属性 ----
    rknn_tensor_attr attr_logic;
    memset(&attr_logic, 0, sizeof(attr_logic));
    attr_logic.index = 0;
    if (rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, &attr_logic, sizeof(attr_logic)) != 0) {
        if (err) *err = "查询模型输入属性失败: " + path;
        destroy();
        return false;
    }

    derive_shape(attr_logic, &in_h_, &in_w_, &in_c_);
    const size_t need = static_cast<size_t>(in_h_) * in_w_ * in_c_;

    // ★ 优先用"原生 NHWC uint8"通路: pass_through = 1, 驱动不做任何布局转换。
    //   生产链路(npu_pool.cc:298-329)用的是同一条判断, 这样第 1 层的
    //   单帧耗时可以和生产的 `npu=` 直接比。
    rknn_tensor_attr nat;
    memset(&nat, 0, sizeof(nat));
    nat.index = 0;
    bool use_native = false;
    if (rknn_query(ctx_, RKNN_QUERY_NATIVE_NHWC_INPUT_ATTR, &nat, sizeof(nat)) == 0 &&
        nat.type == RKNN_TENSOR_UINT8 && nat.fmt == RKNN_TENSOR_NHWC &&
        nat.n_dims == 4 && nat.dims[3] == 3) {
        use_native = true;
    }

    // ---- 输入内存(零拷贝) ----
    if (input_fd >= 0) {
        // 外部 dma-buf: 这块物理内存同时是 RGA 的目标和 NPU 的输入。
        in_mem_ = rknn_create_mem_from_fd(ctx_, static_cast<int32_t>(input_fd), input_ptr,
                                          static_cast<uint32_t>(input_size), 0);
        if (!in_mem_) {
            if (err) *err = "rknn_create_mem_from_fd 失败";
            destroy();
            return false;
        }
        input_virt_ = input_ptr;
        input_fd_ = static_cast<int>(input_fd);
        owns_in_mem_ = true;
    } else {
        in_mem_ = rknn_create_mem(ctx_, static_cast<uint32_t>(need));
        if (!in_mem_) {
            if (err) *err = "rknn_create_mem 失败";
            destroy();
            return false;
        }
        input_virt_ = in_mem_->virt_addr;
        input_fd_ = in_mem_->fd;
        owns_in_mem_ = true;
    }

    // ---- 绑定 ----
    // ★★ 绑定属性必须【以运行时查出来的 attr 为底】再改 type/fmt/dims:
    //   rknn_set_io_mem 会拿 attr 里的 n_elems/size 去校验内存大小,
    //   自己 memset 出来的 attr 这两个字段是 0, 运行时会直接拒绝。
    rknn_tensor_attr bind;
    memset(&bind, 0, sizeof(bind));
    if (use_native) {
        bind = nat;
        bind.pass_through = 1;
        in_size_ = need;
    } else {
        bind = attr_logic;
        bind.pass_through = 0;
        bind.type = RKNN_TENSOR_UINT8;
        bind.fmt = RKNN_TENSOR_NHWC;
        bind.n_dims = 4;
        bind.dims[0] = 1;
        bind.dims[1] = in_h_;
        bind.dims[2] = in_w_;
        bind.dims[3] = in_c_;
        bind.n_elems = static_cast<uint32_t>(need);
        bind.size = static_cast<uint32_t>(need);
        in_size_ = need;
    }

    if (rknn_set_io_mem(ctx_, in_mem_, &bind) != 0) {
        if (err) *err = "rknn_set_io_mem 失败";
        destroy();
        return false;
    }

    // ---- 往输入缓冲写一次图案(之后热循环里不再写) ----
    // 外部 fd 的情况不能写: 那块内存是 RGA 的目标, 归 RGA 填。
    if (input_fd < 0 && input_virt_) {
        fill_pattern(static_cast<uint8_t*>(input_virt_), need, 0x9e3779b9u);
    }

    // ---- 输出缓冲预分配 ----
    rknn_input_output_num num;
    memset(&num, 0, sizeof(num));
    if (rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &num, sizeof(num)) != 0) {
        if (err) *err = "查询输入输出个数失败";
        destroy();
        return false;
    }
    outs_.resize(num.n_output);
    out_bufs_.resize(num.n_output);
    for (uint32_t i = 0; i < num.n_output; ++i) {
        rknn_tensor_attr oa;
        memset(&oa, 0, sizeof(oa));
        oa.index = i;
        if (rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &oa, sizeof(oa)) != 0) {
            if (err) *err = "查询输出属性失败";
            destroy();
            return false;
        }
        // ★ want_float = 1 时 runtime 写的是 float32, 缓冲要按
        //   n_elems * sizeof(float) 开(不是 attr.size —— 那是量化格式的字节数)。
        //   这和生产 npu_pool.cc 的做法一致。
        size_t bytes = static_cast<size_t>(oa.n_elems) * sizeof(float);
        if (bytes < oa.size) bytes = oa.size;
        if (bytes == 0) bytes = 4;
        out_bufs_[i].assign(bytes, 0);
        outs_[i].want_float = want_float ? 1 : 0;
        outs_[i].is_prealloc = 1;
        outs_[i].index = i;
        outs_[i].buf = out_bufs_[i].data();
        outs_[i].size = static_cast<uint32_t>(out_bufs_[i].size());
    }
    return true;
}

void NpuModel::destroy() {
    if (ctx_ == 0) return;
    if (in_mem_ && owns_in_mem_) {
        rknn_destroy_mem(ctx_, in_mem_);
    }
    in_mem_ = nullptr;
    outs_.clear();
    out_bufs_.clear();
    rknn_destroy(ctx_);
    ctx_ = 0;
}

int NpuModel::run() {
    const int rc = rknn_run(ctx_, nullptr);
    if (rc != 0) return rc;
    return rknn_outputs_get(ctx_, static_cast<uint32_t>(outs_.size()), outs_.data(), nullptr);
}

uint64_t NpuModel::query_run_us() const {
    rknn_perf_run pr;
    memset(&pr, 0, sizeof(pr));
    if (rknn_query(ctx_, RKNN_QUERY_PERF_RUN, &pr, sizeof(pr)) == 0 && pr.run_duration > 0) {
        last_run_us_ = static_cast<uint64_t>(pr.run_duration);
    }
    return last_run_us_;
}

std::string NpuModel::perf_detail() const {
    rknn_perf_detail pd;
    memset(&pd, 0, sizeof(pd));
    if (rknn_query(ctx_, RKNN_QUERY_PERF_DETAIL, &pd, sizeof(pd)) == 0 &&
        pd.perf_data && pd.data_len > 0) {
        return std::string(pd.perf_data, pd.data_len);
    }
    return std::string();
}

}  // namespace nlb
