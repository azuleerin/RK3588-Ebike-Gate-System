// nlb/sys_probe.h —— 整机资源采样(CPU / RSS / 活堆 / NPU 三核 load)
//
// ★ 口径对齐主工程与 thread_bench:
//   cpu_pct : 整机口径 —— (1 - idle/total) × 100, 8 核全跑满 = 100%
//   cpu_cores: cpu_pct × 8 = 折合多少个满载 CPU 核
//   活堆     : mallinfo2().uordblks = 【正在使用】的堆字节
//              (RSS 会被 arena 保留/碎片干扰, 单看它会把碎片误判成泄漏)
//
// ★ NPU 三核 load:
//   /sys/kernel/debug/rknpu/load 需要 root。读不到时 npu_load 全 -1,
//   此时用【应用侧】口径(Σ 各核上下文的 run 时间 / 窗口)当替代。
#ifndef NLB_SYS_PROBE_H_
#define NLB_SYS_PROBE_H_

#include <cstdint>
#include <string>

namespace nlb {

class SysProbe {
public:
    // 取一次 CPU 快照(内部保存上一次, 第二次调用时给出区间均值)。
    void sample_cpu();

    // 区间 CPU 占用: 整机 % 与折合核数。
    double cpu_pct() const { return cpu_pct_; }
    double cpu_cores() const { return cpu_pct_ * cpu_count_ / 100.0; }

    int cpu_count() const { return cpu_count_; }

    // 进程 RSS(MB) 与峰值 RSS(MB)。
    double rss_mb() const;
    double rss_peak_mb() const;

    // 活堆字节(mallinfo2().uordblks)。
    static uint64_t heap_bytes();

    // 读一次 debugfs 的 NPU 三核 load。成功返回 true 并填 npu_load_[0..2]。
    bool sample_npu_load();
    const double* npu_load() const { return npu_load_; }
    bool npu_load_available() const { return npu_load_ok_; }

    // 读一次 NPU 当前频率(devfreq)。
    // ★ 必须记录: RK3588 的 NPU 是 300MHz~1GHz 可调的, 而这台板子上的
    //   target_freq 节点是【只读】的, 想锁频只能靠 min_freq/max_freq。
    //   如果锁没生效, 同一个配置的耗时会在 1000MHz 和 900MHz 之间差 11%,
    //   而数据表面上完全看不出问题 —— 所以把频率一起记下来。
    bool sample_npu_freq();
    double npu_freq_mhz() const { return freq_mhz_; }

    // debugfs 路径(可覆盖)。
    static void set_npu_load_path(const std::string& p) { npu_load_path_ = p; }
    static const std::string& npu_load_path();
    static void set_npu_freq_path(const std::string& p) { npu_freq_path_ = p; }

private:
    double cpu_pct_ = 0.0;
    int cpu_count_ = 8;
    uint64_t prev_total_ = 0, prev_idle_ = 0;
    bool have_prev_ = false;
    bool npu_load_ok_ = false;
    double npu_load_[3] = {-1.0, -1.0, -1.0};
    double freq_mhz_ = -1.0;

    static std::string npu_load_path_;
    static std::string npu_freq_path_;
};

}  // namespace nlb

#endif  // NLB_SYS_PROBE_H_
