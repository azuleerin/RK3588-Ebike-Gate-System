// rk_base/log.cc —— 分级运行日志实现
//
// 落盘策略(为什么这样写):
//   - 用 write(2) 直接写 fd, 不用 stdio 的 FILE*: stdio 会缓冲, 进程被 SIGKILL
//     时缓冲区里的日志会丢, 而崩溃现场恰恰是最需要日志的时候。
//   - 每条日志一次 write(): 长度 < 4096 时对普通文件是原子的, 多线程写不会交错。
//   - WARN/ERROR 额外 fdatasync(): 保证这两级日志真正落到盘上(掉电也不丢)。
//   - 轮转: 单文件超过 max_bytes 就改名成 .1/.2/... 保留 keep_files 份。
//     必须有上限: 写满磁盘会导致整机异常, 这本身就是一种"拖死系统"。
//
// 线程安全:
//   - 单例 + mutex 保护所有状态( fd / 大小 / 计数 )
//   - 写日志本身不会被持锁者阻塞很久(单次 write + 最多一次 fdatasync)

#include "rk/log.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

namespace rk {
namespace {

constexpr uint64_t kDefaultMaxBytes = 64ull * 1024 * 1024;
constexpr int kDefaultKeepFiles = 5;
constexpr size_t kLineBufSize = 4096;

struct Logger {
    std::mutex mu;
    int fd = -1;
    uint64_t bytes = 0;
    uint64_t max_bytes = kDefaultMaxBytes;
    int keep_files = kDefaultKeepFiles;
    bool also_stdout = false;
    bool initialized = false;
    std::string path;
    std::atomic<uint64_t> counts[4] = {{0}, {0}, {0}, {0}};
};

Logger& logger() {
    static Logger g;
    return g;
}

// ---------------------------------------------------------------------------
// ★ 运行期日志限流(令牌桶) —— 防止日志写爆内存盘
//
//   为什么需要它:
//     /var/log 挂的是 47MB zram 内存盘, 写满之后应用的 write() 返回 ENOSPC ->
//     进程直接崩。而 GATE_LOG_LEVEL=info/debug 时帧守卫对【每一帧】都打日志,
//     板上实测 156 行/秒 ≈ 80 MB/小时 —— 现场忘了切盘, 十几分钟就能写满。
//
//   ★ 为什么不能只靠 systemd 的 LogRateLimitIntervalSec / LogRateLimitBurst:
//     那两条是 journald 的指令。systemd.exec(5) 原文:
//       "if you connect a service's stderr directly to a file via
//        StandardOutput=file:... or a similar setting, the rate limiting
//        will not be applied to messages written that way"
//     本项目的 stdout/stderr 是 append: 直写文件, 所以 systemd 那层【管不到】。
//     板上 A/B 实测: 同一份 unit 写 20000 行 —— append: 文件落了 20000 行(零丢弃),
//     换成 journal 只落了 125 行。
//     —— 于是限流必须做在【写之前】, 也就是 log_write() 里。
//
//   语义(令牌桶):
//     容量 = burst, 每 interval 秒补满一次 -> 稳态每 interval 秒放行 burst 行。
//     桶空时丢弃该行并计数; 每隔一个 interval 秒汇报一次被丢弃的条数。
//   环境变量:
//     RK_LOG_RATE_LIMIT_BURST=1000          桶容量(行/窗口), 0 = 不限流
//     RK_LOG_RATE_LIMIT_INTERVAL_SEC=60     窗口长度(秒), 0 = 不限流
//   ★ WARN/ERROR 与 INFO 共享同一个桶: 洪水里被挤掉的绝大多数是 INFO;
//     真出现 ERROR 风暴时也一样会被丢(但每个窗口的头 burst 条一定写得出去,
//     足以看出错误模式)。
//   ★ 限流只统计【写出去的】行, 被丢弃的不计入 log_count() —— 那个计数器的语义
//     是"写进日志的行数", 不是"产生的行数"。
// ---------------------------------------------------------------------------
struct RateLimiter {
    std::mutex mu;
    uint64_t burst = 0;
    uint64_t window_ns = 0;
    uint64_t tokens = 0;
    uint64_t last_ns = 0;
    uint64_t dropped = 0;
    uint64_t last_report_ns = 0;
    bool inited = false;
};

RateLimiter& rate_limiter() {
    static RateLimiter rl;
    return rl;
}

// 限流用单调时钟: 不受系统时间被校准/回拨影响
uint64_t now_mono_ns() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<uint64_t>(ts.tv_nsec);
}

pid_t current_tid() {
    return static_cast<pid_t>(syscall(SYS_gettid));
}

const char* level_name(LogLevel l) {
    switch (l) {
        case LogLevel::kInfo:  return "INFO";
        case LogLevel::kWarn:  return "WARN";
        case LogLevel::kError: return "ERROR";
        // kOff 只是"过滤阈值", 不会有 kOff 级别的日志被写出来;
        // 但枚举里既然有它, switch 就得把分支补齐 —— 少了会报
        // "enumeration value 'kOff' not handled in switch [-Wswitch]"。
        case LogLevel::kOff:   return "OFF";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// 丢掉一个文件的 page cache
//
// 为什么要做:
//   日志是 write(2) 写普通文件 -> 数据先进 page cache, 由内核异步回写磁盘。
//   这部分内存【不算本进程的 RSS】(RSS 只统计映射进本进程地址空间的页),
//   但它【算整机内存】。长跑几小时日志能到几百 MB, 老文件轮转出去以后
//   page cache 还挂着, 会挤占整机内存, 极端时触发内核回收影响业务。
//
//   posix_fadvise(POSIX_FADV_DONTNEED) 通知内核"这个文件的数据我不要了",
//   让它立刻把干净页丢掉。不需要 root, 也不会丢数据(文件内容在磁盘上)。
//
// ★ 为什么不用 O_DIRECT:
//   O_DIRECT 要求【写长度是块大小的整数倍】且【缓冲区地址对齐】, 而日志行
//   长度是随机的。要用 O_DIRECT 就得自己维护一个对齐的攒批缓冲 + 处理
//   收尾时那半块, 复杂且容易丢日志。
//   关键是: O_DIRECT 解决的是 page cache 不落地, 而客户要的"RSS 直线"
//   跟 page cache 没关系(page cache 不在 RSS 里) —— 真正影响 RSS 的是
//   glibc 的 malloc 池, 那个靠 malloc_trim(0) 解决。
// ---------------------------------------------------------------------------
void drop_page_cache(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    ::close(fd);
}

// 轮转: path -> path.1 -> path.2 ... 超过 keep_files 的删除。
// 调用方必须持有 logger().mu
void rotate_locked(Logger& g) {
    if (g.keep_files <= 0) return;

    // 删掉最老的一份(unlink 之后 inode 才真正释放, page cache 跟着走;
    // 这里仍显式 drop 一次, 保证"已改名但还没被 unlink"的老文件也不占缓存)
    std::string oldest = g.path + "." + std::to_string(g.keep_files);
    drop_page_cache(oldest);
    unlink(oldest.c_str());

    // 依次后移
    for (int i = g.keep_files - 1; i >= 1; --i) {
        std::string from = g.path + "." + std::to_string(i);
        std::string to   = g.path + "." + std::to_string(i + 1);
        if (access(from.c_str(), F_OK) == 0) {
            rename(from.c_str(), to.c_str());
        }
    }
    // 当前文件变成 .1
    std::string first = g.path + ".1";
    rename(g.path.c_str(), first.c_str());

    // ★ 老文件(现在是 .1)的 page cache 立刻释放, 不占整机内存
    drop_page_cache(first);

    // 重新打开(清空计数)
    g.fd = open(g.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    g.bytes = 0;
}

}  // namespace

bool log_init(const std::string& path, bool also_stdout, uint64_t max_bytes,
              int keep_files) {
    Logger& g = logger();
    std::lock_guard<std::mutex> lk(g.mu);

    if (g.fd >= 0) {
        close(g.fd);
        g.fd = -1;
    }

    g.path = path;
    g.also_stdout = also_stdout;
    g.max_bytes = max_bytes ? max_bytes : kDefaultMaxBytes;
    g.keep_files = keep_files ? keep_files : kDefaultKeepFiles;

    g.fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (g.fd < 0) {
        // 打不开不致命: 降级到 stderr, 但要明确告知(否则现场会静默丢失)
        fprintf(stderr, "[LOG][ERROR][log][init] 无法打开日志文件 %s: %s\n",
                path.c_str(), strerror(errno));
        g.initialized = false;
        return false;
    }

    struct stat st {};
    if (fstat(g.fd, &st) == 0) {
        g.bytes = static_cast<uint64_t>(st.st_size);
    }
    g.initialized = true;
    return true;
}

void log_shutdown() {
    Logger& g = logger();
    std::lock_guard<std::mutex> lk(g.mu);
    if (g.fd >= 0) {
        fdatasync(g.fd);
        close(g.fd);
        g.fd = -1;
    }
    g.initialized = false;
}

// ---------------------------------------------------------------------------
// ★ 运行期日志级别过滤(环境变量 RK_LOG_LEVEL)
//   info  : 全打(默认, 保持现有行为)
//   warn  : 只打 WARN/ERROR
//   error : 只打 ERROR
//   off   : 全不打
//
//   为什么需要: 帧守卫(av/mpp frame guard)对【每一帧】都打构造/析构 INFO,
//   4 路 20fps 就是 8000 行/秒 —— 实测 96 秒就写了 8.8MB(其中 99.7% 是守卫的
//   逐帧日志, 真正有业务价值的统计行只有 90 行)。折算 7.9GB/天, 现场不可能容忍。
//   这个开关是【一行环境变量】的应急阀: 生产上设 RK_LOG_LEVEL=warn,
//   守卫的计数器(构造/析构/存活数)照常维护, 只是不再逐帧打日志。
//   默认 info = 行为完全不变, 不影响任何现有部署。
// ---------------------------------------------------------------------------
static LogLevel env_min_level() {
    static LogLevel lvl = []() {
        const char* v = getenv("RK_LOG_LEVEL");
        if (!v || !*v) return LogLevel::kInfo;
        if (strcasecmp(v, "warn") == 0) return LogLevel::kWarn;
        if (strcasecmp(v, "error") == 0) return LogLevel::kError;
        if (strcasecmp(v, "off") == 0) return LogLevel::kOff;
        return LogLevel::kInfo;
    }();
    return lvl;
}

// ---------------------------------------------------------------------------
// 限流判定。返回值:
//   1 = 放行
//   0 = 丢弃(静默, 只计数)
//  -1 = 丢弃, 且到了汇报时间点(*dropped 带出本窗口累计丢弃数)
// ---------------------------------------------------------------------------
int rate_limit_check(uint64_t* dropped, uint64_t* burst, uint64_t* window_sec) {
    RateLimiter& rl = rate_limiter();
    std::lock_guard<std::mutex> lk(rl.mu);

    if (!rl.inited) {
        rl.inited = true;
        const char* b = getenv("RK_LOG_RATE_LIMIT_BURST");
        const char* i = getenv("RK_LOG_RATE_LIMIT_INTERVAL_SEC");
        rl.burst = b ? strtoull(b, nullptr, 10) : 1000ull;
        const uint64_t sec = i ? strtoull(i, nullptr, 10) : 60ull;
        rl.window_ns = sec * 1000000000ull;
        rl.tokens = rl.burst;
        rl.last_ns = now_mono_ns();
        rl.last_report_ns = rl.last_ns;
    }

    if (burst) *burst = rl.burst;
    if (window_sec) *window_sec = rl.window_ns / 1000000000ull;

    if (rl.burst == 0 || rl.window_ns == 0) return 1;  // 不限流

    const uint64_t now = now_mono_ns();
    if (now > rl.last_ns) {
        // 补充令牌: 稳态速率 = burst / window。用 __uint128_t 防溢出。
        const __uint128_t add =
            (__uint128_t)rl.burst * (now - rl.last_ns) / rl.window_ns;
        if (add > 0) {
            rl.tokens += (uint64_t)add;
            if (rl.tokens > rl.burst) rl.tokens = rl.burst;
            rl.last_ns = now;
        }
    }

    if (rl.tokens > 0) {
        rl.tokens--;
        return 1;
    }

    rl.dropped++;
    // 每隔一个窗口汇报一次, 避免汇报本身变成噪声
    if (now - rl.last_report_ns >= rl.window_ns) {
        rl.last_report_ns = now;
        if (dropped) *dropped = rl.dropped;
        rl.dropped = 0;
        return -1;
    }
    return 0;
}

// 汇报"被丢弃了多少条"。绕开限流本身写出去(否则汇报也会被丢)。
void write_rate_limit_report(uint64_t dropped, uint64_t burst, uint64_t window_sec) {
    struct timespec ts {};
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_buf {};
    localtime_r(&ts.tv_sec, &tm_buf);

    char line[512];
    int n = snprintf(line, sizeof(line),
                     "[%04d-%02d-%02d %02d:%02d:%02d.%03d][tid=%d][WARN][log][rate-limit] "
                     "Suppressed %llu messages (日志限流: 每 %llu 秒最多 %llu 行, 超出部分已丢弃)\n",
                     tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                     tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                     static_cast<int>(ts.tv_nsec / 1000000),
                     static_cast<int>(current_tid()),
                     static_cast<unsigned long long>(dropped),
                     static_cast<unsigned long long>(window_sec),
                     static_cast<unsigned long long>(burst));
    if (n <= 0) return;
    if (n >= static_cast<int>(sizeof(line))) n = static_cast<int>(sizeof(line)) - 1;

    Logger& g = logger();
    {
        std::lock_guard<std::mutex> lk(g.mu);
        if (g.fd >= 0) {
            ssize_t w = write(g.fd, line, static_cast<size_t>(n));
            if (w > 0) g.bytes += static_cast<uint64_t>(w);
        }
    }
    if (g.also_stdout || g.fd < 0) {
        fwrite(line, 1, static_cast<size_t>(n), stderr);
    }
}

void log_write(LogLevel level, const char* module, const char* func,
               const char* fmt, ...) {
    // ★ 先过滤, 再做格式化 —— 省掉的不只是写盘, 还有 vsnprintf 和时间戳格式化
    if (level < env_min_level()) return;

    // ★ 再限流, 再做格式化 —— 被丢弃的行连 vsnprintf 都不用做。
    //   放在级别过滤之后: 生产默认 warn 时 INFO 早就被级别过滤掉了,
    //   限流只在有人把级别调高时才真正生效 —— 它是"防手滑"的兜底, 不是常态开销。
    uint64_t dropped = 0;
    uint64_t rl_burst = 0;
    uint64_t rl_window = 0;
    const int rl = rate_limit_check(&dropped, &rl_burst, &rl_window);
    if (rl < 0) {
        write_rate_limit_report(dropped, rl_burst, rl_window);
        return;
    }
    if (rl == 0) return;

    Logger& g = logger();

    // 行首时间戳: 墙钟, 带毫秒。崩溃后用它和 ramoops/dmesg 对时。
    struct timespec ts {};
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_buf {};
    localtime_r(&ts.tv_sec, &tm_buf);

    char line[kLineBufSize];
    int n = snprintf(line, sizeof(line),
                     "[%04d-%02d-%02d %02d:%02d:%02d.%03d][tid=%d][%s][%s][%s] ",
                     tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                     tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                     static_cast<int>(ts.tv_nsec / 1000000),
                     static_cast<int>(current_tid()),
                     level_name(level), module ? module : "-", func ? func : "-");
    if (n < 0) return;
    if (n >= static_cast<int>(sizeof(line))) n = static_cast<int>(sizeof(line)) - 1;

    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);
    if (m > 0) {
        n += m;
        if (n >= static_cast<int>(sizeof(line)) - 1) {
            n = static_cast<int>(sizeof(line)) - 2;
        }
    }
    line[n++] = '\n';

    g.counts[static_cast<int>(level)].fetch_add(1, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lk(g.mu);
    if (g.fd >= 0) {
        if (g.bytes + static_cast<uint64_t>(n) > g.max_bytes) {
            rotate_locked(g);
        }
        if (g.fd >= 0) {
            ssize_t w = write(g.fd, line, static_cast<size_t>(n));
            if (w > 0) {
                g.bytes += static_cast<uint64_t>(w);
                // WARN/ERROR 立刻落盘: 崩溃时最关键的就是这两级
                if (level != LogLevel::kInfo) {
                    fdatasync(g.fd);
                }
            }
        }
    }
    if (g.also_stdout || g.fd < 0) {
        // 未初始化日志文件时降级到 stderr, 至少别让现场静默丢失
        fwrite(line, 1, static_cast<size_t>(n), stderr);
    }
}

uint64_t log_count(LogLevel level) {
    Logger& g = logger();
    return g.counts[static_cast<int>(level)].load(std::memory_order_relaxed);
}

uint64_t log_total() {
    return log_count(LogLevel::kInfo) + log_count(LogLevel::kWarn) +
           log_count(LogLevel::kError);
}

const std::string& log_path() {
    return logger().path;
}

uint64_t log_bytes_written() {
    Logger& g = logger();
    std::lock_guard<std::mutex> lk(g.mu);
    return g.bytes;
}

}  // namespace rk
