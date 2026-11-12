// rk_base/fd_ledger.cc —— fd 台账实现
//
// 对账原理:
//   台账里的每个 fd 都应该在 /proc/self/fd 里能找到; 反之 /proc/self/fd 里
//   出现的每个 fd 都应该在台账里。两者取差集, 就能发现:
//     A) 未登记的 fd  -> 有代码绕过了台账(可能是泄漏)
//     B) 已消失的 fd  -> 有人提前 close 了本进程仍在使用的 fd(P0-1 的特征)
//
// 注意: /proc/self/fd 扫描时自身会打开一个目录 fd, 扫描前要把这个 fd 也算进来,
//       否则每次对账都会多报一个"未登记 fd"。

#include "rk/fd_ledger.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include "rk/log.h"

namespace rk {
namespace {

struct FdEntry {
    std::string tag;
    size_t bytes = 0;
    uint32_t thread_id = 0;
    uint64_t seq = 0;
};

struct LedgerState {
    std::mutex mu;
    std::map<int, FdEntry> entries;
    std::set<int> baseline;      // 初始化完成后采集的"已知 fd"
    uint64_t seq = 0;
    size_t untracked = 0;
    size_t vanished = 0;
};

LedgerState& state() {
    static LedgerState s;
    return s;
}

uint32_t tid() {
    return static_cast<uint32_t>(syscall(SYS_gettid));
}

}  // namespace

int FdLedger::open_tracked(const char* path, int flags, const char* tag) {
    int fd = ::open(path, flags);
    if (fd < 0) {
        RK_LOG_ERROR("fd", "open", "open(%s) 失败: %s", path ? path : "(null)",
                     strerror(errno));
        return -1;
    }
    track(fd, tag, fstat_size(fd));
    return fd;
}

void FdLedger::track(int fd, const char* tag, size_t size) {
    if (fd < 0) return;
    LedgerState& st = state();
    std::lock_guard<std::mutex> lk(st.mu);
    FdEntry e;
    e.tag = tag ? tag : "-";
    e.bytes = size;
    e.thread_id = tid();
    e.seq = ++st.seq;
    st.entries[fd] = e;
    RK_LOG_INFO("fd", "track", "登记 fd=%d tag=%s size=%zu tid=%u", fd, e.tag.c_str(),
                size, e.thread_id);
}

int FdLedger::dup_tracked(int fd, const char* tag) {
    int n = ::dup(fd);
    if (n < 0) {
        RK_LOG_ERROR("fd", "dup", "dup(%d) 失败: %s", fd, strerror(errno));
        return -1;
    }
    track(n, tag, fstat_size(n));
    return n;
}

void FdLedger::close_tracked(int fd, const char* tag) {
    if (fd < 0) return;
    LedgerState& st = state();
    {
        std::lock_guard<std::mutex> lk(st.mu);
        auto it = st.entries.find(fd);
        if (it != st.entries.end()) {
            RK_LOG_INFO("fd", "close", "注销并关闭 fd=%d tag=%s seq=%llu", fd,
                        it->second.tag.c_str(),
                        static_cast<unsigned long long>(it->second.seq));
            st.entries.erase(it);
        } else {
            RK_LOG_WARN("fd", "close", "关闭了未登记的 fd=%d tag=%s", fd,
                        tag ? tag : "-");
        }
    }
    ::close(fd);
}

size_t FdLedger::fstat_size(int fd) {
    struct stat st {};
    if (fd < 0 || ::fstat(fd, &st) != 0) return 0;
    return static_cast<size_t>(st.st_size);
}

bool FdLedger::validate(int fd, size_t need_bytes, const char* tag) {
    if (fd < 0) {
        RK_LOG_ERROR("fd", "validate", "fd 无效 (fd=%d tag=%s) —— 拒绝提交给硬件",
                     fd, tag ? tag : "-");
        return false;
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        RK_LOG_ERROR("fd", "validate",
                     "fstat 失败 fd=%d tag=%s: %s —— fd 可能已被关闭或复用",
                     fd, tag ? tag : "-", strerror(errno));
        return false;
    }
    const size_t real = static_cast<size_t>(st.st_size);
    if (real < need_bytes) {
        RK_LOG_ERROR("fd", "validate",
                     "尺寸不足 fd=%d tag=%s 实际 %zu B < 需要 %zu B —— 拒绝提交给硬件",
                     fd, tag ? tag : "-", real, need_bytes);
        return false;
    }
    return true;
}

void FdLedger::capture_baseline() {
    LedgerState& st = state();
    std::set<int> sys_fds;
    DIR* d = ::opendir("/proc/self/fd");
    const int dir_fd = d ? ::dirfd(d) : -1;
    if (d) {
        struct dirent* de = nullptr;
        while ((de = ::readdir(d)) != nullptr) {
            if (de->d_name[0] == '.') continue;
            const int fd = atoi(de->d_name);
            if (fd == dir_fd) continue;
            sys_fds.insert(fd);
        }
        ::closedir(d);
    }
    std::lock_guard<std::mutex> lk(st.mu);
    // 取并集而不是覆盖: main 初始化时采集一次(stdio/日志),
    // 单元初始化完成后再采集一次(RKNN runtime 内部 fd)。
    st.baseline.insert(sys_fds.begin(), sys_fds.end());
    RK_LOG_INFO("fd", "baseline", "采集基线: 累计 %zu 个 fd 记为已知(含 stdio/日志/runtime 内部)",
                st.baseline.size());
}

void FdLedger::dump_open() {
    LedgerState& st = state();

    // 1) 扫 /proc/self/fd
    std::set<int> sys_fds;
    DIR* d = ::opendir("/proc/self/fd");
    int dir_fd = d ? ::dirfd(d) : -1;
    if (d) {
        struct dirent* de = nullptr;
        while ((de = ::readdir(d)) != nullptr) {
            if (de->d_name[0] == '.') continue;
            const int fd = atoi(de->d_name);
            if (fd == dir_fd) continue;  // 扫描目录自身占用的 fd, 不算未登记
            sys_fds.insert(fd);
        }
        ::closedir(d);
    }

    std::lock_guard<std::mutex> lk(st.mu);

    // 2) 台账里登记了, 但系统里已经没有 -> 有人提前 close 了
    size_t vanished = 0;
    for (const auto& kv : st.entries) {
        if (sys_fds.find(kv.first) == sys_fds.end()) {
            ++vanished;
            RK_LOG_ERROR("fd", "audit",
                         "台账中的 fd=%d (tag=%s size=%zu) 在 /proc/self/fd 中已消失"
                         " —— 该 fd 被其它代码提前关闭, 继续使用会导致映射到别的对象!",
                         kv.first, kv.second.tag.c_str(), kv.second.bytes);
        }
    }

    // 3) 系统里有, 台账没有, 且不在基线里 -> 初始化之后新出现的, 才是真嫌疑
    size_t untracked = 0;
    for (int fd : sys_fds) {
        if (st.entries.find(fd) != st.entries.end()) continue;
        if (st.baseline.count(fd)) continue;
        ++untracked;
        char target[256] = {0};
        ::snprintf(target, sizeof(target), "/proc/self/fd/%d", fd);
        char real[256] = {0};
        ssize_t m = ::readlink(target, real, sizeof(real) - 1);
        const char* name = (m > 0) ? real : "(读取失败)";
        // ★ runtime 内部管理的 fd(MPP 的 dma-buf 池、RGA/NPU 设备节点等)
        //   不是我们代码开的, 降级成 INFO, 免得长跑时噪音淹没真正的泄漏信号。
        const bool runtime_owned =
            (strncmp(name, "/dmabuf:", 8) == 0) ||
            (strncmp(name, "/dev/rga", 8) == 0) ||
            (strncmp(name, "/dev/mpp_service", 16) == 0) ||
            (strncmp(name, "/dev/dri/", 9) == 0) ||
            (strncmp(name, "/dev/rkvdec", 11) == 0) ||
            (strncmp(name, "anon_inode:", 11) == 0) ||
            (strncmp(name, "socket:", 7) == 0);
        if (runtime_owned) {
            RK_LOG_INFO("fd", "audit", "runtime 内部 fd=%d -> %s (非本程序登记)", fd, name);
        } else {
            RK_LOG_WARN("fd", "audit", "基线之后新出现的未登记 fd=%d -> %s "
                         "—— 本程序开的却没登记, 查漏", fd, name);
        }
    }

    st.untracked = untracked;
    st.vanished = vanished;

    RK_LOG_INFO("fd", "audit",
                "fd 台账: 登记 %zu 个 | 基线 %zu 个 | 新增未登记 %zu 个 | 已消失 %zu 个",
                st.entries.size(), st.baseline.size(), untracked, vanished);
    for (const auto& kv : st.entries) {
        RK_LOG_INFO("fd", "audit", "  fd=%-4d tag=%-16s size=%-10zu tid=%u seq=%llu",
                    kv.first, kv.second.tag.c_str(), kv.second.bytes,
                    kv.second.thread_id,
                    static_cast<unsigned long long>(kv.second.seq));
    }
}

size_t FdLedger::tracked_count() {
    LedgerState& st = state();
    std::lock_guard<std::mutex> lk(st.mu);
    return st.entries.size();
}

size_t FdLedger::untracked_count() {
    LedgerState& st = state();
    std::lock_guard<std::mutex> lk(st.mu);
    return st.untracked;
}

size_t FdLedger::vanished_count() {
    LedgerState& st = state();
    std::lock_guard<std::mutex> lk(st.mu);
    return st.vanished;
}

}  // namespace rk
