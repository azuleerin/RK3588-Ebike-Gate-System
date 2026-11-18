// rk_base/npu_pipe.cc —— NPU 多实例并行检测的实现
#include "rk/npu_pipe.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "dmabuf.h"
#include "rk/core_mask.h"

namespace rk {
namespace {

double now_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

bool NpuPipe::init(const std::string& model_path, int model_w, int model_h, int n_instance,
                   const int* core_masks, float conf, float nms, int n_output) {
    n_instance_ = n_instance;
    // ★ 把每个实例要绑的核记下来。
    //   以前这里是硬编码的 "idx==0 ? core0 : core1" —— 传进来的 core_masks
    //   参数【根本没被用过】。于是 --npu-pipe 3 时第三个实例会【和第二个
    //   抢同一个核】, 三个核只用了两个 —— 测出来的"三核并行"其实是假的。
    //   现在按参数逐实例绑核(1=CORE0, 2=CORE1, 4=CORE2, 0=自动)。
    core_masks_.clear();
    for (int i = 0; i < n_instance; ++i) {
        const int cm = (core_masks && core_masks[i] != 0) ? core_masks[i]
                                                           : (1 << (i % 3));
        core_masks_.push_back(cm);
    }
    model_w_ = model_w;
    model_h_ = model_h;
    conf_ = conf;
    nms_ = nms;
    n_output_ = n_output > 0 ? n_output : 1;
    // ★ 张量大小 vs 实际分配大小 —— 以前只有一个数, 于是并行路径的
    //   RGA 目标缓冲【一字节余量都没有】:
    //     RGA 缩放比例不是整数倍时(1920x1080 -> 640x640, sy=1.6875),
    //     行缓冲里没写完的 1~2 行会被刷进目标缓冲 = 越界写 1~2 行。
    //     main.cc 的串行路径一直有 16 行余量(kRgaDstPadRows), 这里没有 ——
    //     同一类堆损坏, 在并行模式下换了个面目出现。
    //   现在: 分配按 buf_alloc_(带余量), 交给 rknn 的仍然是 buf_size_(张量)。
    buf_size_ = static_cast<size_t>(model_w) * model_h * 3;
    buf_alloc_ = rk::rga_padded_size(static_cast<size_t>(model_w) * 3,
                                      static_cast<size_t>(model_h));

    // ---- 读模型 ----
    FILE* f = fopen(model_path.c_str(), "rb");
    if (!f) { fprintf(stderr, "[NPU-Pipe] 打不开模型 %s\n", model_path.c_str()); return false; }
    fseek(f, 0, SEEK_END);
    const size_t sz = static_cast<size_t>(ftell(f));
    fseek(f, 0, SEEK_SET);
    model_data_.resize(sz);
    if (fread(model_data_.data(), 1, sz, f) != sz) { fclose(f); return false; }
    fclose(f);

    // ---- 每个实例一块输入缓冲(RGA 的目标) ----
    for (int i = 0; i < n_instance_; i++) {
        const int fd = rk::dma_buf_alloc(buf_alloc_, true);
        if (fd < 0) { fprintf(stderr, "[NPU-Pipe] dma-buf 分配失败\n"); return false; }
        void* p = rk::dma_buf_map(fd, buf_alloc_);
        if (p == MAP_FAILED) { ::close(fd); return false; }
        fds_.push_back(fd);
        ptrs_.push_back(p);
        ctxs_.push_back(0);
        slots_.push_back(std::unique_ptr<Slot>(new Slot()));
    }

    // ---- 起工作线程(上下文在各自线程里建) ----
    for (int i = 0; i < n_instance_; i++) {
        workers_.emplace_back(&NpuPipe::worker_loop, this, i);
    }
    // 等所有实例就绪
    //
    // ★ 超时给足: 板子上还有别的 NPU 任务在跑时, rknn_init / rknn_create_mem_from_fd
    //   会排在驱动后面, 可能要好几秒。以前的 5 秒(500×10ms)一超时就把整个
    //   并行管线【静默降级成串行】, 表现是"--npu-pipe 3 一点没变快"。
    //   现在给 30 秒, 并且把实际耗时打出来, 免得再被静默降级坑一次。
    const int kReadyWaitMs = 30000;
    const int kReadySteps = kReadyWaitMs / 10;
    for (int i = 0; i < n_instance_; i++) {
        for (int k = 0; k < kReadySteps; k++) {
            std::lock_guard<std::mutex> lk(slots_[i]->m);
            if (slots_[i]->ready) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    for (int i = 0; i < n_instance_; i++) {
        std::lock_guard<std::mutex> lk(slots_[i]->m);
        if (!slots_[i]->ready) {
            fprintf(stderr,
                    "[NPU-Pipe] 实例 %d 启动失败(等 %d ms 还没就绪 —— "
                    "多半是 NPU 被别的任务占着, rknn_init 排在后面了)\n",
                    i, kReadyWaitMs);
            return false;
        }
    }
    fprintf(stderr, "[NPU-Pipe] 就绪: %d 个实例, 每个一个核, 输入 %dx%d "
                    "(缓冲 %zu 字节 = 张量 %zu + %d 行余量)\n",
            n_instance_, model_w, model_h, buf_alloc_, buf_size_, rk::kRgaDstPadRows);
    return true;
}

void NpuPipe::worker_loop(int idx) {
    Slot& s = *slots_[idx];
    rknn_context ctx = 0;
    const int ret = rknn_init(&ctx, model_data_.data(),
                               static_cast<uint32_t>(model_data_.size()),
                               RKNN_FLAG_PRIOR_HIGH, nullptr);
    if (ret != 0) { fprintf(stderr, "[NPU-Pipe] rknn_init 失败: %d\n", ret); return; }
    ctxs_[idx] = ctx;

    // 绑核(按 init 传进来的掩码, 不再是硬编码的"0->core0, 其它->core1")
    const int cm = (idx < static_cast<int>(core_masks_.size())) ? core_masks_[idx] : (1 << (idx % 3));
    const int safe_cm = rk::sanitize_core_mask(cm, "NpuPipe 检测实例");
    const int cret = rknn_set_core_mask(ctx, static_cast<rknn_core_mask>(safe_cm));
    fprintf(stderr, "[NPU-Pipe] 实例 %d core_mask=0x%x -> %d\n", idx, cm, cret);

    // 查属性
    rknn_tensor_attr attr_logic;
    memset(&attr_logic, 0, sizeof(attr_logic));
    attr_logic.index = 0;
    rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &attr_logic, sizeof(attr_logic));
    rknn_tensor_attr nat;
    memset(&nat, 0, sizeof(nat));
    nat.index = 0;
    bool use_native = false;
    if (rknn_query(ctx, RKNN_QUERY_NATIVE_NHWC_INPUT_ATTR, &nat, sizeof(nat)) == 0 &&
        nat.type == RKNN_TENSOR_UINT8 && nat.fmt == RKNN_TENSOR_NHWC &&
        nat.n_dims == 4 && nat.dims[3] == 3) {
        use_native = true;
    }
    std::vector<rknn_tensor_attr> out_attrs(n_output_);
    for (uint32_t i = 0; i < static_cast<uint32_t>(n_output_); i++) {
        memset(&out_attrs[i], 0, sizeof(rknn_tensor_attr));
        out_attrs[i].index = i;
        rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &out_attrs[i], sizeof(out_attrs[i]));
    }

    // 绑定输入内存(零拷贝)
    rknn_tensor_mem* mem = rknn_create_mem_from_fd(ctx, fds_[idx], ptrs_[idx], buf_size_, 0);
    if (!mem) { fprintf(stderr, "[NPU-Pipe] rknn_create_mem_from_fd 失败\n"); return; }
    rknn_tensor_attr bind;
    memset(&bind, 0, sizeof(bind));
    if (use_native) {
        bind = nat;
        bind.pass_through = 1;
    } else {
        bind = attr_logic;
        bind.pass_through = 0;
        bind.type = RKNN_TENSOR_UINT8;
        bind.fmt = RKNN_TENSOR_NHWC;
        bind.n_dims = 4;
        bind.dims[0] = 1;
        bind.dims[1] = static_cast<uint32_t>(model_h_);
        bind.dims[2] = static_cast<uint32_t>(model_w_);
        bind.dims[3] = 3;
    }
    if (rknn_set_io_mem(ctx, mem, &bind) != 0) {
        fprintf(stderr, "[NPU-Pipe] rknn_set_io_mem 失败\n");
        return;
    }

    // ---- 零拷贝审计 ------------------------------------------------------
    // 这一行是"到底有没有零拷贝"的【运行时证据】, 不是口头声明:
    //   RGA 的目标 fd  = 主线程 rga_nv12_to_rgb888() 写进去的那块 dma-buf
    //   RKNN 输入 fd  = rknn_create_mem_from_fd() 绑给模型的那块 dma-buf
    // 两个 fd 是同一个数字 -> NPU 直接读 RGA 写出来的那块物理内存,
    // 中间【没有】任何一次 CPU memcpy。
    // 如果哪天这里两个 fd 不一样了, 说明链路退化成"先落 CPU 内存再拷给 NPU"。
    fprintf(stderr,
            "[零拷贝审计] 检测[pipe 实例 %d]: RGA 目标 fd=%d / RKNN 输入 fd=%d -> %s\n",
            idx, fds_[idx], fds_[idx],
            "同一块物理内存 (零 CPU 拷贝)");

    {
        std::lock_guard<std::mutex> lk(s.m);
        s.ready = true;
    }
    s.cv.notify_all();

    for (;;) {
        NpuPipeJob job;
        uint64_t seq = 0;
        {
            std::unique_lock<std::mutex> lk(s.m);
            s.cv.wait(lk, [&] { return s.have || s.stop; });
            if (s.stop) break;
            job = s.desc;
            seq = s.seq;
            s.have = false;
            s.busy = true;
        }

        const double t0 = now_ms();
        rknn_run(ctx, nullptr);
        rknn_output outs[16];
        memset(outs, 0, sizeof(outs));
        for (uint32_t i = 0; i < static_cast<uint32_t>(n_output_); i++) {
            outs[i].want_float = 1;
            outs[i].is_prealloc = 0;
            outs[i].index = i;
        }
        rknn_outputs_get(ctx, static_cast<uint32_t>(n_output_), outs, nullptr);

        // ---- 后处理(在 worker 里做: 输出缓冲属于这个线程的上下文) ----
        std::vector<OutTensor> ts(n_output_);
        for (size_t i = 0; i < ts.size(); i++) {
            ts[i].data    = outs[i].buf;
            ts[i].is_int8 = false;
            ts[i].zp      = out_attrs[i].zp;
            ts[i].scale   = out_attrs[i].scale;
            ts[i].n_elems = out_attrs[i].n_elems;
            ts[i].n_dims  = out_attrs[i].n_dims;
            for (uint32_t k = 0; k < out_attrs[i].n_dims && k < 4; k++)
                ts[i].dims[k] = out_attrs[i].dims[k];
        }
        BoxMapper mapper;
        mapper.img_w = job.img_w;
        mapper.img_h = job.img_h;
        if (job.letterbox) {
            mapper.mode = BoxMapper::LETTERBOX;
            mapper.inv_scale = job.scale;
            mapper.pad_x = job.pad_x;
            mapper.pad_y = job.pad_y;
        } else {
            mapper.mode = BoxMapper::STRETCH;
            mapper.scale_x = static_cast<float>(job.img_w) / model_w_;
            mapper.scale_y = static_cast<float>(job.img_h) / model_h_;
        }
        std::vector<YoloBox> boxes =
            yolov8_postprocess(ts.data(), static_cast<int>(ts.size()), conf_, nms_, mapper);
        rknn_outputs_release(ctx, static_cast<uint32_t>(n_output_), outs);
        const double dt = now_ms() - t0;

        {
            std::lock_guard<std::mutex> lk(s.m);
            s.infer_ms = dt;
            s.busy = false;
            s.seq_done = seq;
        }
        {
            // ★ 结果进全局队列(带序号): 主线程按【序号顺序】取, 保证帧序不乱
            //   (两个 worker 并行, 完成顺序可能乱, 但跟踪器必须按原始帧序吃结果)
            std::lock_guard<std::mutex> lk(res_m_);
            Result r;
            r.seq = seq;
            r.boxes = std::move(boxes);
            r.infer_ms = dt;          // 本实例本次的真实推理耗时
            r.ts_ms = job.ts_ms;      // 这一帧被采集到的时刻(给跟踪/测速用)
            results_.push_back(std::move(r));
            std::sort(results_.begin(), results_.end(),
                      [](const Result& a, const Result& b) { return a.seq < b.seq; });
        }
        res_cv_.notify_all();
        s.cv.notify_all();
    }

    rknn_destroy_mem(ctx, mem);
}

int  NpuPipe::fd(int i) const { return (i < static_cast<int>(fds_.size())) ? fds_[i] : -1; }
void* NpuPipe::ptr(int i) const { return (i < static_cast<int>(ptrs_.size())) ? ptrs_[i] : nullptr; }

void NpuPipe::submit(int instance, const NpuPipeJob& job) {
    if (instance < 0 || instance >= n_instance_) return;
    Slot& s = *slots_[instance];
    uint64_t seq;
    {
        std::unique_lock<std::mutex> lk(s.m);
        // 等这块缓冲上一次的推理结束(否则 RGA 覆写会打断正在跑的推理)
        s.cv.wait(lk, [&] { return !s.busy; });
        s.desc = job;
        {
            std::lock_guard<std::mutex> lk2(res_m_);
            seq = next_seq_++;
        }
        s.seq = seq;
        s.have = true;
    }
    s.cv.notify_all();
}

bool NpuPipe::acquire(std::vector<YoloBox>* out, double* last_infer_ms,
                      int64_t* out_ts_ms) {
    for (;;) {
        std::unique_lock<std::mutex> lk(res_m_);
        if (!results_.empty()) {
            const Result r = results_.front();     // 最小 seq = 最早的帧
            results_.erase(results_.begin());
            lk.unlock();
            if (out) *out = r.boxes;
            if (last_infer_ms) *last_infer_ms = r.infer_ms;
            if (out_ts_ms) *out_ts_ms = r.ts_ms;
            return true;
        }
        if (stop_) return false;
        res_cv_.wait_for(lk, std::chrono::milliseconds(100));
    }
}

bool NpuPipe::try_acquire(std::vector<YoloBox>* out, double* last_infer_ms,
                          int64_t* out_ts_ms) {
    std::unique_lock<std::mutex> lk(res_m_);
    if (results_.empty()) return false;
    const Result r = results_.front();
    results_.erase(results_.begin());
    lk.unlock();
    if (out) *out = r.boxes;
    if (last_infer_ms) *last_infer_ms = r.infer_ms;
    if (out_ts_ms) *out_ts_ms = r.ts_ms;
    return true;
}

bool NpuPipe::wait_slot_free(int instance, int timeout_ms) {
    if (instance < 0 || instance >= n_instance_) return true;
    Slot& s = *slots_[instance];
    std::unique_lock<std::mutex> lk(s.m);
    return s.cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                         [&] { return !s.busy || s.stop; });
}

void NpuPipe::stop() {
    if (stop_) return;
    stop_ = true;
    for (auto& s : slots_) {
        {
            std::lock_guard<std::mutex> lk(s->m);
            s->stop = true;
        }
        s->cv.notify_all();
    }
    for (auto& t : workers_) if (t.joinable()) t.join();
    workers_.clear();
    for (auto& c : ctxs_) if (c) rknn_destroy(c);
    ctxs_.clear();
    for (size_t i = 0; i < fds_.size(); i++) {
        if (ptrs_[i]) rk::dma_buf_unmap(ptrs_[i], buf_alloc_);
        if (fds_[i] >= 0) ::close(fds_[i]);
    }
    fds_.clear();
    ptrs_.clear();
}

}  // namespace rk
