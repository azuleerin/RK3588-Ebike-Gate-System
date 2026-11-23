# 任务② 报告 · fork9(DFL 解码头移出 NPU)板端 A/B

> 2026-11-21 23:05 ~ 2026-11-22 00:05　　范围: 独立沙盒 `opt_dfl/`, **未改任何生产推理代码, 未 merge**

---

## 一、结论先行

| 问题 | 答案 |
|---|---|
| **ΔNPU 是多少** | **−7.27 ms/帧**(4 路不限帧, NPU 1GHz); 生产配置下 **−6.83 ms/帧** |
| 按决策条件该不该上 | **技术上该上**(ΔNPU ≥ 3ms 且框精度无损), **但今天在生产配置下 FPS 收益是 0** —— 生产已经被 20fps/路 的限帧卡住(79.96/80 FPS), fork9 的价值是**余量**而不是当下的帧率 |
| 候选模型 | `opt_dfl/models/armB_fork9_dfl_on_cpu_int8.rknn` (4327819 B, 9 输出) |
| 精度 | **INT8 下比图内 DFL 还准 2.5pp**(1724 帧匹配率 **92.17%** vs 图内 DFL 的 89.68%, IoU 0.9709) → **不掉点, 反而更准** |
| ~~阻塞项~~ | ~~形态B 漏乘 stride~~ **已于 2026-11-22 修复并对拍通过**(见 `docs/p0_fixes_report.md`) |
| **阻塞项** | `src/yolo_post.cc` 的形态B(CPU 侧 DFL)**漏乘 stride**, 框坐标平均偏 28~47 px。**上线前必须先修这个** |

---

## 二、前置检查: 检测模型 ONNX 源

任务书要求"先查 ONNX 源, 没有就跳过"。实际情况:

| 模型 | ONNX 源 | 状态 |
|---|---|---|
| 生产 `ebike_int8_rk2332.rknn` (2 类: person/ebike) | **没有** | 仓库里只有编译好的 .rknn, 没有对应 ONNX, 也没有 .pt |
| yolov8n (COCO 80 类) | **有** | `_bench/check0_base_optimize.onnx` —— 而且它**本身就是瑞芯微 fork9 的 9 输出形态** |

`npu_layer_bench/scripts/make_res_models.sh` 里已经把这个事实写死了:

> ★ 它是【1 类】的 YOLOv8n, 生产模型是【2 类】—— 差别只在检测头(model.22.cv3.*)的那几个卷积,
> 而这部分计算量在三种分辨率下是个【常数】。

**所以本任务用的是 yolov8n(COCO)这一对, 不是生产 ebike 模型。** 这对"DFL 解码值多少 NPU 时间"
这个问题是等价的 —— DFL 解码作用在 **box 分支的 64 个通道**上, 与类别数无关;
而且本仓库自己的 `npu_layer_bench` 一直用同一套口径做分辨率扫描。

---

## 三、怎么保证"只有一个变量"

DFL 解码在图内 vs 在 CPU, 要比出可信的数, 必须保证 **权重相同、量化集相同、转换配方相同**。
做法: **不重新导出模型, 而是在 fork9 的 ONNX 上原地做手术, 把 DFL 解码塞回图里**。

```
opt_dfl/scripts/make_dfl_in_npu_onnx.py   ← v1
opt_dfl/scripts/make_dfl_in_npu_onnx_v2.py ← v2 (softmax 轴放最后)
opt_dfl/scripts/make_dfl_in_npu_onnx_v3.py ← v3 (输出张量物理上就是 NCHW)
```

三个变体都先用 onnxruntime 验证过 **与 fork9+CPU 解码逐像素一致**:

```
=== Arm A(图内 DFL) vs fork9+CPU DFL ===
  单位正确版: max|Δ|=0.0000 px   mean|Δ|=0.0000 px      ← 构造正确
  生产代码现状: max|Δ|=142.7525 px  mean|Δ|=47.7583 px  ← yolo_post.cc 形态B 有 bug(见第六节)
```

**两臂模型**:

| 臂 | 模型 | 形态 | 输出 |
|---|---|---|---|
| Arm A | `opt_dfl/models/armA_dfl_in_npu_int8.rknn` | 形态C (2 输出) | box[1,4,8400] + cls[1,80,8400] |
| **Arm B** | `opt_dfl/models/armB_fork9_dfl_on_cpu_int8.rknn` | **形态B (9 输出)** | 3 尺度 × [box(原始DFL logits), cls, sum] |

Arm B 与 `_bench/dl/yolov8n_int8_640.rknn` **字节数完全一致**(4327819), 可复现。

---

## 四、逐算子 NPU 归因(这是最硬的数)

用 `audit/model_audit.py`(走 `RKNN_QUERY_PERF_DETAIL`, 驱动自报的逐算子表), NPU 锁 1GHz:

| 模型 | 单次推理 | 算子数 | NPU 算子耗时 | 内部内存 |
|---|---|---|---|---|
| 生产 ebike(形态C, nc=2) | 35814 us | 122 | 34194 us | — |
| Arm A (DFL 在 NPU) | 27736 us | 154 | 25074 us | 7.52 MB |
| **Arm B (DFL 在 CPU)** | **24746 us** | **108** | **23195 us** | **4.30 MB** |

### 4.1 Arm A 里"DFL 解码"这一段值多少

我插入的解码节点都带 `s0/ s1/ s2/` 前缀, 可以直接按名字挑出来求和:

```
  NPU  Transpose        2237 us   Transpose:s0/Squeeze_2rs
  NPU  exSoftmax13      1611 us   exSoftmax13:s0/Softmax
  NPU  Transpose         500 us   Transpose:s1/Squeeze_2rs
  NPU  exSoftmax13       430 us   exSoftmax13:s1/Softmax
  CPU  Sub               393 us   Sub:s0/sl0_sub          ← 回落 CPU
  ... (共 46 个节点)
  合计                    7876 us  (NPU 6848 / CPU 1028)
  占 Arm A 单次推理 27736 us 的 28.4%
```

### 4.2 生产模型自己的 DFL 路径(**更可靠的锚点**)

生产模型的图内 DFL 是 ultralytics 原生实现(不是我的手搓), 直接从它的逐算子表里摘:

| 算子 | 耗时 |
|---|---|
| `exSoftmax13:/model.22/dfl/Softmax` | 3898 us |
| `Transpose:/model.22/dfl/Reshape_1_output_0_rs` | 1149 us |
| `Transpose:/model.22/dfl/Transpose` | 1055 us |
| `Reshape:/model.22/dfl/Reshape` | 465 us |
| `Conv:/model.22/dfl/conv/Conv` | 399 us |
| `Split` / `Sub`×2 / `Add`×2 / `Mul`×2 / `Concat_4` / `Reshape` | 1529 us |
| **合计** | **≈ 8.5 ms / 次推理** |

> 这跟此前 audit 报告里"DFL 解码头占检测模型 NPU 算子总时长的 39.8%~49.6%"是同一件事的两种口径:
> 那份算的是整个 `model.22`(含检测头卷积), 我这个只算**纯解码**那部分(8.5/34.2 = 25%)。

---

## 五、板端 4 路 A/B

**口径**(与 `opt_audit/run_ab.sh` 同源): 吞吐 = 4 路**帧计数器差分**(不用自报的 deliv);
统计窗口 = 启动 +15s ~ 结束前 5s; 两组**交错**跑(base,new,base,new)抵消温度漂移;
每轮前查 `/sys/kernel/debug/rknpu/load` **逐核**, >8% 作废重跑; NPU 锁 1GHz(本板 NPU 最高频)。

### 5.1 设置① 不限帧(max-fps=0) —— 看吞吐天花板

| 臂 | 轮数 | **总FPS(中位)** | 单帧NPU | detwait | ocrwait | NPU 每核% |
|---|---|---|---|---|---|---|
| 生产 ebike(形态C, nc=2) | 3 | **107.14** | 23.69 ms | 30.81 | 51.41 | 84/85/85 |
| Arm A (DFL 在 NPU, nc=80) | 3 | 72.58 | 27.77 ms | 51.49 | 77.76 | 55/56/60 |
| **Arm B (fork9, DFL 在 CPU)** | 3 | **93.83** | **20.50 ms** | 35.73 | 66.40 | 55/56/58 |

**Δ(armA → armB): 总 FPS +29.3%, 单帧 NPU −7.27 ms**

### 5.2 设置② 生产配置(max-fps=20)

| 臂 | 轮数 | **总FPS(中位)** | 单帧NPU | NPU 每核% |
|---|---|---|---|---|
| 生产 ebike | 3 | **79.96** | 25.22 ms | 72/74/74 |
| Arm A | 3 | 70.59 | 27.96 ms | 55/55/56 |
| **Arm B (fork9)** | 3 | **76.27** | **21.13 ms** | 54/54/54 |

**Δ(armA → armB): 总 FPS +8.0%, 单帧 NPU −6.83 ms**

### 5.3 ⚠️ 这张表最重要的一行: 生产模型 79.96 ≈ 80

4 路 × 20fps = **80 FPS 就是限帧天花板**。生产模型在 max-fps=20 下**已经打到 79.96**,
也就是说 **今天的生产瓶颈是限帧, 不是 NPU**。

→ **fork9 换上去, 生产 FPS 当下不会涨**(已经贴在 80 的天花板上了)。
→ 它的价值是**余量**: 同样 4 路, 不限帧时生产模型能跑 107 FPS, 换成 fork9 后按
armA→armB 的实测比例(×1.29)外推大约 **135~140 FPS**。要吃到这块余量, 需要同时
把 `GATE_MAX_FPS` 提上去(或者加路数/加 OCR 负载)。

---

## 六、精度回归(1724 帧, 板端批量推理)

用 `accuracy/acc_batch` + `accuracy/aggregate.py` 的**同一套**判定(匹配率 / IoU≥0.5 / 类别一致率),
参考 = `yolov8n_fp16`(它本身就是从同一个 fork9 ONNX 转的, 是天然基准):

| 模型 | 匹配率 | 类别一致 | 平均IoU | 说明 |
|---|---|---|---|---|
| Arm A v3 FP16 (图内 DFL) | **100.00%** | 1724 | **0.9885** | 图内 DFL 重建版与标准解码完全一致 |
| Arm A v3 INT8 (图内 DFL) | 89.68% | 1546 | 0.8375 | 解码中间量被量化, 掉得更多 |
| **Arm B INT8 (fork9)** | **92.17%** | **1589** | **0.9709** | **比图内 DFL 还准 2.5pp** |

> ⚠ **本节数字已于 2026-11-22 用【修复后的 acc_batch 解码器】重算**。
> 旧数字(84.86% / IoU 0.8814)是**坏解码器**算出来的 —— 那时 `acc_batch.cc` 把
> DFL 的 16 个 bin 读成了【连续 16 个】而不是【跨步 grid_len】, 框坐标本来就是错的。
> 详见 `docs/p0_fixes_report.md` 第〇节与 1.4。

**结论: 把 DFL 解码从 NPU 搬到 CPU, 精度不掉。** 道理上也说得通 —— box 分支那 64 个通道的
**量化完全没变**(同一个 conv、同一个输入、同一套 quant 参数), 变的只是"谁来做这一步解码"。

### 6.1 ~~附带发现: ONNX 手术版图内 DFL 过不了 RKNN 编译器~~ **(已推翻, 2026-11-22)**

> **这条结论是错的, 已作废。** 当时 Arm A 与参考只匹配 3.13%, 看起来像"图内 DFL 过不了
> RKNN 编译器"; 真实原因是**参考解码器本身读错了 DFL 布局**(见
> `docs/p0_fixes_report.md` 第〇节)。用修好的解码器重测:
> **Arm A v3 FP16 与 FP16 参考的匹配率 = 100.00%, IoU 0.9885** —— 图内 DFL 重建版是好的。
> 下面这段保留, 是"当时为什么会那么想"的记录。

Arm A 的三个变体(v1/v2/v3, 图结构差别很大: softmax 轴位置不同、输出张量物理布局不同)
**在 RKNN 上给出完全相同的错误结果**, 而它们的 ONNX 用 onnxruntime 跑是**逐像素精确**的
(max|Δ| = 0.0000 px)。分数是对的(最高分 0.720215 与 FP16 完全一致), **只有框坐标错**。

也就是说: **"把 DFL 解码塞回 ONNX 图里"这条路, RKNN 编译器并不可靠** ——
这反过来是支持 fork 形态的一条理由: **fork9 才是 RKNN 工具链真正支持的形态**。

(排查过的三个假设, 都不成立: softmax 轴在 5 维张量上被重解释 / 输出张量的 NCHW-NHWC 物理布局 /
INT8 量化 scale —— FP16 变体同样错, 且三个变体结果逐位相同。)

---

## 七、★ 已修复(2026-11-22): yolo_post.cc 形态B 漏乘 stride

生产代码的 CPU 侧 DFL(`src/yolo_post.cc`):

```cpp
void compute_dfl(const OutTensor& t, uint32_t base, uint32_t grid_len, float box[4]) {
    ...
    box[b] = acc;          // ★ 注释自己写了: "l, t, r, b (格点单位)"
}
...
float cx = (static_cast<float>(j) + 0.5f) * static_cast<float>(stride);   // ← 像素
o.x1 = cx - d[0];                                                     // ← 像素 - 格点单位  ✗
```

DFL 解出来的是**格点单位**, 必须 ×stride 才是像素; 现在**像素减格点**, 单位混用。

**实测偏差**(4 帧, 与正确解码逐像素比):

| 帧 | 最大偏差 | 平均偏差 |
|---|---|---|
| f00000 | 142.75 px | 47.76 px |
| f00120 | 382.59 px | 45.13 px |
| f00400 | 95.95 px | 28.17 px |
| f00800 | 188.16 px | 46.53 px |

> **仓库里其实已经记录了这件事** —— `accuracy/acc_batch.cc:58-65`:
> "① DFL 解出来的距离是【格子单位】, 要乘 stride 才是像素; fork 导出的模型把 stride 烤进图里了,
>  所以工程里那一路不乘。**不乘的话框会小 8/16/32 倍** —— 实测出来是一堆几像素的碎框。"
>
> `accuracy/acc_batch.cc` 自己的解码器是对的(`o.x1 = cx - d[0] * stride`),
> `src/yolo_post.cc` 的形态B 分支漏了这个 ×stride。**这条路径从来没在生产上跑过**
> (生产模型是形态C), 所以 bug 一直潜伏着。

**修法**(2026-11-22 已在分支 `fix/p0-correctness-stability` 上实施并对拍通过,
偏差 0.0019 px / 1724 帧, 详见 `docs/p0_fixes_report.md`):

```cpp
o.x1 = cx - d[0] * stride;
o.y1 = cy - d[1] * stride;
o.x2 = cx + d[2] * stride;
o.y2 = cy + d[3] * stride;
```

---

## 八、选型建议(不 merge, 待人决策)

| 决策条件 | 实测 | 判定 |
|---|---|---|
| ΔNPU ≤ 1ms → 选形态C(现状), 不动 | **ΔNPU = −7.27 ms** | 不满足 |
| ΔNPU ≥ 3ms 且框精度是硬指标 → 选 fork9 | ΔNPU ≥ 3ms ✅; 框精度**不掉点(还准 2.5pp)** ✅ | **满足** |

**建议: 走 fork9, 但按这个顺序**

1. ~~**先修 `yolo_post.cc` 形态B 的 ×stride**(第七节)~~ —— **已完成**(2026-11-22, 偏差 0.0019 px)。
2. **生产模型目前拿不到 fork9 版本**(ebike 的 ONNX 源不在仓库) —— ebike 模型的 ONNX 源不在仓库里。
   要出候选, 需要算法组补 `ebike` 的 ONNX(或 .pt), 再用 `npu_layer_bench/scripts/make_res_models.sh`
   那套口径导一次 fork9。**这一步没做完之前, 手上只有 yolov8n 的 fork9 候选, 不是生产模型的。**
3. **先别急着提 GATE_MAX_FPS** —— 生产现在是限帧(79.96/80), 提上去之前 fork9 的收益是 0;
   提上去之前要先把 fork9 的模型、×stride 修复、精度回归三件事做完。
4. **DFL 解码搬上 CPU 之后要盯 CPU**: 形态B 是 4 路共用一个 OCR/检测池, DFL 每个候选框都要算,
   审计报告里"CPU 后处理(DFL+NMS) 实测 < 1 ms/帧"是形态C 的数, 换形态B 之后要重新测。

## 九、产物清单(都在沙盒, 未 merge)

```
opt_dfl/models/armB_fork9_dfl_on_cpu_int8.rknn    ← ★ 候选(fork9, DFL 在 CPU)
opt_dfl/models/armA_dfl_in_npu_int8.rknn          ← 对照臂(图内 DFL)
opt_dfl/models/yolov8n_dfl_in_npu{,_v2,_v3}.onnx   ← 手术出来的三个图内 DFL ONNX
opt_dfl/scripts/{make_dfl_in_npu_onnx*,verify_dfl_equiv,attribute_dfl_cost,
                 ab_harness,run_ab.sh,summarize_ab,board_quiesce}.*
opt_dfl/out/{audit_*.json,ab/*.json,ab_run_*.log}
```

**没有改任何生产文件**(`rk3588_zero_copy/src/**`、`config/gate.env`、生产模型一个都没碰)。
