// rk/minimal/unit.h —— 最小化测试单元的公共接口
//
// 隔离原则（AGENT_RULES_FOR_CRASH_DEBUGGING.md 第一节）:
//   每个单元只允许触碰一个硬件模块, 启动时互斥校验, 绝不允许组合运行。
//
//   --decoder-only : 只跑 MPP/FFmpeg 解码, 禁用 RGA 与 RKNN
//   --rga-only     : 只跑 RGA 转换,   禁用 MPP(默认)与 RKNN
//   --npu-only     : 只跑 RKNN 推理,   禁用 MPP 与 RGA
//
// 带病复现原则（D7 批复）:
//   单元必须忠实复现可疑行为, 不得顺手修好 —— 否则死机不复现, 排查作废。
//   因此本层只提供"观测"能力(日志/fd 台账/canary/栈水位),
//   不提供任何"改变时序"的逻辑。
#ifndef RK_MINIMAL_UNIT_H_
#define RK_MINIMAL_UNIT_H_

#include <cstdint>
#include <string>
#include <vector>

namespace rk {
namespace minimal {

struct UnitOptions {
    std::string mode;          // decoder-only / rga-only / npu-only
    std::string src;           // 输入源(按模式解释)
    std::string dump;          // 输出落盘路径(供下一单元使用)
    std::string model;         // rknn 模型路径(npu-only)
    std::string save_frame;    // 存一帧 PNG(肉眼验证)
    int frames = 0;            // 跑多少帧后退出; 0 = 不限
    int duration_sec = 0;      // 跑多少秒后退出; 0 = 不限
    bool loop = false;         // 到达末尾回到开头
    std::string log_path;
    int stats_interval = 60;   // 每多少秒做一次巡检
    int core_mask = 0;         // NPU 核掩码(0 = 不设置)
    int batch = 1;             // NPU batch
    bool no_legacy_rga = false;// RGA 硬件路径对照开关
    // RGA A/B 对照开关(rga-only 用):
    //   wstride_mode: "pixels"=正确(librga 的 wstride 单位是像素)
    //                 "bytes" =复现 license_plate_rec.cc:553 的单位错误
    //   import_mode : "per-frame"=带病(每帧 import/release, 对应 P0-3)
    //                 "cached"   =对照(import 一次复用)
    std::string wstride_mode = "pixels";
    std::string import_mode = "per-frame";
};

// 单元执行入口。返回 0 = 正常结束, 非 0 = 异常。
// 单元内部会自己调用 periodic_audit() 做巡检。
int run_npu_unit(const UnitOptions& opt);

// step/03 / step/04 实现。未实现的单元返回 2 并明确说明。
int run_decoder_unit(const UnitOptions& opt);
int run_rga_unit(const UnitOptions& opt);

// ---- 公共工具 ----

// 巡检: fd 台账对账 + canary 校验 + 内存统计 + 栈水位
void audit_now(const char* why);

// 读整个文件到受控内存。返回 nullptr 表示失败。
uint8_t* load_file(const std::string& path, size_t* out_size, const char* tag);

// NV12 -> RGB888 (CPU 版, 仅用于 npu-only 的输入准备)
//   宽高必须为偶数; 输出长度 = w*h*3
//   注意: 这是**纯 CPU**转换, 目的是把 RGA 从这个单元里彻底摘出去。
uint8_t* nv12_to_rgb888(const uint8_t* nv12, int width, int height, const char* tag);

// 生成一张带明显特征的测试图(用于 --save-frame 肉眼验证)
//   纯 CPU 写, 不依赖 OpenCV
uint8_t* make_test_pattern(int width, int height, int frame_index, const char* tag);

// 统计: 简单的耗时直方图
struct TimingStat {
    double sum = 0;
    uint64_t n = 0;
    double min = 0;
    double max = 0;
    std::vector<double> samples;

    void push(double v);
    double mean() const { return n ? sum / static_cast<double>(n) : 0.0; }
    double pct(double p) const;
    void reset();
};

}  // namespace minimal
}  // namespace rk

#endif  // RK_MINIMAL_UNIT_H_
