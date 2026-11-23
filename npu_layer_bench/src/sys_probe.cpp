// src/sys_probe.cpp —— 整机资源采样
#include "nlb/sys_probe.h"

#include <malloc.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace nlb {

std::string SysProbe::npu_load_path_ = "/sys/kernel/debug/rknpu/load";
std::string SysProbe::npu_freq_path_ = "/sys/class/devfreq/fdab0000.npu/cur_freq";

bool SysProbe::sample_npu_freq() {
    FILE* f = fopen(npu_freq_path_.c_str(), "r");
    if (!f) return false;
    long long hz = 0;
    if (fscanf(f, "%lld", &hz) != 1) {
        fclose(f);
        return false;
    }
    fclose(f);
    freq_mhz_ = static_cast<double>(hz) / 1e6;
    return true;
}

const std::string& SysProbe::npu_load_path() { return npu_load_path_; }

void SysProbe::sample_cpu() {
    cpu_count_ = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
    if (cpu_count_ <= 0) cpu_count_ = 8;

    // /proc/stat 第一行是汇总行: cpu user nice system idle iowait irq softirq steal ...
    unsigned long long u = 0, n = 0, s = 0, i = 0, w = 0, ir = 0, sr = 0, st = 0;
    FILE* f = fopen("/proc/stat", "r");
    if (!f) return;
    char line[512];
    if (fgets(line, sizeof(line), f)) {
        // ★ 必须跳过 "cpu " 这个标签再解析数字:
        //   直接 sscanf(line, "%llu ...") 会在 "cpu" 上就失败, 8 个变量全是 0,
        //   于是 total=0 -> "total > prev_total" 永远不成立 -> CPU% 永远是 0。
        const char* p = strchr(line, ' ');
        if (p) {
            sscanf(p + 1, "%llu %llu %llu %llu %llu %llu %llu %llu",
                   &u, &n, &s, &i, &w, &ir, &sr, &st);
        }
    }
    fclose(f);

    const unsigned long long total = u + n + s + i + w + ir + sr + st;
    const unsigned long long idle = i + w;

    if (have_prev_ && total > prev_total_) {
        const unsigned long long dt = total - prev_total_;
        const unsigned long long di = idle - prev_idle_;
        cpu_pct_ = 100.0 * (1.0 - static_cast<double>(di) / static_cast<double>(dt));
    }
    prev_total_ = total;
    prev_idle_ = idle;
    have_prev_ = true;
}

double SysProbe::rss_mb() const {
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f) return 0.0;
    unsigned long long size = 0, resident = 0;
    if (fscanf(f, "%llu %llu", &size, &resident) != 2) {
        fclose(f);
        return 0.0;
    }
    fclose(f);
    const long page = sysconf(_SC_PAGESIZE);
    return static_cast<double>(resident) * static_cast<double>(page) / (1024.0 * 1024.0);
}

double SysProbe::rss_peak_mb() const {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return 0.0;
    char line[256];
    double mb = 0.0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmHWM:", 6) == 0) {
            long kb = 0;
            if (sscanf(line + 6, "%ld", &kb) == 1) mb = kb / 1024.0;
            break;
        }
    }
    fclose(f);
    return mb;
}

uint64_t SysProbe::heap_bytes() {
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    struct mallinfo2 mi = mallinfo2();
    return static_cast<uint64_t>(mi.uordblks);
#else
    struct mallinfo mi = mallinfo();
    return static_cast<uint64_t>(mi.uordblks);
#endif
}

bool SysProbe::sample_npu_load() {
    npu_load_ok_ = false;
    for (int i = 0; i < 3; ++i) npu_load_[i] = -1.0;

    FILE* f = fopen(npu_load_path_.c_str(), "r");
    if (!f) return false;
    // 实际内容是一行:
    //   "NPU load:  Core0: 72%, Core1: 72%, Core2: 70%,\n"
    // ★ 所以不能拿整行去 sscanf("Core%d: %d%%") —— 行首是 "NPU load:",
    //   格式对不上, 三个核全解析不出来。改成在行里【逐个找 "Core"】。
    char line[512];
    bool got = false;
    while (fgets(line, sizeof(line), f)) {
        const char* p = line;
        while ((p = strstr(p, "Core")) != nullptr) {
            int core = -1;
            int pct = -1;
            if (sscanf(p, "Core%d: %d%%", &core, &pct) == 2 && core >= 0 && core < 3) {
                npu_load_[core] = pct;
                got = true;
            }
            p += 4;
        }
    }
    fclose(f);
    npu_load_ok_ = got;
    return got;
}

}  // namespace nlb
