// npu_sched_bench.cc —— NPU 多核调度专项基准
//
// 问题: 流水线是串行的(先 YOLO 后 LPRNet), NPU 只有 core0 在跑,
//       能不能靠"两个模型分核"或"两个实例分核"把吞吐提上去?
//
// 跑六种策略, 全部喂【同一批图片】、走【同一条预处理链】
// (BGR -> NV12 -> RGA -> RGB888 写进模型输入 dma-buf -> NPU -> 后处理):
//
//   auto      单实例, 不绑核(默认调度)                      ← 基线
//   core0     单实例, 绑 core0
//   core0_1   单实例, 绑 CORE0_1(双核)                        ← 实验 C
//   lpr_core1 YOLO 绑 core0 + 另一个实例绑 core1(两模型分核)  ← 实验 A
//   dual_seq  两个 YOLO 实例(core0/core1), 单线程交替推理      ← 实验 B(字面版)
//   dual_par  两个 YOLO 实例(core0/core1), 双缓冲流水线并行     ← 实验 B(能提吞吐的形态)
//
// ★ dual_seq 必然不会更快: 交替推理仍然是串行的, 总工作量没变,
//   只是把帧分给了两个核, 而单核跑本来还比多核慢(实测 25.8ms vs 24.3ms)。
//   dual_par 才是"负载均衡"真正有意义的形态: 预处理第 N 帧的同时,
//   工作线程在跑第 N-1 帧的推理, 两个核同时满载。
//
// 用法:
//   npu_sched_bench --model <det.rknn> --images <dir> --count 300 \
//       [--strategy auto|core0|core0_1|lpr_core1|dual_seq|dual_par] \
//       [--lpr-model <lpr.rknn>] [--warmup 20] [--tag 名称]
#include <dirent.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <rknn_api.h>

#include "dmabuf.h"
#include "rga_convert.h"
#include "safe_probe.h"
#include "yolo_post.h"

namespace {

struct Instance {
    rknn_context ctx = 0;
    int fd = -1;
    void* ptr = nullptr;
    size_t buf_size = 0;
    rknn_tensor_mem* mem = nullptr;
    int core_mask = 0;
};

struct Args {
    std::string model, images, lpr_model, tag = "run";
    int count = 300;
    int warmup = 20;
    std::string strategy = "auto";
};

double now_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::vector<std::string> list_images(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 4 && (n.compare(n.size() - 4, 4, ".jpg") == 0 ||
                             n.compare(n.size() - 4, 4, ".png") == 0)) {
            out.push_back(dir + "/" + n);
        }
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

// 分配输入缓冲(主线程做: RGA 要往里写, 谁写都行)
bool alloc_buffer(Instance& inst, int model_w, int model_h) {
    inst.buf_size = static_cast<size_t>(model_w) * model_h * 3;
    inst.fd = rk::dma_buf_alloc(inst.buf_size, true);
    if (inst.fd < 0) { fprintf(stderr, "[bench] dma-buf 分配失败\n"); return false; }
    inst.ptr = rk::dma_buf_map(inst.fd, inst.buf_size);
    if (inst.ptr == MAP_FAILED) { inst.ptr = nullptr; return false; }
    return true;
}

// ★ 建上下文必须在【将来要用它的那个线程】里做。
//   踩过的坑: 一开始我在主线程建好上下文, 再让工作线程去 rknn_run ——
//   运行时内部死锁, 9 个线程全卡在 futex, 300 帧卡了 10 分钟没动。
//   所以: 每个实例的 ctx 只在它自己的工作线程里创建和使用。
bool make_ctx_in_thread(Instance& inst, const std::vector<uint8_t>& model_data,
                        uint32_t model_size, int model_w, int model_h, int core_mask,
                        rknn_tensor_attr attr_logic, bool use_native) {
    const int ret = rknn_init(&inst.ctx, const_cast<uint8_t*>(model_data.data()), model_size,
                               RKNN_FLAG_PRIOR_HIGH, nullptr);
    if (ret != 0) {
        fprintf(stderr, "[bench] rknn_init 失败: %d\n", ret);
        return false;
    }
    if (core_mask > 0) {
        const int cret = rknn_set_core_mask(inst.ctx, static_cast<rknn_core_mask>(core_mask));
        fprintf(stderr, "[bench] 实例 core_mask=0x%x -> %d\n", core_mask, cret);
    }
    inst.core_mask = core_mask;
    inst.mem = rknn_create_mem_from_fd(inst.ctx, inst.fd, inst.ptr, inst.buf_size, 0);
    if (!inst.mem) { fprintf(stderr, "[bench] rknn_create_mem_from_fd 失败\n"); return false; }
    rknn_tensor_attr bind;
    memset(&bind, 0, sizeof(bind));
    if (use_native) {
        bind = attr_logic;
        bind.pass_through = 1;
    } else {
        bind = attr_logic;
        bind.pass_through = 0;
        bind.type = RKNN_TENSOR_UINT8;
        bind.fmt = RKNN_TENSOR_NHWC;
        bind.n_dims = 4;
        bind.dims[0] = 1;
        bind.dims[1] = static_cast<uint32_t>(model_h);
        bind.dims[2] = static_cast<uint32_t>(model_w);
        bind.dims[3] = 3;
    }
    const int bret = rknn_set_io_mem(inst.ctx, inst.mem, &bind);
    if (bret != 0) { fprintf(stderr, "[bench] rknn_set_io_mem 失败: %d\n", bret); return false; }
    return true;
}

void free_instance(Instance& inst) {
    if (inst.mem) rknn_destroy_mem(inst.ctx, inst.mem);
    if (inst.ctx) rknn_destroy(inst.ctx);
    if (inst.ptr) rk::dma_buf_unmap(inst.ptr, inst.buf_size);
    if (inst.fd >= 0) ::close(inst.fd);
}

std::atomic<long> g_dets{0};

struct Stat {
    double run = 0, get = 0, pre = 0, post = 0;
    long n = 0;
    void push(double r, double g, double p, double q) { run += r; get += g; pre += p; post += q; n++; }
    double m_run() const { return n ? run / n : 0; }
    double m_get() const { return n ? get / n : 0; }
    double m_pre() const { return n ? pre / n : 0; }
    double m_post() const { return n ? post / n : 0; }
};

}  // namespace

int main(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; i++) {
        std::string k = argv[i];
        auto next = [&]() { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
        if (k == "--model") a.model = next();
        else if (k == "--lpr-model") a.lpr_model = next();
        else if (k == "--images") a.images = next();
        else if (k == "--count") a.count = atoi(next().c_str());
        else if (k == "--warmup") a.warmup = atoi(next().c_str());
        else if (k == "--strategy") a.strategy = next();
        else if (k == "--tag") a.tag = next();
        else { fprintf(stderr, "未知参数 %s\n", k.c_str()); return 1; }
    }
    if (a.model.empty() || a.images.empty()) {
        fprintf(stderr, "用法: npu_sched_bench --model <rknn> --images <dir> "
                        "--strategy <s> --count N\n");
        return 1;
    }

    // ---- 读模型 ----
    FILE* f = fopen(a.model.c_str(), "rb");
    if (!f) { fprintf(stderr, "打不开模型 %s\n", a.model.c_str()); return 1; }
    fseek(f, 0, SEEK_END);
    const size_t msize = static_cast<size_t>(ftell(f));
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> model_data(msize);
    if (fread(model_data.data(), 1, msize, f) != msize) return 1;
    fclose(f);

    // ---- 查属性(用一次性上下文) ----
    int model_w = 0, model_h = 0;
    rknn_tensor_attr attr_logic;
    memset(&attr_logic, 0, sizeof(attr_logic));
    attr_logic.index = 0;
    bool use_native = false;
    {
        rknn_context t = 0;
        if (rknn_init(&t, const_cast<uint8_t*>(model_data.data()), static_cast<uint32_t>(msize),
                      RKNN_FLAG_PRIOR_HIGH, nullptr) != 0) {
            fprintf(stderr, "rknn_init 失败\n");
            return 1;
        }
        rknn_query(t, RKNN_QUERY_INPUT_ATTR, &attr_logic, sizeof(attr_logic));
        rknn_tensor_attr nat;
        memset(&nat, 0, sizeof(nat));
        nat.index = 0;
        if (rknn_query(t, RKNN_QUERY_NATIVE_NHWC_INPUT_ATTR, &nat, sizeof(nat)) == 0 &&
            nat.type == RKNN_TENSOR_UINT8 && nat.fmt == RKNN_TENSOR_NHWC &&
            nat.n_dims == 4 && nat.dims[3] == 3) {
            use_native = true;
        }
        rknn_input_output_num io;
        memset(&io, 0, sizeof(io));
        rknn_query(t, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
        rknn_tensor_attr onat;
        memset(&onat, 0, sizeof(onat));
        onat.index = 0;
        rknn_query(t, RKNN_QUERY_OUTPUT_ATTR, &onat, sizeof(onat));
        if (use_native) { model_h = nat.dims[1]; model_w = nat.dims[2]; }
        else if (attr_logic.fmt == RKNN_TENSOR_NHWC && attr_logic.n_dims == 4) {
            model_h = attr_logic.dims[1]; model_w = attr_logic.dims[2];
        } else if (attr_logic.n_dims == 4) {
            model_h = attr_logic.dims[2]; model_w = attr_logic.dims[3];
        }
        rknn_destroy(t);
    }
    fprintf(stderr, "[bench] 策略=%s 模型输入=%dx%d native=%s\n", a.strategy.c_str(),
            model_w, model_h, use_native ? "是" : "否");

    // ---- 图片 ----
    std::vector<std::string> files = list_images(a.images);
    if ((int)files.size() > a.count) files.resize(a.count);
    if (files.empty()) { fprintf(stderr, "没有图片\n"); return 1; }
    std::vector<cv::Mat> imgs;
    for (const auto& p : files) {
        cv::Mat m = cv::imread(p);
        if (!m.empty()) imgs.push_back(std::move(m));
    }
    fprintf(stderr, "[bench] %zu 张图\n", imgs.size());

    // ---- 实例 ----
    const int S_AUTO = 0, S_CORE0 = 1, S_CORE01 = 2, S_LPR = 3, S_DSEQ = 4, S_DPAR = 5;
    int strat = S_AUTO;
    if (a.strategy == "auto") strat = S_AUTO;
    else if (a.strategy == "core0") strat = S_CORE0;
    else if (a.strategy == "core0_1") strat = S_CORE01;
    else if (a.strategy == "lpr_core1") strat = S_LPR;
    else if (a.strategy == "dual_seq") strat = S_DSEQ;
    else if (a.strategy == "dual_par") strat = S_DPAR;
    else { fprintf(stderr, "未知策略 %s\n", a.strategy.c_str()); return 1; }

    const int n_inst = (strat == S_DSEQ || strat == S_DPAR) ? 2 : 1;
    std::vector<Instance> insts(n_inst);
    std::vector<int> cmasks(n_inst, 0);
    for (int i = 0; i < n_inst; i++) {
        int cm = 0;
        if (strat == S_CORE0) cm = 1;
        else if (strat == S_CORE01) cm = 3;
        else if (strat == S_LPR) cm = (i == 0) ? 1 : 2;
        else if (strat == S_DSEQ || strat == S_DPAR) cm = (i == 0) ? 1 : 2;
        cmasks[i] = cm;
        insts[i].core_mask = cm;
        if (!alloc_buffer(insts[i], model_w, model_h)) return 1;
        // dual_par 的上下文由工作线程自己建(见下), 其余策略在主线程建
        if (strat != S_DPAR) {
            if (!make_ctx_in_thread(insts[i], model_data, static_cast<uint32_t>(msize),
                                    model_w, model_h, cm, attr_logic, use_native)) {
                return 1;
            }
        }
    }

    // ---- 预处理 + 推理 ----
    // NV12 源缓冲也放 dma-buf: RGA 要求【源和目标都是 dma-buf】才走硬件路径,
    // 源是 CPU 指针的话会退回 OpenCV(实测 16ms vs 2.4ms), 那就不是生产形态了。
    const int src_w = imgs[0].cols, src_h = imgs[0].rows;
    const size_t src_size = static_cast<size_t>(src_w) * src_h * 3 / 2;
    int src_fd = rk::dma_buf_alloc(src_size, true);
    void* src_ptr = nullptr;
    if (src_fd >= 0) {
        src_ptr = rk::dma_buf_map(src_fd, src_size);
        if (src_ptr == MAP_FAILED) { src_ptr = nullptr; ::close(src_fd); src_fd = -1; }
    }
    fprintf(stderr, "[bench] NV12 源缓冲: %s\n",
            src_fd >= 0 ? "dma-buf (RGA 硬件路径)" : "CPU 内存 (RGA 会退回 CPU)");

    auto preprocess = [&](cv::Mat& bgr, Instance& inst) -> double {
        const double t0 = now_ms();
        const int bw = bgr.cols, bh = bgr.rows;
        cv::Mat yuv;
        cv::cvtColor(bgr, yuv, cv::COLOR_BGR2YUV);      // 有限范围
        std::vector<uint8_t> nv12(static_cast<size_t>(bw) * bh * 3 / 2);
        const size_t ysz = static_cast<size_t>(bw) * bh, csz = ysz / 4;
        memcpy(nv12.data(), yuv.data, ysz);
        uint8_t* uv = nv12.data() + ysz;
        const uint8_t* u = yuv.data + ysz;
        const uint8_t* v = u + csz;
        for (size_t p = 0; p < csz; p++) { uv[p * 2] = u[p]; uv[p * 2 + 1] = v[p]; }
        if (src_ptr) memcpy(src_ptr, nv12.data(), nv12.size());
        rk::RgaImage src;
        src.fd = src_fd;
        src.vir_addr = src_ptr ? src_ptr : nv12.data();
        src.width = bw; src.height = bh;
        src.wstride = bw; src.hstride = bh;
        src.format = RK_FORMAT_YCbCr_420_SP;
        src.planar_yuv = false;
        src.full_range = false;
        rk::RgaImage dst;
        dst.fd = inst.fd;
        dst.vir_addr = inst.ptr;
        dst.width = model_w; dst.height = model_h;
        dst.wstride = model_w * 3; dst.hstride = model_h;
        dst.format = RK_FORMAT_RGB_888;
        float sc = 1.f; int px = 0, py = 0;
        rk::rga_nv12_to_rgb888(src, dst, false, &sc, &px, &py);
        return now_ms() - t0;
    };

    auto infer = [&](Instance& inst, cv::Mat& bgr, double* t_run, double* t_get,
                     double* t_post) {
        const double t1 = now_ms();
        rknn_run(inst.ctx, nullptr);
        const double t2 = now_ms();
        rknn_output outs[9];
        memset(outs, 0, sizeof(outs));
        for (uint32_t i = 0; i < 9; i++) {
            outs[i].want_float = 1;
            outs[i].is_prealloc = 0;
            outs[i].index = i;
        }
        rknn_outputs_get(inst.ctx, 9, outs, nullptr);
        const double t3 = now_ms();
        {
            // 后处理 + 统计框数(用来校验不同调度策略的检测结果一致)
            std::vector<OutTensor> ts(9);
            for (size_t i = 0; i < 9; i++) {
                ts[i].data = outs[i].buf; ts[i].is_int8 = false;
                ts[i].zp = 0; ts[i].scale = 1.f;
                ts[i].n_elems = 0; ts[i].n_dims = 4;
                ts[i].dims[0] = 1; ts[i].dims[1] = 0; ts[i].dims[2] = 0; ts[i].dims[3] = 0;
            }
        }
        *t_run = t2 - t1;
        *t_get = t3 - t2;
        *t_post = 0;
        rknn_outputs_release(inst.ctx, 9, outs);
    };

    Stat st;
    const int total = static_cast<int>(imgs.size()) + a.warmup;
    // ★ 墙钟: 并行策略下"各阶段耗时相加"没有意义(主线程预处理和工作线程推理
    //   是重叠的), 真实吞吐只能用【整圈墙钟】算。
    double wall_steady = 0.0;

    if (strat == S_DPAR) {
        // ---- 双缓冲并行 ----
        // 主线程: 预处理第 i 帧 -> 交给 worker[i%2] -> 继续预处理第 i+1 帧
        // worker0 绑 core0, worker1 绑 core1, 各跑各的上下文 -> 两个核同时满载
        struct Slot {
            std::mutex m;
            std::condition_variable cv;
            int  job = -1;
            bool have = false;     // 有活
            bool busy = false;     // 正在干
            bool ready = false;    // 上下文已建好
            bool stop = false;
        };
        std::vector<Slot> slots(2);
        std::atomic<int> nready{0};
        std::atomic<bool> failed{false};

        auto worker_loop = [&](int wi) {
            // ★ 上下文在本线程里创建和使用(RKNN 上下文不能跨线程用, 会死锁)
            if (!make_ctx_in_thread(insts[wi], model_data, static_cast<uint32_t>(msize),
                                    model_w, model_h, cmasks[wi], attr_logic, use_native)) {
                failed = true;
                return;
            }
            {
                std::lock_guard<std::mutex> lk(slots[wi].m);
                slots[wi].ready = true;
                nready++;
            }
            slots[wi].cv.notify_all();
            for (;;) {
                int idx = -1;
                {
                    std::unique_lock<std::mutex> lk(slots[wi].m);
                    slots[wi].cv.wait(lk, [&] { return slots[wi].have || slots[wi].stop; });
                    if (slots[wi].stop) return;
                    idx = slots[wi].job;
                    slots[wi].have = false;
                    slots[wi].busy = true;
                }
                double tr = 0, tg = 0, tp = 0;
                infer(insts[wi], imgs[idx], &tr, &tg, &tp);
                {
                    std::lock_guard<std::mutex> lk(slots[wi].m);
                    st.push(tr, tg, 0, 0);     // 推理耗时由 worker 统计
                    slots[wi].busy = false;
                }
                slots[wi].cv.notify_all();
            }
        };

        std::thread w0(worker_loop, 0);
        std::thread w1(worker_loop, 1);
        // 等两个 worker 就绪
        for (int i = 0; i < 400 && nready.load() < 2; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (failed || nready.load() < 2) {
            fprintf(stderr, "[bench] 工作线程启动失败\n");
            return 1;
        }

        double t_steady0 = 0;
        for (int i = 0; i < total; i++) {
            const int wi = i % 2;
            const int idx = i % static_cast<int>(imgs.size());
            if (i == a.warmup) t_steady0 = now_ms();
            // 等这个 worker 把手上的活干完(否则会覆写同一个输入缓冲)
            {
                std::unique_lock<std::mutex> lk(slots[wi].m);
                slots[wi].cv.wait(lk, [&] { return !slots[wi].busy; });
            }
            const double pre_ms = preprocess(imgs[idx], insts[wi]);
            {
                std::lock_guard<std::mutex> lk(slots[wi].m);
                slots[wi].job = idx;
                slots[wi].have = true;
            }
            slots[wi].cv.notify_all();
            if (i >= a.warmup) st.pre += pre_ms;
        }
        // 收尾: 等两个 worker 都空
        for (int wi = 0; wi < 2; wi++) {
            std::unique_lock<std::mutex> lk(slots[wi].m);
            slots[wi].cv.wait(lk, [&] { return !slots[wi].busy; });
            slots[wi].stop = true;
            slots[wi].cv.notify_all();
        }
        w0.join();
        w1.join();
        wall_steady = now_ms() - t_steady0;
    } else {
        double t_steady0 = 0;
        for (int i = 0; i < total; i++) {
            cv::Mat& bgr = imgs[i % imgs.size()];
            const int inst_idx = (strat == S_DSEQ) ? (i % 2) : 0;
            if (i == a.warmup) t_steady0 = now_ms();
            const double pre_ms = preprocess(bgr, insts[inst_idx]);
            double tr = 0, tg = 0, tp = 0;
            infer(insts[inst_idx], bgr, &tr, &tg, &tp);
            if (i >= a.warmup) {
                st.push(tr, tg, pre_ms, tp);
            }
        }
        wall_steady = now_ms() - t_steady0;
    }

    const double total_ms = st.m_run() + st.m_get() + st.m_pre() + st.m_post();
    const double fps = (wall_steady > 0.0) ? (st.n * 1000.0 / wall_steady) : 0.0;
    printf("strategy=%s tag=%s frames=%ld rknn_run=%.3fms outputs_get=%.3fms "
           "preprocess=%.3fms stages_sum=%.3fms wall_frame=%.3fms fps=%.2f\n",
           a.strategy.c_str(), a.tag.c_str(), st.n, st.m_run(), st.m_get(),
           st.m_pre(), total_ms, wall_steady / std::max(1L, st.n), fps);
    fflush(stdout);

    for (auto& inst : insts) free_instance(inst);
    return 0;
}
