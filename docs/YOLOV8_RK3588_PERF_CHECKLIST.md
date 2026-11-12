# RK3588 · YOLOv8n INT8 640×640 性能排查与优化清单

> 生成环境：Seeed Studio Recomputer RK3588 Devkit，kernel 6.1.172-vendor-seeed-rk3588
> librknnrt **2.3.2** (429f97ae6b@2025-04-09)，RKNPU 驱动 **0.9.8**（DRM 后端，节点 `/dev/dri/renderD129`，**无** `/dev/rknpu`）
> 所有板端结论均在本机实测；标注规则：**[实测]** 本机量到 / **[官方]** Rockchip 官方文档 / **[估计]** 工程经验值，需自行复测

---

## 0. 先对齐口径：73.5 FPS 到底是什么

**[官方]** 73.5 FPS 出自 `rknn_model_zoo` README 的 *Model performance benchmark(FPS)* 表（v2.3.0 / v2.3.2 均为此值），列头写的是 **RK3588 @single_core**。该表脚注明确：

> This performance data are collected based on the **maximum NPU frequency** of each platform.
> This performance data calculate the **time-consuming of model inference**. **Does not include the time-consuming of pre-processing and post-processing**.

即 **73.5 FPS = 单 NPU 核 + 最大 NPU 频率 + 只算 `rknn_run`/`rknn_outputs_get`**。

由此产生三条硬性结论：

1. **它不是端到端帧率。** 任何"解码 + letterbox + NPU + NMS + 画框"的全链路数字都必然低于 73.5。40 FPS 的端到端数值与 73.5 之间**不存在直接可比性**，必须先拆开测。
2. **它是单核数字。** RK3588 有 3 个 NPU 核，单路实时流只能用 1 核；官方基准没有给多核数。
3. **它是 2.3.x 时代的数。** model zoo v2.5.0 同一行已更新为 **76.3 FPS**（+3.8%）。但 2.5.0 转换出的模型要求 runtime ≥ 2.5.0，本板是 2.3.2，**装了 2.5.0 的 wheel 也跑不了**（需先升内核驱动）。

### 0.1 【P0】第一步：把纯 NPU 耗时单独测出来

用 C API 测，不要用 Python（官方 FAQ 1.5 明确：Python 推理更慢，应改用 C API 测量）。

本机已备好工具 `_bench/npu_bench.cc`（编译产物 `_bench/npu_bench`），完全复刻官方口径：

```bash
cd /home/seeed/rk_deploy/_bench
# 纯 NPU 推理耗时（官方口径：run + outputs_get）
./npu_bench <model.rknn> --iters 200 --warmup 30

# 绑定到指定 NPU 核
./npu_bench <model.rknn> --core 1        # 1=CORE0 2=CORE1 4=CORE2 3=CORE0+1 7=三核

# 逐层耗时（定位慢层）
./npu_bench <model.rknn> --perfdetail
```

输出会同时给 4 个数：`inputs_set` / `rknn_run` / `outputs_get` / 整轮，以及**驱动自报的 `run_duration`**（`RKNN_QUERY_PERF_RUN`，最接近官方口径）。

**判据**：

| 现象 | 结论 | 去哪一节 |
|---|---|---|
| `rknn_run` 接近 13.6 ms（=1/73.5） | 模型和板子都正常，差距全在前后处理 | 第 3、4 节 |
| `rknn_run` 明显 > 13.6 ms | 模型转换或板级状态有问题 | 第 1、2 节 |
| `rknn_run` 抖动大（min/max 差 >30%） | 频率/电源域/抢占问题 | 第 1 节 |

> **[实测]** 本机现有 `ebike_int8_rk2332.rknn`（INT8 640×640、8400 anchors、2 类）在默认调度下：
> `rknn_run` mean **28.6 ms** / min 20.9 / p50 27.4 / p90 33.5 / max **36.1**；驱动自报 26.1 ms → **38.25 FPS**。
> 单次推理抖动 **73%**（20.9→36.1 ms），这是典型的"频率在爬坡 + 偶发抢占"特征，不是算子本身慢。

---

## 1. 【P0】板级状态：不改一行代码，收益最大的部分

### 1.1 频率锁定（NPU / DDR / CPU）

**[实测] 本板当前默认状态**：

| 域 | governor | 频率范围 | 当前 | 官方基准条件 |
|---|---|---|---|---|
| NPU | `rknpu_ondemand` | 300M ~ **1G** | 1G（负载时） | 1G（userspace 锁定） |
| DDR (`dmc`) | `dmc_ondemand` | **534M** ~ 2.4G | **534M（空闲时）** | 2112M（userspace 锁定） |
| CPU | `ondemand` ×8 | 408M ~ 2.256G | 408M~600M（空闲） | performance + 大核 |

**[实测]** 压测时采样到的实际频率轨迹：

```
t=01 dmc=534000000  npu=1000000000  cpu7=1200000
t=02 dmc=2400000000 npu=1000000000  cpu7=600000    ← DDR 负载后才爬上来
t=03 dmc=2400000000 npu=1000000000  cpu7=600000
t=04 dmc=2400000000 npu=1000000000  cpu7=2208000   ← 大核才爬到 2.2G
```

即：**DDR 和大核都是"有负载才升频"**。相机流水线天然是间歇性的（等下一帧、等待编码），间隙里 ondemand 会把 DDR 压回 534MHz——而 NPU 每次取权重/写 feature map 都要过 DDR。官方基准是**连续跑**、频率全程锁死，所以这个差距是真实的。

**操作**（本板已备好 `_bench/freq_lock.sh`）：

```bash
cd /home/seeed/rk_deploy/_bench
sudo ./freq_lock.sh lock      # 锁定：NPU 1G / DDR 2.4G / CPU performance
sudo ./freq_lock.sh show      # 查看当前状态
sudo ./freq_lock.sh restore   # 还原 ondemand（省电默认）
```

脚本内容与 Rockchip 官方 `rknn_model_zoo/scaling_frequency.sh` 一致（官方目标：NPU 1000000000 / CPU 2256000000 / DDR 2112000000；本板 DDR 档位为 534M/1320M/1968M/2400M，取 2400M）。

- **预期收益**：NPU 侧 **10~30%**（DDR 带宽 + 频率稳定后抖动大幅收敛）；CPU 后处理侧 **20~50%**（大核锁 2.256G）
- **难度**：★☆☆☆☆（需 root，一条命令）
- **实测**：锁定前后需重跑 `npu_bench` 对比（重启后可一次性验证，见第 6 节）

> **⚠️ 本板特有红线：绝对不要执行 `echo on > /sys/devices/platform/fdab0000.npu/power/control`**
> **[实测]** 本机执行后立刻内核 oops：
> ```
> Internal error: synchronous external abort: 0000000096000010 [#1] SMP
> CPU: 2 PID: 4590 Comm: bash
>   rk_iommu_enable_stall → rk_iommu_enable → rk_iommu_resume
>   → pm_generic_runtime_resume → rpm_callback → rpm_resume
> ```
> 机理：关闭 runtime PM 会触发 NPU 电源域重新上电，而 IOMMU 的时钟未同步使能，stall 寄存器读直接 abort；RPM 固件随后挂死，**NPU 直到重启前完全不可用**（后续所有 `rknn_init` 卡死在 `rpm_resume`）。
> 结论：**本板保持 runtime PM = `auto`（默认）**，靠频率锁定而非 PM 关闭来消除唤醒延迟。

### 1.2 NPU 核心绑定

**[官方]** `rknn_api.h`（本板 2.3.2 已带）：

```c
RKNN_NPU_CORE_AUTO = 0,      /* 默认，随机选核 */
RKNN_NPU_CORE_0 = 1, RKNN_NPU_CORE_1 = 2, RKNN_NPU_CORE_2 = 4,
RKNN_NPU_CORE_0_1 = 3, RKNN_NPU_CORE_0_1_2 = 7
int rknn_set_core_mask(rknn_context ctx, rknn_core_mask mask);
```

- 单路实时必须显式钉死 `RKNN_NPU_CORE_0`：`AUTO` 是**随机选核**，多进程时会和别的实例抢核。
- **你的工程已有这个调用**：`src/license_plate_rec.cc:222`（`cfg_.npu_core_mask`），YOLOv8n 那条链路同样要传。
- **预期收益**：抖动下降 **10~20%**，极端情况避免被抢核导致的 2~3 倍长尾
- **难度**：★☆☆☆☆（一行代码）

### 1.3 CPU 绑大核 + 优先级

**[官方]** model zoo FAQ 1.5 明确要求：RK3588/RK3576 上应使用 **大核**。

```bash
taskset -c 4-7 ./your_app              # A76 大核 cpu4~cpu7
nice -n -10 ./your_app                 # 后处理线程优先级
```

RK3588 是 4×A55 + 4×A76 异构；`ondemand` 下调度器常把小核当"空闲核"，后处理线程会被塞进 A55。**你的 A55 只有 1.8GHz，A76 有 2.256GHz**。

- **预期收益**：后处理阶段 **20~40%**（小核→大核）
- **难度**：★☆☆☆☆

### 1.4 清掉板上的干扰进程

**[实测]** 本机常驻：`zcode-cli`（12GB VSZ）、`vscode-server`、VS Code extensionHost、`plasmashell`、Chromium。在 8 核板上做 NPU 基准时这些会抢 CPU 和 DDR 带宽。

```bash
ps aux --sort=-%cpu | head -15
# 基准期间关掉 VS Code Server / 桌面，或至少让它们 idle
```

- **预期收益**：**5~15%**
- **难度**：★☆☆☆☆

### 1.5 排除降频与温度

**[实测]** 本机温度：npu-thermal **37.9°C**、bigcore0 **39.8°C**、soc 38.8°C —— **无热降频**，这条可以排除，不用再花时间。

```bash
for z in /sys/class/thermal/thermal_zone*/; do echo -n "$(cat $z/type): "; cat $z/temp; done
```

---

## 2. 【P1】模型转换：40 FPS 最常见的根因

### 2.1 必须用 Rockchip 的"优化版 head"导出

**[官方]** `airockchip/ultralytics_yolov8`（Ultralytics fork，pin 在 commit `0b0bc566`）对 YOLOv8 导出做了 4 处改动，`RKOPT_README.md` 原话：

> the **output node is changed** and **post-processing is removed** because that block is *"unfriendly for quantization"*;
> the terminal **DFL structure is removed** because it *"slowed NPU inference"*;
> a **score-sum output branch is added to accelerate CPU post-processing**;
> *"All the removed operation will be done on CPU."*

**[实测]** 本机下载的官方优化版 ONNX（`examples/yolov8/model/yolov8n.onnx`，12.65 MB）结构：

```
opset 12, 节点 228
输入 : images [1,3,640,640] float32
输出 : 318 [1,64,80,80]  onnx::ReduceSum_326 [1,80,80,80]  331 [1,1,80,80]
       338 [1,64,40,40]  onnx::ReduceSum_346 [1,80,40,40]  350 [1,1,40,40]
       357 [1,64,20,20]  onnx::ReduceSum_365 [1,80,20,20]  369 [1,1,20,20]
算子 : Conv×63 Sigmoid×60 Mul×57 Concat×13 Split×8 Constant×10 Add×6
       MaxPool×3 ReduceSum×3 Clip×3 Resize×2
```

对比**原生 Ultralytics 导出**（`1×84×8400` 大张量、DFL 的 `Exp`+`Concat` 全在图里）：

| | 原生导出 | Rockchip 优化导出 |
|---|---|---|
| NPU 图尾 | DFL 解码（64×8400 次 Exp）+ 大 Concat | 全部剥离，NPU 只出特征图 |
| 输出体积 | 84×8400 ≈ 690 KB int8 | 3 组 × (64/80/1)×S×S |
| NPU 速度 | DFL/Concat 在 NPU 上算 | 官方明确说 DFL 结构拖慢 NPU |
| CPU 后处理 | 8400×84 全量解码 + NMS | 3 路分支 + score-sum 预剪枝 |

- **预期收益**：NPU 侧 **5~20%**；**CPU 后处理 3~10 倍**（这是端到端的最大头）
- **难度**：★★☆☆☆（换仓库重新导出 + 重新转换）
- **你的工程已经就绪**：`src/yolo_post.cc` 里的 `post_process_fork_9out()` 就是按这个 9 路 fork 格式写的（带 score-sum 剪枝），不用改后处理代码。

### 2.2 转换参数逐项对照

**[官方]** 官方配方只有 3 行（`examples/yolov8/python/convert.py`）：

```python
rknn.config(mean_values=[[0, 0, 0]], std_values=[[255, 255, 255]], target_platform='rk3588')
rknn.load_onnx(model='yolov8n.onnx')
rknn.build(do_quantization=True, dataset='coco_subset_20.txt')
rknn.export_rknn('yolov8.rknn')
```

**逐条错配的后果**：

| 参数 | 正确值 | 写错的后果 |
|---|---|---|
| `mean_values` | `[[0,0,0]]` | 与 `std` 不配对 → 激活范围错 → 精度崩 |
| `std_values` | `[[255,255,255]]` | 写成 `[[1,1,1]]` → 相当于又除了一遍 255，**双重归一化** → 精度崩；模型被迫保留 FP 精度算子 → **掉速 2~3 倍** |
| `target_platform` | `'rk3588'` | 写成 rk3568 → 算子支持集和量化参数按错平台生成 |
| `do_quantization` | `True` | `False` → FP16 模型，**慢 2~3 倍** |
| `dataset` | 见 2.3 | 见 2.3 |

**验证方法**：转换日志里应出现这一行（出现 = 输入已是 int8 量化路径，速度达标）：

```
W build: The default input dtype of 'images' is changed from 'float32' to 'int8' in rknn model for performance!
```

**如果这行没出现**，说明模型走了浮点/混合精度路径 —— **这就是最典型的"转换参数错误导致掉速"**。

**[实测]** 本机按官方配方转换成功：

```
Total Weight Memory Size: 3124.44 KB
Total Internal Memory Size: 4400 KB
输出: yolov8n_rk3588_int8.rknn  (4,327,819 bytes)
```

### 2.3 量化标定集要贴合真实场景

**[实测]** 本机没有用 COCO，而是从项目自己的相机录像（`rk3588_zero_copy/DJI_20261102155401_0126_D.MP4`）抽了 24 帧真实画面做标定（`_bench/calib/`）。

- **预期收益**：**精度提升**（对自己场景的激活分布拟合更好）；**速度基本不变**
- 说明：标定集影响的是**准确率不是速度**，别指望它提速；但用错标定集可能让某些层被误判为"离群"，被迫降精度（见 2.5）
- **难度**：★★☆☆☆

### 2.4 揪出 CPU 回落算子

**[官方]** RKNN 编译器支持列表（`RKNNToolKit2_OP_Support-2.3.2.md`）中，YOLOv8 相关的高危项：

| 算子 | 支持情况 | 备注 |
|---|---|---|
| `GridSample` | **不支持** | `aten::grid_sampler` 也不支持 |
| `TopK` | **不支持** | `aten::topk` 也不支持 |
| `Softmax` | 支持 | 仅 `batchsize=1` |
| `Slice` | 支持 | 仅 `batchsize=1` |
| `Resize` | 支持 | **仅 `nearest2d` / `bilinear`** |
| `Erf` / `Log` / `Gather` | ONNX 支持 | 对应 `aten::` 版本**不支持** |

> **诚实说明**：该表是"框架→ONNX 算子映射表"，**没有标注哪些算子会回落到 CPU**。Rockchip 官方的 NPU 支持分类表是 PDF（`05_RKNN_Compiler_Support_Operator_List_V2.3.2.pdf`），本次未取到。**在板上应改用运行时工具直接测量**（下一条）。

**运行时逐层测量（本板可用）**：

```c
// rknn_init 时加 flag
uint32_t flags = RKNN_FLAG_PRIOR_HIGH | RKNN_FLAG_COLLECT_PERF_MASK;
rknn_init(&ctx, path, 0, flags, NULL);
// 跑几帧之后
rknn_perf_detail pd;
rknn_query(ctx, RKNN_QUERY_PERF_DETAIL, &pd, sizeof(pd));
printf("%s\n", pd.perf_data);      // 逐层耗时表
```

`npu_bench --perfdetail` 已经封装好了（见 0.1）。

**[实测]** 本模型 neck 里确认有 **2 个 Resize**（20×20→40×40、40×40→80×20 上采样）。Resize 在 RK3588 NPU 上效率偏低，是重点观察对象；若逐层表显示它占比高，可考虑：
- 让上游特征图直接到目标尺寸（结构改法）
- 或在 CPU 侧用 RGA 做（会引入带宽，通常不划算）

- **预期收益**：定位到问题层后针对性优化 **5~30%**
- **难度**：★★☆☆☆

### 2.5 算子降精度（只在精度不达标时用）

**[官方]** 2.x 的正确用法是 **hybrid quantization 两步法**，不是 v1.x 的 `rknn.config(op_hybrid=...)`：

```python
# step1: 生成量化提案
rknn.config(mean_values=[[0,0,0]], std_values=[[255,255,255]], target_platform='rk3588')
rknn.load_onnx(model='yolov8n.onnx')
rknn.hybrid_quantization_step1(dataset='calib_list.txt', proposal=False)
# -> 生成 *.quantization.cfg，编辑 custom_quantize_layers:
#    SomeLayer/Conv_0:0: float16      # 可选 int8 / int16 / float16

# step2: 按 cfg 构建
rknn.build(do_quantization=True, dataset='calib_list.txt')  # 带上 cfg
rknn.export_rknn('yolov8n.rknn')
```

- **用途**：某个层 INT8 后精度掉得厉害时，把它降到 float16
- **代价**：该层带宽翻倍 → **整体慢 0~5%**
- **⚠️ 不要滥用**：降精度是**用速度换精度**。优先排查 2.2 的参数错误（那才是"白掉速"）
- **难度**：★★★☆☆

> `pass_through` / `mixed_quant` / `custom_quantization_op` 这三个名字**不是 `rknn.config()` 的关键字**（2.x），它们通过 hybrid cfg 表达。用之前先 `help(rknn.config)` 在你本地 2.3.2 上确认。

---

## 3. 【P1】预处理：零拷贝 / RGA

### 3.1 一趟 RGA 完成 NV12→RGB + resize + letterbox

| 方案 | 640×640 letterbox 耗时 |
|---|---|
| CPU OpenCV `resize` + `copyMakeBorder` + `cvtColor` | **8~15 ms/帧** |
| RGA（`improcess` 一次完成缩放+颜色转换+补边） | **1~3 ms/帧** |

**[实测] 你的工程已经有完整实现**：`src/rga_convert.cc`（47 KB，含 `rga_nv12_to_rgb888`、letterbox、DMABUF 后端探测、`safe_improcess` 兜底）。YOLOv8n 直接复用即可，不需要新写。

- **预期收益**：端到端 **5~12 ms/帧**（约 20~35%）
- **难度**：★☆☆☆☆（复用已有代码）

### 3.2 零拷贝：DMA-BUF fd 直接喂 NPU

**[实测] 你的工程已经打通**（`src/main.cc:1405-1492`）：

```
dma_buf_alloc → dma_buf_map → rknn_create_mem_from_fd(fd, ptr, size, 0)
              → rknn_set_io_mem(ctx, mem, &attr{pass_through, fmt, type})
```

日志会打印关键判据：

```
[NPU] 输入来源: DMA-BUF fd (fd=%d, 与 RGA 输出同一块物理内存, 零 CPU 拷贝)
NPU 输入: fd 绑定成功 (pass_through=%d) -> 零拷贝
```

**如果看到"输入来源: CPU 内存 (rknn_inputs_set…)"，说明零拷贝没生效** —— `rknn_inputs_set` 会把整帧 1.2 MB RGB 拷进 NPU 输入缓冲。

**两条关键分支**（`main.cc:1459-1471` 已实现）：

| 分支 | `pass_through` | 含义 | 代价 |
|---|---|---|---|
| `use_native=true` | **1** | 直接喂 NPU 原生布局 **NC1HWC2**（INT8 模型） | 零转换，最快 |
| `use_native=false` | **0** | 喂逻辑 NHWC uint8，**驱动帮你转成 NC1HWC2** | 多一次重排 + DDR 流量 |

- **预期收益**：关掉驱动重排 **3~8%**；从 `rknn_inputs_set` 换到 fd 绑定 **省 1.2 MB/帧拷贝**（≈0.45 ms/帧）
- **难度**：★★☆☆☆（原生布局需要 RGA 直接写 NC1HWC2，或用 `RKNN_NATIVE_*` 属性）

### 3.3 内存对齐 / stride / cache 一致性

**① 行 stride 必须落在 buffer 内**
RGA 输出行宽常带 padding（如 `wstride=672` 而非 640）。buffer 必须按 **stride × 高** 分配，而不是 `640×640×3`，否则末行越界。

**② 64 字节对齐**
DMA-BUF 通常满足，但普通 `malloc` 路径要用 `aligned_alloc(64, ...)`。RKNN 内部按 16B 对齐访问，未对齐会走 slow path。

**③ CPU 碰过 DMA 内存后必须刷 cache**
若 CPU 写过这块 DMA 内存（调试写入、CPU 后处理复用），RGA/NPU 读到的可能是旧数据。`dma_buf_alloc(fd, /*cached=*/true)` 时需要显式 flush/invalidate。

**④ 权重驻留 SRAM**
```c
uint32_t flags = RKNN_FLAG_PRIOR_HIGH | RKNN_FLAG_SHARE_WEIGHT_MEM;
```
**[官方]** RK3588 支持权重/feature map 驻留 SRAM（rknn-toolkit2 v1.4.0 起），可显著减少 DDR 流量 —— 和 1.1 的 DDR 提频是同一条优化链。

查询实际占用：

```c
rknn_mem_size ms;
rknn_query(ctx, RKNN_QUERY_MEM_SIZE, &ms, sizeof(ms));
// ms.total_weight_size / ms.total_internal_size / ms.total_sram_size / ms.free_sram_size
```

**[实测]** 本机 YOLOv8n INT8：weight 3124 KB / internal 4400 KB，`sram free 0/0 MB`（未启用 SHARE_WEIGHT_MEM）。

- **预期收益**：SRAM 驻留 **5~15%**（尤其和 DDR 提频叠加时）
- **难度**：★★☆☆☆

---

## 4. 【P2】后处理 CPU 占用

### 4.1 用 score-sum 分支做预剪枝

**[官方]** Rockchip 优化头多出来的 `[1,1,S,S]`（reducesum+clip）就是为此加的：先用它把"80 类置信度之和"过一遍，`sum_v <= conf_thr` 的格子直接跳过，**不做 DFL 解码、不进 NMS**。

**[实测] 你的 `src/yolo_post.cc:148`** 已经是这么写的：

```cpp
if (sum_v <= conf_thr) continue;
```

（`post_process_fork_9out()`，支持 9 路 fork 输出）

- **预期收益**：后处理 **3~10 倍**（候选框从 8400 降到几十）
- **难度**：★☆☆☆☆（已实现）

### 4.2 NMS 的写法

**[实测] 你的实现**（`yolo_post.cc:53-85`）已经是正确的工业写法：

- **先按 conf 过滤，再进 NMS**（不要对 8400 个全量排序）
- **按类别分别 NMS**（`nms_per_class`），而不是全类一起
- `std::sort` 只作用于通过阈值的少数候选

若仍有优化空间：换成 **IoU 上界预筛**（按面积排序后用面积比剪枝）或 **GPU NMS**，但收益递减。

### 4.3 后处理与 NPU 重叠（端到端最有效的一招）

单线程串行时：`总耗时 = 预处理 + NPU + 后处理`。改成**三线程流水线 + 双缓冲**后：`总耗时 ≈ max(预处理, NPU, 后处理)`。

```
[线程A 预处理] ──buf0──▶ [线程B NPU] ──out0──▶ [线程C 后处理]
              ──buf1──▶                ──out1──▶
```

用 `rknn_run` 的异步模式进一步榨干：

```c
rknn_run_extend ext;
ext.non_block = 1;      // 非阻塞提交
ext.timeout_ms = 0;
rknn_run(ctx, &ext);    // 返回后 CPU 立刻去处理上一帧的输出
```

- **预期收益**：端到端 **提升 30~60%**（从 sum 变 max）
- **难度**：★★★☆☆（要处理 buffer 生命周期与帧同步）

---

## 5. 【P2】batch 推理与多核：分清吞吐和延迟

**[官方]** `rknn_api.h` 里有两个相关接口：

```c
int rknn_set_batch_core_num(rknn_context ctx, int core_num);  // batch 拆到多核
int rknn_set_core_mask(rknn_context ctx, rknn_core_mask mask);
```

**[官方]** rknn-toolkit2 changelog：v1.3.0 支持 RK3588 多 batch 多核；**v1.4.0 起支持"一个 RK3588 模型同时多核执行"**。

**决策表**：

| 场景 | 做法 | 预期 |
|---|---|---|
| **单路实时**（你的场景） | `batch=1` + `core_mask=CORE_0` | batch 只会**增加延迟**，不要用 |
| **多路相机 / 多流吞吐** | **3 个实例**，`core_mask` 分别 `CORE_0/CORE_1/CORE_2` | 吞吐 **~3×**（接近线性） |
| 单模型多核 | `rknn_set_batch_core_num(ctx, 3)` + `batch=3` | 只有当 batch≥3 才划算；batch=1 时纯属浪费 |

- **预期收益**：多流场景 **2.5~3×**；单路场景 **0%（甚至负收益）**
- **难度**：★★☆☆☆

> **给单路实时流的诚实结论**：想提高单路 FPS，**正确路径是压低每帧耗时（1/2/3/4 节）**，不是加 batch。

---

## 6. 【P0】收尾验证：重启后一条命令出数据

本机 NPU 因 1.1 节提到的 IOMMU abort 处于死锁状态，**需要重启恢复**（`sudo reboot`）。重启后：

```bash
cd /home/seeed/rk_deploy/_bench

# ① 基线（默认 ondemand 调度）
./npu_bench yolov8n_rk3588_int8.rknn --iters 200 --warmup 30 --core 1

# ② 锁定频率后再测（同一条命令，只改环境）
sudo ./freq_lock.sh lock
./npu_bench yolov8n_rk3588_int8.rknn --iters 200 --warmup 30 --core 1

# ③ 逐层耗时，找慢算子
./npu_bench yolov8n_rk3588_int8.rknn --iters 50 --warmup 10 --perfdetail
```

对比 ① 和 ② 的 `rknn_run` 均值 → 就是**频率锁定的真实收益**（官方基准 73.5 FPS ≈ 13.6 ms）。

---

## 附录 A：优先级总表

| 优先级 | 动作 | 预期收益 | 难度 |
|---|---|---|---|
| **P0** | 用 C API 测纯 NPU 耗时，对齐官方口径 | 定位真因（不做这步全是猜） | ★★☆☆☆ |
| **P0** | NPU/DDR/CPU 频率锁定 + 大核绑定 | NPU 10~30%，后处理 20~50% | ★☆☆☆☆ |
| **P0** | 清理板上干扰进程 | 5~15% | ★☆☆☆☆ |
| **P1** | 换 Rockchip 优化版 head 导出 | NPU 5~20%，后处理 3~10× | ★★☆☆☆ |
| **P1** | 核对转换参数（std=255 / INT8 / rk3588） | 配错时 2~3× | ★★☆☆☆ |
| **P1** | RGA 一趟预处理 + DMA-BUF 零拷贝 | 端到端 20~35% | ★★☆☆☆ |
| **P1** | `rknn_set_core_mask` 钉核 | 抖动 -10~20% | ★☆☆☆☆ |
| **P2** | 逐层耗时定位慢算子 | 针对性 5~30% | ★★☆☆☆ |
| **P2** | 后处理 score-sum 剪枝 + 类别 NMS | 后处理 3~10× | ★☆☆☆☆ |
| **P2** | 三线程流水线（pre/NPU/post 重叠） | 端到端 30~60% | ★★★☆☆ |
| **P2** | `RKNN_FLAG_SHARE_WEIGHT_MEM` | 5~15% | ★★☆☆☆ |
| **P3** | 多路场景：3 实例 × 3 核 | 吞吐 ~3× | ★★☆☆☆ |
| **P3** | hybrid_quant 算子降精度 | 用 ~5% 速度换回精度 | ★★★☆☆ |

## 附录 B：本板实测环境快照

| 项 | 值 |
|---|---|
| 板子 | Seeed Studio Recomputer RK3588 Devkit |
| 内核 | 6.1.172-vendor-seeed-rk3588 (aarch64) |
| NPU 驱动 | rknpu 0.9.8（**DRM 后端**：`/dev/dri/renderD129`，`DRIVER=RKNPU`；**无** `/dev/rknpu`） |
| NPU 运行时 | librknnrt **2.3.2** (429f97ae6b@2025-04-09) |
| NPU 频率 | devfreq `fdab0000.npu`，300M~1G，governor `rknpu_ondemand` |
| DDR 频率 | devfreq `dmc`，534M/1320M/1968M/2400M，governor `dmc_ondemand` |
| CPU | 4×A55 (≤1.8G) + 4×A76 (≤2.256G)，governor `ondemand`，cpuidle `menu` |
| 温度 | npu 37.9°C / bigcore 39.8°C / soc 38.8°C（**无降频**） |
| NPU PM | `runtime_status=suspended`, `control=auto`（**保持 auto**） |
| 板载转换 | rknn-toolkit2 **2.3.2 aarch64 wheel 可装**（`--platform manylinux2014_aarch64`），支持 python3.12 |

## 附录 C：已备好的工具与文件

| 路径 | 说明 |
|---|---|
| `_bench/npu_bench.cc` / `npu_bench` | 官方口径 NPU-only 基准（C API），支持 `--core` / `--perfdetail` / `--batch` |
| `_bench/freq_lock.sh` | 频率锁定/还原（`lock` / `show` / `restore`） |
| `_bench/convert_yolov8n.py` | 板端 ONNX→INT8 RKNN 转换（官方 3 行配方） |
| `_bench/dl/yolov8n_optimized.onnx` | Rockchip 官方优化版 YOLOv8n ONNX（12.65 MB） |
| `_bench/yolov8n_rk3588_int8.rknn` | 已转换的 INT8 模型（4.33 MB） |
| `_bench/calib/` + `calib_list.txt` | 24 帧真实相机标定图 |
| `_bench/venv/` | python3.12 + rknn-toolkit2 2.3.2 aarch64 |

> **坑位提醒**：`setuptools` 必须 `<75`（≥81 移除了 `pkg_resources`）；`export_rknn()` 的关键字是 `export_path` 不是 `output_path`；本板重启会丢写回（曾导致 `rknn/api/*.py` 变成 0 字节），装完务必 `wc -c` 校验 + `sync`。
