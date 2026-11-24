# context.md —— 工作区上下文快照（RK3588 边缘 AI 大门感知系统）

> **本文件给谁看**：拿到项目概括、但**无法直接访问本地工作区**的规划侧（网页端 AI / 外部评审）。
> 目的是让它在做下一步规划前，拿到**已验证的事实 + 可定位的代码坐标**，而不是靠通用硬件知识猜。
> **本文件里的每个数字都来自板端实测**，没有估算；凡是推断的地方都标了 `〔待基线确认〕`。
> **事实源**：仓库 `RK3588-Ebike-Gate-System` @ tag `release/v2.0-gate-4ch`（commit `05ae802`）。

---

## 1. 一句话现状

单块 RK3588 上跑 **4 路大门**（2 路近景 PP-OCR 车牌识别 + 2 路高位 YOLO 检测测速），
**单进程 + 全局 NPU 线程池**调度，4 路交付合计 **73.6 FPS**，CPU 65.7%，NPU 三核合计 173.7%（满配 300%）。

---

## 2. 仓库地图（规划侧要看的文件在哪）

| 路径 | 内容 |
|---|---|
| `rk3588_zero_copy/src/gate_main.cc` | **单进程 4 路入口**（1200 行）：每路一个通道线程 = 解码→RGA→检测→跟踪/测速→识别→上报 |
| `rk3588_zero_copy/src/rk_base/npu_pool.cc` | **NPU 全局调度核心**（667 行）：优先级队列 + 每核 worker + 每核闸门 |
| `rk3588_zero_copy/include/rk/npu_pool.h` | 池的接口与数据结构（`DetTask` / `OcrTask` / `CoreStat`） |
| `rk3588_zero_copy/src/main.cc` | 单路版（4200 行，**已冻结为稳定基线**）：Web 控制台联动的单机流水线 |
| `rk3588_zero_copy/src/yolo_post.cc` | YOLOv8 后处理（三种导出形态 + cls 空间自动判别） |
| `rk3588_zero_copy/src/license_plate_rec.cc` | PP-OCRv4 车牌识别封装（CTC 解码、字典、白名单） |
| `rk3588_zero_copy/src/rga_convert.cc` | RGA 封装（NV12→RGB888、零拷贝导入、CPU 兜底路径） |
| `rk3588_zero_copy/scripts/run_4ch_gate_single.sh` | 单进程 4 路压测（10 分钟） |
| `rk3588_zero_copy/scripts/run_4ch_gate_ab.sh` | **两种拓扑的公平 A/B**（同板同源同参，自动出对比表） |
| `rk3588_zero_copy/scripts/gate_ab_clean.py` | A/B 的「干净窗口」统计（剔除板上有别的进程在跑的时段） |
| `docs/RK3588边缘AI系统交付白皮书.md` | 交付白皮书（35 页：架构/实测/长跑/手册/排错/风险清单） |
| `docs/RELEASE_NOTES_v2.0-gate-4ch.md` | 本版发布说明 |
| `docs/AGENT_PROJECT_BASELINE.md` | **项目基线**（环境事实/目录权限/7 个实测坑位/交付标准/Agent 行为规范） |
| `logs/gate_4ch_ab/summary.txt` | 4 进程 vs 单进程的 A/B 原始结论 |
| `logs/gate_4ch_single_ab/gate.log` | 单进程 4 路运行日志（每 100 帧一行分通道统计） |

---

## 3. 关键代码坐标（规划侧要的「rk_gate 入口 + NPU 调度核心片段」）

### 3.1 入口

- `rk3588_zero_copy/src/gate_main.cc:1194` —— `int main()`，解析 `--ch "ch1:源:near|high[:IPM四点]"` 逐路声明
- `rk3588_zero_copy/src/gate_main.cc:448` —— `class Channel`（每路的流水线；`run()` 是主循环）
- `rk3588_zero_copy/src/gate_main.cc:745` —— `Channel::run()`（每路的主循环：解码→RGA→提交检测→跟踪/测速→提交识别→上报）
- `rk3588_zero_copy/src/gate_main.cc:524` —— `Channel::open_source()`：**所有源在主线程里打开**
  （`SafeProbe` 用的是进程级 `sigaction` + 静态 `sigjmp_buf`，**不能并发调用**）

### 3.2 NPU 调度核心（`src/rk_base/npu_pool.cc`）

| 环节 | 位置 | 要点 |
|---|---|---|
| `rknn_init` 封装 | `:269` | **在 worker 自己的线程里**建上下文 —— RKNN 上下文跨线程用会死锁 |
| 核绑定 | `:277` | `rknn_set_core_mask(ctx, w.core_mask)`，掩码 1/2/4 = Core0/1/2 |
| 零拷贝输入 | `:297` | `rknn_create_mem_from_fd(ctx, s.fd, s.ptr, s.buf_size, 0)`，fd 就是 RGA 的目标缓冲 |
| `rknn_run` | `:359` | 紧跟 `rknn_outputs_get`（`:360`），**输出缓冲属于本线程的上下文** |
| 耗时口径 | `:393` | `dt_npu` = rknn_run + outputs_get（真正占核的时间）；`dt_all` = 再加 CPU 后处理 |
| 上下文销毁 | `:438` | **同样在本线程**销毁，跨线程销毁 RKNN 上下文会死锁 |
| 优先级赋值 | `:563` / `:583` | 检测 `priority=0`，**识别 `priority=1`** |
| 入队 | `:574` / `:592` | 一条全局 `multimap<priority, Task, greater>`，识别必然排在检测前 |
| 取任务 | `:229` / `:245` | `pop_task`（按 slot 绑 worker）/ `pop_ocr_task`（谁都能跑） |
| 每核闸门 | `:48` / `:349` / `:467` | `CoreGate::acquire(priority)`：同核多 worker 时，**识别永远排在检测前面** |
| worker 线程 | `:195` / `:198` | 每个 worker 一个 `std::thread` |

### 3.3 RGA 预处理调用点

- `rk3588_zero_copy/src/gate_main.cc:863` —— `rga_nv12_to_rgb888(src, mdst, false, &scale, &pad_x, &pad_y)`，
  目标 `mdst.vir_addr = pool_->slot_ptr(slot)`（**就是 NPU 的输入 dma-buf**，零 CPU 拷贝）
- `rk3588_zero_copy/src/gate_main.cc:92` —— `std::mutex g_rga_m`：
  librga 在**一个进程里是串行的**（每次调用都要 `importbuffer_fd` 注册目标缓冲，进程内共享一张全局表 + 锁）。
  4 个通道线程同时转就会在这把锁上排队 —— **实测单次 RGA 从 6.7 ms（4K）涨到 24.6 ms**。
  现在用互斥量把「抢锁 + 排队」的抖动变成干净排队。

---

## 4. 瓶颈判定：**73.6 FPS 卡在哪儿**（这是规划侧最需要的那条结论）

### 4.1 证据（`logs/gate_4ch_single_ab/gate.log`，4 路稳态段平均）

| 路 | 交付 FPS | 源帧率 | `rga` ms | `npu` ms（纯 NPU） | `detwait` ms（含排队） | `ocrwait` ms | 判读 |
|---|---|---|---|---|---|---|---|
| ch1 近景/车牌 | 15.7 | 30.0 | 5.42 | 26.94 | **45.07** | **80.92** | 检测排队 + 识别候选串行 |
| ch2 高位/测速 | 19.7 | 21.5 | 5.99 | 27.12 | **41.92** | 0.00 | **源限速**（已跑满 21.5） |
| ch3 近景/车牌 | 18.6 | 60.0 | 7.54 | 27.09 | **42.17** | **71.65** | 检测排队 + 识别候选串行 |
| ch4 高位/测速(4K) | 19.6 | 60.0 | 14.98 | 25.77 | **34.50** | 0.00 | 检测排队 |

> 口径：`npu` = worker 线程里测的 `rknn_run + rknn_outputs_get`；
> `detwait` = 通道线程从提交任务到拿到结果的**墙钟**。**两者之差就是 NPU 排队时间。**

### 4.2 结论：**不是 DDR 带宽，不是 CPU 后处理，是「NPU 算力 + 调度」两处**

| 候选瓶颈 | 判定 | 依据 |
|---|---|---|
| **DDR 带宽** | ❌ **不是** | 全链路 **0 次 CPU memcpy**（RGA 目标 = NPU 输入 = 同一块 dma-buf，DMA 直通）；RGA 单次仅 5~15 ms |
| **CPU 侧后处理** | ❌ **不是** | CPU 总占用 65.7%（8 核才用 65.7%）；DFL+NMS 每帧 < 1 ms；后处理只占核内时间的 0.5% |
| **NPU 算力（检测）** | ✅ **是主瓶颈** | 检测两核各 **77.2% / 77.3%**；纯推理 27 ms/帧，但 `detwait` 42~45 ms —— **15~18 ms 是排队**（占 30~40%） |
| **调度/串行化（识别）** | ✅ **是第二瓶颈，且不是算力问题** | 识别核 Core0 **只用了 19.1%**，但近景两路 `ocrwait` 高达 **72~81 ms/帧** |

### 4.3 识别那条的机理（已定位，可直接优化）

一块**两行车牌**会拆成 **4 个候选**（上行试 3 个切分点 + 下行 1 个，`kRowSplitTrials=3`），
4 个候选**一次性全部提交**给识别 worker，但**识别 worker 只有 1 个**（Core0），
于是这 4 个候选在 Core0 上**串行**跑完：4 × ~24 ms ≈ **96 ms**。
**Core0 明明只有 19% 占用，却把近景两路各拖了 72~81 ms** —— 这是**串行化**造成的等待，不是算力不足。

> 由此得到优先级排序（**规划侧据此排期**）：
> **P0 = 检测算力**（两个检测核 77%，已接近打满；出路是降模型尺寸 / 降输入分辨率 / 加核 / 抽帧）
> **P1 = 识别候选并行化**（Core0 空闲 81%，把同一块牌的 4 个候选分发到多个 OCR worker，等待可望减半到 1/4；
> 或对上行候选做**早停** —— 只认一个就不必试满 3 个切分点）
> **P2 = 都不是瓶颈，不要在这两处花时间**（DDR 带宽、CPU 后处理、编解码）

---

## 5. 已验证的能力基线（规划时当作约束，不要推翻）

| 能力 | 实测 | 出处 |
|---|---|---|
| 单板 4 路稳定 | 9.44 h / 827,280 帧 / 0 崩溃 | 白皮书第 4 章 |
| 内存绝对平线 | 48,000 帧活堆净增 **+31 KB**（斜率降 1000×） | 白皮书 §3.6 |
| 4 进程 → 1 进程 | 同样 NPU 算力（173%），吞吐 **+24%**，内存 **−23%** | `logs/gate_4ch_ab/summary.txt` |
| 关硬解的代价 | 只关 MPP → CPU **98.1%（4.1×）**；只关 RGA → **103.3%（4.3×）** | `docs/assets/fair_ab_production.png` |
| 车牌识别准确率 | 整牌 86.64% / 单字符 97.06%（217 张实拍） | 白皮书 §3.5 |
| 像素保真 ROI | 从 1080P 原帧裁，有效像素 5.1×、清晰度 7.1× | `docs/assets/roi_fidelity_1080p.png` |

**已修的坑位**（细节见 `docs/AGENT_PROJECT_BASELINE.md` 第四节）：并行检测路径 RGA 目标缓冲越界写、
识别路径 46 KB/次冗余 memcpy、`--npu-core` 在 pipe 模式静默失效、cls 空间（logit/概率）误判、
控制台开关假联动。

**回归证据**：4 组数据集前后逐框对比 **4/4 完全一致**（含分数与坐标）；ROI **44 张逐字节一致**；
识别日志 **68 条逐条一致**。

---

## 6. 已知未闭环项（规划时应纳入考虑）

1. ~~**单进程版车牌 ROI 纵向比单流版高 6 px**，两行牌的省份字会丢（`菱ALA981908` → `AA981908`）。~~
   **根因**：单进程版用**帧采集时刻**喂跟踪器（`dr.ts_ms`），单流版用**处理时刻**，
   运动预测的 dt 不同 → 预测框差几像素。**不是 bug，是精度差异，上线前需对齐时基口径。**

   > 📌 **2026-11-24 口径更正（Wave 1 封版）—— 本条已关闭，不立项改 C++**：
   >
   > - **上面那句"根因是时基口径、需对齐时基"已被证伪**。2026-11-24 的只读复核发现：
   >   ① `SpeedTracker::predict()`（`src/speed_track.cc:195`）在 `src/gate_main.cc` 里
   >      **没有调用点** —— 唯一调用点在演示单流 `src/main.cc:3406`。生产链路**根本没执行运动预测**，
   >      所以"dt 不同 → 预测框差几像素 → ROI 差 6px"这条因果链的中间一步不存在；
   >   ② ROI 取的是**原始检测框**（`src/speed_track.cc:389` `tr.box = dets[i]`，无平滑/EMA），
   >      不是预测框（`src/gate_main.cc:1214` → `include/rk/plate_roi.h:108`
   >      `top = b.y1 + bh*roi_y + bh*below`）。
   > - **真正的性质**：12.26% 的残余是"**整车框推车牌**"在**车姿变化**下的**固有几何边界** ——
   >   13/106 帧的 GT 牌宽 192~380 px（中等距离），框→牌的相对位置随车姿/距离漂移，
   >   单一 `roi_y` 覆盖不了。**号码行识别率不受影响。**
   > - **决策（规划侧）**：**不立项改 C++ 代码**，**保持现网 F8 参数**
   >   （`roi_ratio/roi_y/roi_height/roi_shift/roi_below` = `0.30/0.38/0.22/0.126/0`，
   >   即 `rk3588_zero_copy/config/gate.env:96-100`），**仅在文档中如实记录为模型几何边界**。
   > - 归口：`rk3588_zero_copy/docs/RISK_REGISTER.md` **R25（已关闭）**；
   >   查证过程见 `docs/o11_web_console_audit.md` §四。
2. **12 路 @25 fps 需要 4 块板 × 3 路**，单板 4 路 @25 fps 不可行（NPU 超载 8%，必然掉帧）。
3. **IPM 标定值与机位强绑定**，换机位必须重新标定，否则测速距离全错。
4. `ebike_int8.rknn`（toolkit 2.5.0 编译）与板端 runtime 2.3.2 **不兼容会加载失败**，
   生产必须用 `ebike_int8_rk2332.rknn`。

---

## 7. 复现命令（可直接执行）

```bash
cd /home/seeed/rk_deploy/rk3588_zero_copy

# 编译（板端原生）
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j8

# 单进程 4 路压测 10 分钟
./scripts/run_4ch_gate_single.sh 600 20

# 两种拓扑的公平 A/B（同板同源同参，自动出对比表）
./scripts/run_4ch_gate_ab.sh 600 20

# 单路版（含 Web 控制台联动）
./build/rk_zero_copy <源> model/ebike_int8_rk2332.rknn --labels model/labels_ebike.txt \
    --decoder mpp --ppocr model/ppocrv4_rec_fp16.rknn --plate-cls 1 --track-cls 1 \
    --sn rk3588-01 --mqtt-host <broker> --mqtt-port 1883

# Web 控制台
sudo ./scripts/start_console_stack.sh        # → http://<板子IP>:8080/
```

---

## 8. 给规划侧的三条硬约束

1. **禁止改动 `main.cc` 的单路核心逻辑**（已冻结的稳定基线）；新功能一律新建文件或加宏开关。
2. **性能结论必须来自板端实测**（`debugfs` / `top` 采样），**禁止写「预计 / 大概 / 估算」**；
   对比必须单变量（同跑 600 帧、同开 PP-OCR、只关一个变量）。
3. **模型目录 `models/` 只读**（唯一真源）；`archive/rk12ch/` 与 `third_party/` 禁止触碰。
