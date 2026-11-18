// tb/npu_worker.cpp —— NPU 工作线程实现
#include "tb/npu_worker.h"

#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <rknn_api.h>

namespace tb {

namespace {

inline int64_t now_ns() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// 合成图案: 【只在启动时】往输入缓冲里写一次。
//
//   为什么要图案而不是全 0: 全 0 的输入在某些量化模型上会触发异常的中间值
//   (全 0 的 ReLU 会让整张特征图塌成 0), 跑出来的耗时可能不代表真实负载。
//   用一个斜向渐变 + 周期方块: 内容非平凡, 但取值温和, 不产生极端值。
//
//   ★ 热循环里绝不再调用它 —— 循环里对输入缓冲的写次数必须是 0。
void fill_pattern(uint8_t* p, size_t n, uint32_t seed) {
    const uint32_t s = seed ? seed : 1u;
    size_t i = 0;
    const size_t px = n / 3;
    for (; i + 3 <= n; i += 3) {
        const uint32_t k = static_cast<uint32_t>(i / 3);
        p[i + 0] = static_cast<uint8_t>((k + s) & 0xFF);
        p[i + 1] = static_cast<uint8_t>((k / 2 + s * 3) & 0xFF);
        p[i + 2] = static_cast<uint8_t>(((k / 64 + s) % 200) + 20);
    }
    for (; i < n; ++i) p[i] = static_cast<uint8_t>((i + s) & 0xFF);
    (void)px;
}

// 模型输入尺寸
struct InputShape {
    uint32_t h = 0;
    uint32_t w = 0;
    uint32_t c = 3;
    bool ok = false;
};

// 查输入张量属性。
//
// ★★ 这里返回的是【运行时自己填好的】attr, 不是我们自己拼的 ——
//   原因: rknn_set_io_mem 会拿 attr 里的 n_elems/size 去校验内存大小。
//   自己 memset 出来的 attr 这两个字段是 0, 运行时会直接报
//   "input memory size(N) < model input size(M)" 然后返回 -1。
//   主工程 npu_pool.cc 也是这么做的(bind = attr_logic; 再改 type/fmt)。
bool query_input_attr(rknn_context ctx, rknn_tensor_attr* out) {
    memset(out, 0, sizeof(*out));
    out->index = 0;
    return rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, out, sizeof(*out)) == 0;
}

// 从输入属性推出 H/W/C。
//
//   RKNN 报的 INPUT_ATTR 两种布局都见过:
//     NCHW: dims = [1, C, H, W]
//     NHWC: dims = [1, H, W, C]     (实测 ebike_int8_rk2332 就是这种)
//   万一布局不认识, 退一步: 找出等于 3 的那一维当通道, 剩下两维按大/小当 H/W。
InputShape derive_shape(const rknn_tensor_attr& a) {
    InputShape s;
    uint32_t d[4] = {0, 0, 0, 0};
    const uint32_t nd = a.n_dims < 4 ? a.n_dims : 4;
    for (uint32_t i = 0; i < nd; ++i) d[i] = a.dims[i];

    if (a.fmt == RKNN_TENSOR_NHWC) {
        s.h = d[1];
        s.w = d[2];
        s.c = d[3];
    } else {
        s.c = d[1];
        s.h = d[2];
        s.w = d[3];
    }

    if (s.c != 3) {
        int cidx = -1;
        for (uint32_t i = 0; i < nd; ++i) {
            if (d[i] == 3) { cidx = static_cast<int>(i); break; }
        }
        if (cidx >= 0) {
            s.c = 3;
            uint32_t others[4];
            int no = 0;
            for (uint32_t i = 0; i < nd; ++i) {
                if (static_cast<int>(i) == cidx) continue;
                if (no < 4) others[no++] = d[i];
            }
            if (no >= 2) {
                s.h = others[0] > others[1] ? others[0] : others[1];
                s.w = others[0] > others[1] ? others[1] : others[0];
            }
        }
    }
    s.ok = (s.h > 0 && s.w > 0 && s.c > 0);
    return s;
}

// NPU 侧内存(权重/内部缓冲/DMA)
void query_mem_size(rknn_context ctx, uint64_t* weight, uint64_t* internal, uint64_t* dma) {
    rknn_mem_size ms;
    memset(&ms, 0, sizeof(ms));
    if (rknn_query(ctx, RKNN_QUERY_MEM_SIZE, &ms, sizeof(ms)) == 0) {
        *weight = ms.total_weight_size;
        *internal = ms.total_internal_size;
        *dma = ms.total_dma_allocated_size;
    }
}

// 一个 ctx 的全部资源
struct CtxBundle {
    rknn_context ctx = 0;
    std::vector<rknn_tensor_mem*> mems;      // 输入缓冲(每路一块 或 一块)
    std::vector<std::vector<uint8_t>> outbuf; // 预分配的输出缓冲
    std::vector<rknn_output> outs;
    rknn_tensor_attr bind;                   // 输入绑定属性(重绑时复用)
    uint32_t in_h = 0, in_w = 0;
    int bound_channel = -1;
    bool ok = false;
};

// 在【本线程内】建一个上下文 + 绑定零拷贝输入缓冲 + 预分配输出缓冲
bool setup_ctx(const ModelBlob& model, int core_mask, uint32_t seed,
               int per_channel_count, bool multi_buffer,
               CtxBundle* b, std::string* err) {
    if (rknn_init(&b->ctx, const_cast<uint8_t*>(model.bytes.data()),
                  static_cast<uint32_t>(model.bytes.size()),
                  RKNN_FLAG_PRIOR_HIGH, nullptr) != 0) {
        *err = "rknn_init 失败: " + model.path;
        return false;
    }
    // mask 0 = RKNN_NPU_CORE_AUTO(驱动自己选核), 1/2/4 = 固定核, 7 = 三核全开
    const int cret = rknn_set_core_mask(b->ctx, static_cast<rknn_core_mask>(core_mask));
    if (cret != 0) {
        fprintf(stderr, "[bench]   rknn_set_core_mask(0x%x) 返回 %d\n", core_mask, cret);
    }

    rknn_tensor_attr ia;
    if (!query_input_attr(b->ctx, &ia)) {
        *err = "查询模型输入属性失败";
        return false;
    }

    const InputShape sh = derive_shape(ia);
    if (!sh.ok) {
        *err = "查询模型输入属性失败";
        return false;
    }
    b->in_h = sh.h;
    b->in_w = sh.w;

    const size_t in_bytes = static_cast<size_t>(sh.h) * sh.w * 3;
    const int n_buf = (multi_buffer && per_channel_count > 1) ? per_channel_count : 1;

    // ★ 绑定属性: 以运行时查出来的 attr 为底, 只改"我们喂的是什么格式"。
    //   n_elems / size 必须保持和模型一致 —— runtime 拿它们校验内存大小。
    rknn_tensor_attr bind = ia;
    bind.index = 0;
    bind.pass_through = 0;          // 由驱动做 uint8 NHWC -> 模型输入的转换
    bind.type = RKNN_TENSOR_UINT8;
    bind.fmt = RKNN_TENSOR_NHWC;
    bind.n_dims = 4;
    bind.dims[0] = 1;
    bind.dims[1] = sh.h;
    bind.dims[2] = sh.w;
    bind.dims[3] = 3;
    bind.n_elems = static_cast<uint32_t>(in_bytes);
    bind.size = static_cast<uint32_t>(in_bytes);
    b->bind = bind;

    for (int i = 0; i < n_buf; ++i) {
        rknn_tensor_mem* mem = rknn_create_mem(b->ctx, static_cast<uint32_t>(in_bytes));
        if (!mem) {
            *err = "rknn_create_mem 失败";
            return false;
        }
        // ★ 启动期写一次: 之后热循环里再也不碰这块缓冲
        fill_pattern(static_cast<uint8_t*>(mem->virt_addr), in_bytes,
                     seed + static_cast<uint32_t>(i) * 7919u);
        if (rknn_set_io_mem(b->ctx, mem, &bind) != 0) {
            *err = "rknn_set_io_mem 失败";
            return false;
        }
        b->mems.push_back(mem);
        b->bound_channel = (n_buf > 1) ? i : 0;
    }

    // ---- 输出缓冲预分配 ----
    // ★ 主工程 npu_pool.cc 用的是 is_prealloc = 0, 也就是每帧由 runtime 去
    //   malloc 一块输出缓冲。本测试台是"内存平线"红线, 所以改成 is_prealloc = 1:
    //   输出缓冲在启动时分配一次, 每帧直接写进去, 热循环里零 malloc。
    //   want_float = 0: 保持模型原生量化格式, 避免驱动额外做一次 float 转换(那也要分配)。
    rknn_input_output_num num;
    memset(&num, 0, sizeof(num));
    if (rknn_query(b->ctx, RKNN_QUERY_IN_OUT_NUM, &num, sizeof(num)) != 0) {
        *err = "查询输入输出个数失败";
        return false;
    }
    b->outbuf.resize(num.n_output);
    b->outs.resize(num.n_output);
    for (uint32_t i = 0; i < num.n_output; ++i) {
        rknn_tensor_attr oa;
        memset(&oa, 0, sizeof(oa));
        oa.index = i;
        if (rknn_query(b->ctx, RKNN_QUERY_OUTPUT_ATTR, &oa, sizeof(oa)) != 0) {
            *err = "查询输出属性失败";
            return false;
        }
        const size_t sz = oa.size ? oa.size : 1;
        b->outbuf[i].resize(sz);
        b->outs[i].want_float = 0;
        b->outs[i].is_prealloc = 1;
        b->outs[i].index = i;
        b->outs[i].buf = b->outbuf[i].data();
        b->outs[i].size = static_cast<uint32_t>(b->outbuf[i].size());
    }

    b->ok = true;
    return true;
}

// 在【本线程内】销毁
void teardown_ctx(CtxBundle* b) {
    if (b->ctx == 0) return;
    for (rknn_tensor_mem* m : b->mems) {
        if (m) rknn_destroy_mem(b->ctx, m);
    }
    b->mems.clear();
    rknn_destroy(b->ctx);
    b->ctx = 0;
    b->ok = false;
}

// 跑一次推理: run + outputs_get + outputs_release
// 返回 0 表示成功。
inline int run_once(rknn_context ctx, uint32_t n_out, rknn_output* outs) {
    int rc = rknn_run(ctx, nullptr);
    if (rc != 0) return rc;
    rc = rknn_outputs_get(ctx, n_out, outs, nullptr);
    if (rc != 0) return rc;
    rknn_outputs_release(ctx, n_out, outs);
    return 0;
}

// 问驱动"这次推理真正占了核多久"(微秒)。
//
//   RKNN_QUERY_PERF_RUN 报的是硬件侧的推理耗时, 不含在 NPU 队列里等调度的时间。
//   ★ 这就是"核占用率"该用的口径: 同一颗核上挂 4 个上下文时, 它们的等待时间
//     会互相重叠, 用墙钟算出来的核占用会超过 100%(那是排队, 不是利用率)。
inline uint64_t query_npu_us(rknn_context ctx) {
    rknn_perf_run pr;
    memset(&pr, 0, sizeof(pr));
    if (rknn_query(ctx, RKNN_QUERY_PERF_RUN, &pr, sizeof(pr)) != 0) return 0;
    return pr.run_duration > 0 ? static_cast<uint64_t>(pr.run_duration) : 0;
}

}  // namespace

NpuWorker::NpuWorker(int idx, int core_mask, int core_mask2,
                     const ModelBlob* m1, const ModelBlob* m2,
                     LatencyRecorder* rec, const Config* cfg, FrameQueue* queue)
    : idx_(idx),
      core_mask1_(core_mask),
      core_mask2_(core_mask2),
      m1_(m1),
      m2_(m2),
      rec_(rec),
      cfg_(cfg),
      queue_(queue) {
    for (int i = 0; i < 16; ++i) chan_[i].store(0, std::memory_order_relaxed);
}

NpuWorker::~NpuWorker() { join(); }

uint64_t NpuWorker::chan_frames(int c) const {
    if (c < 0 || c >= 16) return 0;
    return chan_[c].load(std::memory_order_relaxed);
}

void NpuWorker::reset_counters() {
    frames_.store(0, std::memory_order_relaxed);
    errors_.store(0, std::memory_order_relaxed);
    busy1_us_.store(0, std::memory_order_relaxed);
    busy2_us_.store(0, std::memory_order_relaxed);
    npu1_us_.store(0, std::memory_order_relaxed);
    npu2_us_.store(0, std::memory_order_relaxed);
    for (int i = 0; i < 16; ++i) chan_[i].store(0, std::memory_order_relaxed);
}

void NpuWorker::apply_affinity() const {
    if (cpus_.empty()) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int c : cpus_) {
        if (c >= 0 && c < CPU_SETSIZE) CPU_SET(c, &set);
    }
    const int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    if (rc != 0) {
        fprintf(stderr, "[bench] worker %d: pthread_setaffinity_np 失败: %s\n",
                idx_, strerror(rc));
    }
}

void NpuWorker::start() {
    th_ = std::thread(&NpuWorker::loop, this);
}

void NpuWorker::stop() {
    stop_.store(true, std::memory_order_relaxed);
}

void NpuWorker::join() {
    if (th_.joinable()) th_.join();
}

void NpuWorker::loop() {
    apply_affinity();

    const uint32_t seed = 0x9e3779b9u ^ (static_cast<uint32_t>(idx_) * 2654435761u);
    const int channels = cfg_ ? cfg_->channels : 1;
    const bool multi = cfg_ ? cfg_->per_channel_buffers : false;

    // ★★ ctx 的创建 / 使用 / 销毁全部在这个线程里(见头文件里的说明) ★★
    CtxBundle b1;
    if (!setup_ctx(*m1_, core_mask1_, seed, channels, multi, &b1, &err_)) {
        teardown_ctx(&b1);
        failed_.store(true, std::memory_order_release);
        return;
    }

    CtxBundle b2;
    if (m2_ != nullptr) {
        if (!setup_ctx(*m2_, core_mask2_, seed ^ 0x5bf03635u, channels, multi, &b2, &err_)) {
            teardown_ctx(&b1);
            teardown_ctx(&b2);
            failed_.store(true, std::memory_order_release);
            return;
        }
    }

    in_h_ = b1.in_h;
    in_w_ = b1.in_w;
    {
        uint64_t w = 0, i = 0, d = 0;
        query_mem_size(b1.ctx, &w, &i, &d);
        npu_weight_bytes_ = w;
        npu_internal_bytes_ = i;
        npu_dma_bytes_ = d;
        if (m2_ != nullptr) {
            uint64_t w2 = 0, i2 = 0, d2 = 0;
            query_mem_size(b2.ctx, &w2, &i2, &d2);
            npu_weight_bytes_ += w2;
            npu_internal_bytes_ += i2;
            npu_dma_bytes_ += d2;
        }
    }

    ready_.store(true, std::memory_order_release);

    // ---------------- 主循环 ----------------
    while (!stop_.load(std::memory_order_relaxed)) {
        FrameQueue::Item it;
        if (!queue_->pop(&it)) break;          // 队列已 stop
        if (stop_.load(std::memory_order_relaxed)) break;

        // 多缓冲模式: 帧的通道和当前绑定的不一致 -> 重绑一次输入缓冲
        if (b1.mems.size() > 1) {
            const int ch = static_cast<int>(it.channel) % static_cast<int>(b1.mems.size());
            if (ch != b1.bound_channel) {
                if (rknn_set_io_mem(b1.ctx, b1.mems[ch], &b1.bind) == 0) {
                    b1.bound_channel = ch;
                }
            }
        }

        const int64_t t_a = now_ns();
        int rc = run_once(b1.ctx, static_cast<uint32_t>(b1.outs.size()), b1.outs.data());
        const int64_t t_b = now_ns();
        // ★ 问驱动要真实核耗时(放在 t_b 之后, 不污染上面的墙钟测量)
        const uint64_t npu1_us = query_npu_us(b1.ctx);

        int64_t t_c = t_b;
        uint64_t npu2_us = 0;
        if (m2_ != nullptr) {
            const int rc2 = run_once(b2.ctx, static_cast<uint32_t>(b2.outs.size()),
                                     b2.outs.data());
            if (rc2 != 0 && rc == 0) rc = rc2;
            t_c = now_ns();
            npu2_us = query_npu_us(b2.ctx);
        }

        if (rc != 0) {
            errors_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        frames_.fetch_add(1, std::memory_order_relaxed);
        // 累加【微秒】(t_* 是纳秒, 别忘了换算)
        busy1_us_.fetch_add(static_cast<uint64_t>((t_b - t_a) / 1000),
                            std::memory_order_relaxed);
        npu1_us_.fetch_add(npu1_us, std::memory_order_relaxed);
        if (m2_ != nullptr) {
            busy2_us_.fetch_add(static_cast<uint64_t>((t_c - t_b) / 1000),
                                std::memory_order_relaxed);
            npu2_us_.fetch_add(npu2_us, std::memory_order_relaxed);
        }
        if (static_cast<int>(it.channel) < 16) {
            chan_[it.channel].fetch_add(1, std::memory_order_relaxed);
        }

        // 延迟采样: 只记入队时刻在测量窗口内的帧(切掉预热期)
        if (it.t_enqueue_ns >= rec_->gate()) {
            rec_->add(kLatE2E, static_cast<uint32_t>((t_c - it.t_enqueue_ns) / 1000));
            rec_->add(kLatModel1, static_cast<uint32_t>((t_b - t_a) / 1000));
            if (m2_ != nullptr) {
                rec_->add(kLatModel2, static_cast<uint32_t>((t_c - t_b) / 1000));
            }
        }
    }

    teardown_ctx(&b1);
    teardown_ctx(&b2);
}

}  // namespace tb
