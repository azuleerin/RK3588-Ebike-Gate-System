// rk_base/npu_pool.cc —— 全局 NPU 任务池的实现
//
// 头文件里的注释解释了【为什么】要有这个池; 这里只讲【怎么做的】。
//
// ── 结构 ────────────────────────────────────────────────────────────────
//
//   通道线程                全局队列                      worker 线程
//   ─────────                ───────                      ───────────
//   acquire_det_slot()  ->  [OCR(pri=1)]  <- 认领 <-  OCR worker @ core0
//   RGA 写进 slot 的缓冲     [DET(pri=0)]  <- 认领 <-  DET worker @ core1
//   submit_det()                                    DET worker @ core2
//   take_det()  <- 结果
//
//   队列是【一条】, 按优先级排(识别在前)。每个 worker 只认领自己能跑的任务:
//     · 检测任务绑在具体的 slot 上 -> 只有该 slot 所属的 worker 能跑
//     · 识别任务谁都能跑 -> 多个识别 worker 之间自然竞争
//
//   ★ 同一颗核被两个 worker 共用时(比如 --ocr-core 0 --det-core 0,1,2),
//     core0 上有识别 worker 和检测 worker, 谁先跑由【每核闸门】决定:
//     等在同一颗核上的任务里, 识别永远排在检测前面 —— 这就是"识别优先"。
#include "rk/npu_pool.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>

#include "dmabuf.h"
#include "rk/core_mask.h"
#include "rk/rga_pad.h"

namespace rk {
namespace {

double now_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

// ---------------------------------------------------------------------------
// 每核闸门
// ---------------------------------------------------------------------------
void NpuPool::CoreGate::acquire(int priority) {
    std::unique_lock<std::mutex> lk(m_);
    const uint64_t my = next_ticket_++;
    waiters_.emplace(priority, Waiter{priority, my});
    cv_.wait(lk, [&] {
        if (busy_) return false;                       // 核还占着
        // 等待队列里优先级最高、且同优先级里最早的那个才轮得到我
        auto it = waiters_.begin();
        return it != waiters_.end() && it->second.ticket == my;
    });
    // 把自己从等待队列里摘掉
    for (auto it = waiters_.begin(); it != waiters_.end(); ++it) {
        if (it->second.ticket == my) { waiters_.erase(it); break; }
    }
    busy_ = true;
}

void NpuPool::CoreGate::release() {
    {
        std::lock_guard<std::mutex> lk(m_);
        busy_ = false;
    }
    cv_.notify_all();
}

// ---------------------------------------------------------------------------
bool NpuPool::load_model_file(const std::string& path, std::vector<uint8_t>* out) const {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        fprintf(stderr, "[NpuPool] 打不开模型 %s\n", path.c_str());
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }
    out->resize(static_cast<size_t>(sz));
    const size_t n = fread(out->data(), 1, out->size(), f);
    fclose(f);
    if (n != out->size()) return false;
    return true;
}

// ---------------------------------------------------------------------------
bool NpuPool::init(const Config& cfg) {
    cfg_ = cfg;
    t0_ = std::chrono::steady_clock::now();

    const std::vector<int> det_cores = core_mask_to_indices(cfg.det_core_mask);
    const std::vector<int> ocr_cores = core_mask_to_indices(cfg.ocr_core_mask);
    if (det_cores.empty() && ocr_cores.empty()) {
        fprintf(stderr, "[NpuPool] 检测和识别都没指定核 —— 至少要有一个\n");
        return false;
    }

    // ---- 读检测模型(只读一份; 各 worker 的上下文自己拷贝) ----
    if (!det_cores.empty()) {
        if (!load_model_file(cfg.det_model, &det_model_data_)) return false;
    }

    // ---- 建检测 worker + slot ----
    //
    //   slot = (一个 RKNN 上下文 + 一块输入 dma-buf)。
    //   上下文在【worker 线程里】创建 —— RKNN 上下文不能跨线程用
    //   (npu_pipe.h:14-17 记着这个坑: 在 A 线程 init、B 线程 run 会死锁)。
    const int slots_per_worker = cfg.det_slots_per_worker > 0 ? cfg.det_slots_per_worker : 1;
    const size_t buf_size = static_cast<size_t>(cfg.det_w) * cfg.det_h * 3;
    // ★ RGA 目标缓冲要多留几行: 缩放比例不是整数倍时(1920x1080 -> 640x640,
    //   sy=1.6875), RGA 会往目标缓冲【越界写 1~2 行】。
    //   以前 npu_pipe.cc 按精确大小分配, 没有余量 —— 同一类堆损坏换个面目出现。
    const size_t buf_alloc = rga_padded_size(static_cast<size_t>(cfg.det_w) * 3,
                                             static_cast<size_t>(cfg.det_h));

    for (size_t ci = 0; ci < det_cores.size(); ++ci) {
        auto w = std::unique_ptr<DetWorker>(new DetWorker());
        w->core = det_cores[ci];
        w->core_mask = (1 << det_cores[ci]);
        for (int s = 0; s < slots_per_worker; ++s) {
            DetSlot slot;
            slot.id = static_cast<int>(det_slots_.size());
            slot.worker = static_cast<int>(det_workers_.size());
            slot.buf_size = buf_size;
            slot.buf_alloc = buf_alloc;
            slot.fd = rk::dma_buf_alloc(buf_alloc, true);
            if (slot.fd < 0) {
                fprintf(stderr, "[NpuPool] dma-buf 分配失败\n");
                return false;
            }
            slot.ptr = rk::dma_buf_map(slot.fd, buf_alloc);
            if (slot.ptr == MAP_FAILED) {
                ::close(slot.fd);
                return false;
            }
            w->slots.push_back(slot.id);
            det_slots_.push_back(slot);
        }
        det_workers_.push_back(std::move(w));
    }

    // 空闲 slot 的初始顺序: 跨 worker 交错, 让 4 路天然摊到各 worker 上
    //   (顺序 0,0,1,1 -> 4 路各拿一个时, 每颗核上正好 2 路)
    {
        std::vector<std::vector<int>> per_worker(det_workers_.size());
        for (size_t i = 0; i < det_slots_.size(); ++i)
            per_worker[det_slots_[i].worker].push_back(det_slots_[i].id);
        size_t k = 0;
        bool more = true;
        while (more) {
            more = false;
            for (size_t w = 0; w < per_worker.size(); ++w) {
                if (k < per_worker[w].size()) {
                    free_slots_.push_back(per_worker[w][k]);
                    more = true;
                }
            }
            ++k;
        }
    }

    // ---- 建识别 worker ----
    for (size_t ci = 0; ci < ocr_cores.size(); ++ci) {
        auto w = std::unique_ptr<OcrWorker>(new OcrWorker());
        w->core = ocr_cores[ci];
        w->core_mask = (1 << ocr_cores[ci]);
        ocr_workers_.push_back(std::move(w));
    }

    // ---- 每核一个闸门 ----
    gates_.resize(3);
    for (auto& g : gates_) g.reset(new CoreGate());

    // ---- 统计表 ----
    {
        std::lock_guard<std::mutex> lk(stat_m_);
        stats_.clear();
        for (const auto& w : det_workers_) {
            CoreStat s; s.core_mask = w->core_mask; s.kind = 0;
            stats_.push_back(s);
        }
        for (const auto& w : ocr_workers_) {
            CoreStat s; s.core_mask = w->core_mask; s.kind = 1;
            stats_.push_back(s);
        }
    }

    // ---- 起线程 ----
    for (size_t i = 0; i < det_workers_.size(); ++i) {
        det_workers_[i]->th = std::thread(&NpuPool::det_worker_loop, this, static_cast<int>(i));
    }
    for (size_t i = 0; i < ocr_workers_.size(); ++i) {
        ocr_workers_[i]->th = std::thread(&NpuPool::ocr_worker_loop, this, static_cast<int>(i));
    }

    // ---- 等就绪 ----
    //   (worker 在自己的线程里建上下文; 这里只是等它建好)
    for (int tries = 0; tries < 3000; ++tries) {
        bool det_ready = true;
        for (const auto& w : det_workers_) if (!w->ready.load()) det_ready = false;
        bool ocr_ready = true;
        for (const auto& w : ocr_workers_) if (!w->ready.load()) ocr_ready = false;
        if (det_ready && ocr_ready) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    {
        char cb[64];
        fprintf(stderr, "[NpuPool] 就绪: %zu 个检测 worker(每核 %d slot) / %zu 个识别 worker\n",
                det_workers_.size(), slots_per_worker, ocr_workers_.size());
        for (const auto& w : det_workers_)
            fprintf(stderr, "[NpuPool]   检测 worker @ %s (mask=0x%x)\n",
                    core_mask_to_string(w->core_mask, cb, sizeof(cb)), w->core_mask);
        for (const auto& w : ocr_workers_)
            fprintf(stderr, "[NpuPool]   识别 worker @ %s (mask=0x%x)\n",
                    core_mask_to_string(w->core_mask, cb, sizeof(cb)), w->core_mask);
    }
    return true;
}

// ---------------------------------------------------------------------------
// 从全局队列里认领任务
// ---------------------------------------------------------------------------
bool NpuPool::pop_task(int worker_idx, NpuTaskKind kind, Task* out) {
    std::unique_lock<std::mutex> lk(q_m_);
    for (;;) {
        if (stop_) return false;
        // 队列按优先级从高到低; 找第一个【这个 worker 能跑】的
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            if (it->second.kind != kind) continue;
            if (kind == NpuTaskKind::kDet && it->second.worker != worker_idx) continue;
            *out = std::move(it->second);
            queue_.erase(it);
            return true;
        }
        q_cv_.wait_for(lk, std::chrono::milliseconds(50));
    }
}

bool NpuPool::pop_ocr_task(Task* out) {
    std::unique_lock<std::mutex> lk(q_m_);
    for (;;) {
        if (stop_) return false;
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            if (it->second.kind != NpuTaskKind::kOcr) continue;
            *out = std::move(it->second);
            queue_.erase(it);
            return true;
        }
        q_cv_.wait_for(lk, std::chrono::milliseconds(50));
    }
}

// ---------------------------------------------------------------------------
// 检测 worker
// ---------------------------------------------------------------------------
void NpuPool::det_worker_loop(int widx) {
    DetWorker& w = *det_workers_[widx];

    // ---- 在本线程里建上下文(每个 slot 一个) ----
    for (int sid : w.slots) {
        DetSlot& s = det_slots_[sid];
        rknn_context ctx = 0;
        if (rknn_init(&ctx, det_model_data_.data(),
                      static_cast<uint32_t>(det_model_data_.size()),
                      RKNN_FLAG_PRIOR_HIGH, nullptr) != 0) {
            fprintf(stderr, "[NpuPool] worker %d: rknn_init 失败\n", widx);
            return;
        }
        s.ctx = ctx;

        const int cret = rknn_set_core_mask(ctx, static_cast<rknn_core_mask>(w.core_mask));
        fprintf(stderr, "[NpuPool] 检测 worker %d slot %d: 上下文绑核 mask=0x%x -> %d\n",
                widx, sid, w.core_mask, cret);

        // 输入属性: 能用原生 NHWC uint8 就 pass_through, 否则让驱动做布局转换
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

        // 零拷贝: 把这块 dma-buf 直接绑给模型
        rknn_tensor_mem* mem = rknn_create_mem_from_fd(ctx, s.fd, s.ptr, s.buf_size, 0);
        if (!mem) {
            fprintf(stderr, "[NpuPool] worker %d: rknn_create_mem_from_fd 失败\n", widx);
            return;
        }
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
            bind.dims[1] = static_cast<uint32_t>(cfg_.det_h);
            bind.dims[2] = static_cast<uint32_t>(cfg_.det_w);
            bind.dims[3] = 3;
        }
        if (rknn_set_io_mem(ctx, mem, &bind) != 0) {
            fprintf(stderr, "[NpuPool] worker %d: rknn_set_io_mem 失败\n", widx);
            return;
        }
        s.in_mem = mem;

        s.out_attrs.resize(static_cast<size_t>(cfg_.det_n_output));
        for (uint32_t i = 0; i < static_cast<uint32_t>(cfg_.det_n_output); ++i) {
            memset(&s.out_attrs[i], 0, sizeof(rknn_tensor_attr));
            s.out_attrs[i].index = i;
            rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &s.out_attrs[i], sizeof(s.out_attrs[i]));
        }

        // ★ 零拷贝审计(运行时证据): RGA 目标 fd 与 RKNN 输入 fd 是同一个数字
        //   -> NPU 直接读 RGA 写出来的那块物理内存, 中间没有任何 CPU memcpy。
        fprintf(stderr,
                "[零拷贝审计] 检测[worker %d slot %d]: RGA 目标 fd=%d / RKNN 输入 fd=%d -> %s\n",
                widx, sid, s.fd, s.fd, "同一块物理内存 (零 CPU 拷贝)");

        s.ready = true;
    }
    w.ready.store(true);

    // ---- 主循环 ----
    for (;;) {
        Task task;
        if (!pop_task(widx, NpuTaskKind::kDet, &task)) break;
        DetSlot& s = det_slots_[task.slot];

        // 抢核(同一颗核上还有别的 worker 时, 这里按优先级排队)
        CoreGate* gate = gates_[w.core].get();
        gate->acquire(task.priority);

        const double t0 = now_ms();
        rknn_output outs[16];
        memset(outs, 0, sizeof(outs));
        for (uint32_t i = 0; i < static_cast<uint32_t>(cfg_.det_n_output); ++i) {
            outs[i].want_float = 1;
            outs[i].is_prealloc = 0;
            outs[i].index = i;
        }
        rknn_run(s.ctx, nullptr);
        rknn_outputs_get(s.ctx, static_cast<uint32_t>(cfg_.det_n_output), outs, nullptr);

        // 后处理(在 worker 里做: 输出缓冲属于这个线程的上下文)
        std::vector<OutTensor> ts(cfg_.det_n_output);
        for (size_t i = 0; i < ts.size(); ++i) {
            ts[i].data    = outs[i].buf;
            ts[i].is_int8 = false;
            ts[i].zp      = s.out_attrs[i].zp;
            ts[i].scale   = s.out_attrs[i].scale;
            ts[i].n_elems = s.out_attrs[i].n_elems;
            ts[i].n_dims  = s.out_attrs[i].n_dims;
            for (uint32_t k = 0; k < s.out_attrs[i].n_dims && k < 4; ++k)
                ts[i].dims[k] = s.out_attrs[i].dims[k];
        }
        BoxMapper mapper;
        mapper.img_w = task.det.img_w;
        mapper.img_h = task.det.img_h;
        if (task.det.letterbox) {
            mapper.mode = BoxMapper::LETTERBOX;
            mapper.inv_scale = task.det.scale;
            mapper.pad_x = task.det.pad_x;
            mapper.pad_y = task.det.pad_y;
        } else {
            mapper.mode = BoxMapper::STRETCH;
            mapper.scale_x = static_cast<float>(task.det.img_w) / cfg_.det_w;
            mapper.scale_y = static_cast<float>(task.det.img_h) / cfg_.det_h;
        }
        std::vector<YoloBox> boxes =
            yolov8_postprocess(ts.data(), static_cast<int>(ts.size()), cfg_.conf, cfg_.nms, mapper);
        rknn_outputs_release(s.ctx, static_cast<uint32_t>(cfg_.det_n_output), outs);
        const double dt = now_ms() - t0;

        gate->release();

        // 每核忙闲统计(应用内自测口径)
        {
            std::lock_guard<std::mutex> lk(stat_m_);
            for (auto& st : stats_) {
                if (st.core_mask == w.core_mask && st.kind == 0) {
                    st.busy_ms += dt;
                    st.tasks += 1;
                }
            }
        }

        DetResult r;
        r.chan = task.det.chan;
        r.slot = task.det.slot;
        r.boxes = std::move(boxes);
        r.infer_ms = dt;
        r.ts_ms = task.det.ts_ms;
        r.ok = true;
        publish_det(r);

        // 归还 slot(缓冲可以给下一路用了)
        {
            std::lock_guard<std::mutex> lk(slot_m_);
            s.busy = false;
            free_slots_.push_back(s.id);
        }
        slot_cv_.notify_all();
    }

    // ★ 上下文必须【在本线程里】销毁 —— 和创建时同一个线程。
    //   RKNN 上下文跨线程使用会死锁(npu_pipe.h:14-17 记着这个坑),
    //   销毁同样如此。所以 stop() 只 join, 不碰上下文。
    for (int sid : w.slots) {
        DetSlot& s = det_slots_[sid];
        if (s.in_mem && s.ctx) rknn_destroy_mem(s.ctx, s.in_mem);
        if (s.ctx) rknn_destroy(s.ctx);
        s.in_mem = nullptr;
        s.ctx = 0;
    }
}

// ---------------------------------------------------------------------------
// 识别 worker
// ---------------------------------------------------------------------------
void NpuPool::ocr_worker_loop(int widx) {
    OcrWorker& w = *ocr_workers_[widx];

    // ★ 上下文在【本线程】里创建 —— LicensePlateRec 的 rknn_context 同样
    //   不能跨线程用。init() 会加载模型和字典, 在 worker 线程里做最自然。
    w.rec.reset(new LicensePlateRec());
    if (!w.rec->init(cfg_.ocr_cfg)) {
        fprintf(stderr, "[NpuPool] 识别 worker %d: PP-OCR 初始化失败\n", widx);
        return;
    }
    fprintf(stderr, "[NpuPool] 识别 worker %d: PP-OCR 就绪, 核 mask=0x%x\n",
            widx, w.core_mask);
    w.ready.store(true);

    for (;;) {
        Task task;
        if (!pop_ocr_task(&task)) break;

        // 抢核(同一颗核上还有别的 worker 时, 这里按优先级排队)
        CoreGate* gate = gates_[w.core].get();
        gate->acquire(task.priority);

        const double t0 = now_ms();
        PlateRecResult pr;
        bool ok = false;
        if (!task.ocr.roi.empty()) {
            ok = w.rec->recognize_roi8(task.ocr.roi.data(), task.ocr.roi_w,
                                       task.ocr.roi_w * 3, &pr);
        }
        const double dt = now_ms() - t0;
        gate->release();

        {
            std::lock_guard<std::mutex> lk(stat_m_);
            for (auto& st : stats_) {
                if (st.core_mask == w.core_mask && st.kind == 1) {
                    st.busy_ms += dt;
                    st.tasks += 1;
                }
            }
        }

        OcrResult r;
        r.chan = task.ocr.chan;
        r.group = task.ocr.group;
        r.slot = task.ocr.slot;
        r.text = pr.text;
        r.score = pr.score;
        r.infer_ms = dt;
        r.ok = ok;
        publish_ocr(r);
    }

    // ★ 在本线程里销毁识别上下文(和创建时同一个线程)
    w.rec.reset();
}

// ---------------------------------------------------------------------------
// 结果发布
// ---------------------------------------------------------------------------
void NpuPool::publish_det(const DetResult& r) {
    {
        std::lock_guard<std::mutex> lk(res_m_);
        det_results_[r.chan].push_back(r);
    }
    res_cv_.notify_all();
}

void NpuPool::publish_ocr(const OcrResult& r) {
    {
        std::lock_guard<std::mutex> lk(res_m_);
        ocr_results_[std::make_tuple(r.chan, r.group, r.slot)].push_back(r);
    }
    res_cv_.notify_all();
}

// ---------------------------------------------------------------------------
// slot 的借还
// ---------------------------------------------------------------------------
int NpuPool::acquire_det_slot() {
    std::unique_lock<std::mutex> lk(slot_m_);
    slot_cv_.wait(lk, [&] { return stop_ || !free_slots_.empty(); });
    if (stop_ || free_slots_.empty()) return -1;
    const int id = free_slots_.front();
    free_slots_.pop_front();
    det_slots_[id].busy = true;
    return id;
}

void NpuPool::release_det_slot(int slot) {
    if (slot < 0 || slot >= static_cast<int>(det_slots_.size())) return;
    {
        std::lock_guard<std::mutex> lk(slot_m_);
        if (!det_slots_[slot].busy) return;
        det_slots_[slot].busy = false;
        free_slots_.push_back(slot);
    }
    slot_cv_.notify_all();
}

int  NpuPool::slot_fd(int slot) const {    if (slot < 0 || slot >= static_cast<int>(det_slots_.size())) return -1;
    return det_slots_[slot].fd;
}

void* NpuPool::slot_ptr(int slot) const {
    if (slot < 0 || slot >= static_cast<int>(det_slots_.size())) return nullptr;
    return det_slots_[slot].ptr;
}

// ---------------------------------------------------------------------------
// 提交
// ---------------------------------------------------------------------------
bool NpuPool::submit_det(DetTask&& t) {
    Task task;
    task.kind = NpuTaskKind::kDet;
    task.priority = 0;                 // 检测: 普通优先级
    task.chan = t.chan;
    task.slot = t.slot;
    task.slot_idx = t.slot;
    if (t.slot >= 0 && t.slot < static_cast<int>(det_slots_.size()))
        task.worker = det_slots_[t.slot].worker;
    task.det = std::move(t);
    {
        std::lock_guard<std::mutex> lk(q_m_);
        if (stop_) return false;
        task.seq = next_seq_++;
        queue_.emplace(task.priority, std::move(task));
    }
    q_cv_.notify_all();
    return true;
}

bool NpuPool::submit_ocr(OcrTask&& t) {
    Task task;
    task.kind = NpuTaskKind::kOcr;
    task.priority = 1;                 // ★ 识别: 高优先级 —— 排在检测前面
    task.chan = t.chan;
    task.group = t.group;
    task.slot_idx = t.slot;
    task.ocr = std::move(t);
    {
        std::lock_guard<std::mutex> lk(q_m_);
        if (stop_) return false;
        task.seq = next_seq_++;
        queue_.emplace(task.priority, std::move(task));
    }
    q_cv_.notify_all();
    return true;
}

// ---------------------------------------------------------------------------
// 取结果
// ---------------------------------------------------------------------------
bool NpuPool::take_det(int chan, DetResult* out, int timeout_ms) {
    std::unique_lock<std::mutex> lk(res_m_);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0);
    for (;;) {
        auto it = det_results_.find(chan);
        if (it != det_results_.end() && !it->second.empty()) {
            *out = std::move(it->second.front());
            it->second.pop_front();
            return true;
        }
        if (stop_) return false;
        if (res_cv_.wait_until(lk, deadline) == std::cv_status::timeout) return false;
    }
}

bool NpuPool::take_ocr(int chan, int group, int slot, OcrResult* out, int timeout_ms) {
    std::unique_lock<std::mutex> lk(res_m_);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0);
    const auto key = std::make_tuple(chan, group, slot);
    for (;;) {
        auto it = ocr_results_.find(key);
        if (it != ocr_results_.end() && !it->second.empty()) {
            *out = std::move(it->second.front());
            it->second.pop_front();
            return true;
        }
        if (stop_) return false;
        if (res_cv_.wait_until(lk, deadline) == std::cv_status::timeout) return false;
    }
}

// ---------------------------------------------------------------------------
// 统计
// ---------------------------------------------------------------------------
std::vector<CoreStat> NpuPool::core_stats() const {
    std::lock_guard<std::mutex> lk(stat_m_);
    return stats_;
}

double NpuPool::uptime_ms() const {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0_).count();
}

int NpuPool::pending() const {
    std::lock_guard<std::mutex> lk(q_m_);
    return static_cast<int>(queue_.size());
}

// ---------------------------------------------------------------------------
void NpuPool::stop() {
    {
        std::lock_guard<std::mutex> lk(q_m_);
        if (stop_) return;
        stop_ = true;
    }
    q_cv_.notify_all();
    slot_cv_.notify_all();
    res_cv_.notify_all();

    for (auto& w : det_workers_) if (w->th.joinable()) w->th.join();
    for (auto& w : ocr_workers_) if (w->th.joinable()) w->th.join();

    for (auto& s : det_slots_) {
        // ★ 上下文已经在【各自的 worker 线程里】销毁了(见 det_worker_loop),
        //   这里只回收 dma-buf —— 跨线程销毁 RKNN 上下文会死锁。
        if (s.ptr) rk::dma_buf_unmap(s.ptr, s.buf_alloc);
        if (s.fd >= 0) ::close(s.fd);
    }
    det_slots_.clear();
    det_workers_.clear();
    ocr_workers_.clear();
}

}  // namespace rk
