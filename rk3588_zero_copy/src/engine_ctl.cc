#include "engine_ctl.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "mqtt_pub.h"

namespace rk {
namespace {

// ---------------------------------------------------------------------------
// 开关状态
//
// ★ 为什么用三个独立的 atomic 而不是一把锁:
//   控制线程(MQTT)写, 主线程(解码/推理)读, 每秒好几百次。
//   一把互斥锁也能用, 但开关必须【立刻】生效, 宁可多写几行也不想在
//   热路径上引入锁竞争 —— 原子变量读一次是几条指令, 而且不会有"忘了解锁"。
// ---------------------------------------------------------------------------
std::atomic<bool> g_mpp{true};
std::atomic<bool> g_rga{true};
std::atomic<bool> g_npu{true};

// ---------------------------------------------------------------------------
// 读一个文件, 去掉首尾空白。读不到返回 false。
bool read_file_trim(const char* path, std::string* out, size_t max_len = 4096) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0 && max_len > 0) {
        // 有些节点是空的(比如 load_interval), 算读到了
        out->clear();
        return true;
    }
    buf[n] = '\0';
    // 去掉首尾空白
    char* s = buf;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') ++s;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                       s[len - 1] == '\n' || s[len - 1] == '\r')) --len;
    out->assign(s, len);
    return true;
}

// 从 "load: 12.68% utilization: 11.82%" 里抠出 12.68
bool parse_pct_after(const std::string& s, const char* key, double* out) {
    const char* p = strstr(s.c_str(), key);
    if (!p) return false;
    p += strlen(key);
    // 跳过非数字字符
    while (*p && (*p < '0' || *p > '9') && *p != '-') ++p;
    if (*p < '0' || *p > '9') return false;
    *out = atof(p);
    return true;
}

// 从 "Core0: 12% Core1: 5% Core2: 0%" 里抠出每个核
bool parse_core_pcts(const std::string& s, double* c0, double* c1, double* c2) {
    bool any = false;
    if (parse_pct_after(s, "Core0:", c0)) any = true;
    if (parse_pct_after(s, "Core1:", c1)) any = true;
    if (parse_pct_after(s, "Core2:", c2)) any = true;
    return any;
}

// rkrga 的负载格式(逐行):
//     scheduler[0]: rga3
//          load = 3%
// 注意:
//   1) 是 "load = 3%" 不是 "load: 3%" —— 不能直接用 parse_pct_after
//   2) 文件头还有一行 "================= load ==================", 用"跳过非数字"
//      的办法会一路跳到 scheduler[0] 的 0, 取到错值
// 所以逐行找 "load" + 空白 + "=" + 空白 + 数字, 取【最后一个】有效值
// (最后一行对应最后一个 scheduler)
bool parse_pct_eq(const std::string& s, double* out) {
    double best = -1.0;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t eol = s.find('\n', pos);
        if (eol == std::string::npos) eol = s.size();
        std::string line = s.substr(pos, eol - pos);
        const char* p = strstr(line.c_str(), "load");
        if (p) {
            p += 4;
            while (*p == ' ' || *p == '\t') ++p;
            if (*p == '=') {
                ++p;
                while (*p == ' ' || *p == '\t') ++p;
                if (*p >= '0' && *p <= '9') best = atof(p);
            }
        }
        pos = eol + 1;
    }
    if (best < 0) return false;
    *out = best;
    return true;
}

// devfreq 的 load 文件格式: "12@2400000000Hz" -> 12
bool parse_devfreq_load(const std::string& s, double* pct, double* hz) {
    const char* p = s.c_str();
    char* end = nullptr;
    const double v = strtod(p, &end);
    if (end == p) return false;
    if (pct) *pct = v;
    if (hz) {
        const char* at = strchr(p, '@');
        *hz = at ? atof(at + 1) : 0.0;
    }
    return true;
}

// CPU 占用率: 靠 /proc/stat 两次采样的差值
struct CpuSampler {
    unsigned long long total = 0;
    unsigned long long idle = 0;
    bool primed = false;

    // 返回 -1 表示还没采够两次
    double sample() {
        std::string s;
        if (!read_file_trim("/proc/stat", &s)) return -1.0;
        // "cpu  user nice system idle iowait irq softirq steal ..."
        if (s.compare(0, 3, "cpu") != 0) return -1.0;
        unsigned long long vals[10] = {0};
        int n = 0;
        const char* p = s.c_str() + 3;
        while (n < 10) {
            while (*p == ' ' || *p == '\t') ++p;
            if (*p < '0' || *p > '9') break;
            vals[n++] = strtoull(p, nullptr, 10);
            while (*p >= '0' && *p <= '9') ++p;
        }
        if (n < 5) return -1.0;
        unsigned long long t = 0;
        for (int i = 0; i < n; ++i) t += vals[i];
        const unsigned long long idle_all = vals[3] + vals[4];   // idle + iowait
        double pct = -1.0;
        if (primed && t > total) {
            const unsigned long long dt = t - total;
            const unsigned long long di = (idle_all > idle) ? (idle_all - idle) : 0;
            pct = 100.0 * (1.0 - static_cast<double>(di) / static_cast<double>(dt));
            if (pct < 0.0) pct = 0.0;
            if (pct > 100.0) pct = 100.0;
        }
        total = t;
        idle = idle_all;
        primed = true;
        return pct;
    }
};

CpuSampler g_cpu_sampler;

}  // namespace

// ---------------------------------------------------------------------------
// 一、运行时开关
// ---------------------------------------------------------------------------

void engine_init_switches(bool mpp, bool rga, bool npu) {
    g_mpp.store(mpp);
    g_rga.store(rga);
    g_npu.store(npu);
}

const EngineSwitches& engine_switches() {
    // 用一个静态对象把三个原子值打包返回(读三次不会撕裂: 都是独立的 bool,
    // 最多出现"这一瞬间 mpp 刚切、npu 还没切"的不一致, 下一次读就一致了)
    static thread_local EngineSwitches s;
    s.mpp = g_mpp.load(std::memory_order_relaxed);
    s.rga = g_rga.load(std::memory_order_relaxed);
    s.npu = g_npu.load(std::memory_order_relaxed);
    return s;
}

bool engine_set_mpp(bool on) {
    const bool old = g_mpp.exchange(on);
    return old != on;
}

bool engine_set_rga(bool on) {
    const bool old = g_rga.exchange(on);
    return old != on;
}

bool engine_set_npu(bool on) {
    const bool old = g_npu.exchange(on);
    return old != on;
}

// ---------------------------------------------------------------------------
// 二、硬件负载
// ---------------------------------------------------------------------------

HwLoad read_hw_load() {
    HwLoad h;
    std::string s;

    // ---- NPU ----
    // ① /sys/kernel/debug/rknpu/load —— 【真】的单核占用, 但要 root。
    //    进程是普通用户时读不到, 走板端的 hw_load_mqtt.py(root)补上。
    if (read_file_trim("/sys/kernel/debug/rknpu/load", &s)) {
        double c0 = -1, c1 = -1, c2 = -1;
        if (parse_core_pcts(s, &c0, &c1, &c2)) {
            double mx = 0.0;
            if (c0 >= 0) mx = c0;
            if (c1 > mx) mx = c1;
            if (c2 > mx) mx = c2;
            h.npu_core = mx;
            h.npu = mx;
            // ★ 分核原值也留着(网页上要画 Core0/Core1/Core2 三根柱子)
            h.npu_c0 = c0;
            h.npu_c1 = c1;
            h.npu_c2 = c2;
            h.npu_src = "rknpu";
        }
    }
    // ② /sys/class/devfreq/fdab0000.npu/load —— 不用 root, 但这台板子上
    //    它【恒等于 100%】(空闲时也是 100), 所以只能当"参考", 不能当负载。
    //    读不到内核真值时才用它兜底, 并在 npu_src 里说清楚来源。
    if (h.npu < 0) {
        if (read_file_trim("/sys/class/devfreq/fdab0000.npu/load", &s)) {
            double pct = -1, hz = 0;
            if (parse_devfreq_load(s, &pct, &hz) && pct >= 0) {
                h.npu = pct;
                h.npu_src = "devfreq";
            }
        }
    }
    if (h.npu_src.empty()) h.npu_src = "none";

    // ---- RGA ----
    // RGA 的负载只在 debugfs 里, 同样要 root。
    // ★ 这块板子的内核节点是 /sys/kernel/debug/rkrga/load(不是 rga/load),
    //   而且格式是 " load = 3%"(空格 + 等号), 不是 "load: 3%"。
    //   之前路径写错 -> RGA 负载永远是 -1, 网页上的 RGA 曲线是空的。
    if (read_file_trim("/sys/kernel/debug/rkrga/load", &s)) {
        double v = -1;
        if (parse_pct_after(s, "load:", &v) || parse_pct_after(s, "Load:", &v) ||
            parse_pct_eq(s, &v)) {
            h.rga = v;
            h.rga_src = "debugfs";
        }
    }
    if (h.rga_src.empty()) h.rga_src = "none";

    // ---- VPU(MPP 解码核) ----
    // /proc/mpp_service/load 是 world-readable 的, 不用 root。
    if (read_file_trim("/proc/mpp_service/load", &s)) {
        // 形如: "fdc38100.rkvdec-core      load:  12.68% utilization:  11.82%"
        double mx = -1.0;
        size_t pos = 0;
        while (pos < s.size()) {
            size_t nl = s.find('\n', pos);
            if (nl == std::string::npos) nl = s.size();
            const std::string line = s.substr(pos, nl - pos);
            if (line.find("rkvdec") != std::string::npos) {
                double v = -1;
                if (parse_pct_after(line, "load:", &v) && v >= 0 && v > mx) mx = v;
            }
            pos = nl + 1;
        }
        h.vpu = mx;
    }

    // ---- CPU ----
    h.cpu = g_cpu_sampler.sample();

    // ---- DMC / GPU ----
    if (read_file_trim("/sys/class/devfreq/dmc/load", &s)) {
        double pct = -1, hz = 0;
        if (parse_devfreq_load(s, &pct, &hz) && pct >= 0) h.dmc = pct;
    }
    if (read_file_trim("/sys/class/devfreq/fb000000.gpu-panthor/load", &s)) {
        double pct = -1, hz = 0;
        if (parse_devfreq_load(s, &pct, &hz) && pct >= 0) h.gpu = pct;
    }
    return h;
}

// ---------------------------------------------------------------------------
// 三、状态上报器
// ---------------------------------------------------------------------------

struct StatusPublisher::Impl {
    std::string host, topic, client_id;
    int  port = 1883;
    int  qos = 0;
    int  interval_ms = 1000;

    std::mutex m;
    PerfSnapshot snap;

    std::thread th;
    std::atomic<bool> quit{false};
    MqttPublisher pub;

    bool running() const { return th.joinable(); }
};

StatusPublisher::~StatusPublisher() { stop(); }

bool StatusPublisher::start(const std::string& host, int port, const std::string& topic,
                            const std::string& client_id, int qos, int interval_ms) {
    if (!impl_) impl_ = new Impl();
    impl_->host = host;
    impl_->port = port;
    impl_->topic = topic;
    impl_->client_id = client_id;
    impl_->qos = qos;
    impl_->interval_ms = interval_ms > 100 ? interval_ms : 100;
    impl_->quit.store(false);

    if (impl_->th.joinable()) impl_->th.join();

    impl_->th = std::thread([this]() {
        Impl* im = impl_;
        double last_pub = 0.0;
        auto t0 = std::chrono::steady_clock::now();
        try {
        while (!im->quit.load()) {
            // ---- 发一包(最多每 interval_ms 一次) ----
            const double now = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now - last_pub >= im->interval_ms / 1000.0) {
                last_pub = now;

                PerfSnapshot s;
                {
                    std::lock_guard<std::mutex> lk(im->m);
                    s = im->snap;
                }
                const HwLoad ld = read_hw_load();
                const EngineSwitches sw = engine_switches();

                char buf[1400];
                snprintf(buf, sizeof(buf),
                         "{\"sn\":\"%s\",\"ts\":%.3f,\"frames\":%ld,"
                         "\"fps\":%s,\"src_fps\":%s,"
                         "\"wait_ms\":%s,\"dec_ms\":%s,\"rga_ms\":%s,\"npu_ms\":%s,"
                         "\"post_ms\":%s,\"draw_ms\":%s,\"total_ms\":%s,"
                         "\"n_objs\":%d,"
                         "\"engine\":{\"mpp\":%s,\"rga\":%s,\"npu\":%s},"
                         "\"decoder\":\"%s\",\"rga_mode\":\"%s\","
                         "\"load\":{\"npu\":%s,\"npu_core\":%s,\"npu_src\":\"%s\","
                         "\"npu_c0\":%s,\"npu_c1\":%s,\"npu_c2\":%s,"
                         "\"rga\":%s,\"rga_src\":\"%s\",\"vpu\":%s,\"cpu\":%s,"
                         "\"dmc\":%s,\"gpu\":%s}}",
                         s.sn.c_str(),
                         static_cast<double>(std::chrono::duration<double>(
                             std::chrono::system_clock::now().time_since_epoch()).count()),
                         s.frames,
                         json_num(s.fps).c_str(), json_num(s.src_fps).c_str(),
                         json_num(s.wait_ms).c_str(), json_num(s.dec_ms).c_str(),
                         json_num(s.rga_ms).c_str(), json_num(s.npu_ms).c_str(),
                         json_num(s.post_ms).c_str(), json_num(s.draw_ms).c_str(),
                         json_num(s.total_ms).c_str(),
                         s.n_objs,
                         sw.mpp ? "true" : "false",
                         sw.rga ? "true" : "false",
                         sw.npu ? "true" : "false",
                         s.decoder.c_str(), s.rga_mode.c_str(),
                         json_num_opt(ld.npu).c_str(), json_num_opt(ld.npu_core).c_str(),
                         ld.npu_src.c_str(),
                         json_num_opt(ld.npu_c0).c_str(), json_num_opt(ld.npu_c1).c_str(),
                         json_num_opt(ld.npu_c2).c_str(),
                         json_num_opt(ld.rga).c_str(), ld.rga_src.c_str(),
                         json_num_opt(ld.vpu).c_str(), json_num_opt(ld.cpu).c_str(),
                         json_num_opt(ld.dmc).c_str(), json_num_opt(ld.gpu).c_str());

                // 没连上就试一次(带超时); broker 晚起/网络抖动都能自己恢复
                if (!im->pub.connected()) {
                    if (!im->pub.ensure_connected(im->host, im->port, im->client_id, 2000)) {
                        // 连不上不是致命错误 —— 下一轮再试
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        continue;
                    }
                }
                if (!im->pub.publish(im->topic, buf, im->qos)) {
                    fprintf(stderr, "[状态] 上报失败: %s\n", im->pub.last_error().c_str());
                }
            }
            // 200ms 一跳: 退出要快, 发包间隔由上面控制
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        } catch (const std::exception& e) {
            // ★ 线程里绝不能有异常逃出去: 一旦逃出去就是 std::terminate,
            //   整个视频流水线跟着陪葬。状态上报只是"锦上添花", 挂了也不能影响推理。
            fprintf(stderr, "[状态] 上报线程异常: %s (已停止上报, 推理不受影响)\n", e.what());
        }
        im->pub.disconnect();
        (void)t0;
    });
    return true;
}

void StatusPublisher::update(const PerfSnapshot& s) {
    if (!impl_) return;
    std::lock_guard<std::mutex> lk(impl_->m);
    impl_->snap = s;
}

void StatusPublisher::stop() {
    if (!impl_) return;
    impl_->quit.store(true);
    if (impl_->th.joinable()) impl_->th.join();
    impl_->pub.disconnect();
}

bool StatusPublisher::connected() const {
    return impl_ && impl_->pub.connected();
}

// ---------------------------------------------------------------------------
// 四、极简 JSON 工具
// ---------------------------------------------------------------------------

namespace {

// 找到 "key" 这个【键】的【值】的起始位置。
//
// 返回 false 表示报文里没有这个键。
//
// ★ 关键: 返回的是【值】的位置, 不是键的位置 —— 中间要跳过
//   空白 + ':' + 空白。以前直接拿"键的位置 + 键长 + 2"当值的位置,
//   结果正好落在 ':' 上, 于是 compare(":tru","true") 永远不相等 ——
//   所有开关字段都被当成"没找到"(踩过, 2026-11-15)。
bool find_value(const std::string& s, const char* key, size_t* value_pos) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t pos = s.find(pat);
    while (pos != std::string::npos) {
        size_t i = pos + pat.size();
        // 跳过键后面的空白
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' ||
                                s[i] == '\n' || s[i] == '\r')) ++i;
        if (i < s.size() && s[i] == ':') {
            ++i;                       // 跳过 ':'
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t' ||
                                    s[i] == '\n' || s[i] == '\r')) ++i;
            *value_pos = i;
            return true;
        }
        pos = s.find(pat, pos + 1);
    }
    return false;
}

}  // namespace

bool json_get_bool(const std::string& s, const char* key, bool* out, bool* found) {
    if (found) *found = false;
    size_t i = 0;
    if (!find_value(s, key, &i)) return false;
    if (i >= s.size()) return false;
    if (s.compare(i, 4, "true") == 0) {
        if (out) *out = true;
        if (found) *found = true;
        return true;
    }
    if (s.compare(i, 5, "false") == 0) {
        if (out) *out = false;
        if (found) *found = true;
        return true;
    }
    // 也接受 1/0(有些前端会把布尔写成数字)
    if (s[i] == '1' || s[i] == '0') {
        if (out) *out = (s[i] == '1');
        if (found) *found = true;
        return true;
    }
    return false;
}

bool json_get_string(const std::string& s, const char* key, std::string* out,
                     bool* found) {
    if (found) *found = false;
    size_t i = 0;
    if (!find_value(s, key, &i)) return false;
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    std::string v;
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) ++i;   // 简单转义
        v.push_back(s[i]);
        ++i;
    }
    if (i >= s.size()) return false;
    if (out) *out = v;
    if (found) *found = true;
    return true;
}

std::string json_num(double v) {
    if (!(v > 0.0)) return "0";     // 负数/零/NaN 一律当 0
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", v);
    // 去掉尾零: "12.50" -> "12.5", "12.00" -> "12"
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s.empty() ? "0" : s;
}

std::string json_num_opt(double v) {
    if (!(v >= 0.0)) return "-1";   // 负数 = 读不到
    return json_num(v);
}

}  // namespace rk
