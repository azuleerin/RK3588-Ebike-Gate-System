// tools/kern_log.cc —— 内核日志"硬挂死可取证"捕获器
//
// 为什么不能用 `dmesg -w >> file`:
//   dmesg 用 stdio 缓冲, 写进文件后数据还在 page cache 里。
//   硬挂死 -> 断电重启 -> 未回写的 page cache 全部丢失,
//   恰恰是最关键的那几行(死机前最后几秒)没了。
//   ramoops 也救不了: 只有真的发生 panic/oops 才会写进去,
//   而"CPU 死循环"这种硬挂死根本不进 panic 路径 -> pstore 通常是空的。
//
// 本工具:
//   直接读 /dev/kmsg (内核消息环形缓冲区), 每条消息一次 write(2),
//   并且每条都 fdatasync() —— 保证"断电也还在"。
//   /dev/kmsg 支持多个读者, 与 dmesg -w 可以并存, 互不抢消息。
//
// 用法: kern_log <输出文件> [轮转上限字节]
//   输出行格式: [内核时间戳][级别] 原始消息
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr size_t kMaxMsg = 8192;

// /dev/kmsg 每条记录: "<prio>,<seq>,<ts_usec>,<flag>;<text>\n"
bool parse_kmsg(const char* buf, long* prio, long long* ts_usec, char* text, size_t text_sz) {
    const char* p = buf;
    char* end = nullptr;
    *prio = strtol(p, &end, 10);
    if (*end != ',') return false;
    p = end + 1;
    (void)strtol(p, &end, 10);   // seq
    if (*end != ',') return false;
    p = end + 1;
    *ts_usec = strtoll(p, &end, 10);
    if (*end != ',') return false;
    p = end + 1;
    if (*p != ';') return false;
    // 去掉结尾换行, 再拷进调用者的缓冲(buf 是只读的, 不能就地改)
    size_t len = strlen(p + 1);
    while (len > 0 && (p + 1)[len - 1] == '\n') --len;
    if (len >= text_sz) len = text_sz - 1;
    memcpy(text, p + 1, len);
    text[len] = '\0';
    return true;
}

const char* prio_name(long prio) {
    switch (prio) {
        case 0: return "EMERG";
        case 1: return "ALERT";
        case 2: return "CRIT";
        case 3: return "ERR";
        case 4: return "WARN";
        case 5: return "NOTICE";
        case 6: return "INFO";
        case 7: return "DEBUG";
        default: return "?";
    }
}

}  // namespace

int main(int argc, char** argv) {
    const char* out_path = (argc > 1) ? argv[1] : "/home/seeed/rk_deploy/logs/kernel_raw.log";
    const size_t max_bytes = (argc > 2) ? strtoull(argv[2], nullptr, 10) : 64ull * 1024 * 1024;

    int fd = open(out_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        fprintf(stderr, "[kern_log] 打开 %s 失败: %s\n", out_path, strerror(errno));
        return 1;
    }

    int kfd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (kfd < 0) {
        fprintf(stderr, "[kern_log] 打开 /dev/kmsg 失败: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    // ⚠️ 故意【不】seek 到末尾:
    //   从开头读 = 把内核环形缓冲区里的历史消息全量落盘。
    //   好处: 硬挂死后服务重启时, 崩溃前的消息能【补回来】(环形缓冲区没丢),
    //         相当于给 ramoops 补了一条"环形缓冲区转存"通道。
    //   代价: 文件开头是本次开机早期的消息(几 MB), 有 64MB 轮转上限兜底。

    fprintf(stderr, "[kern_log] 开始捕获 /dev/kmsg -> %s (每条 fdatasync, 断电不丢)\n",
            out_path);

    char buf[kMaxMsg];
    char line[kMaxMsg + 128];
    size_t written = 0;

    for (;;) {
        const ssize_t n = read(kfd, buf, sizeof(buf) - 1);
        if (n <= 0) {
            usleep(20000);
            continue;
        }
        buf[n] = '\0';

        long prio = 0;
        long long ts = 0;
        char text[kMaxMsg];
        if (!parse_kmsg(buf, &prio, &ts, text, sizeof(text))) {
            snprintf(text, sizeof(text), "%s", buf);
            prio = 7;
        }
        const int len = snprintf(line, sizeof(line), "[%lld.%06lld][%s] %s\n", ts / 1000000,
                                 ts % 1000000, prio_name(prio), text);
        if (len <= 0) continue;

        const ssize_t w = write(fd, line, static_cast<size_t>(len));
        if (w > 0) {
            written += static_cast<size_t>(w);
            // ★ 关键: 每条都回写。硬挂死+断电时, 只有 fsync 过的数据还在。
            fdatasync(fd);
        }

        // 轮转: 写满磁盘会拖死整机, 必须限容
        if (written > max_bytes) {
            // 重新开一个新文件, 旧的改名
            const std::string old = std::string(out_path) + ".1";
            close(fd);
            rename(out_path, old.c_str());
            fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            written = 0;
        }
    }
    return 0;
}
