// tb/sys_probe.cpp —— 系统资源采样实现
#include "tb/sys_probe.h"

#include <fcntl.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <malloc.h>

namespace tb {

namespace {

inline int64_t now_ns() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// 堆用量(KB)。glibc >= 2.33 有 mallinfo2; 老版本退回 mallinfo。
int64_t heap_in_use_kb() {
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    struct mallinfo2 mi = mallinfo2();
    return static_cast<int64_t>(mi.uordblks / 1024);
#else
    struct mallinfo mi = mallinfo();
    return static_cast<int64_t>(static_cast<unsigned>(mi.uordblks) / 1024);
#endif
}

}  // namespace

SysProbe::SysProbe() = default;

SysProbe::~SysProbe() { close(); }

long SysProbe::clk_tck() {
    static const long t = sysconf(_SC_CLK_TCK);
    return t > 0 ? t : 100;
}

bool SysProbe::open() {
    close();
    fd_stat_ = ::open("/proc/self/stat", O_RDONLY | O_CLOEXEC);
    fd_status_ = ::open("/proc/self/status", O_RDONLY | O_CLOEXEC);

    // ---- NPU load 节点 ----
    // 显式指定优先; 否则按候选列表探测。探测不到就当没有这个指标(不影响主指标)。
    if (!npu_load_path_.empty()) {
        const int fd = ::open(npu_load_path_.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ::close(fd);
            return true;
        }
        npu_load_path_.clear();
        return false;
    }
    static const char* kCandidates[] = {
        "/sys/kernel/debug/rknpu/load",
        "/sys/kernel/debug/rknpu/load_without_perf",
        "/sys/kernel/debug/rknpu/load_extend",
    };
    for (const char* p : kCandidates) {
        const int fd = ::open(p, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ::close(fd);
            npu_load_path_ = p;
            break;
        }
    }
    return !npu_load_path_.empty();
}

void SysProbe::close() {
    if (fd_stat_ >= 0) { ::close(fd_stat_); fd_stat_ = -1; }
    if (fd_status_ >= 0) { ::close(fd_status_); fd_status_ = -1; }
}

bool SysProbe::read_proc_stat(uint64_t* ticks) {
    if (fd_stat_ < 0) return false;
    char buf[2048];
    const ssize_t n = ::pread(fd_stat_, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return false;
    buf[n] = '\0';

    // 格式: pid (comm) state ppid pgrp session tty_nr tpgid flags
    //        minflt cminflt majflt cmajflt utime stime ...
    // comm 里可能有空格甚至括号, 所以从【最后一个 ')'】后面开始数。
    char* p = strrchr(buf, ')');
    if (!p) return false;
    ++p;

    // 字段序号(0 基): state=0 ... minflt=7 cminflt=8 majflt=9 cmajflt=10
    //                 utime=11 stime=12
    int field = 0;
    uint64_t utime = 0, stime = 0;
    while (*p) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        char* end = nullptr;
        const unsigned long long v = strtoull(p, &end, 10);
        if (end == p) { ++p; continue; }   // 非数字字段, 跳一个字符
        if (field == 11) utime = v;
        else if (field == 12) stime = v;
        ++field;
        p = end;
        if (field > 12) break;
    }
    *ticks = utime + stime;
    return true;
}

bool SysProbe::read_proc_status(int64_t* rss_kb, int64_t* hwm_kb) {
    if (fd_status_ < 0) return false;
    char buf[4096];
    const ssize_t n = ::pread(fd_status_, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return false;
    buf[n] = '\0';

    *rss_kb = 0;
    *hwm_kb = 0;
    char* p = buf;
    while (p && *p) {
        if (strncmp(p, "VmRSS:", 6) == 0) {
            *rss_kb = strtoll(p + 6, nullptr, 10);
        } else if (strncmp(p, "VmHWM:", 6) == 0) {
            *hwm_kb = strtoll(p + 6, nullptr, 10);
        }
        char* nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }
    return *rss_kb > 0;
}

bool SysProbe::read_npu_load(double out[3]) {
    if (npu_load_path_.empty()) return false;
    // 这个节点是动态的, 每次重新 open/read/close(系统调用, 不分配堆内存)
    const int fd = ::open(npu_load_path_.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[1024];
    const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';

    // 转小写再匹配(驱动的输出格式各种各样, 大小写不统一)
    char low[1024];
    const size_t len = static_cast<size_t>(n) < sizeof(low) - 1
                            ? static_cast<size_t>(n)
                            : sizeof(low) - 1;
    for (size_t i = 0; i < len; ++i) {
        low[i] = static_cast<char>(tolower(static_cast<unsigned char>(buf[i])));
    }
    low[len] = '\0';

    // 典型格式: "NPU load:  Core0:  0%    Core1:  0%    Core2:  0%"
    bool any = false;
    for (int c = 0; c < 3; ++c) {
        char pat[8];
        snprintf(pat, sizeof(pat), "core%d", c);
        const char* p = strstr(low, pat);
        if (!p) continue;
        p += strlen(pat);
        while (*p && (*p < '0' || *p > '9')) ++p;
        if (!*p) continue;
        out[c] = strtod(p, nullptr);
        any = true;
    }
    return any;
}

bool SysProbe::sample(ProbeSample* out) {
    ProbeSample s;
    s.ts_ns = now_ns();
    read_proc_stat(&s.cpu_ticks);
    read_proc_status(&s.rss_kb, &s.hwm_kb);
    s.heap_kb = heap_in_use_kb();
    for (int c = 0; c < 3; ++c) s.npu_load[c] = -1.0;
    if (!npu_load_path_.empty()) {
        double load[3] = {-1.0, -1.0, -1.0};
        if (read_npu_load(load)) {
            for (int c = 0; c < 3; ++c) s.npu_load[c] = load[c];
        }
    }
    *out = s;
    return true;
}

int64_t SysProbe::mem_available_kb() {
    const int fd = ::open("/proc/meminfo", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char buf[2048];
    const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    const char* p = strstr(buf, "MemAvailable:");
    if (!p) return -1;
    return strtoll(p + 13, nullptr, 10);
}

std::vector<int> SysProbe::detect_big_cores() {
    // RK3588: cpu0-3 = A55(capacity 422), cpu4-7 = A76(capacity 1024)。
    // 用 cpu_capacity 取最大值的那些核 —— 比硬编码 cpu4-7 更稳。
    std::vector<int> cpus;
    int best = -1;
    long caps[16];
    int n = 0;
    for (int i = 0; i < 16; ++i) {
        char path[128];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpu_capacity", i);
        const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char b[32];
        const ssize_t r = ::read(fd, b, sizeof(b) - 1);
        ::close(fd);
        if (r <= 0) continue;
        b[r] = '\0';
        caps[n] = strtol(b, nullptr, 10);
        if (caps[n] > best) best = static_cast<int>(caps[n]);
        cpus.push_back(i);
        ++n;
        if (n >= 16) break;
    }
    std::vector<int> big;
    for (int i = 0; i < n; ++i) {
        if (caps[i] == best) big.push_back(cpus[i]);
    }
    if (big.empty()) {
        // 读不到 capacity 就退回"后一半是大核"的常规布局
        for (size_t i = cpus.size() / 2; i < cpus.size(); ++i) big.push_back(cpus[i]);
    }
    return big;
}

}  // namespace tb
