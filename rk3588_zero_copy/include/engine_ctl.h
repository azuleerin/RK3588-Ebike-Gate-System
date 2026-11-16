// engine_ctl.h —— 运行时硬件开关 + 性能状态上报 + 硬件负载采集
//
// 这个模块解决三件事:
//   1. 【动态开关】MPP 硬解 / RGA 硬件加速 / NPU 推理 能在运行时来回切,
//      不用重启进程。开关用原子变量存 —— 控制线程只写, 主线程只读,
//      天然无锁, 也不会把解码器的状态搅乱(解码器的切换动作由主线程做)。
//   2. 【性能上报】主循环每秒钟把 FPS 和各阶段耗时打包成 JSON 发到 MQTT,
//      网页端就能画出实时曲线。发报在后台线程里做, 绝不占用推理线程。
//   3. 【硬件负载】读内核给的负载接口(NPU/VPU/RGA/CPU/DMC/GPU),
//      跟流水线耗时一起报上去 —— 两者对得上才说明数据可信。
#ifndef RK_ENGINE_CTL_H_
#define RK_ENGINE_CTL_H_

#include <string>

namespace rk {

// ---------------------------------------------------------------------------
// 一、运行时开关
// ---------------------------------------------------------------------------

// 三个引擎的开关状态。默认全开。
struct EngineSwitches {
    bool mpp = true;     // MPP 硬解 (false = FFmpeg 软解)
    bool rga = true;     // RGA 硬件缩放 (false = OpenCV CPU 缩放)
    bool npu = true;     // NPU 推理 (false = 只解码+预处理, 不产出检测框)
};

// 当前状态(控制线程写, 主线程读)
const EngineSwitches& engine_switches();

// 设置开关。返回 true 表示状态真的变了。
bool engine_set_mpp(bool on);
bool engine_set_rga(bool on);
bool engine_set_npu(bool on);

// 启动时的初始值(要在开控制线程之前调一次)
void engine_init_switches(bool mpp, bool rga, bool npu);

// ---------------------------------------------------------------------------
// 二、性能快照 + 状态上报
// ---------------------------------------------------------------------------

// 主循环每帧填一份, 状态线程按周期取最新的发出去。
struct PerfSnapshot {
    double fps = 0.0;        // 流水线 FPS
    double src_fps = 0.0;     // 源(相机)实际推送 FPS
    double wait_ms = 0.0;    // 等流时间(不算我们的计算)
    double dec_ms = 0.0;     // 解码
    double rga_ms = 0.0;     // 预处理
    double npu_ms = 0.0;     // NPU 推理
    double post_ms = 0.0;    // 后处理
    double draw_ms = 0.0;    // 画框+显示
    double total_ms = 0.0;   // 整帧墙钟
    long   frames = 0;       // 已处理帧数
    int    n_objs = 0;       // 当前目标数
    std::string decoder;     // "mpp" / "ffmpeg"
    std::string rga_mode;    // "hw" / "cpu"
    std::string sn = "rk3588-01";
};

// 硬件负载(百分比; -1 = 读不到)
struct HwLoad {
    double npu = -1.0;       // NPU 整体占用
    double npu_core = -1.0;  // NPU 单核最高占用
    // ★ 分核占用: 之前只留了 max, 网页上只能画一根"NPU 100%"的柱子,
    //   看不出负载到底压在哪个核上(并行/绑核的效果就讲不清了)。
    //   这三个是内核 rknpu/load 的原值: "Core0: 12% Core1: 5% Core2: 0%"。
    double npu_c0 = -1.0;
    double npu_c1 = -1.0;
    double npu_c2 = -1.0;
    double rga = -1.0;       // RGA 占用
    double vpu = -1.0;       // MPP/VPU 解码核占用
    double cpu = -1.0;       // CPU 总占用
    double dmc = -1.0;       // 内存控制器(DMC)占用
    double gpu = -1.0;       // GPU 占用
    std::string npu_src;     // "rknpu" / "devfreq" / "none"
    std::string rga_src;     // "debugfs" / "none"
};

// 读一次硬件负载(内部自己做 CPU% 的差分, 所以要连续调用才有意义)
HwLoad read_hw_load();

// 状态上报器: 自己开一条 MQTT 连接, 后台线程按周期发。
class StatusPublisher {
public:
    StatusPublisher() {}
    ~StatusPublisher();

    bool start(const std::string& host, int port, const std::string& topic,
               const std::string& client_id, int qos = 0, int interval_ms = 1000);

    // 主循环调用: 只做一次加锁拷贝, 不发网络包(绝不能拖慢推理线程)
    void update(const PerfSnapshot& s);

    void stop();
    bool connected() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

// ---------------------------------------------------------------------------
// 三、极简 JSON 工具(只认扁平结构, 不做完整 JSON 解析)
// ---------------------------------------------------------------------------

// 取布尔字段: {"mpp":true} -> true
// 找不到 / 不是布尔 -> false(并把 found 置 false)
bool json_get_bool(const std::string& s, const char* key, bool* out, bool* found = nullptr);

// 取字符串字段: {"cmd":"set_engine"} -> set_engine
bool json_get_string(const std::string& s, const char* key, std::string* out,
                     bool* found = nullptr);

// 把浮点数写成 JSON 数字(保留 2 位小数, 去掉多余的尾零)
//   负数一律当 0 —— 耗时这类值不可能为负; 滑动窗口求和的浮点误差
//   会让均值出现 -1e-13 这种"负零", 不钳掉的话 JSON 里就是 -1, 很难解释。
std::string json_num(double v);

// 同上, 但负数表示"读不到"(原样写成 -1)。
// 用于硬件负载这类"没有数据就是没有数据"的字段。
std::string json_num_opt(double v);

}  // namespace rk

#endif  // RK_ENGINE_CTL_H_
