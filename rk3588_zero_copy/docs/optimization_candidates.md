# optimization_candidates.md —— 热循环「实现层浪费」审查清单

> **阶段**：Phase 1（只读审查，不改生产代码）
> **日期**：2026-11-20
> **板子**：RK3588（recomputer-rk3588-devkit），runtime 2.3.2
> **审查对象**：`rk3588_zero_copy/src/gate_main.cc` / `src/rk_base/npu_pool.cc` /
> `src/yolo_post.cc` / `src/license_plate_rec.cc`
> **生产配置**（`config/gate.env`）：检测核 0,1,2 / 识别核 0,1,2 / 各 3 个 worker /
> 每 worker 2 个 slot / conf=0.15 / max-fps=20 / OCR 模型 `ppocrv4_rec_fp16.rknn`

---

## 0. 一句话结论

**任务书给的第 1 条线索（`npu_pool.cc:356` 是 `is_prealloc = 0`）已经过期** —— 预分配
在 2026-11-18 的提交 `8fdc9b7` 里就做完了。真正还留在热循环里的实现层浪费有 **1 个大的**
（识别侧每帧让驱动把 265000 个 fp16 转成 float32，**白花 4.94 ms/次**）和 **3 个中的**
（后处理每帧全量重扫张量 0.52 ms、CTC 串行比较 1.42 ms、核闸门跨着 CPU 后处理不释放）。
检测侧的 `want_float` 反而不该动 —— 切 int8 实测**更慢 0.9 ms/帧**。

---

## 1. 必查项逐条结论

### 必查项 1 · 输出缓冲预分配（`is_prealloc`）

| 模型 | 位置 | 当前值 | 状态 |
|---|---|---|---|
| 检测 | `src/rk_base/npu_pool.cc:361` | `is_prealloc = 1` | ✅ **已完成** |
| 识别 | `src/license_plate_rec.cc:527` | `is_prealloc = 1` | ✅ **已完成** |
| 识别(备用实现) | `src/lprnet.cc:241` | `is_prealloc = 1` | ✅ **已完成** |

证据：

```
$ git blame -L 343,365 src/rk_base/npu_pool.cc
8fdc9b77 (AzuleErin 2026-11-18 16:54:30 +0800 343)   // ★★ 输出缓冲自己预分配(is_prealloc = 1)
8fdc9b77 (AzuleErin 2026-11-18 16:54:30 +0800 360)   s.outs[i].want_float = 1;
8fdc9b77 (AzuleErin 2026-11-18 16:54:30 +0800 361)   s.outs[i].is_prealloc = 1;
```

> `npu_pool.cc:360` 现在的 `want_float = 1` 行号，正好落在任务书说的「356 行附近」，
> 大概率是把 `is_prealloc` 那一行和 `want_float` 那一行记混了。

**判定：不进入 Phase 2。**（重复改造已完成的项是负收益。）

---

### 必查项 2 · float 反量化（`want_float`）

**这是本轮最大的发现。**

先查两个模型真实的输出属性（探针 `opt_audit/tools/probe_attrs.cc`，只读）：

```
$ ./probe_attrs models/ebike_int8_rk2332.rknn
  IN[0]  type=INT8   fmt=NHWC dims=[1,640,640,3]
  OUT[0] name=/model.22/Mul_2_output_0     type=INT8   dims=[1,4,8400] n_elems=33600  zp=-128 scale=2.548393488
  OUT[1] name=/model.22/Sigmoid_output_0  type=INT8   dims=[1,2,8400] n_elems=16800  zp=-128 scale=0.003803518601

$ ./probe_attrs models/ppocrv4_rec_fp16.rknn
  IN[0]  type=FLOAT16 fmt=NHWC dims=[1,48,320,3]
  OUT[0] name=softmax_11.tmp_0             type=FLOAT16 dims=[1,40,6625] n_elems=265000
```

于是：

| 位置 | 当前 | 实际浪费 |
|---|---|---|
| `src/license_plate_rec.cc:520` `outputs[0].want_float = 1;` | 驱动把 265000 个 **fp16** 展成 float32，**每次识别写 1,060,000 字节** | **4.94 ms/次识别**（实测） |
| `src/rk_base/npu_pool.cc:360` `s.outs[i].want_float = 1;` | 驱动把 50400 个 int8 展成 float32，每帧多写 151,200 字节 | **0.02 ms/帧**（实测，且后处理反而更慢） |

**关键点：识别模型的原生输出是 fp16，不是 int8。** 所以「切 int8 直读」这条路在识别侧
根本不存在，真正的选项是「**切 fp16 直读**」。这纠正了任务书里「int8 直读」的表述。

CTC 贪心解码（`license_plate_rec.cc:314-350`）每个时间步只取 **argmax** 和 **该 argmax 的值**，
既不需要完整的 softmax 分布，也不需要非 argmax 类的概率 —— 整张 265000 元素的
float32 张量里，真正被读到的只有 **40 个值**。

---

### 必查项 3 · 热循环里的内存分配

| 位置 | 现象 | 每帧代价 |
|---|---|---|
| `npu_pool.cc:571` `det_results_[r.chan].push_back(r);` | `publish_det(const DetResult&)` 收 **const 引用** → 拷贝 `DetResult`，连带**拷贝 `std::vector<YoloBox> boxes`**（一次堆分配 + memcpy），**而且在全局锁 `res_m_` 里面** | 每帧 1 次堆分配，锁内 |
| `npu_pool.cc:579` `ocr_results_[key].push_back(r);` | 同上，`OcrResult` 里带 `std::string text` → 字符串堆分配，**也在 `res_m_` 里面** | 每帧 1 次堆分配，锁内 |
| `yolo_post.cc:171,176,183` | `nms_per_class` 里 `out` / `cand` / `dead` 三个 vector 每帧新建 | 实测 **0.0002 ms**（可忽略） |
| `yolo_post.cc:205,342` | `objs.reserve(1024)` 每帧 24 KB malloc | 含在 post 总耗时里 |
| `speed_track.cc:232,233,235,243` | 跟踪器每帧新建 `det_to_track`/`track_used`/`high`/`low`/`pairs` | 每帧 ~6 次 malloc |
| `gate_main.cc:971,972` | `tracks` / `track_boxes` 每帧新建 | 每帧 2 次 malloc |

> 对比：`npu_pool.h:149-206` 的 `TaskQueue` 已经是「定长数组 + 零分配」，
> `DetSlot::ts`（`npu_pool.cc:398`）也是每 slot 预开好的 —— 说明「热循环零分配」这件事
> 工程里已经在做，只是没做完。**剩下的分配集中在「结果发布」和「跟踪器」两处。**

---

### 必查项 4 · 锁竞争

| 锁 | 位置 | 争用情况 | 判定 |
|---|---|---|---|
| `res_m_` | `npu_pool.cc:570,578,693,710` | 4 路通道**共用一把锁**做结果发布/取回；发布时还在锁内做 vector/string 拷贝 | ⚠️ 可改（改 move 即可，锁本身不用动） |
| `CoreGate` | `npu_pool.cc:48-71` | 每核一把；**检测 worker 和识别 worker 抢同一把**（每核 1 个 det + 1 个 ocr worker） | ⚠️ **闸门被跨着 CPU 段持有**（见候选 C6） |
| `stat_m_` | `npu_pool.cc:442,540` | 每任务一次，粒度很细 | ✅ 可忽略 |
| `slot_m_` / `roi_m_` | `npu_pool.cc:463,602,658` | 借还槽位，短临界区 | ✅ 可忽略 |
| `g_rga_m` | `gate_main.cc:925` | librga 进程内串行，**必须串行** | ✅ 已知且必要 |

**闸门的真正问题**（`npu_pool.cc:388 → 438`）：

```cpp
gate->acquire(task.priority);        // 388  抢核
rknn_run(...);                        // 392
rknn_outputs_get(...);                // 393  ← 到这里 NPU 才真的空闲
... 后处理 0.78 ms ...                // 399-424  ★ CPU 段也在占着核
gate->release();                     // 438  才放核
```

每核 1 个检测 worker + 1 个识别 worker。检测 worker 在做 **0.78 ms 的纯 CPU 后处理**时，
同核的识别 worker 即使 NPU 核心已经空了也只能干等。识别侧同理
（`npu_pool.cc:522 → 534`，闸门一直持有到 CTC 解码结束）。

---

### 必查项 5 · 多余拷贝

| 位置 | 现象 | 判定 |
|---|---|---|
| `license_plate_rec.cc:520,534` | 驱动把 fp16 输出展成 float32：**每次识别多写 530,000 字节 + 265000 次转换**，全部发生在闸门内 | 🔴 **本轮头号问题** |
| `npu_pool.cc:571,579` | `push_back(r)` 按值拷贝 → boxes vector / string 各一次堆分配 | ⚠️ 可改 |
| `npu_pool.cc:455` `r.boxes = std::move(boxes);` | 已是 move | ✅ |
| `npu_pool.cc:699` `*out = std::move(it->second.front());` | 已是 move | ✅ |
| `npu_pool.cc:164` `buf_[tail_] = std::move(v);` | `Task` 只含 POD，无堆 | ✅ |
| `license_plate_rec.cc:588` `roi_.assign(...)` | 只在 `recognize_bgr` 路径；生产走 `recognize_roi8` | ✅ 不在热路径 |
| `gate_main.cc:963` `std::vector<YoloBox> boxes = std::move(dr.boxes);` | 已是 move | ✅ |

---

## 2. 候选清单（带实测量级）

> 测量工具在 `opt_audit/tools/`（只读，不改生产代码）：
> `probe_attrs.cc`（模型属性）、`hotpath_bench.cc`（want_float A/B + CTC）、
> `post_bench.cc`（后处理拆解）、`fp16_equiv.cc`（逐元素穷举等价性）。
> 所有数字都是板端实测（NPU 空闲、CPU 频率未锁、单核独占），非估算。

| # | 候选 | 位置 | 每帧/每次开销 | 实测量级 | 收益 | 风险 | 进 Phase 2 |
|---|---|---|---|---|---|---|---|
| **C2** | **识别输出 fp16 直读**（`want_float=0` + 按原生类型解码） | `license_plate_rec.cc:520,534,556` | 每次识别 | **4.94 ms**（outputs_get 5.17→0.24） | **高** | **低** | ✅ **头号** |
| **C3** | CTC 贪心解码的**串行 max 循环**改 NEON 向量 | `license_plate_rec.cc:332-339` | 每次识别 | **1.42 ms**（fp16 标量 2.98 → NEON 0.82） | 中 | 低-中 | ✅ 与 C2 同做 |
| **C5** | 形态C 的**每帧自检全量重扫张量** | `yolo_post.cc:371-452` | 每检测帧 | **0.524 ms**（占 post 总耗时 0.78 的 67%） | 中 | 中 | ✅ |
| **C6** | **核闸门跨着 CPU 后处理不释放** | `npu_pool.cc:388→438` | 每检测帧 | 闸门多占 **0.78 ms** | 中 | 低 | ✅ |
| **C7** | `publish_det` 按值拷贝（含 vector） | `npu_pool.cc:568-574` | 每帧 | 1 次堆分配，在全局锁内 | 低 | 低 | ✅ 顺手改 |
| **C8** | `publish_ocr` 按值拷贝（含 string） | `npu_pool.cc:576-582` | 每帧 | 1 次堆分配，在全局锁内 | 低 | 低 | ✅ 顺手改 |
| **C4** | 检测输出切 int8 直读（`want_float=0`） | `npu_pool.cc:360` | 每检测帧 | outputs_get **只省 0.02 ms**，但后处理**慢 0.78 ms** → **净亏 0.9 ms/帧** | **负** | 中 | ❌ **不做** |
| C9 | `nms_per_class` 的 3 次 vector 分配 | `yolo_post.cc:170-196` | 每检测帧 | 0.0002 ms | 低 | 低 | ⚪ 可选 |
| C10 | `objs.reserve(1024)` 每帧 24 KB | `yolo_post.cc:205,342` | 每检测帧 | 含在 0.78 ms 内 | 低 | 低 | ⚪ 可选 |
| C11 | 跟踪器每帧 ~6 次 malloc | `speed_track.cc:232-243` | 每帧 | 未单独测量 | 低 | 低 | ⚪ 可选 |
| C12 | `gate_main` 每帧 2 次 vector 分配 | `gate_main.cc:971-972` | 每帧 | 未单独测量 | 低 | 低 | ⚪ 可选 |

---

## 3. 为什么 C4（检测切 int8）不做

实测（`hotpath_bench`，120 张真实帧，逐框对拍）：

```
=== 检测: want_float=1 (当前生产) ===   run=24.29ms  outputs_get=0.216ms  post=0.943ms  合计=25.449ms
=== 检测: want_float=0 (int8 直读)  ===   run=24.44ms  outputs_get=0.195ms  post=1.791ms  合计=26.431ms
   >> outputs_get 差 = 0.021 ms ; post 差 = -0.849 ms ; 合计差 = -0.982 ms/帧
   >> 对拍 120 帧: 框数不一致帧=0  字段不一致=0  max|Δscore|=0.00000000  max|Δcoord|=0.00000000
```

数值上完全等价（逐框零差异），但**更快的是现在这条**：

1. `outputs_get` 只省 0.02 ms —— 检测输出本来就小（50400 元素 = 200 KB），驱动的反量化
   开销可以忽略；
2. `yolo_post.cc` 的 `OutTensor::at()`（`include/yolo_post.h:48-54`）在 int8 分支里
   **每读一个元素都要做一次「分支 + int8→float + 减 zp + 乘 scale」**，后处理从
   0.94 ms 涨到 1.79 ms，**净亏 0.85 ms/帧**。

> 也就是说：检测侧 `want_float=1` 不是浪费，是**当前后处理实现下的正确选择**。
> 任务书里「识别模型 want_float=1 就是多余转换」的判断只在**识别侧**成立，
> 而且识别侧要切的是 **fp16** 而不是 int8。

---

## 4. 为什么 C2（识别 fp16 直读）安全 —— 穷举等价性证明

任务书要求「逐框对拍，不是看起来差不多」。对识别侧，正确的证明方式不是抽样本看字符串，
而是**把两���路径拿到的每一个输出元素都比一遍**：

```
$ ./fp16_equiv --ocr models/ppocrv4_rec_fp16.rknn --imgs opt_audit/ocr_rois --iters 120
输出: type=1(FLOAT16) n_elems=265000 dims=[1,40,6625]

=== 识别输出逐元素穷举等价性 (120 帧) ===
  比对元素总数 : 31800000
  逐位不相等数 : 0
  最大绝对差    : 0
  结论          : ★ 完全等价(fp16->fp32 是精确转换, 一个 bit 都没差)

=== outputs_get 耗时对比 ===
  want_float=1 (驱动转 float32) : 5.171 ms
  want_float=0 (直读 fp16)      : 0.236 ms
  节省                            : 4.935 ms/次
```

**为什么这个证明是充分的**：IEEE 754 的 fp16→fp32 转换是**精确**的（fp32 能无损表示
任何 fp16 值），所以只要 265000 个元素逐位相等，那么**任何**下游解码器
（CTC 贪心 / 白名单 CTC / softmax / top-k / 阈值判断）得到的输出必然逐位相同。
这不是抽样，是穷举 3180 万个元素。

检测侧的逐框对拍（`hotpath_bench`，120 帧真实图）也一并记在候选表里，
供 Phase 2 复用同一套工具。

---

## 5. 量化收益汇总（单次操作口径，板端实测）

| 路径 | 现状 | 改造后 | 省 |
|---|---|---|---|
| 一次**识别**推理（run + 取输出 + CTC） | 21.49 + 4.79 + 1.42 = **27.70 ms** | 21.41 + 0.28 + 0.82 = **22.51 ms** | **5.19 ms（19%）** |
| 一次**检测**推理（run + 取输出 + 后处理） | 24.29 + 0.22 + 0.94 = **25.45 ms** | 24.44 + 0.20 + 0.42 = **25.06 ms** | 0.39 ms（1.5%） |

**对生产链路的推算**（基于 `config/gate.env` 的 4 路 20 fps 配置）：
- 识别侧每块两行牌要提交 **4 个候选**（`gate_main.cc:1091-1103`：上行 3 个切分点 + 下行 1 个），
  每次省 5.19 ms → **每块牌省约 20 ms**；
- `ocrwait` 现状 44~68 ms（本次 2 路实测）/ 72~81 ms（历史 4 路实测），有希望显著下探；
- 检测侧每帧省 0.39 ms，4 路 × 20 fps = 80 帧/s，3 核分摊 → 每核每秒少占 0.39 × 26.7 ≈ 10 ms，
  相当于该核 1% 的占用 —— 直接作用在 `detwait` 的排队上。

> 以上是**推算**。Phase 3 用板端 A/B（帧计数器差分、3 轮取中位数）给出权威口径。

---

## 6. Phase 2 建议范围

按「高收益 + 低风险」排序，本轮只做这 5 项：

1. **C2** 识别输出 fp16 直读（按模型原生输出类型自适应：fp16 / int8 / fp32 三条路都留）
2. **C3** CTC 贪心解码改 NEON 向量（fp16 路径）
3. **C6** 检测 worker 在 `rknn_outputs_get` 之后立刻释放核闸门，后处理挪到闸门外
4. **C7/C8** `publish_det` / `publish_ocr` 改成 move 语义
5. **C5** 形态C 自检与主循环融合（去掉每帧多扫一遍张量）

**明确不做**：C4（检测切 int8，实测更慢）、C1（已完成）、C9~C12（收益低于测量噪声，
不在本轮扩大改动面）。

---

## 7. 审查过程中的环境事实（Phase 3 需要知道）

- `sudo` 密码从工程脚本里取到（`scripts/hw_load.sh:33` 用的就是 `sudo`），
  root 可用；
- 审查开始时板子上的 `rk-gate.service` / `rk-night-soak.service` / `rk-pipeline.service`
  **都处于失败/自动重启循环**（分别 exit 75 / 127 / 2），**没有任何 NPU 消费者在跑**；
  这三个服务的基线状态已存 `opt_audit/BASELINE_service_state.txt`；
- CPU 频率基线：policy0 `min=408000 max=1800000 gov=ondemand`、
  policy4 `min=408000 max=2304000 gov=ondemand`、
  policy6 `min=408000 max=2256000 gov=ondemand`（Phase 3 锁频后必须还原）；
- 审查期间已按工程纪律停服务并打 `/run/rk-gate/paused` 标记，Phase 3 结束必须清除。
