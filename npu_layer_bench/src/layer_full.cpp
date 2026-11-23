// src/layer_full.cpp —— 第 4/5 层: 全链路测试
//
// ★ 为什么第 4/5 层是"驱动生产二进制"而不是在这里重写一遍流水线:
//
//   全链路 = 解码(MPP) + RGA 预处理 + NPU 检测 + 跟踪/测速 + NPU 识别 +
//            CTC 解码 + 结果上报 + MQTT。这条链路上有【几十处】业务逻辑
//            (车牌两行切分、候选并发、每核闸门、优先级队列、fd 守卫、
//            帧缓冲组重建 ...)。
//
//   如果在压测台里重写一遍, 测到的就不是"生产那条链路的真实上限" ——
//   那是一个【简化版】, 拿来验证生产链路的 93 FPS 是不成立的。
//
//   所以第 4/5 层的做法是: 【原样跑生产二进制 rk_gate】, 只改命令行参数
//   (核绑定 / 每路帧率上限 / 时长), 然后解析它自己打的统计行。
//   ★ 主工程代码【一行都不改】, 只是把它当黑盒驱动。
//
// ★ 统计行的口径(gate_main.cc:1258-1268):
//   [ch1][frame 100] fps=18.4 deliv=19.2 src=30.0 rga=6.71ms npu=29.84ms
//       detwait=42.13ms ocrwait=96.40ms objs=3 tracks=2 spd=12.4 sv=1
//       rss=245712KB heap=74775KB
//
//   deliv   : 真实墙钟交付帧率(每路)
//   rga     : 最近 60 帧的 RGA 转换均值(含 librga 抢锁的排队)
//   npu     : 最近 60 帧的 rknn_run + rknn_outputs_get 均值(纯 NPU)
//   detwait : 从提交检测到拿到结果的【墙钟】(含 NPU 排队)
//   ocrwait : 每个【被识别的轨迹】的平均等待(不是每帧)
//
//   ★ 排队延迟 = detwait - npu —— 这就是 NPU 前面排了多少队。
#include "nlb/layers.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "nlb/sys_probe.h"

namespace nlb {
namespace {

// ---------------------------------------------------------------------------
// 统计行解析
// ---------------------------------------------------------------------------
struct GateStat {
    std::string ch;
    int frame = 0;
    double fps = 0, deliv = 0, src = 0;
    double rga = 0, npu = 0, detwait = 0, ocrwait = 0;
    int objs = 0, tracks = 0;
    double t_recv = 0;   // 父进程收到这一行的墙钟
    bool has_deliv = false;
};

// 从一行里取 "key=" 后面的浮点数。
bool grab_num(const char* line, const char* key, double* out) {
    char pat[32];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char* p = strstr(line, pat);
    if (!p) return false;
    p += strlen(pat);
    char* end = nullptr;
    const double v = strtod(p, &end);
    if (end == p) return false;
    *out = v;
    return true;
}

// 从一行里取 "key=" 后面的整数。
bool grab_int(const char* line, const char* key, int* out) {
    char pat[32];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char* p = strstr(line, pat);
    if (!p) return false;
    p += strlen(pat);
    char* end = nullptr;
    const long v = strtol(p, &end, 10);
    if (end == p) return false;
    *out = static_cast<int>(v);
    return true;
}

bool parse_stat_line(const char* line, GateStat* g) {
    // 形状: [ch1][frame 100] fps=...
    const char* lb = strchr(line, '[');
    if (!lb) return false;
    const char* rb = strchr(lb + 1, ']');
    if (!rb) return false;
    g->ch.assign(lb + 1, rb - lb - 1);

    const char* fb = strstr(line, "[frame ");
    if (!fb) return false;
    g->frame = atoi(fb + 7);

    bool any = false;
    if (grab_num(line, "fps", &g->fps)) any = true;
    if (grab_num(line, "deliv", &g->deliv)) { any = true; g->has_deliv = true; }
    if (grab_num(line, "src", &g->src)) any = true;
    if (grab_num(line, "rga", &g->rga)) any = true;
    if (grab_num(line, "npu", &g->npu)) any = true;
    if (grab_num(line, "detwait", &g->detwait)) any = true;
    if (grab_num(line, "ocrwait", &g->ocrwait)) any = true;
    grab_int(line, "objs", &g->objs);
    grab_int(line, "tracks", &g->tracks);
    return any;
}

// ---------------------------------------------------------------------------
// 资源采样线程(在子进程跑的时候采 CPU / NPU 三核 load)
// ---------------------------------------------------------------------------
class ResourceSampler {
public:
    void start() {
        stop_.store(false);
        th_ = std::thread([this]() {
            SysProbe p;
            p.sample_cpu();
            while (!stop_.load(std::memory_order_relaxed)) {
                p.sample_cpu();
                if (p.sample_npu_load()) {
                    for (int c = 0; c < 3; ++c) npu_sum_[c] += p.npu_load()[c];
                    npu_n_++;
                }
                if (p.sample_npu_freq()) {
                    const double f = p.npu_freq_mhz();
                    if (freq_min_ < 0 || f < freq_min_) freq_min_ = f;
                    if (f > freq_max_) freq_max_ = f;
                }
                cpu_sum_ += p.cpu_pct();
                cpu_n_++;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        });
    }

    void stop_and_join() {
        stop_.store(true);
        if (th_.joinable()) th_.join();
    }

    double cpu_pct() const { return cpu_n_ ? cpu_sum_ / cpu_n_ : 0.0; }
    bool npu_ok() const { return npu_n_ > 0; }

    // ★ 必须是【均值】, 不是累加和。
    //   累加和会随采样次数线性放大(60 次采样 x ~82% = 4954%),
    //   报告里就会出现"核占用 4954%"这种一眼假的数。
    double npu(int core) const {
        if (npu_n_ <= 0 || core < 0 || core > 2) return -1.0;
        return npu_sum_[core] / npu_n_;
    }

    double freq_min() const { return freq_min_; }
    double freq_max() const { return freq_max_; }

private:
    std::thread th_;
    std::atomic<bool> stop_{false};
    double cpu_sum_ = 0;
    int cpu_n_ = 0;
    double npu_sum_[3] = {0, 0, 0};
    int npu_n_ = 0;
    double freq_min_ = -1.0;
    double freq_max_ = -1.0;
};

// ---------------------------------------------------------------------------
// 跑一次生产二进制, 收集它的统计行
// ---------------------------------------------------------------------------
struct FullRun {
    std::vector<GateStat> stats;
    double cpu_pct = 0;
    bool npu_ok = false;
    double npu[3] = {0, 0, 0};
    int child_exit = -1;
    double wall_s = 0;
    double freq_min = -1.0;
    double freq_max = -1.0;
};

double now_sec() {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()) / 1000.0;
}

// argv 拼装(不经过 shell, 免得引号问题)
std::vector<std::string> build_argv(const Config& cfg, Layer layer,
                                     const std::vector<std::string>& ch_specs,
                                     int duration_sec) {
    std::vector<std::string> a;
    a.push_back(cfg.gate_bin);
    a.push_back("--model");       a.push_back(cfg.det_model);
    if (!cfg.det_dict.empty()) { a.push_back("--labels"); a.push_back(cfg.det_dict); }
    if (!cfg.ocr_model.empty()) {
        a.push_back("--ppocr");   a.push_back(cfg.ocr_model);
        if (!cfg.ocr_dict.empty()) { a.push_back("--ppocr-dict"); a.push_back(cfg.ocr_dict); }
    }
    // 核绑定: 检测与识别都用三核(现行生产方案)
    a.push_back("--det-core");   a.push_back("0,1,2");
    a.push_back("--ocr-core");   a.push_back("0,1,2");
    a.push_back("--det-workers"); a.push_back("3");
    a.push_back("--ocr-workers"); a.push_back("3");
    // ★ --max-fps 0 = 解除限帧(第 5 层要的"满负载")
    a.push_back("--max-fps");    a.push_back(std::to_string(static_cast<int>(cfg.fullchain_max_fps)));
    // 统计行密一点, 采样点够算均值
    a.push_back("--stats-interval"); a.push_back("20");
    a.push_back("--duration");   a.push_back(std::to_string(duration_sec));
    for (const auto& s : ch_specs) {
        a.push_back("--ch");
        a.push_back(s);
    }
    (void)layer;
    return a;
}

FullRun run_gate_once(const Config& cfg, Layer layer,
                       const std::vector<std::string>& ch_specs,
                       int duration_sec, bool verbose) {
    FullRun out;

    std::vector<std::string> args = build_argv(cfg, layer, ch_specs, duration_sec);
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    int fds[2];
    if (pipe(fds) != 0) {
        fprintf(stderr, "[nlb] pipe() 失败: %s\n", strerror(errno));
        return out;
    }

    // 日志降级: 帧守卫逐帧 INFO 会打出 GB 级日志, 压测时不需要
    setenv("RK_LOG_LEVEL", "warn", 1);

    const double t0 = now_sec();
    const pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[nlb] fork() 失败: %s\n", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return out;
    }
    if (pid == 0) {
        // ---- 子进程 ----
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        execv(argv[0], argv.data());
        _exit(127);
    }
    close(fds[1]);

    ResourceSampler sampler;
    sampler.start();

    // ---- 父进程: 读子进程的输出, 解析统计行 ----
    FILE* pipef = fdopen(fds[0], "r");
    if (pipef) {
        char* line = nullptr;
        size_t cap = 0;
        ssize_t n;
        while ((n = getline(&line, &cap, pipef)) > 0) {
            GateStat g;
            if (parse_stat_line(line, &g)) {
                g.t_recv = now_sec();
                out.stats.push_back(g);
                if (verbose) {
                    fprintf(stderr, "  [统计] %s frame=%d deliv=%.2f npu=%.2f detwait=%.2f\n",
                            g.ch.c_str(), g.frame, g.deliv, g.npu, g.detwait);
                }
            }
        }
        free(line);
        fclose(pipef);
    } else {
        close(fds[0]);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    sampler.stop_and_join();

    out.wall_s = now_sec() - t0;
    out.child_exit = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    out.cpu_pct = sampler.cpu_pct();
    out.npu_ok = sampler.npu_ok();
    for (int c = 0; c < 3; ++c) out.npu[c] = sampler.npu(c);
    out.freq_min = sampler.freq_min();
    out.freq_max = sampler.freq_max();
    return out;
}

// ---------------------------------------------------------------------------
// 汇总
// ---------------------------------------------------------------------------
struct ChanAgg {
    double deliv = 0;
    double fps = 0;
    double src = 0;
    double rga = 0, npu = 0, detwait = 0, ocrwait = 0;
    double queue = 0;      // detwait - npu
    int n = 0;
};

}  // namespace

void run_layer_full(const Config& cfg, Layer layer) {
    fprintf(stderr, "\n========== 第 %s 层: 全链路 ==========\n",
            layer == Layer::kL4 ? "4" : "5");
    fprintf(stderr, "生产二进制: %s\n", cfg.gate_bin.c_str());
    fprintf(stderr, "检测模型: %s\n", cfg.det_model.c_str());
    fprintf(stderr, "识别模型: %s\n", cfg.ocr_model.c_str());
    fprintf(stderr, "通道数: %zu   --max-fps: %.0f (0 = 解除限帧)\n",
            cfg.ch_specs.size(), cfg.fullchain_max_fps);
    fprintf(stderr, "每组: 重复 %d 次, 每次 %d 秒\n", cfg.repeats,
            static_cast<int>(cfg.warmup_sec + cfg.duration_sec));

    // 第 4 层只用第 1 路; 第 5 层用全部
    std::vector<std::string> specs =
        (layer == Layer::kL4) ? std::vector<std::string>{cfg.ch_specs.front()} : cfg.ch_specs;

    const int duration_sec = static_cast<int>(cfg.warmup_sec + cfg.duration_sec);

    for (int rep = 1; rep <= cfg.repeats; ++rep) {
        const FullRun fr = run_gate_once(cfg, layer, specs, duration_sec, false);

        // 只用测量窗口内的统计行(前 warmup_sec 的丢弃)
        const double t_end = fr.wall_s;
        std::vector<const GateStat*> keep;
        for (const auto& g : fr.stats) {
            // 子进程从 0 秒开始跑, 统计行是"每 N 帧一条", 没法精确对齐时间;
            // 这里按"最后 duration_sec 内的行"来取, 这是最稳的切法。
            if (g.t_recv >= t_end - cfg.duration_sec) keep.push_back(&g);
        }
        if (keep.empty()) {
            for (const auto& g : fr.stats) keep.push_back(&g);   // 兜底: 全用
        }

        // 按通道聚合
        std::vector<std::string> chans;
        for (const auto* g : keep) {
            bool found = false;
            for (const auto& c : chans) {
                if (c == g->ch) { found = true; break; }
            }
            if (!found) chans.push_back(g->ch);
        }
        std::vector<ChanAgg> agg(chans.size());

        for (const auto* g : keep) {
            size_t idx = 0;
            for (size_t i = 0; i < chans.size(); ++i) {
                if (chans[i] == g->ch) { idx = i; break; }
            }
            ChanAgg& a = agg[idx];
            a.deliv += g->deliv;
            a.fps += g->fps;
            a.src += g->src;
            a.rga += g->rga;
            a.npu += g->npu;
            a.detwait += g->detwait;
            a.ocrwait += g->ocrwait;
            a.n++;
        }

        RunResult r;
        r.layer = layer;
        r.case_name = (layer == Layer::kL4) ? "fullchain_1ch" : "fullchain_4ch";
        r.repeat = rep;
        r.duration_s = cfg.duration_sec;
        r.warmup_s = cfg.warmup_sec;

        double total_fps = 0;
        double sum_rga = 0, sum_npu = 0, sum_detwait = 0, sum_ocrwait = 0;
        int nch = 0;
        for (size_t i = 0; i < agg.size(); ++i) {
            ChanAgg& a = agg[i];
            if (a.n == 0) continue;
            a.deliv /= a.n; a.fps /= a.n; a.src /= a.n;
            a.rga /= a.n; a.npu /= a.n; a.detwait /= a.n; a.ocrwait /= a.n;
            a.queue = a.detwait - a.npu;
            if (a.queue < 0) a.queue = 0;
            total_fps += a.deliv;
            sum_rga += a.rga;
            sum_npu += a.npu;
            sum_detwait += a.detwait;
            sum_ocrwait += a.ocrwait;
            nch++;

            char key[64];
            snprintf(key, sizeof(key), "ch_%s_deliv", a.n ? chans[i].c_str() : "x");
            r.extra.emplace_back(key, std::to_string(a.deliv));
            snprintf(key, sizeof(key), "ch_%s_npu_ms", chans[i].c_str());
            r.extra.emplace_back(key, std::to_string(a.npu));
            snprintf(key, sizeof(key), "ch_%s_detwait_ms", chans[i].c_str());
            r.extra.emplace_back(key, std::to_string(a.detwait));
            snprintf(key, sizeof(key), "ch_%s_ocrwait_ms", chans[i].c_str());
            r.extra.emplace_back(key, std::to_string(a.ocrwait));
            snprintf(key, sizeof(key), "ch_%s_rga_ms", chans[i].c_str());
            r.extra.emplace_back(key, std::to_string(a.rga));
        }

        r.fps = total_fps;
        r.frames = static_cast<uint64_t>(total_fps * cfg.duration_sec);
        if (nch > 0) {
            r.rga_ms = sum_rga / nch;
            r.det_wall_ms = sum_npu / nch;
            r.det_npu_ms = sum_npu / nch;
            r.ocr_wall_ms = sum_ocrwait / nch;
            r.frame_ms = sum_detwait / nch;
            r.queue_ms = r.frame_ms - r.det_wall_ms;
            if (r.queue_ms < 0) r.queue_ms = 0;
        }
        r.cpu_pct = fr.cpu_pct;
        r.cpu_cores = fr.cpu_pct * 8.0;
        r.npu_load_ok = fr.npu_ok;
        for (int c = 0; c < 3; ++c) r.npu_load[c] = fr.npu[c];
        r.npu_freq_min_mhz = fr.freq_min;
        r.npu_freq_max_mhz = fr.freq_max;

        char note[512];
        snprintf(note, sizeof(note), "统计行 %zu 条(窗口内 %zu), 子进程退出码 %d",
                 fr.stats.size(), keep.size(), fr.child_exit);
        r.note = note;

        emit_json(r, cfg);
    }
}

}  // namespace nlb
