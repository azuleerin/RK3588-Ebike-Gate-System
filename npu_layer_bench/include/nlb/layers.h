// nlb/layers.h —— 五个分层测试的入口
#ifndef NLB_LAYERS_H_
#define NLB_LAYERS_H_

#include <string>

#include "nlb/bench_types.h"

namespace nlb {

// 第 1 层: 纯 NPU 算力天花板(裸推理, 无前后处理)
void run_layer1(const Config& cfg);

// 第 2 层: 检测 + OCR 双模型纯推理(串行)
void run_layer2(const Config& cfg);

// 第 3 层: RGA 预处理 + 检测推理
void run_layer3(const Config& cfg);

// 第 4/5 层: 全链路(驱动生产二进制 rk_gate, 解析它的统计行)
void run_layer_full(const Config& cfg, Layer layer);

// 额外验证项: 分辨率扫描 + 算子回落统计
void run_layer_extra(const Config& cfg);

}  // namespace nlb

#endif  // NLB_LAYERS_H_
