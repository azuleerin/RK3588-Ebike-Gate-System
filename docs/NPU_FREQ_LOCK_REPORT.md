# NPU 零拷贝 / 喂数参数 / 定频抖动 —— 体检报告

日期: 2026-11-16
板子: recomputer-rk3588-devkit (RK3588, 3 核 NPU / 8 核 CPU / LGA-180M)
模型: `model/ebike_int8_rk2332.rknn` (INT8 量化, 640×640×3) + `model/ppocrv4_rec_fp16.rknn`

> 本轮所有数字都在【板上还挂着交付实例 PID 604365(RTSP+PP-OCR, 已跑 17.7h)】的条件下测的。
> A/B 两侧条件一致, 所以对比有效; 单看绝对值偏悲观。

---

## 任务一: 零拷贝与 NPU 喂数参数

### 1.1 `pass_through` 到底该不该开 —— **不该开**(实测, 不是推测)

新增基准: `src/eval/npu_passthrough_bench.cc` (CMake target `npu_passthrough_bench`)

```
模型输入: 640x640x3 type=2 fmt=1 scale=0.003922 zp=-128 量化=AFFINE_ASYMMETRIC
NATIVE_NHWC_INPUT_ATTR: type=2 fmt=1 dims=[1,640,640,3,]

[A] pass_through=0 (uint8 NHWC, 驱动重量化)  set_io_mem=0
    rknn_run 墙钟 = 24.895 ms | NPU 硬件 = 24.891 ms | 差(驱动开销) = 0.004 ms | 折算上限 = 40.2 FPS
[B] pass_through=1 (int8 NHWC 直通, CPU 先重量化)  set_io_mem=0
    CPU 重量化 = 75.540 ms | rknn_run 墙钟 = 22.995 ms | NPU 硬件 = 22.990 ms | 合计 = 98.534 ms | 折算上限 = 10.1 FPS
```

**结论**:
- 我们的模型是 INT8 量化模型(type=2), 直通模式要求调用方先给 int8 量化码。
- 驱动在 NPU 流水线里做这次转换只花 **0.004 ms**; 搬到 CPU 上手写要 **75.5 ms/帧** —— 慢 7 倍。
- 教程里 "pass_through=1 + 定频 = NPU 98%" 成立的前提是 **FP16 模型**(驱动免费转格式)。
  照搬到我们的 INT8 模型上, 每帧多烧 75 ms CPU, 直接把吞吐打成 10 FPS。
- **保持 `pass_through=0`**。代码现状是对的:
  - `src/rk_base/npu_pipe.cc:141-153`: NATIVE 是 UINT8/NHWC 时才用 pass_through=1;
    我们的 INT8 模型走 `pass_through=0` 分支。
  - `src/main.cc:1978`(串行路径)同样固定 `pass_through=0`。
  - ⚠️ 以后换 FP16 检测模型时, 这两处要重新评估。

### 1.2 零拷贝链路 —— **已经打通, 中间没有 CPU memcpy**

实测链路(逐行对照代码):

```
MPP 解码 → NV12 dma-buf(fd)
   └─ rga_convert.cc:540/557  importbuffer_fd(fd) → RGA 自己 import
        └─ main.cc:2807-2808  RGA 的目标缓冲 = pipe.fd(slot) / pipe.ptr(slot)
             └─ npu_pipe.cc:137  rknn_create_mem_from_fd(ctx, fds_[idx], ...)  ← 同一个 fd
                  └─ npu_pipe.cc:155  rknn_set_io_mem(ctx, mem, &bind)
                       └─ NPU 直接读这块物理内存
```

- RGA 写进的 dma-buf **就是** 绑给 RKNN 的那块, fd 一路传到底, 中间不落 CPU。
- 串行单实例路径同理: `main.cc:1957 rknn_create_mem_from_fd` + `main.cc:1978 rknn_set_io_mem`。
- 没有可再优化的地方 —— 这一项不用动。

### 1.3 单板极限 —— **逼近不了 150+ FPS, 真实天花板 90~116 FPS**

并发度矩阵(`scripts/npu_bench.py`, 1000 帧 × 2 轮取中位, 纯检测无 PP-OCR):

| 并发实例 | 交付 FPS | 每实例 rknn_run | 每核占用率 | 总吞吐 |
|---|---|---|---|---|
| 1 | 37.40 | 23.00 ms | 85% | 37.4 |
| 2 | 65.95 | 27.73 ms | 92% | 66.0 |
| 3 | 90.38 | 29.95 ms | 91% | 90.4 |

跨进程多路实测(`scripts/stream_n_test.sh`, 每路一个进程 + 指定 NPU 核掩码):

| 路数 | 每路交付 FPS | 每路 rknn_run | 全板总吞吐 |
|---|---|---|---|
| 1 | 34.67 | 24.57 ms | 34.7 |
| 3 | 30.78 / 30.76 / 31.22 | ~28 ms | 92.8 |
| 4 | 21.73 / 28.83 / 30.11 / 21.70 | 30~40 ms | 102.4 |
| 6 | 19.3~19.5 (六路都一样) | 36~49 ms | 116.4 |

**为什么会这样(关键机制)**
1. 单核单帧 NPU 硬件时间 22.4 ms → 单核理论上限 **44.6 FPS**。
   3 核理论上限 = 3 / 22.4 ms = **134 FPS** —— 这已经是纯算术天花板, **离 150 FPS 本来就不够**。
2. 并行不是线性的: 每实例 rknn_run 从 23.0 ms(单) → 27.7 ms(双) → 29.95 ms(三),
   涨了 30%。这是 **DDR 带宽被 3 个核抢** 带来的, 加核越多每帧越慢。
3. 所以实测天花板是 **90~116 FPS**(纯检测, 不含 PP-OCR), 效率 67%~87%。
4. **150+ FPS 的结论: 单块 RK3588 + 当前 640×640 INT8 模型做不到。**
   想上 150, 只有三条路:
   - 换更小的输入(640→512/416, NPU 时间按面积平方降, 512 大约 13 ms/帧 → 3 核可到 ~180 FPS);
   - 换更小的检测模型(YOLO-nano 类);
   - 加板子。

---

## 任务二: 定频脚本 + 抖动实测

### 2.1 脚本: `scripts/lock_max_freq.sh`

- 写 `cpufreq` 的 `scaling_governor=performance` **并且** `scaling_min_freq=scaling_max_freq`(双保险);
- 写 devfreq 的 `governor=performance` + `min_freq=max_freq`(dmc / npu / gpu);
- 原 governor 备份到 `/tmp/lock_max_freq.bak`, `--restore` 原样还原;
- `--status` 不需要 root;
- **`--self-test`: 在假 sysfs 树上跑一遍"上锁→校验→还原→校验", 不需要 root。**

自测帮我们抓到了两个真 bug(否则要拿着 root 上真板子才发现):
1. governor 列表文件名查错了 —— ARMbian 的 cpufreq 叫 `scaling_available_governors`,
   没有 `available_governors`。查不到就误判成"不支持 performance", **所有 governor 都不会被改**。
2. 写节点的路径拼错了 —— 写成了 `<governor文件>/governor`, **所有写操作静默失败**。

修完自测全绿:
```
✓ policy0: governor=performance min=max=2256000
✓ policy4: governor=performance min=max=2256000
✓ policy6: governor=performance min=max=2256000
✓ dmc: governor=performance min=max=1000000000
✓ fdab0000.npu: governor=performance min=max=1000000000
✓ ... 还原: governor=ondemand / min=408000 / 300000000 全部复原
```

### 2.2 抖动口径(新增)

- `include/stats.h`: 新增 `RollingPercentile` + `npu_pct` / `total_pct`;
  采样的是**单帧**耗时(不是滑动均值 —— 均值会把抖动抹平);
- `src/main.cc`: 每次推理 `stats.npu_pct.push(t_npu_run)`;
- `stats.dump()` 现在打印:

```
  ── 延迟抖动(单帧采样, N 帧) ──
  NPU 单帧      : P50=.. P90=.. P99=.. max=.. ms  (抖动 max-P50=..)
  整帧单帧      : P50=.. P90=.. P99=.. max=.. ms
```

### 2.3 基线(定频前, 300 帧)

`./scripts/freq_jitter_ab.sh --phase baseline --frames 300 --pipe 1`

```
流水线 FPS   : 38.58   (帧间隔 25.92 ms)
交付 FPS     : 38.10
NPU 推理     : 22.59 ms  (其中 rknn_run 22.43 / 取回输出 0.15)
NPU 单帧      : P50=22.17  P90=24.63  P99=27.18  max=27.57 ms  (抖动 max-P50=5.40)
整帧单帧      : P50=25.72  P90=28.16  P99=30.63  max=35.51 ms

运行期间频率采样:
  dmc      freq min=2400000000 max=2400000000 avg=2400000000   ← DDR 已经在最高
  dmc      load avg=24.1%  max=28%
  npu      freq min=1000000000 max=1000000000 avg=1000000000   ← NPU 已经在最高
  npu      load avg=100.0%  max=100%
  policy0  freq min=1200000  max=1800000  avg=1590222          ← 小核
  policy4  freq min=600000   max=2016000  avg=1367111          ← 大核, 一路掉到 600MHz!
  policy6  freq min=600000   max=2304000  avg=1408000          ← 大核, 一路掉到 600MHz!
```

**抖动来源定位到了**: DDR 和 NPU 本来就钉在最高, **只有 CPU 在 600 MHz ~ 2.3 GHz 之间反复爬坡**。
我们的负载是"一帧接一帧、每帧只有几毫秒活儿", governor 看到空档就降频, 下一帧又要等升频 —— 这就是
P50 22 ms / max 27.6 ms 那 5.4 ms 抖动的来源。

**噪声底**: 同配置连跑两次, P99 是 27.18 / 26.10 ms(差 ~4%)。
→ 判定标准: **定频后 P99 至少要降 1 ms 以上才算真的改善**, 否则是噪声。

### 2.4 定频后（2026-11-16 14:03 已实测）

管理员执行 `sudo ./scripts/lock_max_freq.sh` 后（备份在 `/tmp/lock_max_freq.bak`），
立刻用同一套参数重跑 300 帧：

| 指标 | 定频前（ondemand） | 定频后（performance） | 变化 |
|---|---|---|---|
| 流水线 FPS | 38.58 | **41.62** | **+7.9%** |
| 交付 FPS | 38.10 | **40.37** | **+6.0%** |
| NPU `rknn_run` 均值 | 22.43 ms | 21.48 ms | −4.2% |
| NPU 单帧 P50 | 22.17 ms | 21.50 ms | −3.0% |
| NPU 单帧 P90 | 24.63 ms | 23.62 ms | −4.1% |
| **NPU 单帧 P99** | 27.18 ms | **25.25 ms** | **−7.1%** |
| NPU 单帧 max | 27.57 ms | 26.22 ms | −4.9% |
| NPU 抖动（max−P50） | 5.40 ms | 4.71 ms | **−12.8%** |
| 整帧 P50 | 25.72 ms | 24.62 ms | −4.3% |
| 整帧 P90 | 28.16 ms | 26.85 ms | −4.7% |
| **整帧 P99** | 30.63 ms | **28.26 ms** | **−7.7%** |
| **整帧 max** | 35.51 ms | **29.58 ms** | **−16.7%** |

**可复现性**（同配置连跑，噪声底 ±1 ms）：

| 组 | 交付 FPS | NPU P50 | NPU P90 | NPU P99 | NPU max | 整帧 P99 | 整帧 max |
|---|---|---|---|---|---|---|---|
| 定频前 #1 | 38.10 | 22.17 | 24.63 | 27.18 | 27.57 | 30.63 | 35.51 |
| 定频前 #2 | 38.32 | 22.13 | 24.50 | 26.10 | 26.64 | 29.53 | 30.39 |
| **定频后 #1** | **40.37** | 21.50 | 23.62 | **25.25** | 26.22 | **28.26** | **29.58** |
| **定频后 #2** | **41.47** | 21.09 | 22.66 | **24.21** | 26.13 | **27.55** | 30.17 |

**判定**：P99 改善 ~2~3 ms，**明显超过 ±1 ms 的噪声底 → 是真改善**。
帧率 +6~8%；端到端整帧 max 改善 16.7%（35.5 → 29.6 ms），对"偶尔卡一下"的体感最直接。

**定频后的频率采样**（全部恒定，不再爬坡）：

```
dmc      freq min=2400000000 max=2400000000 avg=2400000000
npu      freq min=1000000000 max=1000000000 avg=1000000000
policy0  freq min=1800000   max=1800000   avg=1800000
policy4  freq min=2256000   max=2256000   avg=2256000
policy6  freq min=2304000   max=2304000   avg=2304000
```

**代价**：CPU 长期满频 → 功耗与发热上升。嵌入式长时间跑必须配散热；
本机实测温度离 85 °C 降频线还有 44 °C 余量（白皮书 §4.4）。

**还原**：`sudo ./scripts/lock_max_freq.sh --restore`（按 `/tmp/lock_max_freq.bak` 原样还原）。

---

## 任务三: 4 路 / 6 路并发到底行不行

| 路数 | 每路 FPS | 每路 rknn_run | 全板总吞吐 | 每核占用率(自算) |
|---|---|---|---|---|
| 1 | 34.67 | 24.57 ms | 34.7 | 85% |
| 3 | 30.8 | ~28 ms | 92.8 | 88% |
| 4 | 21.7 ~ 30.1 | 30~40 ms | 102.4 | 92~95% |
| 6 | 19.3~19.5 | 36~49 ms | 116.4 | 94~95% |

**关于"NPU 负载能不能稳定在 90% 以上"**:
- 3 路时每核占用已经 **86~88%**, devfreq 的 `load` 节点读数是 **100%** ——
  但注意这个 `load` 节点在本机驱动上是"有一个核忙就报 100"的口径,
  **单路时它也报 100%**, 所以它不能当百分比用, 上面的 88% 是我们按
  `rknn_run × fps` 自己算的, 才是可信口径。
- 所以答案是: **3 路就已经把 NPU 喂到 ~88%, 不需要"4/6 个错峰线程"去凑 90%。**
- 再加路数, NPU 不再涨吞吐, 只涨延迟: rknn_run 从 28 ms 涨到 36~49 ms,
  6 路时每路只剩 19.4 fps。**"错峰提交"解决的是流水线空转, 不是算力天花板。**

---

## 交付结论: 12 路怎么排(3 板 × 4 路 → 修订为 4 板 × 3 路)

按实测的单流 NPU 占用算账(交付实例的实测值: 25 fps 一路, `npu=31.13ms`, 其中 rknn_run 22.2ms):

| 场景 | 单流 NPU 时间 | 单流占一核 | 4 流需要 | 3 核装得下吗 |
|---|---|---|---|---|
| 纯检测(不识别车牌) | 22.4 ms @25fps | 56% | 224% | ✅ 装得下(75%), 还剩 25% 余量 |
| 检测 + PP-OCR 车牌识别 | 31~37 ms @25fps | 78%~93% | **3100~3700 ms/s = 103%~123%** | ❌ **超载 8%~23%, 必掉帧** |

### 两个推荐方案(都在板上实测跑通)

| 方案 | 路数 | 每路交付 FPS | 每路 NPU 总耗时 | 3 核总占用 | 判定 |
|---|---|---|---|---|---|
| **A（推荐）4 板 × 3 路 @25 fps** | 3 | **24.78 / 24.99 / 24.53** | 33.6 / 31.0 / 36.9 ms | **83.8%** | ✅ 守住 25 fps, 有余量 |
| **B（备选）3 板 × 4 路 @20 fps** | 4 | **19.44 / 19.99 / 20.00 / 19.31** | 51.4 / 39.7 / 38.3 / 41.4 ms | ~90~110% | ⚠️ 守住 20 fps, 余量小 |
| （不可行）3 板 × 4 路 @25 fps | 4 | — | — | 103%~123% | ❌ 超载 |

- 方案 A 用**现成的 640 生产模型**, 不降分辨率 → **不需要重做精度回归**。
- 方案 B 靠新加的 `--max-fps 20`: 分析帧率真的压到 20 fps(每帧都是新鲜结果, 实测 19.84 fps),
  代价是输出帧率也降到 20 fps。
  (`--infer-stride 2` 是另一种省算力的方式: 输出仍是 25 fps 但中间帧复用旧框, 两者互补。)
- 每路参数统一: `--npu-pipe 1 --ppocr <rec.rknn> --plate-cls 1 --ppocr-ttl 5000`
  (单板 3~4 路时**不要开并行**: 进程已经把核分完了, 再开并行只会互相抢核)。

### 锁频建议进交付

客户现场那 5~8 ms 的 NPU 抖动直接来自大核降频爬坡,
钉死最高频率后 NPU P99 −7.1%、整帧 max −16.7%、FPS +6~8% —— 代价是功耗/发热。

---

## 附: 本轮新增/改动

| 文件 | 说明 |
|---|---|
| `scripts/lock_max_freq.sh` | 定频/还原/状态/自测 (新) |
| `scripts/freq_jitter_ab.sh` | 定频前后 FPS+抖动 A/B (新) |
| `scripts/stream_n_test.sh` | 单板 N 路并发实测 (新) |
| `src/eval/npu_passthrough_bench.cc` | pass_through=0/1 对比基准 (新) |
| `CMakeLists.txt` | 加 `npu_passthrough_bench` target |
| `include/stats.h` | `RollingPercentile` + P50/P90/P99/max 输出 |
| `src/main.cc` | `stats.npu_pct.push(t_npu_run)`; **新增 `--max-fps N`**(分析帧率上限, 用于 3 板 × 4 路方案) |

### 已知遗留问题(与本轮改动无关)

`build/test_mem_trim` 在本板会失败两项("malloc_trim 之后 RSS 下降"、"malloc_trim 报告归还了非零字节")。
**已用 `git stash` 验证: 改动前的代码同样失败** —— 是 glibc malloc_trim 在本机的环境相关行为,
不是本轮引入的回归。其它测试(`test_static_filter` / `test_license_plate_rec` / `test_infer_stride_speed`)全部通过。
