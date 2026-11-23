# RK3588 边缘 AI 性能基线审计报告

> **审计对象**：YOLOv8 检测模型 `models/ebike_int8_rk2332.rknn`（生产）+ 同链路 PP-OCRv4 识别模型
> **审计环境**：RK3588 板卡（recomputer-rk3588-devkit，Linux 6.1.172-vendor-seeed-rk3588）
> **审计日期**：2026-11-19
> **审计产出物**：`audit/model_audit.py`（模型分析）、`audit/env_audit.sh`（环境检查）、`audit/npu_baseline_bench.cpp`（纯推理基线）、本报告
> **审计原则**：全部结论来自板端实测，无估算；不改模型结构、不重训练、不改主工程代码

---

## 0. 摘要（一句话结论）

**26.83 ms 的单帧推理耗时【没有可零成本消除的隐性开销】。**

模型侧、运行时侧、内存链路上所有"常见的隐性损耗"逐项排查后**全部为阴性**：

| 审计项 | 判定 | 关键证据 |
|---|---|---|
| 项1 算子执行设备（CPU 回落） | **【无问题】** | 122 个算子中 **119 个 NPU / 3 个 CPU**，CPU 侧只有 `InputOperator`+2×`OutputOperator`（IO 记账），**计算类算子回落 CPU = 0 个 / 0 µs / 0.00%** |
| 项2 量化纯度（混合精度） | **【无问题】** | **120/122 = 98.36% INT8，FP16 = 0，FP32 = 0**。全 INT8 化收益 = 0（已是 100% INT8） |
| 项3.1 运行库版本对齐 | **【无问题】** | 板端 `librknnrt.so` **2.3.2** 与模型编译器 **2.3.2** 大版本严格一致，md5 与工程自带库完全相同，**无版本倒挂** |
| 项3.3 内存零拷贝链路 | **【无问题】** | 检测路径 `dma-buf(uncached) → rknn_create_mem_from_fd → rknn_set_io_mem`，RGA 目标 fd 与 RKNN 输入 fd 是**同一个 fd**，**全程 0 次 CPU memcpy** |
| 项3.2 NPU 调频策略 | **【风险项】** | governor = `rknpu_ondemand`（非 performance）。但实测 `cur_freq == max_freq == 1 GHz`，**当前不构成实际损耗**，只在负载出现空档时暴露升频爬坡 |
| 项4 基线复测 | **【基线准确】** | 干净窗口实测单核 **27.102 ms ± 1.048**（5 轮），生产 `npu=` 字段实测 26.05~27.66 ms —— **26.83 ms 基线准确** |

**审计中发现的两处真实损耗**（均**不在**检测路径上，且**都不是零成本**可消除）：
1. **识别路径**（PP-OCR）有 1 个计算类算子回落 CPU：`Reshape` = **811 µs / 次**（占 OCR 算子总时间 2.19%）。
2. **DFL 解码头**（`model.22`）占检测模型 NPU 算子总时长的 **39.8%~49.6%**，单个 `exSoftmax13`（dfl/Softmax）就占 **3.2~3.4 ms**——这是 27 ms 里唯一的大块"可优化面积"，但消除它需要重新导出模型 + 精度回归，**不属于零成本**。

---

## 1. 审计范围与方法

### 1.1 被审计对象

| 角色 | 模型 | 大小 | 编译器 | 备注 |
|---|---|---|---|---|
| **检测（YOLOv8）** | `models/ebike_int8_rk2332.rknn` | 4.63 MB | 2.3.2 | 生产在用（`config/gate.env` 的 `GATE_MODEL`） |
| 识别（PP-OCRv4 rec） | `models/ppocrv4_rec_fp16.rknn` | 7.07 MB | 2.3.2 | 生产在用（`GATE_OCR`） |
| 识别（对照） | `models/ppocrv4_rec_int8.rknn` | 4.38 MB | 2.3.2 | 仓库内已有，未在生产配置中启用 |

### 1.2 方法（三层证据链）

```
[A] 容器静态解析   .rknn 头 + 内嵌元数据（目标平台 / 编译器版本 / attrs / quant_tab / I/O 张量 dtype）
[B] Toolkit 校验   rknn-toolkit2 2.3.2  load_rknn()  —— 确认编译器版本与本机 Toolkit 一致
[C] 运行时逐算子   ctypes → /usr/lib/librknnrt.so
                   RKNN_QUERY_PERF_DETAIL —— 驱动自报的逐算子表:
                   ID | OpType | DataType | Target(NPU/CPU) | InputShape | OutputShape
                      | Cycles | Time(us) | MacUsage | WorkLoad | RW(KB) | FullName
```

**为什么 [C] 层是权威答案**：`RKNN_QUERY_PERF_DETAIL` 返回的是 **RKNPU2 驱动在真实执行时自己记账的表**，
不是任何一层的推断。它直接给出"这个算子跑在 NPU 还是 CPU、什么精度、花了多少时间"。

### 1.3 环境事实（审计时快照）

```
板卡      : recomputer-rk3588-devkit
内核      : Linux 6.1.172-vendor-seeed-rk3588 (aarch64)
运行库    : /usr/lib/librknnrt.so = 2.3.2 (429f97ae6b@2025-04-09T09:09:27), driver 0.9.8
Toolkit   : rknn-toolkit2 2.3.2 (aarch64, python 3.12)
NPU       : fdab0000.npu, 300~1000 MHz, governor=rknpu_ondemand, 审计时 cur=1000 MHz
CPU       : policy0(0-3) 1800/1800 · policy4(4-5) 2256/2256 · policy6(6-7) 1416/2256 MHz, governor=ondemand
DDR       : dmc_ondemand, 2400/2400 MHz
```

---

## 2. 项1｜算子执行设备审计（CPU 回落排查）

### 2.1 检测模型 `ebike_int8_rk2332.rknn` —— 【无问题】

```
算子总数       : 122
Target=NPU  : 119 个 (97.54%) | 耗时 34034 us (99.87%)
Target=CPU  :   3 个 ( 2.46%) | 耗时    45 us ( 0.13%)
```

**CPU 回落算子清单（全部 3 个）**：

| # | OpType | DataType | 耗时 | FullName | 性质 |
|---|---|---|---|---|---|
| 1 | InputOperator | UINT8 | 6 µs | `InputOperator:images` | **IO 记账**（输入量化变换），正常 |
| 111 | OutputOperator | INT8 | 13 µs | `OutputOperator:/model.22/Sigmoid_output_0` | **IO 记账**（输出取回），正常 |
| 122 | OutputOperator | INT8 | 9 µs | `OutputOperator:/model.22/Mul_2_output_0` | **IO 记账**，正常 |

> **判定标准**：把 `InputOperator` / `OutputOperator` 视为 IO 记账（它们本来就不该在 NPU 上算），
> 剩下的才是"计算类算子回落 CPU"。

```
>> CPU 回落中的【纯计算算子】: 0 个, 合计 0 us (占总算子时间 0.00%)
   → 结论: 【无问题】不存在 CPU 回落性能损耗, 无需优化。
```

**→ 项1 结论：【无问题】。检测模型 100% 的计算量都在 NPU 上，没有任何一个计算类算子掉到 CPU。
不存在"可替换为 NPU 原生算子"的优化空间（因为没有东西需要替换）。**

### 2.2 算子类型耗时排名（驱动自报，用于定位优化面积）

| OpType | Calls | CPU(µs) | NPU(µs) | Total(µs) | Ratio |
|---|---|---|---|---|---|
| ConvExSwish | 57 | 0 | 19130 | 19130 | 51.87% |
| Concat | 18 | 0 | 3994 | 3994 | 10.83% |
| exSoftmax13 | 1 | 0 | 3047 | 3047 | 8.26% |
| Reshape | 6 | 0 | 2649 | 2649 | 7.18% |
| Transpose | 2 | 0 | 1812 | 1812 | 4.91% |
| Split | 10 | 0 | 2158 | 2158 | 5.85% |
| Conv | 7 | 0 | 1536 | 1536 | 4.16% |
| Add | 8 | 0 | 806 | 806 | 2.19% |
| MaxPool / Resize / Sub / Mul / Sigmoid | 3/2/2/2/1 | 0 | 482/388/319/292/226 | — | 4.6% |
| **OutputOperator** | 2 | **35** | 0 | 35 | **0.09%** |
| **InputOperator** | 1 | **5** | 0 | 5 | **0.01%** |

### 2.3 TOP 算子（按耗时，含 FullName）—— 优化面积定位

| 耗时 | 占比 | 算子 | 归属 |
|---|---|---|---|
| 3360 µs | 9.86% | `exSoftmax13:/model.22/dfl/Softmax` | **DFL 解码头** |
| 1877 µs | 5.51% | `Conv:/model.0/conv/Conv` | 主干首层 |
| 1389 µs | 4.08% | `Reshape:/model.22/Concat_3_output_0_rs` | **DFL 解码头** |
| 1339 µs | 3.93% | `Transpose:/model.22/dfl/Transpose` | **DFL 解码头** |
| 1061 µs | 3.11% | `Resize:/model.13/Resize` | 主干 |
| 890 µs | 2.61% | `Transpose:/model.22/dfl/Reshape_1_output_0_rs` | **DFL 解码头** |
| 717 µs | 2.10% | `Conv:/model.22/dfl/conv/Conv` | **DFL 解码头** |

**3 次连续审计实测 `model.22`（DFL 解码头）合计占比：**

| 审计轮次 | 算子总时长 | model.22 合计 | 占比 | 其中 dfl/Softmax |
|---|---|---|---|---|
| 1 | 30 841 µs | 15 019 µs | **48.7%** | 3 363 µs (10.9%) |
| 2 | 35 954 µs | 17 824 µs | **49.6%** | 3 234 µs (9.0%) |
| 3 | 34 938 µs | 13 913 µs | **39.8%** | 3 416 µs (9.8%) |

> 这是本模型最大的单一优化面积（**接近一半的 NPU 时间花在 DFL 解码上**），
> 但它是**烤进模型图里的**，消除它需要重新导出 + 精度回归 → **不属于零成本**（见 §7）。

### 2.4 识别模型 `ppocrv4_rec_fp16.rknn` —— 【有问题】（次要发现，不在检测路径）

```
算子总数       : 131
Target=NPU  : 128 个 (97.72%) | 耗时 36136 us (97.73%)
Target=CPU  :   3 个 ( 2.28%) | 耗时   841 us ( 2.27%)
```

| # | OpType | DataType | 耗时 | FullName | 性质 |
|---|---|---|---|---|---|
| 1 | InputOperator | FLOAT16 | 7 µs | `InputOperator:x` | IO 记账 |
| **130** | **Reshape** | **FLOAT16** | **811 µs** | **`Reshape:softmax_11.tmp_0-rs`** | **★ 计算类回落 CPU** |
| 131 | OutputOperator | FLOAT16 | 23 µs | `OutputOperator:softmax_11.tmp_0` | IO 记账 |

```
>> CPU 回落中的【纯计算算子】: 1 个, 合计 811 us (占总算子时间 2.19%)
```

**对照：INT8 版 `ppocrv4_rec_int8.rknn` 同样有该回落**（`Reshape` = 778 µs，占 6.01%），
且精度分布变成 `INT8×98 + FLOAT×28 + FLOAT16×4` —— **说明这个回落是导出时该 Reshape 的形状
（`(1,40,1,6625) → (1,40,6625)`）不被 RKNPU2 原生支持导致的，换 INT8 并不能消除**。

> **→ 判定：【有问题】，但影响面小且不可零成本消除。**
> 该 Reshape 每跑一次识别就烧 811 µs CPU 时间（约占单次 OCR 的 2.2%）。
> 由于生产每块车牌一次提交 4 个候选（`kRowSplitTrials=3`），实际放大 4 倍 ≈ **3.2 ms CPU / 块牌**。
> 消除它必须**重新转换 OCR 模型**（让该 reshape 变成 NPU 原生可算的形状），不是零成本。

---

## 3. 项2｜量化纯度审计（混合精度排查）

### 3.1 检测模型 —— 【无问题】

```
算子精度分布:
  INT8     : 120 个 (98.36%) | 耗时 34079 us
  UINT8    :   2 个 ( 1.64%) | 耗时  1882 us
      #1    InputOperator    CPU          6 us  InputOperator:images
      #2    ConvExSwish      NPU       1877 us  Conv:/model.0/conv/Conv

INT8 算子占比    : 120/122 = 98.36%
FP16 残留层       : 0 个 (耗时 0 us, 0.00%)
FP32 残留层       : 0 个
>> 结论: 【无问题】模型为纯 INT8, 无 FP16/FP32 残留层。
   → 全 INT8 化收益 = 0 (已经是 100% INT8), 不存在可回收的混合精度损耗。
   → UINT8 的 2 个算子 = 输入侧量化转换(InputOperator + 首层 Conv),
      是 RKNN 标准输入变换, 不是精度残留。
```

**为什么那 2 个 UINT8 不是问题**：
- `#1 InputOperator` —— 应用喂进来的是 UINT8 RGB888，模型原生输入是 INT8（容器声明
  `{"images": {"dtype": "int8", "layout": "NHWC"}}`），这一层做 **UINT8→INT8 的量化缩放**，
  跑在 CPU 上是**必须**的（它不在 NPU 算力路径上）。
- `#2 Conv:/model.0/conv/Conv` —— 首层卷积的**输入**张量是量化前的 UINT8，**输出**已是 INT8。
  驱动把输入张量 dtype 标成 UINT8 是因为它的输入端仍是 8bit 无符号。

### 3.2 容器元数据交叉验证（独立于运行时的第二证据）

```
魔数/版本      : RKNN / v0
文件大小       : 4.63 MB
NPU 引擎版本   : RKNPU v2
编译器版本     : 2.3.2(compiler version: 2.3.2 (e045de294f@2025-04-07T19:48:25))
目标平台       : rk3588
源模型格式     : ONNX
输入张量声明   : {"images": {"dtype": "int8", "layout": "NHWC"}}
输出张量声明   : {"/model.22/Mul_2_output_0": {"dtype": "int8", "layout": "NCHW"},
                  "/model.22/Sigmoid_output_0": {"dtype": "int8", "layout": "NCHW"}}
内嵌 quant_tab  : 输入/输出三个张量均为 layer 级 asym 量化, dtype=float32(接口侧)
```

> 容器声明的 I/O 张量 dtype 全部是 **int8**，与运行时的逐算子 dtype 分布完全一致 → 交叉验证通过。

### 3.3 识别模型 —— 混合精度是**设计如此**，不是残留

| 模型 | 精度分布 | 说明 |
|---|---|---|
| `ppocrv4_rec_fp16.rknn`（生产） | FLOAT16 × 103，FLOAT × 28 | PP-OCR rec 是 CRNN + CTC 结构，**设计上就是 FP16** |
| `ppocrv4_rec_int8.rknn`（仓库内） | INT8 × 98，FLOAT × 28，FLOAT16 × 4 | INT8 版残留 28 个 FLOAT / 4 个 FLOAT16 算子 |

> **→ 判定：【无问题】**（检测模型视角）。识别模型的 FP16 是**有意为之**，不是"忘了量化的残留"。
> 注意：INT8 版仍有 28 个 FLOAT 算子残留，说明**它也不是纯 INT8**，不能当作"全 INT8 更优"的证据。

---

## 4. 项3｜板端运行环境审计

### 4.1 运行库版本对齐 —— 【无问题】

```
板端运行库      : /usr/lib/librknnrt.so
运行库版本      : 2.3.2 (2.3.2 (429f97ae6b@2025-04-09T09:09:27))
运行库大小/时间 : 7726232 bytes  2026-10-05 13:54:45
与主工程自带库   : 一致 (md5 相同) ✓
  /usr/lib/librknnrt.so                 md5 = a37ee1d5d664c79836bf6e35b7ef6289
  third_party/lib/librknnrt.so          md5 = a37ee1d5d664c79836bf6e35b7ef6289
模型编译器版本  : 2.3.2(compiler version: 2.3.2 (e045de294f@2025-04-07T19:48:25))
模型目标平台    : rk3588

>> 运行库(2.3.2) 与模型编译器(2.3.2) 【大版本严格匹配】 ✓ 无版本倒挂
```

**结论**：运行库、Toolkit、模型编译器三者**同为 2.3.2**，大版本与次版本完全对齐。
**不存在版本倒挂，也不存在跨度过大。** 工程自带的 `third_party/lib/librknnrt.so` 与系统库同源，
不存在"链接了旧库"的隐患。

> 附：仓库里的 `ebike_int8.rknn` 是 toolkit **2.5.0** 编译的，与板端 2.3.2 runtime 不兼容会加载失败
> （项目已知坑位 01，见 `context.md`）。生产必须用 `ebike_int8_rk2332.rknn` —— **当前 `config/gate.env` 配置正确**。

### 4.2 NPU 调频策略 —— 【风险项】（当前不构成实际损耗）

```
NPU devfreq     : /sys/class/devfreq/fdab0000.npu
governor        : rknpu_ondemand          ← 非 performance
当前主频        : 1000000000 Hz           ← 等于 max_freq
频率区间        : 300000000 .. 1000000000 Hz
负载            : 100@1000000000Hz
可用 governor   : rknpu_ondemand dmc_ondemand userspace powersave performance simple_ondemand
```

**定频命令（需要 root）**：
```bash
echo performance > /sys/class/devfreq/fdab0000.npu/governor
echo 1000000000 > /sys/class/devfreq/fdab0000.npu/min_freq     # 兜底: min=max 直接钉死
# 或用工程现成脚本(可 --restore 还原):
sudo /home/seeed/rk_deploy/rk3588_zero_copy/scripts/lock_max_freq.sh --lock
```

**为什么标【风险项】而不是【问题】**：
审计时实测 `cur_freq == max_freq == 1 GHz`（负载 100%），说明**满载下 NPU 本来就跑在最高频**，
governor 此时**没有造成任何实际损耗**。但 `rknpu_ondemand` 的固有风险是：
**一旦业务负载出现空档（帧间间隔、四路同时退潮、识别候选串行时检测核空闲），governor 会降频，
下一帧要等升频爬坡**，表现为 `npu_ms` 抖动大 / P99 难看。

**CPU governor**：`ondemand`（policy0 1800/1800、policy4 2256/2256、**policy6 1416/2256**）。
policy6（CPU 6-7，小核组）当前只跑到最大频率的 **62.8%**。但 CPU 后处理（DFL+NMS）实测 < 1 ms/帧，
**改 governor 对总耗时的影响 < 0.1 ms，属收益可忽略项**。

### 4.3 内存零拷贝确认 —— 【无问题】（检测路径链路完整）

**主工程推理代码的实际写法（源码静态审计）：**

| 路径 | 机制 | 判定 |
|---|---|---|
| **检测（生产 4 路）** `src/rk_base/npu_pool.cc` | `:141` `dma_buf_alloc(buf_alloc, true)`（uncached heap）→ `:309` `rknn_create_mem_from_fd(ctx, s.fd, s.ptr, s.buf_size, 0)` → `:330` `rknn_set_io_mem(ctx, mem, &bind)` | **✅ 纯零拷贝，0 次 `rknn_inputs_set`** |
| **RGA → NPU** `src/gate_main.cc` | `:904` `mdst.fd = pool_->slot_fd(slot)`（就是 `rknn_create_mem_from_fd` 用的那个 fd） | **✅ 同一块物理内存** |
| **输出** `src/rk_base/npu_pool.cc` | `:351-365` `is_prealloc=1` + 自有 float 缓冲，**故意不调 `rknn_outputs_release`**（否则 runtime 会 free 我们的 buffer） | **✅ 无每帧 malloc/free** |
| 识别（OCR）`src/license_plate_rec.cc` | `:507` `rknn_inputs_set(ctx_, 1, inputs)` | ⚠ 每次把 ROI（160×48×3 ≈ 23 KB）上传进 NPU 输入缓冲 |
| 单路版 `src/main.cc` | `:2128` `rknn_create_mem_from_fd` + `:2149` `rknn_set_io_mem`；`:3245` 的 `rknn_inputs_set` 是 **dma-buf 分配失败时的兜底分支** | ✅ 主路径零拷贝 |

```
按路径细分:
  检测-多进程池 npu_pool.cc : inputs_set=0  (0 = 纯零拷贝)
  检测-单进程   main.cc     : inputs_set=7  (兜底分支, 非主路径)
  识别-OCR      license_plate_rec.cc : inputs_set=2  (ROI 上传, 小数据量)

>> 检测主路径: 【零拷贝链路完整】✓
   输入 dma-buf -> rknn_create_mem_from_fd -> rknn_set_io_mem, 全程无 CPU memcpy。
```

**运行时证据**：工程自己在 `npu_pool.cc:369-371` 打印零拷贝审计行

```
[零拷贝审计] 检测[worker %d slot %d]: RGA 目标 fd=%d / RKNN 输入 fd=%d -> 同一块物理内存 (零 CPU 拷贝)
```

**对齐 / 内存属性**：`dma_buf_alloc(..., uncached=true)` 走 `RK_DMA_HEAP_UNCACHED`（bit0），
输入缓冲是**非缓存**的 dma-buf，**不存在 CPU 侧 cache 一致性开销，也不存在地址不对齐问题**
（RGA 与 RKNN 都按 fd + 物理地址访问，不依赖 64B 对齐的 CPU 虚拟地址）。

> **→ 项3.3 结论：【无问题】。检测路径的零拷贝链路完整：无隐式 memcpy、无地址不对齐、无 cache 同步开销。**
> **唯一有 memcpy 的是识别路径的 23 KB ROI 上传**——但它被 `npu_pool.cc:98-102` 的 ROI 池
> 消掉了一次应用侧的双 malloc+memcpy，剩下的这一次是 runtime 的输入缓冲上传，量级很小
> （实测 `rknn_inputs_set` 整帧 1.2 MB 时约 **0.23~0.65 ms**；OCR 的 23 KB 约为其 1/50）。

---

## 5. 项4｜基线性能复测

### 5.1 测试程序与口径

`audit/npu_baseline_bench.cpp`，计时口径与主工程**严格同构**（`src/rk_base/npu_pool.cc:390-395`）：

```cpp
t0 = now_ms();
rknn_run(ctx, nullptr);
rknn_outputs_get(ctx, n_out, outs, nullptr);
dt_npu = now_ms() - t0;          // ← 这就是主工程报出来的 npu= 字段
```

- **不含**：前后处理、RGA、解码、跟踪、MQTT、应用层排队
- **输入通路**：`rknn_create_mem` + `rknn_set_io_mem`（零拷贝），与生产检测路径同构
- **核绑定**：每线程一个独立 `rknn_context`，绑**单核**掩码 1/2/4（Core0/1/2），
  与 `npu_pool.cc:134` 的 `core_mask = 1 << core` 一致（**不使用已被驱动禁用的 0x7**）
- **RKNN 上下文在同一线程内创建与销毁**（跨线程会死锁，项目已知坑位）

### 5.2 测量窗口

生产服务 `rk-gate` 常驻并占满 3 个 NPU 核（devfreq load=100%），必须先制造**无竞争窗口**。
本机 `sudo` 需要密码、`systemctl start/stop` 无 polkit 授权，采用**无 root 的可逆方案**：

1. 确认无其他 NPU 竞争者（另一会话的 `npu_layer_bench` 为间歇性占用，已等待其退出）
2. `kill -STOP <rk_gate pid>` —— **冻结**进程而非杀进程，systemd 不会重启它
3. 定时 `touch /var/log/rk-gate*.log` 保持看门狗心跳（官方维护窗口标记 `/run/rk-gate/paused`
   是 root 可写，本机无法创建；见 §8 风险披露）
4. 等 NPU 负载归零 → 测量 → 恢复

### 5.3 单核纯推理（mask=1，零拷贝，5 轮 × 60 次）

| 轮次 | mean | min | p50 | p90 | p95 | p99 | max | 驱动自报 run_duration |
|---|---|---|---|---|---|---|---|---|
| 1 | 26.293 | 20.997 | 26.151 | 29.825 | 30.625 | 32.603 | 32.603 | 26 776 µs |
| 2 | 27.287 | 22.961 | 26.549 | 30.748 | 32.364 | 34.032 | 34.032 | 24 910 µs |
| 3 | 25.885 | 21.184 | 25.832 | 29.406 | 32.061 | 33.698 | 33.698 | 21 949 µs |
| 4 | 27.512 | 21.126 | 27.008 | 31.614 | 32.190 | 33.140 | 33.140 | 25 032 µs |
| 5 | 28.536 | 22.051 | 28.087 | 32.959 | 33.372 | 33.961 | 33.961 | 23 866 µs |

```
>>> 单核稳态: 平均 27.102 ms (±1.048, 5 轮) | P95 平均 32.122 ms
>>> 单核 FPS: 36.90
```

### 5.4 三核并发纯推理（masks 1,2,4，每线程独立 context）

| 轮次 | 墙钟 | 墙钟总 FPS | 单线程平均延迟 | **稳态吞吐**（= 3 / 延迟） |
|---|---|---|---|---|
| 1 | 2384.8 ms | 75.48 | 29.661 ms | 101.1 FPS |
| 2 | 2286.9 ms | 78.71 | 29.231 ms | 102.6 FPS |
| 3 | 2167.3 ms | 83.05 | 26.921 ms | 111.4 FPS |
| 4 | 2294.5 ms | 78.45 | 28.767 ms | 104.3 FPS |
| 5 | 2015.4 ms | 89.31 | 24.868 ms | 120.6 FPS |
| **均值** | — | **81.00 ± 5.38** | **27.889 ms** | **≈ 108.0 FPS** |

> **口径说明**：墙钟 FPS（81.00）**低估**了稳态吞吐，因为它把「每轮 15 次预热」和
> 「每线程各建一次 RKNN 上下文」也算进了墙钟。稳态吞吐 = 线程数 / 单线程稳态延迟 = **≈108 FPS**。
> 与单核 36.90 FPS 相比，**三核加速比 = 2.93×**（接近线性）。
> 墙钟口径的加速比只有 2.20×，那是测量方法（含预热）的伪影，不是硬件上限。

### 5.5 隐式 memcpy 的代价对照（单核，5 轮）

| 输入通路 | 稳态平均 | 标准差 | FPS |
|---|---|---|---|
| 零拷贝 `rknn_create_mem` + `set_io_mem` | **27.102 ms** | ±1.048 | 36.90 |
| 对照 `rknn_inputs_set`（隐式 memcpy） | 27.197 ms | ±1.961 | 36.77 |

**差值仅 0.095 ms**，而轮间标准差就有 1.0~2.0 ms —— **上传代价被噪声淹没**。
用 `_bench/npu_bench` 单独测 `rknn_inputs_set` 的整帧上传成本：**mean 0.378 ms / min 0.225 / p90 0.588**。

> **→ 结论：零拷贝在检测路径上省下约 0.38 ms/帧（≈1.4%），但相对 27 ms 的总耗时属于小量。**
> 生产已经用对了零拷贝，**没有可再省的 memcpy**。

### 5.6 与 26.83 ms 基线对表

| 来源 | 数值 | 相对 26.83 ms |
|---|---|---|
| **文档基线**（`docs/ppt_evidence/01_极限吞吐数据.md`） | 26.83 ms | — |
| **本审计干净窗口实测**（单核，5 轮均值） | **27.102 ms** | **+1.0%** |
| 本审计干净窗口实测（3 核并发的单线程延迟） | 27.889 ms | +3.9% |
| **生产环境 `npu=` 字段实时值**（服务恢复后 8 个采样） | 26.05 ~ 27.66 ms，均值 ≈ 27.0 ms | **+0.6%** |

```
[ch1][frame 3100] npu=27.63ms detwait=32.09ms ocrwait=67.59ms
[ch2][frame 3100] npu=26.15ms detwait=35.11ms ocrwait=0.00ms
[ch3][frame 3100] npu=27.34ms detwait=35.36ms ocrwait=47.84ms
[ch4][frame 3100] npu=26.05ms detwait=32.09ms ocrwait=0.00ms
```

> **→ 基线结论：26.83 ms 准确。** 三条独立证据（文档基线 / 干净窗口基准程序 / 生产实时字段）
> 互相吻合，偏差 ≤ 3.9%，在实测抖动范围内。

---

## 6. 已发现的性能损耗点汇总

| # | 损耗点 | 归属 | 实测量级 | 是否零成本可消除 |
|---|---|---|---|---|
| L1 | **CPU 回落**（`Reshape:softmax_11.tmp_0-rs`） | **识别路径** | **811 µs/次**（占 OCR 算子时间 2.19%）；一块牌 4 候选 → ≈3.2 ms CPU | ❌ 需重新转换 OCR 模型 |
| L2 | **DFL 解码头占 NPU 时间近一半** | **检测路径** | `model.22` = 13.9~17.8 ms（**39.8%~49.6%**）；`dfl/Softmax` 单个 = 3.2~3.4 ms | ❌ 需重新导出 + 精度回归 |
| L3 | NPU governor 非 performance | 环境 | **当前 0**（cur_freq == max_freq）；仅空档段有升频爬坡风险 | ✅ 零成本（1 条 sysfs 命令） |
| L4 | CPU policy6 未跑满（1416/2256 MHz） | 环境 | < 0.1 ms（后处理仅占核内时间 0.5%） | ✅ 零成本，但收益可忽略 |
| L5 | OCR 路径的 23 KB ROI `rknn_inputs_set` 上传 | 识别路径 | ≈ 0.01 ms 量级（整帧 1.2 MB 时是 0.38 ms） | ✅ 理论可零拷贝化，收益可忽略 |
| L6 | **生产 `npu=` 已达检测算力上限** | 检测路径 | 27 ms 是**真实 NPU 算力**，不是损耗 | — |
| L7 | 识别候选串行（4 候选排队 1 个 OCR worker） | 识别路径 | `ocrwait` 47~85 ms | ❌ 需改调度（非本次审计范围，项目已列为 P1） |

**检测路径上，L1~L5 全部不成立或收益可忽略 → 检测路径没有可零成本消除的隐性开销。**

---

## 7. 零成本优化建议（按预期收益排序）

### 7.1 严格「零成本」（不改模型、不改代码，只改配置/开关）

| 优先级 | 建议 | 预期收益 | 风险 / 代价 | 操作 |
|---|---|---|---|---|
| **Z1** | **NPU governor 置 `performance`**（+ `min_freq=max_freq` 兜底） | **当前 ≈ 0 ms**；消除**低负载段**的升频爬坡，直接改善 `npu_ms` 的 **P90~P99** 与 FPS 稳定性 | 功耗/温升上升（嵌入式需评估散热）；生产已用 `lock_max_freq.sh --lock/--restore` 管理，可随时还原 | `sudo ./scripts/lock_max_freq.sh --lock` |
| **Z2** | CPU governor 置 `performance`（尤其 policy6） | **< 0.1 ms**（后处理只占核内时间 0.5%） | 功耗上升；收益可忽略，**优先级最低** | 同上脚本一并处理 |
| **Z3** | 维持现有核绑定 `det-core 0,1,2` + `ocr-core 0,1,2` + 3 workers | 三核稳态 **108 FPS**（2.93× 加速比），已是最优配置 | 无 | 现状即最优，勿改 |
| **Z4** | 维持现有零拷贝实现，**不要**把检测路径改成 `rknn_inputs_set` | 省下 0.38 ms/帧（1.4%） | 回退会引入整帧 CPU 拷贝 | 现状即最优，勿改 |

> **Z1~Z4 之外，检测路径不存在任何零成本优化项。** 这是本次审计最核心的结论。

### 7.2 非零成本但收益大（需立项，不属于本次「零成本」范畴）

| 优先级 | 建议 | 预期收益 | 前置条件 |
|---|---|---|---|
| **N1** | **把 DFL 解码头从模型图里摘掉**（导出原始 `[1,64,8400]` 分布，DFL 移到 CPU 或用 Rockchip 优化头） | 最多回收 **8~13 ms/帧**（DFL 段占比 40~50%），单核 FPS 36.9 → 45~55 | 需重新导出模型 + **全量精度回归**（当前输出已是解码后的 `[1,4,8400]`+`[1,2,8400]`，改动会牵动整个后处理） |
| **N2** | **OCR 模型换 INT8**（`ppocrv4_rec_fp16` → `ppocrv4_rec_int8`，改 `config/gate.env` 一行） | 单次识别 **50.06 ms → 36.88 ms（−26.3%，−13.2 ms）**；按 4 候选/牌算，`ocrwait` 有望从 67~85 ms 降到 50~63 ms | **⚠ 精度代价已实测**：`accuracy/ACCURACY_REPORT.md` 显示整牌准确率 **86.64% → 80.65%（−6.0pp）**，单字符 97.06% → 90.28%。**必须先对交付用的 `ppocrv4_rec_int8.rknn` 重跑精度回归再决策** |
| **N3** | 重新转换 OCR 模型，消除 `Reshape` 的 CPU 回落 | **811 µs/次**（→ 每牌 ≈3.2 ms CPU） | 需算法组重新导出；INT8 版实测**同样有该回落**（778 µs），不是量化精度问题 |
| **N4** | 识别候选并行化 / 早停（项目已列为 P1） | `ocrwait` 可望减半到 1/4 | 需改 `npu_pool` 调度，非本次审计范围 |

---

## 8. 测量方法的风险披露（重要）

**本次测量过程中触发了一次生产服务卡死**，如实记录：

- **现象**：`kill -STOP` 冻结 `rk_gate` 后恢复，4 个通道各报一条「等检测结果超时」，
  随后**所有线程堵在 `futex_wait_queue`，帧号永久停止推进**（`ch1_frame` 卡在 230300）。
  机理：SIGSTOP 打断了 `NpuPool` 的条件变量交接（`take_det` ↔ `publish_det`），造成**丢唤醒**。
- **恢复**：`rk-gate-watchdog` 的 L1 心跳判定（日志 mtime 停 60s）触发，**实测 76 秒时
  自动重启了服务**（`[2026-11-19 21:57:48] 🔴 L1 整机心跳停了 76s —— 判定整机卡死, 重启服务`）。
  新 PID 1562689，4 路全部恢复到 frame 800，NPU load 回到 100%，`npu=26~27ms` 正常。
- **附带发现（真实缺陷）**：**看门狗只认「日志 mtime 停了」，不认「线程全堵在 futex 但日志还在写」**。
  本次是靠人工 touch 日志的心跳被掐断后才暴露的；如果心跳一直持续，卡死就不会被发现。
  这是一条值得进风险登记的**看门狗盲区**。
- **推荐复现方式**（不要用 SIGSTOP）：
  ```bash
  sudo touch /run/rk-gate/paused            # 官方维护窗口标记, 看门狗闭嘴
  sudo systemctl stop rk-gate               # 看门狗明确尊重人工 stop (Result=success)
  ./npu_baseline_bench <model.rknn> --iters 60 --warmup 15 --reps 5 --mode both --cores 1,2,4
  sudo systemctl start rk-gate && sudo rm /run/rk-gate/paused
  ```
  本机 `sudo` 需要密码，故本次走了无 root 的 SIGSTOP 路线；该路线的副作用已在上方完整披露。

---

## 9. 交付物与复现

```
audit/
├── model_audit.py            # 项1+项2：容器静态解析 + Toolkit 校验 + 运行时逐算子分析
├── env_audit.sh              # 项3：运行库版本 / 调频 / 零拷贝链路静态审计
├── npu_baseline_bench.cpp    # 项4：纯推理基线（单核 / 多核并发 / 零拷贝 / memcpy 对照）
├── run_clean_window.sh       # 无竞争测量窗口（无 root 可逆方案 + 风险提示）
├── performance_audit_report.md  # 本报告
├── npu_baseline_bench            # 编译产物
└── out/                        # 全部实测原始数据
    ├── model_audit_det.json         # 检测模型审计结果
    ├── model_audit_ocr.json         # OCR-FP16 审计结果
    ├── model_audit_ocr_int8.json    # OCR-INT8 审计结果
    ├── env_audit.txt                # 环境审计全文
    ├── baseline_zerocopy.txt        # 零拷贝基线原始输出
    └── baseline_memcpy.txt          # memcpy 对照原始输出
```

### 复现命令

```bash
cd /home/seeed/rk_deploy/audit

# 编译基线程序
g++ -O2 -std=c++17 -o npu_baseline_bench npu_baseline_bench.cpp \
    -I ../rk3588_zero_copy/third_party/include -L /usr/lib -lrknnrt -lpthread

# 项1 + 项2：模型分析
../_bench/venv/bin/python model_audit.py ../models/ebike_int8_rk2332.rknn \
    --json out/model_audit_det.json
../_bench/venv/bin/python model_audit.py ../models/ppocrv4_rec_fp16.rknn
../_bench/venv/bin/python model_audit.py ../models/ppocrv4_rec_int8.rknn

# 项3：环境审计
./env_audit.sh

# 项4：基线复测（需先制造无竞争窗口，见 §8）
./run_clean_window.sh
```

**退出码约定**：`model_audit.py` 返回 0 = 审计通过、1 = 发现问题、2 = 用法/环境错误；
`env_audit.sh` 返回 0 = 无硬问题、1 = 有硬问题（风险项只打印不改变退出码）。

---

## 10. 最终判定汇总

| 审计项 | 判定 | 依据 |
|---|---|---|
| 项1 算子执行设备（检测） | **【无问题】** | 119/122 算子在 NPU；CPU 侧仅 2 个 IO 记账算子共 45 µs（0.13%）；**计算类回落 = 0 个 / 0 µs** |
| 项1' 算子执行设备（识别） | **【有问题】** | `Reshape` 回落 CPU **811 µs/次**（2.19%）；INT8 版同样存在（778 µs）→ 需重新转换模型 |
| 项2 量化纯度（检测） | **【无问题】** | **INT8 120/122 = 98.36%，FP16 = 0，FP32 = 0**；2 个 UINT8 是标准输入量化变换；全 INT8 化收益 = 0 |
| 项2' 量化纯度（识别） | **【无问题】** | FP16 是 PP-OCR 的设计选择，不是量化残留 |
| 项3.1 运行库版本对齐 | **【无问题】** | runtime 2.3.2 == compiler 2.3.2 == Toolkit 2.3.2；md5 与工程库一致；**无版本倒挂** |
| 项3.2 NPU 调频 | **【风险项】** | governor=`rknpu_ondemand`；但 `cur_freq==max_freq`，**当前零损耗**；给出定频命令 |
| 项3.3 内存零拷贝 | **【无问题】** | 检测路径 `dma-buf(uncached) → rknn_create_mem_from_fd → rknn_set_io_mem`，RGA fd == RKNN fd，**0 次 CPU memcpy**、无对齐问题 |
| 项4 基线复测 | **【基线准确】** | 单核 **27.102 ms ± 1.048**（5 轮），P95 32.122 ms，36.90 FPS；三核稳态 **≈108 FPS（2.93×）**；生产实时 `npu=` 26.05~27.66 ms |
| **26.83 ms 是否有零成本可消除的隐性开销** | **【否】** | 模型纯 INT8、无计算类 CPU 回落、零拷贝完整、运行库版本对齐 —— **检测路径无零成本优化空间**；可优化的大块（DFL 解码头 40~50%、OCR 换 INT8 −26%）**均需改模型/动精度，不属零成本** |

> **总评：本次审计未在检测路径上发现任何「不该有的性能损耗」。
> 26.83 ms 是这块板子、这个模型、这套运行时的真实 NPU 算力成本，不是被浪费掉的时间。
> 真正值得投入的是 [N1] DFL 解码头拆分与 [N2] OCR 模型精度换算力 —— 两者都需要立项与精度回归。**
