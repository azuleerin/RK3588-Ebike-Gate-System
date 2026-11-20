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

    // ★ 掩码 0x7 在【池子】这一层是合法的: 它只表示"可用核集合 = {0,1,2}",
    //   池子给每个 worker 绑的是【单核】掩码(1/2/4), 永远不会把 0x7 交给驱动。
    //   禁掉的是"把 0x7 直接送给驱动"(RKNN_NPU_CORE_0_1_2 = 一次推理拆三核),
    //   那条禁令落在驱动边界上: rk::sanitize_core_mask()。
    //   要三颗核都用上: 掩码给 0,1,2 + 开 3 个 worker, 每个独占一颗核。

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

    // ★ worker 数量与核绑定: worker i 绑到 cores[i % cores.size()] —— 均匀轮询。
    //   worker 数可以和核数不一样(比如 2 个 worker 都在 core0):
    //   那种情况下同核的多个 worker 由【每核闸门】串行化, 不会同时占核。
    const int n_det_workers = cfg.det_workers > 0 ? cfg.det_workers : 1;
    for (int ci = 0; ci < n_det_workers; ++ci) {
        auto w = std::unique_ptr<DetWorker>(new DetWorker());
        w->core = det_cores[static_cast<size_t>(ci) % det_cores.size()];
        w->core_mask = (1 << w->core);
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

    // ---- 建识别 worker(同样的 i % 核数 均匀轮询) ----
    const int n_ocr_workers = cfg.ocr_workers > 0 ? cfg.ocr_workers : 1;
    for (int ci = 0; ci < n_ocr_workers; ++ci) {
        auto w = std::unique_ptr<OcrWorker>(new OcrWorker());
        w->core = ocr_cores[static_cast<size_t>(ci) % ocr_cores.size()];
        w->core_mask = (1 << w->core);
        ocr_workers_.push_back(std::move(w));
    }

    // ---- 两条独立的定长队列(启动时一次性把数组开好) ----
    //   检测: 每个 det worker 一条私有队列
    det_queues_.resize(det_workers_.size());
    for (size_t i = 0; i < det_queues_.size(); ++i) {
        det_queues_[i].reset(new TaskQueue<Task>());
        det_queues_[i]->init(cfg.det_queue_cap);
    }
    //   识别: 所有识别 worker 共享一条
    ocr_queue_.reset(new TaskQueue<Task>());
    ocr_queue_->init(cfg.ocr_queue_cap);

    // ---- ROI 槽池(识别用) ----
    //   通道线程借一个槽把裁好的车牌图直接写进去, 识别 worker 读完归还。
    //   以前 ROI 走 std::vector 随任务走: 每个候选两次堆分配 + 两次拷贝。
    {
        const size_t n = cfg.ocr_roi_slots > 0 ? cfg.ocr_roi_slots : 1;
        ocr_rois_.resize(n);
        for (size_t i = 0; i < n; ++i) {
            ocr_rois_[i].id = static_cast<int>(i);
            ocr_rois_[i].busy = false;
            ocr_rois_[i].buf.assign(cfg.ocr_roi_max_bytes, 0);
            free_ocr_rois_.push_back(static_cast<int>(i));
        }
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
        // 每颗核上有几个 worker —— 调度模型就是照这个建的, 打印出来便于核对
        int per_core[3] = {0, 0, 0};
        for (const auto& w : det_workers_) if (w->core >= 0 && w->core < 3) ++per_core[w->core];
        for (const auto& w : ocr_workers_) if (w->core >= 0 && w->core < 3) ++per_core[w->core];
        fprintf(stderr, "[NpuPool]   每核 worker 数: core0=%d core1=%d core2=%d\n",
                per_core[0], per_core[1], per_core[2]);
    }
    return true;
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

        // ★ 过一遍消毒: w.core_mask 永远是单核(1/2/4), 但万一以后有人改成
        //   直接用 core_mask_to_indices 之外的掩码, 0x7 会被在这里挡住。
        const int safe_mask = rk::sanitize_core_mask(w.core_mask, "NpuPool 检测 worker");
        const int cret = rknn_set_core_mask(ctx, static_cast<rknn_core_mask>(safe_mask));
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

        // ★★ 输出缓冲自己预分配(is_prealloc = 1)
        //   以前 is_prealloc = 0: 每推理一次, runtime 就【自己 malloc 一块输出缓冲】,
        //   推理完再 free —— 热循环里的 malloc/free。
        //   现在: 启动时按输出属性把缓冲开好, 每帧直接写进去, 循环里零分配。
        //   ★ want_float = 1 时 runtime 写的是 float32, 所以缓冲要按
        //     n_elems * sizeof(float) 开(不是 attr.size —— 那是量化格式的字节数)。
        //     这一点和 license_plate_rec.cc 的做法一致(那边踩过"给小了 runtime
        //     静默失败"的坑)。
        s.out_bufs.resize(static_cast<size_t>(cfg_.det_n_output));
        s.outs.resize(static_cast<size_t>(cfg_.det_n_output));
        s.ts.resize(static_cast<size_t>(cfg_.det_n_output));
        for (uint32_t i = 0; i < static_cast<uint32_t>(cfg_.det_n_output); ++i) {
            const size_t n_elems = s.out_attrs[i].n_elems;
            size_t bytes = n_elems * sizeof(float);
            if (bytes < s.out_attrs[i].size) bytes = s.out_attrs[i].size;
            if (bytes == 0) bytes = 4;      // 兜底: 至少给 1 个元素
            s.out_bufs[i].assign(bytes, 0);
            s.outs[i].want_float = 1;
            s.outs[i].is_prealloc = 1;
            s.outs[i].index = i;
            s.outs[i].buf = s.out_bufs[i].data();
            s.outs[i].size = static_cast<uint32_t>(s.out_bufs[i].size());
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
        // ★ 每条检测队列是【这个 worker 私有的】: 检测任务本来就绑在 slot 的
        //   宿主 worker 上, 所以不需要像以前那样从头扫一条混装队列去找
        //   "自己能跑的那条"(O(n) 扫描 + multimap 节点的 malloc/free)。
        if (!det_queues_[static_cast<size_t>(widx)]->pop(&task)) break;
        DetSlot& s = det_slots_[task.slot];

        // 抢核(同一颗核上还有别的 worker 时, 这里按优先级排队)
        CoreGate* gate = gates_[w.core].get();
        gate->acquire(task.priority);

        const double t0 = now_ms();
        rknn_output* outs = s.outs.data();
        rknn_run(s.ctx, nullptr);
        rknn_outputs_get(s.ctx, static_cast<uint32_t>(cfg_.det_n_output), outs, nullptr);
        // NPU 侧到此为止 —— 后面是 CPU 上的后处理
        const double dt_post0 = now_ms() - t0;

        // ★★ 2026-11-20: 到这里就【提前放核】。
        //
        //   以前闸门一直持有到后处理做完(npu_pool.cc 的 gate->release() 在
        //   整个 yolov8_postprocess 之后)。但 rknn_run + rknn_outputs_get
        //   一返回, NPU 核心就已经空闲了 —— 后面那 0.7 ms 的 DFL/NMS/自检
        //   是【纯 CPU】的活, 不该占着核闸门。
        //
        //   ★ 为什么这要紧: 生产配置是【每核 1 个检测 worker + 1 个识别 worker】
        //     (config/gate.env: 检测核 0,1,2 / 识别核 0,1,2 / 各 3 个 worker)。
        //     两个 worker 共用同一颗核的闸门 —— 检测 worker 占着闸门做 CPU
        //     后处理的时候, 同核的识别 worker 明明有空闲的核, 却只能干等。
        //     这段等待直接进 ocrwait。
        //
        //   ★ 安全性: 输出缓冲是【slot 自己的】(s.out_bufs[i]), 而这个 slot
        //     要到 publish_det 之后才归还(见本函数末尾), 所以放核之后继续
        //     读这块缓冲是安全的 —— 同核另一个 worker 用的是它自己 slot 的缓冲,
        //     不会碰这里。跨线程的槽位归还由 slot_m_ 保护, 顺序没变。
        gate->release();

        // 后处理(在 worker 里做: 输出缓冲属于这个线程的上下文)
        // ★ ts 也提前开好(每个 slot 一份), 避免每帧一次 vector 分配
        std::vector<OutTensor>& ts = s.ts;
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
        // ⚠️ 这里【不能】调 rknn_outputs_release!
        //   is_prealloc=1 时输出缓冲是【我们自己的】(s.out_bufs), 让运行时去 free
        //   会把这块缓冲掏空 —— 下一帧再写就是写已释放的内存。
        //   (license_plate_rec.cc 里踩过同一个坑, 那里还专门写了注释。)
        //   所以: 缓冲由 slot 自己持有, 生命周期跟 slot 一样, 不交给 runtime。
        // ★ 分开计两笔账:
        //   dt_npu : rknn_run + rknn_outputs_get —— 这才是【真正占着 NPU 核】的时间
        //   dt_all : 再加上 CPU 上的后处理(DFL 解码 + NMS)
        //   以前只有一个数, 把 CPU 后处理也算成"核占用", 报出来 99% ——
        //   而 debugfs 的驱动侧数字是 85%, 差的那 14% 就是后处理, 不是 NPU。
        const double dt_all = now_ms() - t0;
        const double dt_npu = dt_post0;

        // 每核忙闲统计(应用内自测口径)
        {
            std::lock_guard<std::mutex> lk(stat_m_);
            for (auto& st : stats_) {
                if (st.core_mask == w.core_mask && st.kind == 0) {
                    st.busy_ms += dt_npu;
                    st.total_ms += dt_all;
                    st.tasks += 1;
                }
            }
        }

        DetResult r;
        r.chan = task.det.chan;
        r.slot = task.det.slot;
        r.boxes = std::move(boxes);
        r.infer_ms = dt_npu;
        r.ts_ms = task.det.ts_ms;
        r.ok = true;
        publish_det(std::move(r));

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

    // ★★ 上下文在【本线程】里创建 —— LicensePlateRec 的 rknn_context 同样
    //   不能跨线程用。init() 会加载模型和字典, 在 worker 线程里做最自然。
    //
    // ★★ 关键: 用【本 worker 自己的核掩码】初始化识别上下文!
    //   以前这里直接用 cfg_.ocr_cfg, 而 gate_main.cc 把 npu_core_mask 置成了 0
    //   (注释写"核绑定由池里的 worker 负责") —— 但池子【并没有】真的去绑:
    //   LicensePlateRec::init() 只在 npu_core_mask > 0 时才调 rknn_set_core_mask,
    //   于是识别上下文的核掩码是 0 = RKNN_NPU_CORE_AUTO = 驱动随便挑一颗核。
    //   后果很严重:
    //     · 识别任务会跑到检测核上去抢时间(检测核实测已经 97% 满载);
    //     · 池子的【每核闸门】按"应该在 core0"仲裁, 管的不是硬件真在用的核,
    //       所以"识别优先"实际上【没有生效】;
    //     · 每核统计按意图核归属, 识别那部分时间被记到了错的核上 ——
    //       "识别核只用了 19%"这个结论反映的不是硬件真实负载。
    //   现在: 拷贝一份配置, 把掩码换成自己的核, 真正绑上去。
    PlateRecConfig ocr_cfg = cfg_.ocr_cfg;
    ocr_cfg.npu_core_mask = w.core_mask;
    w.rec.reset(new LicensePlateRec());
    if (!w.rec->init(ocr_cfg)) {
        fprintf(stderr, "[NpuPool] 识别 worker %d: PP-OCR 初始化失败\n", widx);
        return;
    }
    fprintf(stderr, "[NpuPool] 识别 worker %d: PP-OCR 就绪, 核 mask=0x%x\n",
            widx, w.core_mask);
    w.ready.store(true);

    for (;;) {
        Task task;
        // ★ 识别队列是【所有识别 worker 共享】的: 一块牌的多个候选会被
        //   不同 worker 领走, 并行跑(以前只有 1 个识别 worker, 4 个候选串行)。
        if (!ocr_queue_->pop(&task)) break;

        // 抢核(同一颗核上还有别的 worker 时, 这里按优先级排队)
        CoreGate* gate = gates_[w.core].get();
        gate->acquire(task.priority);

        const double t0 = now_ms();
        PlateRecResult pr;
        bool ok = false;
        const int roi_slot = task.ocr.roi_slot;
        uint8_t* roi = (roi_slot >= 0) ? ocr_roi_ptr(roi_slot) : nullptr;
        if (roi && task.ocr.roi_w > 0) {
            ok = w.rec->recognize_roi8(roi, task.ocr.roi_w,
                                       task.ocr.roi_w * 3, &pr);
        }
        const double dt = now_ms() - t0;
        gate->release();

        // ★ ROI 槽用完立刻归还(通道线程才能借到下一个候选的槽)
        if (roi_slot >= 0) release_ocr_roi(roi_slot);

        {
            std::lock_guard<std::mutex> lk(stat_m_);
            for (auto& st : stats_) {
                if (st.core_mask == w.core_mask && st.kind == 1) {
                    st.busy_ms += dt;
                    st.total_ms += dt;
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
        publish_ocr(std::move(r));
    }

    // ★ 在本线程里销毁识别上下文(和创建时同一个线程)
    w.rec.reset();
}

// ---------------------------------------------------------------------------
// 结果发布
// ---------------------------------------------------------------------------
// ★★ 2026-11-20: 参数从【const 引用】改成【右值引用】+ move。
//
//   以前 push_det(r) 里 push_back(r) 是【按值拷贝】—— DetResult 里带着
//   std::vector<YoloBox> boxes, 每次发布都要【堆分配一次 + 拷贝一遍框数组】,
//   而且这一整套动作是在【全局锁 res_m_ 里面】做的: 4 路通道共用这把锁,
//   等于把一次堆分配塞进了所有通道的竞争路径里。
//   现在 move: 框数组的堆指针直接转手, 零分配零拷贝。
void NpuPool::publish_det(DetResult&& r) {
    {
        std::lock_guard<std::mutex> lk(res_m_);
        det_results_[r.chan].push_back(std::move(r));
    }
    res_cv_.notify_all();
}

void NpuPool::publish_ocr(OcrResult&& r) {
    {
        std::lock_guard<std::mutex> lk(res_m_);
        ocr_results_[std::make_tuple(r.chan, r.group, r.slot)].push_back(std::move(r));
    }
    res_cv_.notify_all();
}

// ---------------------------------------------------------------------------
// slot 的借还
// ---------------------------------------------------------------------------
int NpuPool::acquire_det_slot() {
    std::unique_lock<std::mutex> lk(slot_m_);
    slot_cv_.wait(lk, [&] {
        return stop_.load(std::memory_order_relaxed) || !free_slots_.empty();
    });
    if (stop_.load(std::memory_order_relaxed) || free_slots_.empty()) return -1;
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
    if (stop_.load(std::memory_order_relaxed)) return false;
    task.seq = next_seq_++;
    // ★ 推到这个 slot 归属的 worker 的【私有队列】
    //   (检测任务本来就绑在 slot 的宿主 worker 上 —— 见 task.worker)
    if (task.worker < 0 || task.worker >= static_cast<int>(det_queues_.size())) return false;
    return det_queues_[static_cast<size_t>(task.worker)]->push(std::move(task));
}

bool NpuPool::submit_ocr(OcrTask&& t) {
    Task task;
    task.kind = NpuTaskKind::kOcr;
    task.priority = 1;                 // ★ 识别: 高优先级 —— 同核排队时排在检测前面
    task.chan = t.chan;
    task.group = t.group;
    task.slot_idx = t.slot;
    task.ocr = std::move(t);
    if (stop_.load(std::memory_order_relaxed)) return false;
    task.seq = next_seq_++;
    // ★ 推给【共享】的识别队列: 任何识别 worker 都能领
    return ocr_queue_->push(std::move(task));
}

// ---------------------------------------------------------------------------
// ROI 槽的借还
// ---------------------------------------------------------------------------
int NpuPool::acquire_ocr_roi() {
    std::unique_lock<std::mutex> lk(roi_m_);
    roi_cv_.wait(lk, [this] {
        return stop_.load(std::memory_order_relaxed) || !free_ocr_rois_.empty();
    });
    if (stop_.load(std::memory_order_relaxed)) return -1;
    const int id = free_ocr_rois_.front();
    free_ocr_rois_.pop_front();
    ocr_rois_[static_cast<size_t>(id)].busy = true;
    return id;
}

uint8_t* NpuPool::ocr_roi_ptr(int slot) {
    if (slot < 0 || slot >= static_cast<int>(ocr_rois_.size())) return nullptr;
    return ocr_rois_[static_cast<size_t>(slot)].buf.data();
}

void NpuPool::release_ocr_roi(int slot) {
    if (slot < 0 || slot >= static_cast<int>(ocr_rois_.size())) return;
    {
        std::lock_guard<std::mutex> lk(roi_m_);
        if (!ocr_rois_[static_cast<size_t>(slot)].busy) return;
        ocr_rois_[static_cast<size_t>(slot)].busy = false;
        free_ocr_rois_.push_back(slot);
    }
    roi_cv_.notify_one();
}

size_t NpuPool::ocr_roi_capacity() const {
    return ocr_rois_.empty() ? 0 : ocr_rois_[0].buf.size();
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
        if (stop_.load(std::memory_order_relaxed)) return false;
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
        if (stop_.load(std::memory_order_relaxed)) return false;
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
    int n = static_cast<int>(ocr_queue_->size());
    for (const auto& q : det_queues_) n += static_cast<int>(q->size());
    return n;
}

// ---------------------------------------------------------------------------
void NpuPool::stop() {
    {
        if (stop_.exchange(true)) return;
    }
    // 唤醒所有等着的人: 队列 / slot / ROI 槽 / 结果
    for (auto& q : det_queues_) q->stop();
    ocr_queue_->stop();
    slot_cv_.notify_all();
    roi_cv_.notify_all();
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
