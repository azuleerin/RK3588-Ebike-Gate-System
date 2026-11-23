// nlb/npu_model.h —— RKNN C API 的零拷贝封装
//
// ★ 三条硬约束(全部来自"实测踩过的坑", 不是风格偏好):
//
// 1) 【RKNN 上下文不能跨线程】
//    在 A 线程 rknn_init, 拿 B 线程 rknn_run, 运行时内部死锁 ——
//    表现是所有线程卡在 futex, 一帧都不动。
//    所以本类把这件事做成结构性的: NpuModel 必须在【使用它的那个线程】里
//    构造(init)和销毁(destroy)。
//
// 2) 【零拷贝】
//    输入缓冲用 rknn_create_mem / rknn_create_mem_from_fd 分配, 这是一块
//    "NPU 可直读的物理内存"; 再用 rknn_set_io_mem 绑到输入张量。
//    启动时往里写一次图案, 之后热循环里对输入缓冲的写次数 = 0。
//    第 3 层更进一步: RGA 的目标缓冲【就是】这块 NPU 输入内存
//    (RGA 直接写进 NPU 要读的那块物理内存, 中间没有 memcpy)。
//
// 3) 【输出缓冲预分配】
//    rknn_output.is_prealloc = 1 + 预先给好 buf/size,
//    这样每帧不会由 runtime 去 malloc 一块输出缓冲(那会破坏内存平线)。
//    want_float = 0: 保持模型原生量化格式, 避免驱动额外做一次 float 转换。
#ifndef NLB_NPU_MODEL_H_
#define NLB_NPU_MODEL_H_

#include <cstdint>
#include <string>
#include <vector>

#include <rknn_api.h>

namespace nlb {

// 模型文件的一次性读取(进程内按路径缓存, 多个线程共享同一份字节)。
// rknn_init 会把字节拷进 runtime 自己的内存, 所以每个上下文各建一个即可。
struct ModelBlob {
    std::string path;
    std::vector<uint8_t> bytes;
    bool load(const std::string& p, std::string* err);
};

// 一个 RKNN 上下文 + 它的零拷贝输入 + 预分配输出。
class NpuModel {
public:
    NpuModel() = default;
    ~NpuModel() { destroy(); }

    NpuModel(const NpuModel&) = delete;
    NpuModel& operator=(const NpuModel&) = delete;

    // 在【当前线程】里建立上下文。
    //
    // core_mask : 0=AUTO 1=CORE0 2=CORE1 4=CORE2
    // input_fd  : >=0 时用这块【外部】dma-buf 当输入(零拷贝, RGA 可直接写它);
    //             <0 时用 rknn_create_mem 让 runtime 自己分配。
    // collect_perf: 打开 RKNN_FLAG_COLLECT_PERF_MASK, 用于算子回落统计。
    //               ★ 注意: 打开它会显著拖慢推理(逐层打点), 所以只在
    //               "算子回落检查"里开, 绝不能开在计时路径上。
    // want_float: 输出要不要转成 float32。
    //               ★ 生产链路(npu_pool.cc)用 1, 本测试台默认也是 1,
    //                 这样第 1 层的单帧耗时可以和生产的 `npu=` 直接比。
    //                 设成 0 可以量化"驱动做 float 转换"本身的开销。
    bool init(const std::string& path, int core_mask, int input_fd, void* input_ptr,
              std::size_t input_size, bool collect_perf, bool want_float,
              std::string* err);

    // 在【当前线程】里销毁。
    void destroy();

    bool ok() const { return ctx_ != 0; }
    rknn_context ctx() const { return ctx_; }
    int core_mask() const { return core_mask_; }

    uint32_t in_h() const { return in_h_; }
    uint32_t in_w() const { return in_w_; }
    uint32_t in_c() const { return in_c_; }
    uint32_t in_size() const { return in_size_; }
    uint32_t n_output() const { return static_cast<uint32_t>(outs_.size()); }

    // 输入缓冲(热循环里只读, 不写)。
    void* input_virt() const { return input_virt_; }
    int input_fd() const { return input_fd_; }

    // 跑一次推理: rknn_run + rknn_outputs_get。
    //
    // ★ 故意【不】调 rknn_outputs_release:
    //   生产链路(npu_pool.cc:392-393)也是不调的 —— 输出缓冲是预分配的,
    //   直接读走就行。多一次 runtime 调用就是多一次开销, 会让第 1 层的
    //   单帧耗时和生产的 `npu=` 对不上。
    //
    // ★ 也故意【不】在函数里查 RKNN_QUERY_PERF_RUN:
    //   那是一次额外的 runtime 调用, 会污染热循环。调用方在计时区间
    //   之外单独调 query_run_us()。
    int run();

    // 驱动自报的"这次推理真正占了核多久"(微秒)。
    uint64_t query_run_us() const;
    uint64_t last_run_us() const { return last_run_us_; }

    // 权重/内部/DMA 内存占用(字节)。
    uint64_t weight_bytes() const { return weight_bytes_; }
    uint64_t internal_bytes() const { return internal_bytes_; }
    uint64_t dma_bytes() const { return dma_bytes_; }

    // 逐层性能明细(RKNN_QUERY_PERF_DETAIL), 需要 init 时开了 collect_perf。
    // 返回的字符串由 runtime 持有, 不要 free。
    std::string perf_detail() const;

    // SDK / 驱动版本
    std::string sdk_version() const { return sdk_version_; }
    std::string drv_version() const { return drv_version_; }

private:
    rknn_context ctx_ = 0;
    int core_mask_ = 0;

    rknn_tensor_mem* in_mem_ = nullptr;   // runtime 分配的输入(外部 fd 时为 from_fd)
    bool owns_in_mem_ = false;
    void* input_virt_ = nullptr;
    int input_fd_ = -1;
    uint32_t in_h_ = 0, in_w_ = 0, in_c_ = 0, in_size_ = 0;

    std::vector<rknn_output> outs_;
    std::vector<std::vector<uint8_t>> out_bufs_;

    mutable uint64_t last_run_us_ = 0;   // mutable: query_run_us() 是 const 的
    uint64_t weight_bytes_ = 0, internal_bytes_ = 0, dma_bytes_ = 0;
    std::string sdk_version_, drv_version_;
};

}  // namespace nlb

#endif  // NLB_NPU_MODEL_H_
