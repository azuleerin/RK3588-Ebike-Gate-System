// tb/sys_probe.h —— 系统资源采样(CPU / 内存 / 堆 / NPU load)
//
// ★ 采样必须【零堆分配】, 否则采样线程自己就在破坏内存平线:
//   /proc 全部走 open + pread + 栈缓冲; /proc 文件句柄在启动时打开一次,
//   之后用 pread 按偏移 0 重复读(不需要 lseek, 也不需要 fseek)。
//
// ★ NPU 负载有两个口径:
//   1) 应用侧(默认, 不需要 root): 累加每个 worker 的 rknn_run 墙钟, 按绑定核归属。
//      这个口径更贴近业务 —— 它精确回答"这颗核上跑了多少模型推理时间"。
//   2) debugfs(可选, 需要 root): /sys/kernel/debug/rknpu/load
//      板子上这个节点默认对普通用户是 Permission denied, 所以它是增强项而不是主指标。
#ifndef TB_SYS_PROBE_H_
#define TB_SYS_PROBE_H_

#include <cstdint>
#include <string>
#include <vector>

namespace tb {

struct ProbeSample {
    int64_t ts_ns = 0;
    uint64_t cpu_ticks = 0;   // utime + stime
    int64_t rss_kb = 0;       // VmRSS
    int64_t hwm_kb = 0;       // VmHWM
    int64_t heap_kb = 0;      // mallinfo2().uordblks
    double npu_load[3] = {-1.0, -1.0, -1.0};
};

class SysProbe {
public:
    SysProbe();
    ~SysProbe();

    SysProbe(const SysProbe&) = delete;
    SysProbe& operator=(const SysProbe&) = delete;

    // 指定 debugfs NPU load 节点路径(空 = 自动探测候选列表)
    void set_npu_load_path(const std::string& path) { npu_load_path_ = path; }

    // 打开 /proc 文件句柄(启动期一次)。NPU load 不可读不算失败。
    bool open();
    void close();

    // 采样一次(零堆分配)
    bool sample(ProbeSample* out);

    bool npu_load_available() const { return !npu_load_path_.empty(); }
    const std::string& npu_load_path() const { return npu_load_path_; }

    // /proc/meminfo 的 MemAvailable(kB), 用来在跑双模型前做内存预检
    static int64_t mem_available_kb();

    // CPU 主频计数(每秒的 tick 数)
    static long clk_tck();

    // 探测"大核"(按 /sys/devices/system/cpu/cpuN/cpu_capacity 取最大值的那几个)
    static std::vector<int> detect_big_cores();

private:
    bool read_proc_stat(uint64_t* ticks);
    bool read_proc_status(int64_t* rss_kb, int64_t* hwm_kb);
    bool read_npu_load(double out[3]);

    std::string npu_load_path_;
    int fd_stat_ = -1;
    int fd_status_ = -1;
};

}  // namespace tb

#endif  // TB_SYS_PROBE_H_
