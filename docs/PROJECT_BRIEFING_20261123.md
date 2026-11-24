# RK3588 边缘 AI 大门感知系统 · 项目介绍书

> **文档性质**: 事实汇总，供规划/评审侧做分析与规划输入。
> **成文日期**: 2026-11-23　**复核日期**: 2026-11-23 12:40(增量见 §1.4 与 §11.3.1)
> **覆盖范围**: 仓库 `/home/seeed/rk_deploy`(分支 `fix/p0-correctness-stability`,未 merge),以及本次会话新增的 `opt_ebike_fork9/` 沙盒。
> **口径声明**:
> - 本文中每一个数字都标注了**测量条件**(分辨率 / 路数 / 帧率设置 / NPU 频率 / 时长 / 帧数)。
> - 凡属**文档中的结论性陈述**,一律标注「〈某某文档〉陈述」,与「实测事实」区分。
> - 本文**不下结论、不做选型建议、不预设优化方向**。文末列出的开放项均以「未闭环项 / 缺什么 / 待决」的形式陈述,供规划侧自行判断。
> - **2026-11-24(Wave 1)更新**: 原先并排列出、尚未裁决的 9 处文档口径不一致, **已全部清零** ——
>   §12 保留了原始并列记录(不删历史数据), 并在第 5 列给出本轮结论;
>   **§12.1 是"引用时直接抄这一节"的统一口径速查表**(含实测出处与测量条件)。
> - **所有"没测/没答"的地方照录"没答"**,不由本文代答(见 §11.2)。

---

## 怎么读这份文档

- **想知道"现在到底是什么样"** → §1(现状与实时数据;§1.4 是成文后的复核快照)、§3(代码结构)、§4(生产服务)、§5(模型资产)、§6(流水线实测)。
- **想知道"算力/性能的天花板在哪"** → §7(分层实测:纯 NPU / +RGA / 4 路全链路,以及每层的逐算子归因)。
- **想知道"已经做过什么、结果如何"** → §8(六个阶段/沙盒的完整实测数据)。
- **想知道"精度体系怎么跑、数字是多少"** → §9。
- **想知道"有什么坑、有什么风险"** → §10(全量风险登记表 + 各阶段新发现)。
- **想知道"还缺什么、哪里是空的"** → **§11**(这是给规划侧的主要输入)。
- **想知道"哪些事不能做"** → §13(硬约束与红线)。
- **想核对任何一个数字** → §14.2 给了文件坐标,§15.2 给了测量口径定义。

**本文刻意没有做的三件事**:
1. **没有给"应该往哪个方向优化"的答案** —— §7.5 的"剩余优化空间表"是 `layer_bench_report.md` 的原文表格(它自己列了 P0~P6),不是本文的判断;§11 列的是**缺口**,不是**优先级**。
2. ~~**没有把不同文档里的结论合成一个"统一结论"**~~ —— **2026-11-24(Wave 1)已改变**: 原先并列列出的 9 处口径不一致已全部清零,§12.1 给出统一口径速查表(含实测出处)。§12 的原始并列记录**保留不动**, 用来对照"当时是怎么打架的"。
3. **没有补全文档里没有的数字** —— 例如"NPU 单核 133 FPS 是不是 DDR 带宽限制"(`thread_bench` §7.7 明确写"本测试台答不了")、"多块板能不能线性扩展"(`layer_bench_report.md` §7.4 明确写"没有测多块板"),本文照录这两个"没答",不代答。

---

## 目录

| 章 | 内容 |
|---|---|
| §1 | 项目是什么 · 交付形态 · 当前状态 |
| §2 | 硬件与运行环境 |
| §3 | 代码仓库结构 |
| §4 | 生产服务(部署层) |
| §5 | 模型资产与转换配方 |
| §6 | 生产流水线的实际构成 |
| §7 | 算力基线(分层实测) |
| §8 | 已完成的工作(按阶段) |
| §9 | 精度回归体系 |
| §10 | 已记录的缺陷与风险 |
| §11 | 未闭环项 / 缺口清单 |
| §12 | 文档间口径不一致清单(2026-11-24 已清零, 附 §12.1 统一口径速查) |
| §13 | 硬约束与红线 |
| §14 | 术语与文件坐标速查 |
| §15 | 附录:数据资产与测量口径定义 |

---

# §1 项目是什么 · 交付形态 · 当前状态

## 1.1 业务定位

基于 Rockchip **RK3588** 的边缘 AI **非机动车(电动车)检测、测速与车牌识别**终端,面向**小区/城中村大门 / 路口非机动车道闸**场景。

`docs/AGENT_PROJECT_BASELINE.md` 原文:
> 基于 RK3588 边缘计算平台,面向小区/城中村大门场景,实现 **4 路并发(2 路近景 PP-OCR 识别 + 2 路高位 YOLO 检测测速)**的实时车牌识别与测速系统,当前处于**交付收尾与开源发版**阶段。

`rk3588_zero_copy/README.md` 原文:
> 整条链路走 **MPP 硬解 → RGA 零拷贝预处理 → NPU 推理 → CPU 后处理 → MQTT 上报**,全链一帧图像只存在**一份物理内存,零 CPU 拷贝**。

## 1.2 交付形态(仓库 README 自述,均为板端实测)

| 项 | 原文陈述 |
|---|---|
| 交付件 | 《RK3588 边缘 AI 系统交付白皮书》(PDF, 35 页:架构/实测/长跑/手册/排错/风险清单) |
| 代码仓 | `github.com/azuleerin/RK3588-Ebike-Gate-System`,Release `v2.0-gate-4ch` |
| 一键验收 | `./run_acceptance_demo.sh` — 连续 3 次 9/9 全部通过 |
| 长跑验证 | 9.44 小时 / 827,280 帧 / 0 次崩溃 / 内存零增长 |
| Web 控制台 | 3.0,MPP/RGA/NPU 三个开关**真联动板端** |

## 1.3 当前(2026-11-23 板端实测)运行状态

```
$ systemctl is-active rk-gate rk-gate-watchdog
active
active
```

生产服务正在跑,命令行(实测 `ps auxww`):

```
rk_gate --model .../models/ebike_int8_rk2332.rknn
        --labels .../models/labels_ebike.txt
        --ppocr .../models/ppocrv4_rec_fp16.rknn
        --ppocr-dict .../models/ppocr_keys_v1.txt
        --det-core 0,1,2 --ocr-core 0,1,2
        --det-workers 3 --ocr-workers 3 --det-slots 2
        --conf 0.15 --nms 0.45
        --max-fps 20 --stats-interval 100
        --roi-ratio 0.164 --roi-y 1.0 --roi-height 0.40 --roi-shift 0.126 --roi-below 0
        --ipm-width-m 3.5
        --sn RK3588-GATE-01 --device-name 0001 --ipaddr 192.168.31.77 --duration 0
        --ch ch1:/home/seeed/testvideo3.mp4:near
        --ch ch2:/home/seeed/testvideo2.mp4:high:200,415;1000,429;967,585;367,546
        --ch ch3:/home/seeed/Test2_fixed.mp4:near
        --ch ch4:/home/seeed/电动车素材.mp4:high:600,1244;3000,1286;2900,1754;1100,1638
        --tripwire-interval-ms 1000
```

**没有** `--mqtt-host`(`GATE_MQTT_HOST` 为空 → `run_gate_service.sh:175` 不加这三个参数)。
**没有** `--tripwire-line`(4 个 `GATE_TRIPWIRE_CH*` 全空 → 过线计数未启用)。

最近 4 路业务统计行(实测,2026-11-23):

```
[ch1][frame 1331600] fps=20.1 deliv=19.0 src=30.0 rga=2.47ms npu=27.49ms detwait=34.39ms ocrwait=64.69ms objs=5 tracks=5 spd=0.0 sv=0 rss=641556KB heap=495757KB
[ch2][frame 1400100] fps=25.0 deliv=20.0 src=21.5 rga=2.32ms npu=27.49ms detwait=36.64ms ocrwait=0.00ms objs=2 tracks=1 spd=0.0 sv=0 rss=641556KB heap=495739KB
[ch3][frame 1397900] fps=24.9 deliv=20.0 src=60.0 rga=2.68ms npu=26.70ms detwait=35.24ms ocrwait=54.77ms objs=3 tracks=3 spd=0.0 sv=0 rss=641556KB heap=495628KB
[ch4][frame 1400000] fps=25.2 deliv=20.0 src=60.0 rga=7.03ms npu=26.60ms detwait=33.18ms ocrwait=0.00ms objs=0 tracks=0 spd=0.0 sv=0 rss=641556KB heap=495634KB
```

**同时在看门狗告警(实测,2026-11-23 12:26:37)**:
```
🔴 [rk-gate 看门狗] RSS 626MB 超过阈值 400MB
🔴 [rk-gate 看门狗] 活堆 483MB 超过阈值 200MB
```
systemd 显示 `Memory: 994M (max: 1G, swap max: 0B, available: 30.1M, peak: 1G)`,`Tasks: 30 (limit: 9155)`,进程 fd 数 **167**,线程数 **30**,累计 CPU 时间 21h+。

> 本节仅陈述实测值。RSS/活堆与看门狗阈值的关系、以及 `MemoryMax=1G` 的余量,见 §10 风险表与 §11 开放项。

### 1.4 复核快照(2026-11-23 12:40,成文后约 30 分钟)

板子状态在成文后继续变化。下表为复核实测值,**只陈述事实**。

```
$ systemctl is-active rk-gate rk-gate-watchdog rk-night-soak rk-pipeline
active / active / activating / activating

$ systemctl show rk-gate -p MemoryCurrent -p MemoryPeak -p MemoryMax -p TasksCurrent
MemoryCurrent=1073434624   MemoryPeak=1073741824   MemoryMax=1073741824   TasksCurrent=30
                            ↑ 已顶到 MemoryMax(=1G)

$ cat /sys/fs/cgroup/system.slice/rk-gate.service/memory.events
low 0    high 0    max 3044420    oom 0    oom_kill 0    oom_group_kill 0
        ↑ max 事件 304 万次(触到上限), 但 oom / oom_kill 均为 0

$ cat /sys/kernel/debug/rknpu/load
NPU load:  Core0: 65%, Core1: 68%, Core2: 68%,
```

最新业务统计行(2026-11-23 12:40):

```
[ch1][frame 1414200] fps=26.4 deliv=20.0 src=60.0 rga=3.12ms npu=27.10ms detwait=34.48ms
                   ocrwait=38.92ms objs=0 tracks=0 spd=0.0 sv=0 rss=646832KB heap=500562KB
[ch2][frame 1416500] fps=26.8 deliv=20.0 src=21.5 rga=2.12ms npu=26.98ms detwait=37.06ms
                   ocrwait=0.00ms objs=1 tracks=1 spd=0.0 sv=0 rss=646832KB heap=500786KB
```

与 §1.3 成文时的差别(同一台板,间隔约 30 分钟):
**RSS 641556KB → 646832KB**,**活堆 495757KB → 500562KB**,cgroup `Memory` 由 994M 变成 **顶满 1G**。

仓库状态(复核时):分支 `fix/p0-correctness-stability`,已修改文件 7 个(未提交、未 merge)。

---

# §2 硬件与运行环境

## 2.1 硬件

| 项 | 值 | 出处 |
|---|---|---|
| 板卡 | Seeed Studio **Recomputer RK3588 Devkit** | `AGENT_PROJECT_BASELINE.md` §二 |
| CPU | 8 核(4×A76 + 4×A55) | 同上 |
| NPU | **3 核**,算力 6 TOPS,支持独立调度 | 同上 |
| 内存 | 8 GB | 同上 |
| 可用 NPU 频率 | 300/400/500/600/700/800/900/1000 MHz | 实测 `/sys/class/devfreq/fdab0000.npu/available_frequencies` |
| CPU 可用频率 | **408/600/816/1008/1200/1416/1608/1800 MHz**(policy4/6 上限 2256) | `docs/task1_gate_lock_fix.md` §附带发现 |
| DDR | dmc_ondemand,2400 MHz 理论峰值 19.2 GB/s;实测 STREAM copy **18.60 GB/s (97% 峰值)** | `batch_bench/batch_infer_report.md` §6 |

## 2.2 系统

| 项 | 值 | 出处 |
|---|---|---|
| 内核 | `6.1.172-vendor-seeed-rk3588` (aarch64) | 实测 `uname -r` |
| 发行版 | Armbian / Ubuntu 22.04 系 | `AGENT_PROJECT_BASELINE.md` |
| 板端 RKNN 运行时 | `/usr/lib/librknnrt.so` **2.3.2** (429f97ae6b@2025-04-09),driver 0.9.8 | `audit/out/env_audit.txt` |
| 模型转换工具链 | **rknn-toolkit2 2.3.2** (commit c7d6ffcf),Python 3.12 | `_bench/venv/.../rknn_toolkit2-2.3.2.dist-info/METADATA` |
| 运行时一致性 | `/usr/lib/librknnrt.so` 与 `third_party/lib/librknnrt.so` md5 均为 `a37ee1d5d664c79836bf6e35b7ef6289`;runtime 2.3.2 == compiler 2.3.2 == Toolkit 2.3.2 | `audit/performance_audit_report.md` §1.3 |
| RGA | `librga` 1.9.1_4(`im2d_version.h`) | `third_party/include/rga/im2d_version.h` |
| MPP | `librockchip-mpp1` **1.5.0-1**(系统 `/usr/lib/aarch64-linux-gnu/`) | `dpkg -l` 实测 |
| OpenCV | **4.10.0**(系统) | `pkg-config --modversion opencv4` |
| libjpeg | libjpeg-turbo8 2.1.5-4ubuntu4(系统) | `dpkg -l` |
| MQTT | **自研零依赖** MQTT 3.1.1(`src/mqtt_pub.cc` / `mqtt_sub.cc`) | 源码 |

**`third_party/` 只有 2 个库**:`librknnrt.so`(7.7 MB)+ `librga.so`(212 KB)+ 头文件。README 明确声明该目录**不受仓库 MIT 协议覆盖**,按 Rockchip EULA 分发。OpenCV / jpeg / MPP 都是系统装的。

## 2.3 NPU 与 CPU 调频(实测 2026-11-23)

```
/sys/class/devfreq/fdab0000.npu/cur_freq   = 1000000000   (1 GHz, 本板最高)
/sys/class/devfreq/fdab0000.npu/governor   = rknpu_ondemand
```
- NPU 的 1 GHz 是它的**最高频**,可以锁;CPU **没有 1GHz 档**,`min=max=1GHz` 在 CPU 上做不到。
- `audit/performance_audit_report.md` 把「NPU governor 非 performance」列为风险项 L3,但注明**当前 cur == max,损耗为 0**,并给出 `min_freq = max_freq = 1000000000` 的定频做法。
- `npu_layer_bench` 记录了一个测量陷阱:`target_freq` 只读,**`governor=userspace + target_freq=1GHz` 是假锁**;实测抓到 NPU 跑在 900 MHz(单帧 25.4 ms 而非 23.0 ms,正好 1000/900=1.111),数据表面上完全看不出问题。
- 📌 **2026-11-24 口径统一(Wave 1)**: 上面的 `cur_freq = 1000000000` 是**满载时**的读数。
  现网 governor 是 `rknpu_ondemand`(`min=300 MHz / max=1000 MHz`,
  见 `audit/out/env_audit.txt:18-19`), **业务空档期会降频**, 下一帧要等升频爬坡。
  所以:
  - `layer_bench_report.md` 的 **23.0 ms/帧、133 FPS** 是**主动锁频 1000 MHz 下的"算力天花板"**,
    是**上限口径**, 不是现网保证值;
  - 现网 4 路全链路的实测值是 **105.0 FPS**(同报告 §2), 这个才是生产口径。
  - CPU 侧(`ondemand`): policy0 max 1800 / policy4 max 2256 / policy6 max 2256 MHz;
    `cur` 随负载浮动 —— 满载快照 `1800/1800 · 2256/2256 · 1416/2256`
    (`performance_audit_report.md:64`), 低负载快照 `1200/1200 · 1200/2256 · 1608/2256`
    (`audit/out/env_audit.txt:35-37`)。**两组都是 `cur/max` 对, 都要带采样条件引用。**
  - ⚠️ 判 NPU 占用**只能用** `/sys/kernel/debug/rknpu/load`(逐核, 需 root);
    `/sys/class/devfreq/fdab0000.npu/load` 在本板**恒显 `100@1000000000Hz`, 不可用**。
  - 统一口径见 §12.1。

## 2.4 关键环境限制(实测)

| 限制 | 证据 |
|---|---|
| **本板没有 `/dev/rknpu`**,NPU 设备节点是 `/dev/dri/renderD12[89]` / `renderD130` | `PRODUCTION_GUARD.md` 预检项 ⑤ |
| **`/sys/class/devfreq/fdab0000.npu/load` 恒等于 100**(空闲时也是),不是真实占用率 | `WEB_CONSOLE.md` §硬件负载来源;判断空闲必须用 `/sys/kernel/debug/rknpu/load`(需 root) |
| 本板内核**未实现 dma-buf 的 cache 同步 ioctl**(`RK_DMA_BUF_IOCTL_SYNC` 一律 ENOTTY) | `ZERO_COPY_MPP_RGA.md`;对策:`dma_buf_alloc()` 优先申请 `/dev/dma_heap/system-uncached` |
| 本板 librga/驱动**不支持 BT.709**(`IM_YUV_BT709_LIMIT_RANGE` 报 `Unsupported full csc mode!`) | 同上;源固定用默认 BT.601 limited |
| `RGA_MMU unsupported memory larger than 4G` —— 软件 MMU 在 4GB 以上用户态地址上会失败且**会写坏堆** | 同上;整链路必须端到端 dma-buf,任何一端断掉就退化成拷贝(慢 7.6×) |
| ~~`/var/log` 是 **47MB zram**(`/dev/zram1` ext4)~~ → **2026-11-23 已迁到 `/var/log.hdd`**(105GB 真实盘, 实测剩 78GB); `/var/log` 本身仍是 zram, 只是生产日志不再写它 | `RISK_REGISTER.md` R19(✅ 已闭环);`df -hT /var/log.hdd` 实测 |
| journald `Storage=volatile`,**重启即丢** | `RISK_REGISTER.md` R21 |
| 本板**没有 60fps 视频源概念**:现有素材要么 21.5fps(720p),要么 59.94fps 但 10bit(HEVC,RGA 走不了硬件路径) | `WEB_CONSOLE.md` §兼容性 |
| **10bit HEVC 在本板没有硬件路径** —— MPP/RGA 都不支持 10→8 降位深。⚠️ 2026-11-24 更新: 生产代码已加位深探测 + 拒收护栏(P2-A1), 10bit 源会**自动改走 FFmpeg 软解**(不再静默零检出), 但交付率只有 **~10 FPS/路**(MPP 路径是 20 FPS)。现场对策: 相机/NVR 切 8bit H.264 / 8bit H.265 Main Profile | `RK3588_ENGINEERING_GUIDE.md` §踩坑;`docs/p2a_stability_hardening_report.md` §2.1 / §七-1;`RISK_REGISTER.md` R24 |
| MPP 解出的图高度会**对齐到 16** | 同上 |

---

# §3 代码仓库结构

## 3.1 顶层

```
/home/seeed/rk_deploy/          git repo, 72 MB .git, 工作区 6.8 GB
├── rk3588_zero_copy/           ★ 生产工程(C++17 + CMake,板端原生编译)
├── models/                     ★ 唯一模型真源(只读)
├── accuracy/                   精度回归离线工具链
├── _bench/                     NPU 微基准 + 模型转换工具链(含 venv 与 toolkit wheel)
├── docs/                       交付文档(白皮书/PPT 素材/各阶段报告)
├── audit/                      性能基线审计
├── opt_ocr_quant/              PP-OCR 混合量化沙盒
├── opt_dfl/                    fork9(DFL 移出 NPU)沙盒
├── opt_ebike_fork9/            ★ 本次新增:ebike fork9 沙盒
├── opt_audit/                  rk_gate 改造前/后板端 A/B
├── model_opt/                  YOLOv8 SiLU→ReLU × 分辨率下调
├── npu_layer_bench/            NPU 分层压测台
├── prune_finetune/             结构化通道剪枝
├── batch_bench/                Batch 推理基准
├── thread_bench/               三核 NPU 极限并发测试台
├── opt_p0/                     P0 修复的对拍工具与数据
├── dataset/ (2001 帧)  dataset_mp4/ (1664 帧)  logs/  batch_bench/ ...
```

git 现状(实测):
- 分支:`fix/p0-correctness-stability`(当前)、`main`(同 commit `47a7ebb`)、`perf/hotpath-implementation-waste`(`b9434c5`)
- 提交跨度:**2026-11-12 → 2026-11-19**
- 工作区有 **9 个 M**(阶段 0 的 P0 修复)+ 若干未跟踪目录(各沙盒)

## 3.2 生产工程 `rk3588_zero_copy/`

**编译产物有两个,别混**:

| 目标 | 源文件 | 用途 |
|---|---|---|
| **`rk_gate`** | `src/gate_main.cc` (1639 行) | **生产守护链**。单进程 4 路 + 全局 NPU 线程池 |
| `rk_zero_copy` | `src/main.cc` (4397 行) | 单流版零拷贝主程序,含 `--npu-pipe` 多实例并行。**已冻结为稳定基线** |

`src/` 共 37 个文件 / 866 KB;`include/` 共 31 个文件 / 183 KB。关键文件:

| 文件 | 行 | 作用 |
|---|---|---|
| `src/gate_main.cc` | 1639 | 生产主程序:4 通道 + NpuPool + TripwireService + MQTT/串口上报 |
| `src/rk_base/npu_pool.cc` | 770 | NPU 全局任务池:优先级队列 + 每核 worker + 每核闸门 |
| `include/rk/npu_pool.h` | 443 | 池接口(`NpuPool::Config` / `DetTask` / `DetResult`) |
| `src/yolo_post.cc` | 621 | YOLOv8 后处理:形态 A / **形态 B(9 输出 DFL)** / 形态 C;分类别 NMS;几何过滤 |
| `src/rga_convert.cc` | 1221 | RGA 封装(NV12→RGB888、零拷贝导入、CPU 兜底) |
| `src/license_plate_rec.cc` | 781 | PP-OCRv4_mobile_rec 车牌识别(源自 rknn_model_zoo v2.5.0 示例) |
| `src/lprnet.cc` | 291 | 旧 LPRNet 车牌识别(INT8,160×48)—— **不在生产链路上** |
| `src/mpp_decoder.cc` | 529 | Rockchip MPP 原生 H.264/H.265 硬解封装 |
| `src/speed_track.cc` | 500 | 跟踪 + 测速(Med5 / ByteTrack-lite / 速度估计) |
| `src/ipm.cc` | 347 | 逆透视标定 + 鸟瞰投影 |
| `src/rk_base/log.cc` | 437 | 分级日志(write(2) 直写 fd)+ `RK_LOG_LEVEL` 过滤 + **令牌桶限流** |
| `src/rk_base/mem_guard.cc` | 265 | 受控分配器 + canary 越界哨兵 |
| `src/rk_base/stack_guard.cc` | 286 | 线程栈水位 + 主循环心跳看门狗 |
| `src/rk_base/tripwire.cc` | 527 | 过线计数(虚拟线) |
| `src/rk_base/npu_pipe.cc` | 353 | NPU 多实例并行检测(`--npu-pipe`) |
| `src/mqtt_pub.cc` / `mqtt_sub.cc` | 265 / 407 | 自研零依赖 MQTT |
| `src/dmabuf.cc` | 270 | dma-buf 分配 / mmap / cache 同步(Rockchip `rk-dma-heap` ioctl) |
| `src/engine_ctl.cc` | 522 | 运行时 MPP/RGA/NPU 三开关 + 负载采集 |
| `src/eval/*.cc` | — | 检测/识别精度评测、NPU batch/passthrough/post/sched 基准 |
| `src/minimal/unit_*.cc` | — | 最小化测试单元:decoder-only / npu-only / rga-only(归因用) |

## 3.3 生产流水线每通道的阶段顺序

`gate_main.cc:808-1207` 的 `Channel::run()`:

| 序 | 阶段 | 执行单元 | 绑核 |
|---|---|---|---|
| 1 | 上报通道装配(MQTT/串口,每路独立连接) | 通道线程 | — |
| 2 | **解码**(MPP 硬解 → NV12 dma-buf fd) | VPU | 内核驱动 |
| 3 | **限帧**(`--max-fps 20`,只解码不分析) | 通道线程 | CPU |
| 4 | 借检测 slot(背压) | 池 | — |
| 5 | **RGA 预处理** NV12→RGB888(640×640),直接写进 slot 的 dma-buf | RGA | RGA |
| 6 | 提交检测任务 | 池 | **NPU** |
| 7 | 等检测结果(`take_det(5000ms)`) | 通道线程阻塞 | — |
| 8 | CPU 后处理 + 几何过滤 | CPU | CPU |
| 9 | 跟踪/测速 | CPU | CPU |
| 10 | IPM 标定(仅 high 通道,首帧一次) | CPU | CPU |
| 11 | 车牌 ROI 裁剪 + PP-OCR 提交(仅 near 通道) | CPU → 池 | **NPU**(识别) |
| 12 | 过线计数(仅配了 `--tripwire-line` 才启用) | 独立后台线程 | CPU |

**NPU 核绑定规则**(`npu_pool.cc:96-183`):
- `det_cores = ocr_cores = {0,1,2}`;**worker i 绑 `cores[i % 核数]`** → 3 个检测 worker 分别 mask **0x1/0x2/0x4**,3 个识别 worker 同样。
- `sanitize_core_mask`(`core_mask.h:127-138`):**mask 0x7 被禁用并回退到 0x1(Core0)**。实测依据:0x7 峰值吞吐 45~51 fps vs 均匀分核 134 fps(1/2.6),e2e P99 723~913 ms vs 38 ms(高 25 倍)。
- **优先级**:`kDet = 0, kOcr = 1` → **识别永远排在检测前面**。
- 每核一个 `CoreGate` 闸门串行化同核任务。

**CPU 核绑定:没有。** 全仓库 grep 无 `sched_setaffinity` / `pthread_setaffinity_np`。

**进程结构**:主线程建 NpuPool → 建 TripwireService → **在主线程里打开全部 4 路源**(MPP init 走 SafeProbe 的进程级信号处理,不能并发调)→ 每路 1 个通道线程 → 主线程每 1 秒轮询退出。

**librga 在一个进程内是串行的**(`gate_main.cc:108` 的 `std::mutex g_rga_m`):4 个通道线程共用一把互斥量排队。注释记录:抢锁排队会让单次 RGA 从 **6.7 ms(4K)涨到 24.6 ms**,近景那路被拖到 8 fps。

---

# §4 生产服务(部署层)

## 4.1 systemd 单元 `scripts/systemd/rk-gate.service`

| 段 | 项 | 值 |
|---|---|---|
| `[Unit]` | `StartLimitIntervalSec` / `StartLimitBurst` | 300 / 5 |
| | `OnFailure` | `rk-collect-crash.service`(**必须写在 `[Unit]`**,写 `[Service]` 会被静默忽略) |
| `[Service]` | `Type` / `User` | simple / seeed |
| | `RuntimeDirectory` / `Mode` / `Preserve` | `rk-gate` / 0755 / **yes**(看门狗的 `paused`/`stop_alerted`/退避状态不随重启清) |
| | `EnvironmentFile` | `config/gate.env` |
| | `Environment` | `RK_GATE_SYSTEMD=1`(据此拒绝手动再起一个) |
| | `ExecStartPre` | `scripts/preflight_gate.sh`(预检不过 → exit 78 → 拒绝启动) |
| | `ExecStart` | `scripts/run_gate_service.sh`(内部 `exec` → MainPID 即 rk_gate) |
| | `Restart` / `RestartSec` | on-failure / 10 |
| | `RestartPreventExitStatus` | **78**(EX_CONFIG,配置错不重试,交看门狗指数退避) |
| | `TimeoutStopSec` / `KillSignal` / `KillMode` | 30 / SIGTERM / mixed |
| | `LimitCORE` | 0 |
| | `MemoryMax` / `MemorySwapMax` | **1G** / 0 —— 注释明写「数值待按实测稳态 RSS 校准」 |
| | `StandardOutput` / `StandardError` | `append:/var/log.hdd/rk-gate.log` / `append:/var/log.hdd/rk-gate-error.log`(📌 2026-11-23 随日志目录迁移改过; 仓库里 `scripts/systemd/rk-gate.service` 是**安装前的模板**, 仍是 `/var/log/...`, 由 `install_gate_service.sh` 按 `GATE_LOG_DIR` 重写) |
| | `LogRateLimitIntervalSec` / `Burst` | 60 / 1000 —— 注释明写**对 `append:` 直写文件不生效**(实测:同一份 unit 写 20000 行,append: 落 20000 行,换 journal 只落 125 行) |

**没有设置**:`WatchdogSec=`(systemd 原生 watchdog 未启用)、`CPUQuota`/`CPUWeight`/`TasksMax`/`Nice`、CPU 亲和性。

## 4.2 `config/gate.env`(159 行,10 节)全部可调项

| 变量 | 当前值 | 控制什么 |
|---|---|---|
| `GATE_MODEL` | `models/ebike_int8_rk2332.rknn` | 检测模型。注释:必须用 rk2332 版,toolkit 2.5.0 编的 `ebike_int8.rknn` 在 2.3.2 runtime 上加载失败 |
| `GATE_LABELS` / `GATE_OCR` / `GATE_OCR_DICT` | labels_ebike.txt / ppocrv4_rec_fp16.rknn / ppocr_keys_v1.txt | 类别名 / 识别模型 / 字典 |
| `GATE_CH1..CH4` + `GATE_CH*_KIND` + `GATE_CH2_IPM` / `GATE_CH4_IPM` | 4 个本地 MP4 + near/high + 4 点 IPM | 通道源与标定 |
| `GATE_DET_CORES` / `GATE_OCR_CORES` | `0,1,2` / `0,1,2` | NPU 核序号列表 → mask |
| `GATE_DET_WORKERS` / `GATE_OCR_WORKERS` | 3 / 3 | worker 数 |
| `GATE_CONF` / `GATE_NMS` | 0.15 / 0.45 | 检测阈值 |
| **`GATE_MAX_FPS`** | **20** | 每路分析帧率上限 |
| `GATE_STATS_INTERVAL` | 100 | 每 N 帧打一行统计 |
| `GATE_DET_SLOTS` | 2 | 每检测 worker 的 slot 数(双缓冲) |
| `GATE_ROI_RATIO` / `_Y` / `_HEIGHT` / `_SHIFT` / `_BELOW` | 0.164 / 1.0 / 0.40 / 0.126 / 0 | 车牌 ROI 几何 |
| `GATE_IPM_WIDTH_M` | 3.5 | 标定区车道宽度(米) |
| `GATE_SN` / `GATE_DEVICE_NAME` / `GATE_IPADDR` | RK3588-GATE-01 / 0001 / 192.168.31.77 | MQTT 标识 |
| `GATE_MQTT_HOST` / `_PORT` / `_QOS` | **(空)** / 1883 / 1 | **留空 = 不发 MQTT**;看门狗 MQTT 探针因此不生效 |
| `GATE_TRIPWIRE_CH1..CH4` | 全空 | 过线虚拟线,留空 = 不启用 |
| `GATE_TRIPWIRE_INTERVAL_MS` | 1000 | 过线统计上报周期 |
| `GATE_LOCK_FILE` | `/run/rk-gate/rk-gate.lock` | flock 互斥锁路径 |
| `GATE_DURATION` | 0 | 运行秒数,0 = 一直跑 |
| `GATE_LOG_DIR` | `/var/log` | **注释警告**:本板是 47MB zram,生产建议改 `/var/log.hdd`;改完必须重跑 `install_gate_service.sh` |
| `GATE_SERVICE_USER` | seeed | 服务运行用户 |
| `GATE_LOG_LEVEL` | **warn** | → `RK_LOG_LEVEL`。**注释:必须是 warn** —— 帧守卫逐帧 INFO,4 路 36.5 MB/小时 ≈ 17 GB/天,zram 不到 1 小时写满 → `write()` ENOSPC → 应用崩 |

> 文件被读两遍:systemd `EnvironmentFile=` 原样注入 + `run_gate_service.sh:38` `source` 后拼 CLI。**不得写 `export`**。
> 日志轮转参数不在 gate.env,在 `scripts/logrotate/rk-gate`;看门狗阈值不在 gate.env,在 `scripts/gate_watchdog.sh:69-82`;应用侧令牌桶参数也不在 gate.env,默认写在 `src/rk_base/log.cc:77-78`(1000 行 / 60 秒)。

## 4.3 看门狗 `scripts/gate_watchdog.sh` 判据

| 变量 | 默认 | 含义 |
|---|---|---|
| `CHECK_SEC` | 20 | 检查周期 |
| `STALL_SEC` | 60 | **L1 整机心跳**(日志 mtime 停滞) |
| `CH_STALL_SEC` | 180 | **L2 单路帧号停滞** |
| `CH_SLOW_ROUNDS` | 3 | 其余路在推进时,连续几轮停滞才动手 |
| `L2_COOLDOWN_SEC` | 60 | MainPID 变化后的 L2 冷却 |
| `SILENT_STALL_SEC` | 60 | **L1.5 静默卡死**(日志在写但帧号不涨)—— 阶段 0 新增 |
| `BACKOFF_START` / `BACKOFF_MAX` | 60 / 300 | 服务未运行时的指数退避(60→120→240→300) |
| `HEALTH_RESET_SEC` | 600 | 连续健康这么久退避清零 |
| `FAIL_ALERT_THRESHOLD` | **3** | **连续失败几次才开始升级告警**(2026-11-24 补录 —— 之前的文档漏了这一条) |
| `RSS_WARN_MB` / `HEAP_WARN_MB` / `CGROUP_WARN_MB` / `LOG_FREE_WARN_MB` | **350 / 150 / 400 / 2048** | 水位告警线(只告警不重启)。⚠️ 旧值 400/200/8 是"泄漏还在、页缓存把 cgroup 顶满"那个状态下定的, 已于 2026-11-23 按新稳态重标(现值在 `config/gate.env` §11) |
| `MQTT_PROBE_SEC` | 60 | MQTT 探针窗口(默认关,需 `GATE_MQTT_PROBE=1`) |

L2 区分"卡死"与"变慢":**≥2 路在推进 → 只告警,连续 3 轮(≥9 分钟)仍不恢复才重启**;其它路也停 → 直接重启。

> 📌 **2026-11-24 口径统一(Wave 1)**: `现场运维手册.md` 原先写的"日志停 > 5 分钟"**是错的** ——
> 实际 `STALL_SEC=60`(1 分钟), 另外还有一条它没写的 **L1.5 `SILENT_STALL_SEC=60`**
> (日志**还在写**但所有路帧号都不涨 1 分钟 → 判业务线程卡死), 以及
> **告警升级门限 `FAIL_ALERT_THRESHOLD=3`**(连续失败 3 次才升级)。
> 统一口径见 §12.1。

## 4.4 预检 `scripts/preflight_gate.sh` 12 项

① 二进制存在可执行 ② 模型/标签/OCR/字典可读 ③ **librknnrt 版本 == 2.3.2** ④ librga / librockchip_mpp 就位 ⑤ NPU 设备节点 ⑥ dma_heap `/dev/dma_heap/system` ⑦ 4 路源可用 + high 通道有 IPM ⑧ **目标日志文件可写**(不是目录可写) ⑨ 日志盘剩余空间 FAIL(<8MB)/WARN(<32MB) ⑩ 核心配置项齐全 ⑪ MPP 服务节点(WARN) ⑫ MQTT broker 连通(WARN)。任一 FAIL → exit **78**。

## 4.5 日志体系

> 📌 **2026-11-24 口径统一(Wave 1)**: 日志目录已于 2026-11-23 从 `/var/log`(47MB zram)
> 迁到 **`/var/log.hdd`**(105GB 真实盘, 实测剩 78GB) —— 迁移动机见 §10.1 的 R19。
> 下表路径已全部改成新目录。

| 位置 | 内容 |
|---|---|
| `/var/log.hdd/rk-gate-error.log` | 业务日志 |
| `/var/log.hdd/rk-gate.log` | stdout(预检、启动参数) |
| `/var/log.hdd/rk-gate-alerts.log` | 看门狗告警 |
| `/var/log.hdd/rk-gate-preflight.log` | 预检结果 |
| `journalctl -u rk-gate -n 100` | systemd 层(**本板 journald 是 volatile,重启即丢**) |
| `logs/crash_<时间戳>.tar.gz` | 崩溃取证包 |

**实测日志速率** —— 📌 **已分两层, 见 §12.1**:

- **生产级(`warn`, 现网配置)**: **398 KB/小时 ≈ 9.5 MB/天**
  (2026-11-19 封板复测: 4 路推理 + 看门狗 + rk-minimal 同时在跑的真实生产配置)。
  logrotate 规则里的 ~0.6 MB/小时 是同一量级的估算(4 路每 100 帧一行 → 约 5 秒一行)。
- **历史 `info`/`debug` 失控速率(仅作量级参照, 不是现网速率)**: 三个数来自**不同批次、
  不同二进制、不同采样窗口**, 互不相等 ——
  36.5 MB/小时 ≈ 17 GB/天(2026-11-18 旧二进制)/
  156 行/秒 ≈ 80 MB/小时(2026-11-19)/
  7.9 GB/天(`SOAK_REPORT_20261118.md` R1: 96 秒内 70,023 行, 文件增长 8.8 MB/96 秒)。

**令牌桶实测**:同一台板、同样开 debug 3 分钟 —— **不限流 = 28043 行 / 吃 4 MB;限流 = 2762 行 / 吃 0.3 MB,降 90%**。

**logrotate 现状**:`hourly` / `rotate 12` / `maxsize 4M` / `copytruncate` / `compress` + `delaycompress` / **`su root root`**(历史上 `su seeed root` 导致**轮转从来没成功过**)。实测 `/var/log` 下目前**没有任何 `.1`/`.gz` 轮转文件**。

---

# §5 模型资产与转换配方

## 5.1 `models/` 清单(唯一模型真源,只读)

| 文件 | 字节 | 说明 |
|---|---|---|
| `ebike_int8_rk2332.rknn` | 4856487 | **生产检测模型**。YOLOv8n 骨干,nc=2(person/ebike),形态 C(2 输出),INT8,toolkit 2.3.2 编译(编译器哈希 `e045de294f@2025-04-07`) |
| `ebike_int8.rknn` | 5112295 | 算法组原始版,**toolkit 2.5.0 编译**,在 2.3.2 runtime 上加载失败 |
| `best.rknn` | 4062984 | 板上已验证能跑的那一版(9 输出形态) |
| `ppocrv4_rec_fp16.rknn` | 7066425 | **生产识别模型**,PP-OCRv4 rec,6625 字符集(含省份汉字),FP16 |
| `ppocrv4_rec_int8.rknn` | 4383238 | 识别 INT8 对照版 |
| `lprnet_finetuned_int8.rknn` | 903307 | 旧 LPRNet(生产链路上**未使用**) |
| `ppocr_keys_v1.txt` / `labels_ebike.txt` / `labels_list.txt` | 26249 / 13 / 11 | 字典与类别名 |

**仓库里没有的**:`ebike` 的 **ONNX 源**与 **.pt 权重**(见 §11.1)。全盘 `find / -name "*.onnx" -o -name "*.pt"` 只找到 yolov8n / yolo26 / ppocrv4 的。

## 5.2 生产检测模型(形态 C)的元数据

```
输入 images : [1,3,640,640] 逻辑 nchw / 原生 NHWC int8, mean=[0,0,0] std=[255,255,255], rgb2bgr=False
输出 0 /model.22/Mul_2_output_0      : box [1,4,8400]  (cx,cy,w,h), 像素, scale=2.548393487930298 zp=-128
输出 1 /model.22/Sigmoid_output_0    : cls [1,2,8400]   概率,      scale=0.0038035186007618904 zp=-128
类别: 0=person 1=ebike
算子: 122 个(NPU 119 / CPU 3), 单次推理 35617 us @ NPU 1GHz
```

**归一化已固化进模型**(有一个 Div255 节点)→ 板端**必须喂原始 0~255 像素,不能再除 255**。实测三种喂法:

| 喂法 | box.max | cls_max |
|---|---|---|
| (a) uint8 NHWC 原始[0,255] | 634.550 | **0.867202** |
| (b) float32 NCHW 原始[0,255] | 634.550 | **0.867202**(与 a 完全一致) |
| (c) float32 NCHW **已 /255** | 639.647 | **0.019018** ← 检测被毁 |

## 5.3 转换配方(仓库里已固化的口径)

```python
r.config(mean_values=[[0,0,0]], std_values=[[255,255,255]], target_platform='rk3588')
r.load_onnx(model=src)
r.build(do_quantization=True, dataset=calib_list)   # 或 do_quantization=False 做 FP16
r.export_rknn(dst)
```
出处:`accuracy/convert_models.py:26-51`、`npu_layer_bench/scripts/make_res_models.sh`、`_bench/convert_yolov8n.py`。
**OCR 用 mean/std 127.5/127.5**,与 YOLO 的 0/255 不同。

## 5.4 量化校准集现状

| 集 | 张数 | 来源 |
|---|---|---|
| `_bench/calib_list.txt` | 24 | **全部是办公桌场景**(`ACCURACY_AND_INTEGRATION_20261113.md` §量化校准集风险) |
| `accuracy/calib/calib_yolo_24.txt` | 24 | 复用上面那份 |
| `accuracy/calib/calib_yolo_200.txt` | 200 | 从 1724 帧里均匀采样(**同分布**) |
| `accuracy/calib/calib_rec_24.txt` / `_200.txt` | 24 / 200 | 车牌裁剪图 |
| `opt_ebike_fork9/calib/ebike_calib.txt` | 200 | **本次新增**:取自 `dataset/` + `dataset_mp4/`,**与 1724 帧回归测试集不相交**。算法组原版 `calib/ebike_calib.txt` 拿不到(见 §11.1) |

`ACCURACY_AND_INTEGRATION_20261113.md` §量化校准集风险原文:
> 现在用的校准集是 `_bench/calib/` 里的 **24 张图,全部来自桌面场景**;生产部署是路口机位,建议换成路口实拍帧(已解出 1664 帧)重编 INT8 —— 不影响"INT8 vs FP16"的结论,但可能影响"INT8 的绝对精度"。

`accuracy/ACCURACY_REPORT.md` §结论 2 原文:
> **加校准集数量没用**: 24 → 200 张, 整牌反而从 80.65% 掉到 77.42%。原因很明确: 我这两套校准集是【同一批视频、同一块车牌、同一光照】里取的, 只是数量不同 —— **分布完全一样**。要改的是**分布**不是**数量**。

---

# §6 生产流水线的实测耗时分解

## 6.1 单路(README 与工程指南口径)

| 环节 | ms/帧 |
|---|---|
| MPP 硬解 | 0.49~0.62 |
| RGA 预处理 | 2.31~2.35 |
| RKNN 推理 | 27.87~28.33 |
| CPU 后处理 | 0.46~0.47 |
| **整帧** | **39.89~40.02**(等于源推送间隔) |

## 6.2 公平 A/B/C(同一条命令、同样 600 帧、检测+识别全开,每组只关一个硬件单元)

| 组 | FPS | 解码 ms | 预处理 ms | NPU ms | 单帧总 | **CPU** |
|---|---|---|---|---|---|---|
| 全开(MPP+RGA) | — | — | — | — | — | 23.9% |
| 只关 MPP 硬解 | — | 18.42 | 11.42 | 26.37 | — | **98.1%**(4.1×) |
| 只关 RGA | — | — | — | — | — | **103.3%**(4.3×) |

## 6.3 4 路生产配置(2026-11-18 最终纯净基线,10 分钟)

| 通道 | 交付 FPS | 单帧 NPU | 检测等待 | 识别等待 | RGA |
|---|---|---|---|---|---|
| ch1 近景/车牌 | 19.73 | 26.4 ms | 31.9 ms | 52.5 ms | 2.3 ms |
| ch2 高位/测速 | 19.90 | 26.9 ms | 34.0 ms | — | 3.1 ms |
| ch3 近景/车牌 | 19.73 | 26.7 ms | 33.4 ms | 58.3 ms | 3.1 ms |
| ch4 高位/测速 | 19.83 | 26.6 ms | 32.5 ms | — | 7.0 ms |
| **合计** | **79.20 FPS** | 26.7 ms | 33.0 ms | 27.7 ms | — |

NPU 三核 **65/66/65%**(极差 2);整机 CPU **25.9%**(8 核口径,负重时 68~77%);RSS **238 MB** 稳定;活堆 74.6→75.2 MB 平;**fd 数 165 全程不变**;MPP/EMFILE 错误 0。

## 6.4 限帧与吞吐的关系(实测)

| 配置 | 吞吐 |
|---|---|
| `--max-fps 20`(生产) | 79.96~80.02 FPS(4 路 × 20 = **80 就是天花板**) |
| `--max-fps 0`(解除限帧) | 95.2 FPS(旧核绑定)/ 105.02 FPS(现行,见 §7) |

`docs/task2_fork9_ab.md` §5.3 原文:
> 生产模型在 max-fps=20 下**已经打到 79.96**,也就是说**今天的生产瓶颈是限帧, 不是 NPU**。

## 6.5 NPU 算力预算(以 4 路全链路 105.0 FPS 为准)

| 环节 | ms/帧 | 占「等检测结果」的比例 |
|---|---|---|
| NPU 纯推理 | **25.8** | **79%** |
| NPU 排队(detwait − npu) | **6.8** | **21%** |
| RGA 预处理(1920×1080 NV12 → 640×640 RGB888) | **3.9** | — |

`105.0 FPS × 25.8 ms/帧 = 2.71 核·秒/秒` ÷ 三核总容量 3.00 → **检测吃掉 90% 的 NPU**,留给 OCR 的余量约 10%。

---

# §7 算力基线(分层实测)

出自 `npu_layer_bench/` + `layer_bench_report.md`。测量条件:**NPU 锁 min=max=1000 MHz**,预热 5 s + 测 30 s × 3 轮,表格为干净窗口均值;合并 5 轮共 251 样本,剔除被污染样本(空闲核占用 ≤ 8% 且单位耗时 ≤ 最快样本 × 1.10)。

## 7.1 分层效率漏斗

| 层 | 含义 | 配置 | 吞吐 FPS | 相对天花板 |
|---|---|---|---|---|
| L1 | 纯 NPU 裸推理 | 9 worker,每核 3 | **133.32** | 100.0% |
| L1 | 纯 NPU,每核 1 worker | 3 worker | 112.17 | 84.1% |
| L3 | + RGA 预处理 | 3 worker | 107.00 | 80.3% |
| L5 | 4 路全链路(解除限帧) | 生产二进制黑盒 | **105.02** | 78.8% |
| L4 | 单路全链路 | 生产二进制黑盒 | 31.96 | 24.0% |

L1 每核单 worker:42.15 / 41.41 / 41.65 FPS,23.74 / 24.18 / 24.02 ms。
L1 每核 2 worker:79.21 FPS;每核 3 worker:118.83 FPS;每核 4 worker:132.29 FPS。

## 7.2 分辨率扫描(同架构 1 类 YOLOv8n 重新编译)

| 输入 | 单 worker 单核 | 三核各 1 worker |
|---|---|---|
| 512×512 | 55.26 FPS / 18.09 ms | 171.91 FPS / 17.46 ms |
| 576×576 | 52.60 FPS / 19.01 ms | 154.67 FPS / 19.39 ms |
| 640×640 | 43.44 FPS / 23.03 ms | 128.46 FPS / 23.35 ms |

> `layer_bench_report.md` §7.4 原文边界:「分辨率扫描用的是**同架构的 1 类 YOLOv8n**(生产模型是 2 类 + 完整 DFL 头),绝对帧率不能直接套到生产模型上,但**缩放形状**可以。」

## 7.3 算子回落与量化纯度(`RKNN_QUERY_PERF_DETAIL`)

| 模型 | 算子总数 | NPU | CPU 回落 | NPU 耗时 | CPU 耗时 | CPU 占比 |
|---|---|---|---|---|---|---|
| 检测 `ebike_int8_rk2332.rknn` | 122 | 119 | 3(纯 IO 记账) | 37664 us | 98 us | 0.26% |
| 识别 `ppocrv4_rec_fp16.rknn` | 131 | 128 | 3 | 25231 us | 2331 us | **8.46%** |
| 识别 INT8 版 | — | — | 3 | — | 778 us | 6.01% |

识别的 CPU 回落是 `Reshape:softmax_11.tmp_0-rs`(**811 us/次**,计算类)。

检测模型量化纯度:INT8 120 个(98.36%)+ UINT8 2 个,**FP16 = 0, FP32 = 0**。

## 7.4 检测模型的逐算子耗时排名(驱动自报)

| OpType | Calls | NPU us | 占比 |
|---|---|---|---|
| ConvExSwish | 57 | 19130 | 51.87% |
| Concat | 18 | 3994 | 10.83% |
| **exSoftmax13** | 1 | **3047** | **8.26%** |
| Reshape | 6 | 2649 | 7.18% |
| Split | 10 | 2158 | 5.85% |
| Transpose | 2 | 1812 | 4.91% |
| Conv | 7 | 1536 | 4.16% |

TOP 单算子:

| 耗时 | 占比 | 算子 |
|---|---|---|
| 3360 us | 9.86% | `exSoftmax13:/model.22/dfl/Softmax` |
| 1877 us | 5.51% | `Conv:/model.0/conv/Conv` |
| 1389 us | 4.08% | `Reshape:/model.22/Concat_3_output_0_rs` |
| 1339 us | 3.93% | `Transpose:/model.22/dfl/Transpose` |
| 1061 us | 3.11% | `Resize:/model.13/Resize` |
| 890 us | 2.61% | `Transpose:/model.22/dfl/Reshape_1_output_0_rs` |
| 717 us | 2.10% | `Conv:/model.22/dfl/conv/Conv` |

**`model.22`(检测头)三轮审计实测占比:48.7% / 49.6% / 39.8%**(算子总时长 30.8~36.0 ms,model.22 合计 13.9~17.8 ms)。

## 7.5 剩余优化空间表(`layer_bench_report.md` §6.5 原文)

| # | 方向 | 预期收益 | 依据 | 代价 |
|---|---|---|---|---|
| P0 | 减少检测模型每帧计算量 | **+15~25%** | 640→512 纯推理 +27%(单核)/+34%(三核) | 精度要重新验证 |
| P1 | 换更小的检测模型 / 剪枝 | 更大(线性) | 同 P0 | 重新训练 + 精度回归 |
| P2 | 抽帧(每 N 帧检测一次) | 线性(1/N) | 直接按比例降 NPU 负载 | 牺牲时序精度 |
| P3 | 识别(OCR)并行化 | 小 | L2 单核串行 16.6 → 三核各 1 worker 55.1 FPS;**但生产已用 `ocr-core 0,1,2`,这份收益已经吃到了** | — |
| P4 | RGA 优化 | <2% | RGA 只占 3.9 ms/帧 | — |
| P5 | CPU 后处理优化 | <1% | 整条 0.36 ms/帧 | 收益 < 风险 |
| P6 | 调度 / 加线程 | **0** | 每核 2 worker 就到 132.3,再加只多 0.8% | 只涨延迟不涨吞吐 |

---

# §8 已完成的工作(按阶段)

## 8.1 阶段 0 · P0 正确性修复(`docs/p0_fixes_report.md`,2026-11-22)

分支 `fix/p0-correctness-stability`(未 merge),改动 4 个文件 / +195 −20,**推理算法零改动**。

**任务书的一处前提被文档纠正**:任务书写"stride 的真相不是『形态B 漏乘 stride』"——`p0_fixes_report.md` §〇 指出这不成立,**两个缺陷互相独立**:

| 缺陷 | 位置 | 性质 |
|---|---|---|
| A. DFL 内存布局读错 | `accuracy/acc_batch.cc` 的 `decode_dfl_9out`(参考工具) | 读成【连续 16 个】,实际是【跨步 grid_len】 |
| B. 缺 ×stride | `rk3588_zero_copy/src/yolo_post.cc` 的 `post_process_fork_9out`(生产代码) | DFL 解出来的距离是【格点单位】,却直接和【像素】的 anchor 中心相减 |

B 是真 bug,三条互相独立的证据:① DFL 量纲(softmax 在 16 个 bin 上做,期望值落在 [0,15]);② **softmax 不变量**(把 logits 整体乘常数不改变 softmax 分布 ⇒ "导出时把 stride 烤进 logits"在数学上不可能);③ 工程自己的头文件 `include/yolo_post.h` 写着 "ch0..3 = cx, cy, w, h 单位=模型输入图的像素(**已乘 stride**)"。

**对拍结果**(修复后):

| 对比 | 框数 | 配对 | 类别不一致 | 置信度最大差 | 坐标最大偏差 |
|---|---|---|---|---|---|
| 40 帧: 生产形态B vs 生产形态C(同源喂入) | 171 vs 171 | 171/171 | 0 | 0.000000 | **0.001920 px** |
| 40 帧: 生产形态B vs acc_batch 参考解码器 | 171 vs 171 | 171/171 | 0 | 0.000000 | **0.000000 px** |
| **全量 1724 帧** | **7386 vs 7386** | **7386/7386** | 0 | 0.000000 | **0.001920 px**(平均 0.000060) |
| 修复前对照 | **919 vs 171** | **0** | — | — | 全错 |

**顺带重算的检测精度**(1724 帧,参考 = `yolov8n_fp16`,用修好的解码器):

| 模型 | 匹配率 | 类别一致 | 平均 IoU | 置信度变化 |
|---|---|---|---|---|
| 图内 DFL 版 FP16 | **100.00%** | 1724 | **0.9885** | +0.0000 |
| 图内 DFL 版 INT8 | 89.68% | 1546 | 0.8375 | −0.0095 |
| **fork9 版 INT8(CPU 侧 DFL)** | **92.17%** | 1589 | **0.9709** | −0.0089 |
| *(旧报告数字,坏解码器算的)* | *84.86%* | *1463* | *0.8814* | — |

另两项子项:看门狗新增 **L1.5 静默卡死**判据(场景 B/C 触发自愈,场景 A 不误报);锁文件目录应用层自检(退出码 78)。

**回滚命令**(文档原文):
```bash
cd /home/seeed/rk_deploy
BK=/home/seeed/rk-gate-fix-backup-20261121_225210
git checkout -- rk3588_zero_copy/src/yolo_post.cc
git checkout -- rk3588_zero_copy/scripts/gate_watchdog.sh
git checkout -- rk3588_zero_copy/scripts/run_gate_service.sh
git checkout -- accuracy/acc_batch.cc
cp -p $BK/rk_gate.binary.prep0.bak rk3588_zero_copy/build/rk_gate
sudo systemctl restart rk-gate
```
⚠ 文档注明:`run_gate_service.sh` 里同时含任务①的锁文件修复,若只想回滚 P0 的自检段需手工删那段,不要整个文件回滚。

## 8.2 任务① · 锁文件被 tmpfs 冲掉(`docs/task1_gate_lock_fix.md`,2026-11-21)

**根因**(文档第一句):「**不是"权限没设对", 是锁文件被 tmpfs 冲掉了。**」

因果链:① `/run` 是 tmpfs,重启即清空;② 锁文件只在**安装时**建过;③ 装机 11-19 18:34、重启 11-20 14:55;④ `/run` 是 root:root 755,seeed 无权新建;⑤ 探测失败 → exit 75 → 看门狗反复拉起(`/run/rk-gate/fail_count = 367`)。

修复:`RuntimeDirectory=rk-gate` + `Mode=0755` + **`Preserve=yes`**(unit 加 3 行),锁路径 `/var/run/rk-gate.lock` → `/run/rk-gate/rk-gate.lock`,`flock -n 9` 与 exit 75 的互斥逻辑**一字未改**。

验证 5 项:能启动 ✅;锁文件正常创建且 fd 9 由主进程持有 ✅;4 路出帧(7000/7500/7500/7500)✅;双实例互斥仍生效(手动再起 → exit 75)✅;`kill -9` 后 6 秒自愈 ✅。

**附带发现**:① 服务停了多久(`fail_count = 367`)——「这类『重启后静默失效』不会被告警发现,因为看门狗只保证『尝试拉起』,不保证『成功』」;② 本板 CPU 没有 1GHz 档,**NPU 的 1GHz 可以锁**。

## 8.3 任务② · fork9(DFL 解码移出 NPU)板端 A/B(`docs/task2_fork9_ab.md`,2026-11-22)

**范围**:独立沙盒 `opt_dfl/`,未改生产推理代码,未 merge。

**前提**:ebike 的 ONNX 源不在仓库 → 本次用的是 **yolov8n(COCO,nc=80)** 这一对。文档原文:
> DFL 解码作用在 **box 分支的 64 个通道**上,与类别数无关;而且本仓库自己的 `npu_layer_bench` 一直用同一套口径做分辨率扫描。

**做法**:不重新导出模型,而在 fork9 的 ONNX 上原地做手术,把 DFL 解码塞回图里(三个变体 v1/v2/v3),保证**权重完全相同、量化集完全相同、转换配方完全相同**,唯一变量是「DFL 解码跑在 NPU 还是 CPU」。

**逐算子 NPU 归因**(NPU 锁 1GHz):

| 模型 | 单次推理 | 算子数 | NPU 算子耗时 |
|---|---|---|---|
| 生产 ebike(形态 C, nc=2) | 35814 us | 122 | 34194 us |
| Arm A(DFL 在 NPU, nc=80) | 27736 us | 154 | 25074 us |
| **Arm B(fork9, DFL 在 CPU)** | **24746 us** | **108** | **23195 us** |

Arm A 里"DFL 解码"这一段的算子耗时合计 **7876 us**(NPU 6848 / CPU 1028),占 Arm A 的 **28.4%**。
生产模型自己的 DFL 路径(ultralytics 原生实现)合计 **≈8.5 ms/次推理**。

**4 路板端 A/B**:

| 设置 | 模型 | 总 FPS(中位) | 单帧 NPU | detwait | ocrwait | NPU 每核 % |
|---|---|---|---|---|---|---|
| 不限帧 | 生产 ebike | **107.14** | 23.69 ms | 30.81 | 51.41 | 84/85/85 |
| (max-fps=0) | Arm A | 72.58 | 27.77 ms | 51.49 | 77.76 | 55/56/60 |
| | **Arm B** | **93.83** | **20.50 ms** | 35.73 | 66.40 | 55/56/58 |
| 生产配置 | 生产 ebike | **79.96** | 25.22 ms | — | — | 72/74/74 |
| (max-fps=20) | Arm A | 70.59 | 27.96 ms | — | — | 55/55/56 |
| | **Arm B** | **76.27** | **21.13 ms** | — | — | 54/54/54 |

Δ(Arm A → Arm B):不限帧 总 FPS **+29.3%**,单帧 NPU **−7.27 ms**;生产配置 总 FPS **+8.0%**,单帧 NPU **−6.83 ms**。

**精度**(1724 帧,见 §9)。

**文档 §八 列出的选型条件与判定**(原文表格):
| 决策条件 | 实测 | 判定 |
|---|---|---|
| ΔNPU ≤ 1ms → 选形态C(现状),不动 | ΔNPU = −7.27 ms/帧 | 不满足 |
| ΔNPU ≥ 3ms 且框精度是硬指标 → 选 fork9 | ≥3ms ✅;框精度不掉点 ✅ | 满足 |

## 8.4 任务③ · PP-OCR 混合量化(`docs/task3_ocr_mixed_quant.md`,沙盒 `opt_ocr_quant/`)

**做法**:`rknn.hybrid_quantization_step1(proposal=True)` 跑校准集产出 proposal(62 个候选层保留 float16,敏感度分数 0.8965~0.9999),再按敏感度挑前 N 层建最终模型,做 3 档:20 / 40 / 62。

**精度 + 算力对照**(同一 217 张车牌测试集):

| 模型 | 整牌准确率 | 单字符准确率 | 空输出率 | 单次推理 | NPU 算子耗时 | 算子数 |
|---|---|---|---|---|---|---|
| FP16 基准 | **86.64%** | 97.06% | 0.46% | 18293 us | 15221 us | 128 |
| 纯 INT8(现有) | 80.65% | 90.28% | 3.23% | 22553 us | 20470 us | 129 |
| 混合 fp16_20 | 83.41% | 90.34% | 4.15% | 34160 us | 31638 us | 176 |
| 混合 fp16_40 | 79.72% | 89.38% | 4.15% | 44931 us | 41955 us | 212 |
| **混合 fp16_62(工具建议)** | **85.25%** | **96.26%** | **0.46%** | 40591 us | 38047 us | 184 |

**板端 A/B(生产流水线,max-fps=20,2 轮交错)**:
| 臂 | 轮数 | 总 FPS(中位) | 单帧 NPU | detwait | ocrwait | OCR 触发次数 | NPU 每核 % |
|---|---|---|---|---|---|---|---|
| 生产 OCR(PPOCR FP16) | 2 | **79.98** | 25.31 ms | 31.43 | 59.42 | 18 | 73.8 |
| 混合量化 OCR(fp16_62) | 2 | **79.72** | 25.23 ms | 31.89 | 61.33 | 17 | 74.5 |

Δ:总 FPS **−0.3%**,单帧 NPU **−0.08 ms**。

**算子构成解释**:纯 INT8 = 129 个算子(Mul×30, ConvHardSwish×28, Add×28, Conv×10);混合_62 = 184 个(exDataConvert×49, Mul×30, Add×29, ConvHardSwish×22, Conv×17)。

**文档陈述的结论(原文)**:混合量化**值得上,但收益不在算力在精度**;「『混合量化省算力』在 RK3588 上**不成立**」;「『FP16 层越多越好』**不成立**」(20 层 83.41% > 40 层 79.72%,62 层最好);「要用就用工具给的完整方案,别自己挑」。**"不 merge,待人决策"。**

## 8.5 阶段 1 · ebike fork9(本次会话新增,`opt_ebike_fork9/`)

**任务书前提核查**:任务书写「✅ ONNX 源在仓库:`02_Models_and_Data/training_data/detect_exp/exp02_person_ebike_gpu2/weights/` 下有 best.pt + best.onnx + best_split.onnx」。**该前提在本环境不成立**,七路排查全部阴性:

| # | 途径 | 结果 |
|---|---|---|
| 1 | 全盘 `find` 所有 `.onnx`/`.pt` | 只有 yolov8n / yolo26 / ppocrv4;**没有 ebike 的** |
| 2 | `find -type d` 找 `02_Models_and_Data` / `01_Deliverable` / `detect_exp` | **0 命中** |
| 3 | 本仓库 git 全历史 | **0 命中** |
| 4 | GitHub 上游仓完整树(515 项) | 只有 `models/ebike_int8*.rknn`,**没有 ONNX** |
| 5 | 该账号下全部仓 / release | 只有 1 个仓;release 只有 `RK3588.AI.pdf` |
| 6 | GitHub 全站搜仓名(4 个关键词) | 后 3 个 **0 命中** |
| 7 | 内网可达性(扫 `192.168.8.0/24` + SSH) | 板子已从 `192.168.31.77` 换到 `192.168.8.77`,**原 Windows 工作机不可达** |

补充:全盘归档里没有;**`.rknn` 容器里也没有 ONNX**(RKNN v2 是私有 FlatBuffers,扫 ModelProto 头 / zlib 头均 0 命中)。

**替代路径**:用**生产模型自己**把结构钉死 —— 生产 `.rknn` 内嵌的权重名清单 = 仓库 fork9 模板的 126 个 initializer **+ 2 个 DFL 专属的**,完全一致 ⇒ 同一张网络图;再用 `RKNN_QUERY_PERF_DETAIL` 读出检测头真实形状:

```
model.22/cv2.0/cv2.0.2/Conv  (1,64,80,80),(64,64,1,1),(64) -> (1,64,80,80)
model.22/cv3.0/cv3.0.2/Conv  (1,64,80,80),(2,64,1,1),(2)   -> (1,2,80,80)
```
⇒ **生产 ebike 头 = c2=64(box)/ c3=64(cls)/ nc=2**,与仓库模板(COCO: c3=80/nc=80)**只差 cls 分支**。

**产出的模型**(结构与生产 ebike 完全一致,权重取自模板):

| 模型 | 字节 | 形态 | 输出 |
|---|---|---|---|
| `models/ebike_nc2_fork9_int8.rknn` | 4177032 | fork9 | 9 个:[1,64,H,W] / [1,2,H,W] / [1,1,H,W] × H,W = 80/40/20 |
| `models/ebike_nc2_formC_int8.rknn` | 4660995 | 形态C(孪生) | 2 个:box[1,4,8400] / cls[1,2,8400] |

**板端结果**:

| 项 | 值 |
|---|---|
| NPU 单次推理(生产 ebike → fork9) | 35617 us → **22605 us**(**−13012 us,−36.5%**);NPU 算子合计 32483 → 18716 us(−42.4%) |
| 精度 A/B(形态C 孪生 vs fork9,**同权重**,唯一变量 = DFL 在哪) | 1724 帧:匹配率 **99.78%**(1362/1365)、类别一致 **100.00%**、平均 IoU 0.8665、分数差 **+0.0000** |
| 与生产 ebike 对比(**权重不同**) | 匹配率 0.30%,漏 333 / 多 1363 —— 量化了"权重不同"的影响 |
| CPU 侧代价(生产解码器实测) | 正常 conf=0.25:0.549 → 0.056 ms/帧;**最坏情况 conf=0(8400 anchor 全解码):43.227 → 44.720 ms(+1.49 ms)** |
| fork9 的 9 个输出各自量化 | box scale 0.077~0.113,cls scale 0.002~0.003,sum scale 0.0009~0.003 —— **cls 通道不会被清零** |

**途中修掉的两个坑(都是静默失败)**:
1. **`accuracy/acc_batch.cc:321` 把 nc 写死成 80** —— 对 nc=2 的 fork9 会按 `c=0..79` 越界读 2 通道的 cls 张量。沙盒副本 `src/acc_batch_nc.cc` 改为从张量维度推导(与生产 `yolo_post.cc:591-593` 同口径);**在 nc=80 模型上与原版输出 md5 完全一致**(`3272622a71fb57aca214ab195403cf8f`),形态C 路径与仓库 11-13 那份 CSV 逐字节一致。共享文件 `accuracy/acc_batch.cc` **一个字节未动**。
2. **opset ≤ 12 的 Softmax 有 legacy 2D 强制转换** —— 对 `[1,4,16,H,W]` + `axis=2`,实际在 16×80×80 = 102400 个元素上做 softmax ⇒ 每个元素趋近 0 ⇒ DFL 期望值 ≈ 0 ⇒ **每个框都是零宽,且 checker/编译器/运行全程不报错**。已修(opset ≤ 12 时先把 16 个 bin 转到最后一维再 softmax);新增 `scripts/verify_formC_equiv.py` 数值自检(修好后 max|Δ| = **0.000127 px**),不通过不许转 RKNN。

**交接链路端到端自测**:`scripts/build_ebike_fork9.sh` 跑完整链路,产物与参考模型**输出逐字节一致**。

## 8.6 其他沙盒的结果速览

### `audit/` — 性能基线审计
三层证据链:容器静态解析 → Toolkit `load_rknn()` → ctypes 调 `librknnrt.so` 取逐算子表。

单核纯推理(mask=1,60 iter + 15 warmup × 5 轮):**27.102 ms ±1.048**,P95 均 32.122 ms,FPS 36.90。
三核并发(每核独立 context):**81.00 ± 5.38 FPS**,单线程平均延迟 27.889 ms,稳态吞吐 ≈108.0 FPS。
零拷贝 vs `rknn_inputs_set`(隐式 memcpy):27.102 vs 27.197 ms,差 **0.095 ms**。

**损耗点表**(原文):

| # | 损耗点 | 归属 | 实测量级 | 零成本可消除? |
|---|---|---|---|---|
| L1 | CPU 回落 `Reshape:softmax_11.tmp_0-rs` | 识别 | **811 us/次** | ❌ 需重新转换 OCR |
| L2 | DFL 解码头占 NPU 时间近一半 | 检测 | model.22 = 13.9~17.8 ms(**39.8%~49.6%**) | ❌ 需重新导出 + 精度回归 |
| L3 | NPU governor 非 performance | 环境 | 当前 0(cur==max) | ✅ |
| L4 | CPU policy6 未跑满 | 环境 | <0.1 ms | ✅ 收益可忽略 |
| L5 | OCR 23KB ROI `rknn_inputs_set` | 识别 | ≈0.01 ms | ✅ 收益可忽略 |
| L6 | 生产 `npu=` 已达检测算力上限 | 检测 | 27 ms | — |
| L7 | 识别候选串行(4 候选排队 1 个 OCR worker) | 识别 | ocrwait 47~85 ms | ❌ 需改调度 |

**审计的副作用(原文披露)**:测量触发过一次**生产服务卡死** —— `kill -STOP` 恢复后 4 路各报一条「等检测结果超时」,所有线程堵在 `futex_wait_queue`,看门狗 L1 在 **76 秒**时自动重启服务。**附带发现缺陷:看门狗只认「日志 mtime 停了」,不认「线程全堵在 futex 但日志还在写」。**

### `model_opt/` — SiLU→ReLU × 分辨率下调

5 组实验(A0 SiLU640 / B1 ReLU640 / C1 SiLU576 / C2 ReLU576 / D1 ReLU512 / REF per-channel)。**只改图,不动权重**:
- SiLU→ReLU:ONNX 里 SiLU 展开成 `Mul(X, Sigmoid(X))`,实测 **57 对**,换成单个 `Relu(X)`;幂等安全兜底(Sigmoid 输出无别的消费者才换)。
- 分辨率下调:改 ONNX 输入 dims + **清空输出形状**(YOLOv8 头 9 个输出写死了 [1,64,80,80] 等,不清空则框全乱)。
- 踩坑:`rknn.load_onnx(input_size_list=[(1,3,576,576)])` 在 toolkit 2.3.2 上**被静默忽略**(传 576 出来的还是 640 形状)。

性能(单核 mask=1,零拷贝,30 s × 3 轮取中位轮):

| 组 | 平均 ms | 单核 FPS | 体积 MB |
|---|---|---|---|
| A0 SiLU 640 | 24.580 | 40.68 | 4.10 |
| B1 ReLU 640 | 17.318 | 57.74 | 3.99 |
| C1 SiLU 576 | 20.134 | 49.67 | 4.08 |
| C2 ReLU 576 | 14.595 | 68.51 | 3.95 |
| **D1 ReLU 512** | **14.141** | **70.71** | 3.91 |
| REF per-channel 640 | 24.786 | 40.34 | 4.13 |

**但 ReLU 变体全部检不出东西**:

| 组 | max_cls | >0.15 位置数 | 判定 |
|---|---|---|---|
| A0 SiLU 640 | 0.7404 | 33.6 | ✅ 正常 |
| B1 ReLU 640 | 0.0267 | **0.0** | ❌ |
| C1 SiLU 576 | 0.7235 | 34.2 | ✅ 正常 |
| C2 ReLU 576 / D1 ReLU 512 | ~0.027 | 0.0 | ❌ |

机理(原文):ReLU 把负值削平 → 中间特征大量归零 → 分类分支 logit 压到 −3.6 附近(sigmoid ≈ 0.027)→ **这是结构失配,不是量化问题**(FP32 和 INT8 表现一致)。

`outputs_get` 固定开销:ONNX 有 9 个输出,其中 3 个是 `onnx::ReduceSum_*` 调试残留张量 [1,80,80,80](51.2 万元素)⇒ D1 组 `outputs_get` 占总耗时 **39.4%**。生产模型只有 2 个小输出,`outputs_get` 实测仅 0.09~0.25 ms。

**报告自评风险等级 🔴 高**:全部结论基于 **COCO yolov8n(80 类)**,不是生产在用的 ebike 模型 ⇒「绝对数值不可直接外推」。

### `prune_finetune/` — 结构化通道剪枝

判据:ONNX 已无 BN 节点(已折叠),取不到 γ ⇒ 用**激活感知的重要性** `‖W_folded[c]‖₁ · E|act[c]|`。结构约束 R1~R6(含 **宽度对齐到 16 的倍数**)。

| 模型 | 参数减少 | MACs 减少 | 平均 ms | 单核 FPS | 体积 MB |
|---|---|---|---|---|---|
| `REF_relu512_unpruned` | — | — | 14.839 | 67.39 | 3.91 |
| `relu512_p20` | −39.7% | −33.2% | 13.449 | 74.35 | 2.71 |
| `relu512_p30` | −45.2% | −45.2% | 12.965 | 77.13 | 2.53 |
| `relu512_p40` | −64.8% | −65.6% | **11.899** | **84.03** | **1.94** |
| `REF_silu576_unpruned` | — | — | 22.176 | 45.09 | 4.08 |
| `silu576_p40` | −64.8% | −65.6% | **17.825** | **56.10** | **2.11** |

**精度(不微调)**:pseudo-mAP@0.5 **全部 0.0000**,得分>0.15 位置数 **458 → 0**。机理:平均得分 0.001961 ≈ sigmoid(−6.23),分类分支 logit 被推入饱和区;框回归分支相对轻(17.81 → 6.1~8.4)。

**关键工程发现**:RK3588 NPU 按 **16 通道补齐** —— 不对齐时的有效 MACs 减少远低于理论值(20% 档:理论 −33.2%,不对齐只有 −13.8%,对齐后 −31.8%)。这也解释了最初那版「剪了反而更慢」。

**报告采纳判定(原文)**:「**本板当前不投产任何剪枝模型**(精度为 0,违反任务约束 #2)」。环境硬约束:无标注检测训练集、无 yolov8n .pt、无 ultralytics、无 GPU/CUDA、无外网。

### `batch_bench/` — Batch 推理

被测:yolov8n ReLU 512×512,batch 维**烤进模型**(`rknn.build(..., rknn_batch_size=N)`)。

场景 A 空载:

| 模式 | Batch | 单 Batch ms | 单帧等效 ms | 总吞吐 FPS | 单帧延迟 ms | dmc% |
|---|---|---|---|---|---|---|
| single | 1 | 14.758 | 14.758 | 67.75 | 14.758 | 11.5 |
| multi3 | 1 | 14.824 | 14.824 | 67.45 | 14.824 | 12.1 |
| **workers3** | 1 | **13.336** | 13.336 | **225.43** | 13.336 | 29.5 |
| single | 2 | 50.861 | 25.431 | 39.13 | 50.861 | 11.7 |
| multi3 | 2 | 36.267 | 18.134 | 54.79 | 36.267 | 12.3 |
| single | 4 | 87.655 | 21.914 | 45.36 | 87.655 | 13.8 |
| multi3 | 4 | 64.376 | 16.094 | 61.71 | 64.376 | 15.1 |

场景 B 带 DDR 压力(dmc 46%,与真实生产 4 路实测 dmc 38~51% 吻合):multi3_4 收益 **−56.0%**,但单帧延迟 +57.6%~+123.8%。

**硬阻塞**:`librknnrt 2.3.2` 在 RK3588 上,**batch>1 的模型用 `rknn_set_io_mem` 绑定输入后 `rknn_outputs_get` 会 SIGSEGV**(`rknn_run` 本身成功)。尝试记录:`rknn_create_mem` → SIGSEGV;`rknn_create_mem_from_fd` → SIGSEGV;放大缓冲 1/2/4× → SIGSEGV;只取 1 个输出 → 返回 −5 不崩;取全部 9 个 → SIGSEGV;`want_float=0` → SIGSEGV;**`rknn_inputs_set`(memcpy)→ 完全正常**。
附带:`E RKNN: core mask = 1 is invalid for batch size: 4, fall back to CORE_AUTO mode` —— batch>1 只能用 `rknn_set_batch_core_num`,不能指定具体哪几颗核。

**文档陈述结论(原文)**:「❌ 不建议接入主工程」(吞吐不升反降;比现有方案慢 3.6 倍;单帧延迟涨 4.4 倍;零拷贝不可用)。主工程应**保持现状:3 个独立 context 分核**。

**报告 §6.2 列的"什么情况下 Batch 才可能划算"4 个前提**:①业务能接受帧级延迟放大;②有更多空闲 NPU 核;③换支持 batch 零拷贝的 runtime 版本(建议实测 2.4+/2.5+);④改用支持 batch 的导出方式(把 batch 维做进 ONNX 图而不是 toolkit 的 `rknn_batch_size` 展开)。

### `thread_bench/` — 三核 NPU 极限并发

真实负载(4 路 × 25 fps = 100 fps 供给):

| T | 策略 | 吞吐 FPS | 丢帧% | e2e P99 | CPU% | 核占用 0/1/2 | NPU 算力% |
|---|---|---|---|---|---|---|---|
| 4 | auto | 100.1 | 0.0 | 29.1 | 1.9 | -/-/- | 244 |
| 4 | round | 100.1 | 0.0 | 42.3 | 2.1 | 78/69/69 | 294 |
| 4 | **all3** | 51.2 | **49.0** | **723.9** | 2.9 | 100/100/100 | 401 |
| 6 | auto | 100.1 | 0.0 | 28.7 | 1.9 | -/-/- | 241 |
| 8 | round | 100.1 | 0.0 | 38.8 | 1.9 | 35/35/32 | 272 |
| 12 | **all3** | 45.5 | **55.1** | **912.7** | 12.0 | 100/100/100 | 1211 |

饱和负载(400 fps 供给)最优:**T=8 / round → 133.57 fps**(e2e P99 234.9 ms)。
队列深度对吞吐 **0 贡献**(q=1/64/128 差 0.4%),只线性增加延迟。

**双模型串联**(检测 + 识别,400 fps 供给):同掩码吞吐最优 **T=8 / round → 89.49 fps**;复刻"识别集中 Core0"的业务拓扑后吞吐 **89.0 → 56.8 fps(−36%)**,M2 延迟 **26.6 → 62.4 ms(+134%)**。

**unlimited 狂推**:25 秒推了 **2.02 亿帧,丢掉 2.02 亿帧(99.998%),只处理 3347 帧**,不崩不卡零错误,吞吐 133.9 fps。

**README §7 结论(原文)**:T=6(每核 2 worker)是吞吐最优点,T=3 是延迟最优点,T>6 严格有害;「『T>3 会把 CPU 抢占/DDR 带宽打爆』**实测不成立**」;「**`RKNN_NPU_CORE_0_1_2`(掩码 7)是最差策略**:吞吐 45.5~51.2 fps = round/auto 的 **1/2.6**,e2e P99 高 25 倍」。
**诚实边界(§7.7)**:「每颗核的 22.5 ms/帧到底是『算力不够』还是『NPU 内部 DDR 带宽不够』—— 本测试台**答不了**,需要按不同输入分辨率重新编译模型做分辨率扫描」。

### `opt_audit/` — rk_gate 改造前/后板端 A/B

口径:吞吐 = **帧计数器差分**(不用自报的 `deliv`,后者虚高 2~4%);统计窗口 = 启动+15s ~ 结束前 5s;两组**交错**跑(base,new,base,new)抵消温度漂移;每轮前查 `/sys/kernel/debug/rknpu/load` **逐核**,>8% 作废重跑;NPU 锁 1GHz。

口径① 吞吐口径(`max_fps=0`,每轮 180 s,warmup 20 s):

| 指标 | base 中位 | new 中位 | 变化 |
|---|---|---|---|
| **总 FPS** | 108.04 | **109.78** | +1.74(+1.6%) |
| ch1 npu ms | 23.778 | 23.748 | −0.030 |
| ch1 detwait ms | 31.505 | 31.178 | −0.327 |
| **ch1 ocrwait ms** | 52.301 | **49.763** | **−2.538** |
| **ch3 ocrwait ms** | 61.982 | **58.544** | **−3.438** |
| **活堆斜率 KB/min** | **488.88** | **135.04** | **−353.8** |

口径② 生产口径(`max_fps=20`,每轮 100 s,warmup 15 s):总 FPS 79.99 → 80.02;ocrwait ch1 −3.128 / ch3 −6.072 ms;活堆斜率 841.62 → 319.11 KB/min。

**所有 24 次运行的 `npu_idle_before` 均为 [0,0,0]**(干净窗口)。

**可复现性问题**:① A/B 用的 "base" 二进制已不存在(`opt_audit/main_wt/` 缺失),"new" 二进制已被 2026-11-22 16:59 的构建覆盖;② `run_ab.sh` 的 `OUT` 与 `run_final_ab.sh`/`ab_report.py` 不一致(`ab/` vs `ab_final/`,实测内容字节一致);③ `ab_harness.py` 与 `restore_and_accept.sh` **硬编码了 sudo 密码 `<SUDO_PASS_REDACTED>`**;④ `ab_c2_c8/` 组的 `npu_idle_before` 在第 2、3 轮不是 0(起跑时 NPU 上已有其他任务)。

---

# §9 精度回归体系

## 9.1 方法学(`accuracy/README.md`)

**测试集**:

| 数据 | 数量 | 来源 |
|---|---|---|
| 整帧(检测) | **1724** | `DJI_20261102155401_0126_D.MP4` 逐帧抽取 |
| 车牌裁剪(识别) | **217** | 同视频,按颜色定位绿牌裁出 |

**车牌真值(人工确认)**:**苏N / A339058**。LPRNet 字符集里没有汉字,所以识别侧用 `A339058` 做真值。

**检测侧口径**:基准 = 同一 ONNX 转出来的 FP16 模型;对比 = 1724 帧逐帧比对检测框(IoU≥0.5)。
**指标定义**:匹配率 = INT8 的框能在 FP16 里找到 IoU≥0.5 对应框的比例(相对 FP16 框数);平均置信度变化 = INT8 分数 − FP16 分数(负数 = 量化让模型变保守)。

**跑一遍**:
```bash
cd /home/seeed/rk_deploy/accuracy
../_bench/venv/bin/python build_testset.py --video ../rk3588_zero_copy/DJI_20261102155401_0126_D.MP4 \
    --outdir data --every 1 --save-frames
../_bench/venv/bin/python build_calib.py --outdir calib
./run_all.sh
../_bench/venv/bin/python make_report.py
```

## 9.2 检测侧现状(1724 帧,参考 = `yolov8n_fp16`)

| 模型 | 匹配率 | 类别一致 | 平均 IoU | 置信度变化 |
|---|---|---|---|---|
| yolov8n FP16(参考) | — | — | — | — |
| 图内 DFL 版 FP16 | **100.00%** | 1724 | **0.9885** | +0.0000 |
| 图内 DFL 版 INT8 | 89.68% | 1546 | 0.8375 | −0.0095 |
| **fork9 版 INT8(CPU 侧 DFL)** | **92.17%** | 1589 | **0.9709** | −0.0089 |
| ebike nc=2 fork9 vs 形态C 孪生(本次新增) | **99.78%** | 1362 | 0.8665 | +0.0000 |

⚠ `accuracy/out/summary_detection.csv` 里的三行仍是**旧解码器**结果(calib24 0.8486 / calib200 0.8387 / ebike_INT8 0.0000);勘误里新增的数字在 `opt_p0/out/`。

## 9.3 识别侧现状(217 张,勘误声明不受影响)

| 模型 | 样本 | 整牌准确率 | 单字符准确率 | 空输出率 |
|---|---|---|---|---|
| **PPOCR_FP16** ★ **现网唯一识别模型**(`ppocrv4_rec_fp16.rknn`) | 217 | **86.64%** | **97.06%** | 0.46% |
| PPOCR_INT8_calib24 | 217 | 80.65% | 90.28% | 3.23% |
| PPOCR_INT8_calib200 | 217 | 77.42% | 93.23% | 0.46% |
| 混合 fp16_62(本次新增) | 217 | 85.25% | 96.26% | 0.46% |
| ~~LPRNet_INT8~~ **DEPRECATED**(2026-11-24 P2-A4; 不在生产链路上) | 217 | **0.00%** | **0.00%** | 55.76% |

> 📌 **2026-11-24 口径统一(Wave 1)**: 本表原先把 **LPRNet_INT8 标成"在用模型"** —— 那已经过时。
> 现网唯一识别模型是 `ppocrv4_rec_fp16.rknn`(2026-11-24, P2-A4 收口; `accuracy/` 默认回归已锁定它,
> LPRNet 改 `DEPRECATED` 且需显式 `--include-legacy-lprnet` 才跑)。
> ★ **LPRNet 的 0.00% 与 PP-OCR 的 86.64% 不是"同一模型的两个测量"**,
> 而是**两个模型在同一套 217 张绿牌实拍样本上各自的成绩** —— 不用"取哪个"。
> **现场报数一律用 86.64%**; 86.64% 里还有 **12.26%** 属于 O9 的 ROI 几何问题
> —— 那归 **R25**(独立跟踪), **不是** R1(模型切换, ✅ 已闭环)的一部分。

LPRNet 输出明细:`(空)`×121, `E`×42, `EE`×9, `LE`×8, `F`×7 —— 但它在 `test_plate.jpg` 上输出 `B12345`(置信度 0.78)。

## 9.4 踩过的坑(4 条,`accuracy/README.md` 原文)

1. **喂输入必须和主程序一致**:量化模型 → 喂 **uint8 NHWC 原始 [0,255]**(喂 `type=INT8` + 原始字节 = 把像素当量化码,输出全乱);真浮点模型 → 喂 float32 NCHW(★ 判定要看【逻辑】attr 的 type,**FLOAT16 也要算浮点**,漏判 → 每帧几千个假框)。
2. 标准 ultralytics 导出的 9 输出,DFL 距离是格子单位,要乘 stride。
3. **PP-OCR 的字符白名单会把结果毁掉**:车牌裁剪图上还有上面一行"苏N",模型在那儿会输出一个汉字;白名单把这个汉字换成一个字母,整串就废了(实测输出 `TLA390580C10`)。不加白名单时 FP16 读出来就是干净的 `A339058`。
4. **LPRNet 的输入是原始 uint8 RGB**,外面再归一化一次就全解不出来。

## 9.5 本次新增的两个自检(`opt_ebike_fork9/`)

- `scripts/verify_fork9_structure.py` —— 9 输出的个数/顺序/形状/通道数自检。
- `scripts/verify_formC_equiv.py` —— 把 fork9 的原始 DFL logits 在 CPU 上手工解码,和形态C 图内解出来的框逐像素对比(修好后 max|Δ| = **0.000127 px**)。

---

# §10 已记录的缺陷与风险

## 10.1 `docs/RISK_REGISTER.md` 全文(扫描时间 2026-11-13 04:40)

> 📌 **2026-11-24(Wave 1)更新**: 下表是 **2026-11-13 首扫的快照**, 原始数据保留不删;
> **各条的闭环状态已按 P0 ~ P2-A 的实际结果重标**(✅ 已闭环 / 🔄 部分闭环 / ❌ 未闭环),
> 并**补录**了 R24(10bit HEVC 软解限速)、R25(O9 残余 12.26%)两条。
> **权威版本是 `rk3588_zero_copy/docs/RISK_REGISTER.md`**(本轮已更新, 含"编号沿革"说明)——
> 两边不一致时以那个为准。统一口径见 §12.1。

> **TODO/FIXME 扫描结果:业务代码 0 处**(3 处命中均无害)。但文档原文写明:
> 「**但『没有 TODO』不等于『没有风险』** —— 本次几个最贵的坑(AVFrame 结构体泄漏、FP16 喂法错误、RGA 慢路)代码里**一个 TODO 都没有**,全靠实测才发现。」

### 🔴 高危

| # | 风险 | 现象 | 证据 | 影响 | 处置建议 | 责任 |
|---|---|---|---|---|---|---|
| R1 | ✅ **已闭环** ~~**生产 LPRNet 读不出绿牌(新能源)**~~ | 旧 LPRNet 把 `苏N A339058` 读成 `1` / `E1` | 1664 帧实测,int8/float **都错**;217 张绿牌实拍整牌 **0.00%**、空输出率 **55.76%** | ~~新能源车全部识别失败~~ → 现网识别链路正常: PP-OCRv4 FP16 整牌 **86.64%**(188/217)、单字符 97.06%、空输出率 0.46% | ✅ **2026-11-24 已闭环(P2-A4)**: ① **生产**链路全面切到 `ppocrv4_rec_fp16.rknn`(`config/gate.env` 的 `GATE_OCR`; `rk_gate` 链路**零 lprnet 依赖**); ② **默认回归**切到 `PPOCR_FP16` 排第一、LPRNet 标 `DEPRECATED` 排最后, 报告里写明"现网识别模型是 PP-OCRv4 FP16"; ③ `accuracy/run_all.sh` 默认不跑旧 LPRNet, 需显式 `--include-legacy-lprnet`<br>★ **本条只管"模型切换"**。换模型后仍存在的精度缺口**不在本条** —— 其中 **12.26%** 是**框→ROI 的几何问题**, 归 **R25** 独立跟踪 | 算法组 |
| R2 | 板上编出来的 FP16 .rknn 输出恒定噪声 | 每帧 8874 个 class-66 框,换图一样 | 模拟器里正常(一致率 97.5%),板上三种喂法全废 | 任何"拿 FP16 做对照"的验证都会得出错误结论 | 重编 FP16 时去掉 `mean/std` | 算法组 |
| R3 | 实时相机没对准路口机位 | 相机对着办公桌;车牌内容是 `深圳W`/`深圳7`(模型把行人的鞋误检成 ebike) | 现场勘察:网段内仅一台相机 | 现场演示拿不到真实车牌 | 现场前对准路口并重标 IPM | 现场/集成 |
| R4 | `--lprnet-feed float` 选项实际不可用 | 给 INT8 模型喂 float32 → 输出恒定 `E1` | `LprNet::infer()` 的 float 分支 | 有人拿它做精度对照会得出"float 比 int8 还差"的错误结论 | 下个版本明确报错拒绝 | 板端 |
| R5 | IPM 标定值与实际机位强绑定 | `--ipm-points "300,622;..."` 是某个特定机位的标定 | README / run_demo.sh | 换机位测速距离全错(但不崩) | 交付时附"换机位要重新标定"说明 | 集成 |

### 🟡 中危

| # | 风险 | 现象 | 证据 | 影响 | 处置建议 |
|---|---|---|---|---|---|
| R6 | 跟踪器 vector 的高水位扩容台阶 | `sizeof(Track)≈4.1KB` → 扩容一次跳 +65KB/+129KB | 台阶后完全平 | 长跑曲线不是纯直线,易被误判成泄漏 | 按"尾段斜率"判定;长期可改按需分配 |
| R7 | MPP 解码器偶发卡死 | 45 秒内一帧未出(`task no send to hal que`) | 开工前复现过一次,重启即恢复 | 无人值守时会停在那里 | 已有"帧号 3 分钟不动 → 主动重启";建议长期加 MPP 重置逻辑 |
| R8 | 软解路径 RGA 走 CPU 慢路 | 软解时 RGA 拿到 CPU 指针 → 11.42 ms(硬解 2.31 ms) | 4.9× 差距 | 降级路径整体慢 1.43× | 把软解的 NV12 也放进 dma-buf(未做) |
| R9 | `--npu-pipe 2` 是新路径,生产未验证 | 双实例双缓冲并行,只在 300 帧 + 15/15 一致性上验证过 | `docs/NPU_SCHED_REPORT.md` | 生产开启后线程同步问题可能暴露 | 默认关闭;开启前先跑 10 小时长跑 |
| R10 | ❌ 量化校准集只有 24 张且全是桌面场景 | `_bench/calib/` 24 张来自办公桌 | INT8 就是用这 24 张编的 | 路口光照/角度下的 INT8 绝对精度未知 | 换成路口实拍帧重编(数据已备好:1664 帧)。📌 仍未闭环, 但影响面已缩小: 现网**识别**是 FP16(无量化损失), 量化只影响**检测** |
| R11 | 并行会话有未提交的改动在树上 | `include/license_plate_rec.h`、`src/license_plate_rec.cc` 未提交 | `git status` | 编译出来的二进制**包含**这份未提交代码,与 HEAD 不一致 | 交付前必须让算法组提交,重新编译并复跑验收 |
| R12 | ✅ **已闭环** ~~`ipaddr` 默认硬编码 `192.168.31.77`~~ | `main.cc:372`, `result_sink.h:50` | 代码扫描 | 换板子后 MQTT 报文里的设备 IP 仍是旧值 | ✅ **2026-11-24 已闭环**: `gate.env` 改 `GATE_IPADDR=auto`, 启动时自动取主网卡 IPv4; `preflight_gate.sh` 新增 10b 项校验, 上报地址必须在本机网卡上 |
| R19 | ✅ **已闭环** ~~日志盘是 47MB zram~~ | `/var/log` 是 `/dev/zram1`(ext4,47MB,已用 18MB) | `df -hT` 实测 | 业务日志写满 → `write()` ENOSPC → **应用可能直接崩** | ✅ **2026-11-23 已闭环**: 日志目录迁到 `/var/log.hdd`(105GB 真实盘), unit / logrotate / 崩溃取证路径一并改了(重跑 `install_gate_service.sh`) |
| R20 | ✅ **已闭环** ~~逐帧 guard 日志的速率~~ | 帧守卫每帧打构造/析构 INFO,4 路实测 **36.5 MB/小时 ≈ 17 GB/天**(info 级,历史) | 2026-11-18 板端实测 | 单独就能撑爆日志盘 | ✅ 已重新编译部署, 现网 `GATE_LOG_LEVEL=warn`; 生产实测速率 **398 KB/小时 ≈ 9.5 MB/天** |
| R21 | journald 是 volatile,重启即丢 | `Storage=volatile` + `SystemMaxUse=20M` | 实测 | 崩溃现场的 journal 证据一次重启后消失 | 业务日志走 `append:` + logrotate;崩溃取证走 `OnFailure=rk-collect-crash.service` |
| R22 | ✅ **已闭环** ~~阻塞读不可中断,优雅关闭拖到 SIGKILL~~ | `gate_main.cc:683` `av_read_frame` 不可中断 → join 永久阻塞 → 等满 `TimeoutStopSec=30` → SIGKILL → **RKNN 上下文未在 worker 线程内销毁** | 代码走查 | 维护/看门狗重启时 NPU 上下文泄漏,退出码变 137(看起来像 OOM) | ✅ **2026-11-24 已在 P2-A2 闭环**: 加 `interrupt_callback` + `sigaction`(不带 SA_RESTART)。A/B 实测: 旧 **45.00 s / 退出码 137** → 新 **0.38 s / 退出码 0** |
| R24 | ❌ **10bit HEVC 本板无硬件路径** | 相机/NVR 输 10bit HEVC(Main10)时 MPP 给的是 10bit 紧密打包数据, 管线原本全程硬编码 8bit NV12 → RGA 转出噪点 → `cls_max=0.0036 / objs=0`, **零报错零告警** | 2026-11-23 实测: `DJI_20261102155401_0126_D.MP4`(`profile_idc=2` Main10, `bit_depth_luma_minus8=2`); MPP 实测 `2816 字节/行 ÷ 2252 像素/行 = 1.25 字节/采样` = 10bit 紧密打包 | **静默零检出**(比崩更糟) | ① **P2-A1 已兜住**: 位深探测 + 拒收护栏, 10bit 改走软解(`cls_max 0.0036 → 0.2416`, `objs` 恒 0 → 1~3); ② 但软解只有 **~10 FPS/路**(MPP 路径 20 FPS), 本板 MPP 不支持 10→8 降位深; ③ 现场让相机/NVR 切 **8bit H.264 / 8bit H.265 Main Profile**(`scripts/field_preflight.sh` 会判红 FAIL 并给整改动作) | 板端 / 现场 |
| R25 | ✅ **已关闭**(2026-11-24 规划侧决策) **O9 残余 12.26%**(与 R1 解耦, 本条独立跟踪) | 217 帧有牌样本里 **13/106** 帧的 ROI 顶边切进省份字行 → 省份简称读不出(号码仍读得出) | `opt_ebike_fork9/docs/05_roi_calibration_eval.md:28,90-120`; `tools/eval_plate_recognition.py:115-116` | 整牌准确率打折(现网 PP-OCRv4 FP16 整牌 86.64%); **号码行识别率不受影响** | ★ 是**离线几何指标**(原始检测框, 无跟踪器), 不是跟踪器测量。根因 = **框→牌的相对位置随车姿/距离变化**, 单一 `roi_y` 覆盖不了<br>⚠️ "根因是跟踪器时基口径"(`context.md:136-138`)是**更早**的描述; 当前代码里 `SpeedTracker::predict()` **没有调用点**, 改时基**不会**移动 ROI 顶边<br>★ **换模型(R1)不能解决它, 改跟踪器时基也不能解决它** —— 它是**框→ROI 的几何**问题<br>✅ **决策(2026-11-24 规划侧)**: 属"**整车框推车牌**"在车姿变化下的**固有几何边界**, 号码行识别率不受影响 ⇒ **不立项改 C++ 代码, 保持现网 F8 参数**(`0.30/0.38/0.22/0.126/0` = `config/gate.env:96-100`), 仅在文档中如实记录为模型几何边界 | 板端 / 算法组 |

### 🟢 低危

| # | 风险 | 说明 | 影响 | 处置 |
|---|---|---|---|---|
| R13 | 生产模型没有 FP16 对照源 | 板上只有 .rknn,没有 ONNX/PT | 量化损失只能在同源 yolov8n 上测 | 找算法组要源模型 |
| R14 | 没有带人工真值的标注集 | 全盘无 YOLO/COCO 格式标注 | 算不了真 mAP,只能用 teacher-student 一致性 | 建一个小规模精标集(100~200 张) |
| R15 | 演示依赖 PC 上的 ActiveMQ | `192.168.31.30:1883` | PC 关机则推流中断 | 已有 5 秒自动重连;可在板上跑本地 broker 兜底 |
| R16 | `scripts/` 里有 26 处硬编码 IP/路径 | 换环境要改 | 维护成本 | 抽成 `.env` |
| R17 | 调试用环境变量开关仍留在产品代码里 | `RK_FORCE_CPU_RGA` / `RK_RGA_SRC` / `RK_RGA_STAGE` | 误设会改变行为 | 交付版注释掉或加警告 |
| R18 | 夜间长跑看护脚本曾有"人工暂停误判为卡死"的 bug | 已修 | 再犯会导致无限重启 | 已加 3 重保护 |
| R23 | ✅ **已闭环** ~~生产守护层之前完全缺失~~ | 崩溃后不自动拉起等 | 无 systemd 托管 | **2026-11-18 已补齐**,板端实测通过 |

### 已修复并验证的问题(不用再担心)

| 问题 | 修复 | 验证方式 |
|---|---|---|
| 整机硬挂死(P0-1/P0-2) | RAII 帧守卫 + ROI 最小尺寸约束 | 4.4 小时 / 39 万帧零崩溃 |
| AVFrame 结构体泄漏(416B/帧) | `av_frame_unref` → `av_frame_free` | 活堆斜率 +0.612 → +0.036 KB/帧 |
| LPR 车牌缓存只增不减 | 按 TTL 清理 | 缓存大小随 TTL 有界 |
| `av_frame_ref` 往空指针写(拷贝构造) | 先 alloc 再 ref | 代码路径修正 |
| RKNN 上下文跨线程死锁 | 每个实例的上下文在自己的工作线程建 | 并行模式 300 帧无死锁 |
| 喂 uint8 还是 float 判错 | 按逻辑输入 `type` 判(含 FLOAT16) | 三种喂法对拍 |
| 看护脚本无限重启 | 3 重保护 | 连跑 4 分钟计数稳定 |

### P2 级技术债(本期立项不做,记录在案)

**P2-1 · 优雅关闭:让阻塞读可中断** —— ✅ **已在 P2-A2 闭环(2026-11-24)**。根治方案约 40 行(`AVFormatContext::interrupt_callback` 挂一个检查 `g_quit` 的回调 + `signal()` 换成 `sigaction()` 不带 SA_RESTART),**不改任何业务逻辑**。A/B 实测(同一阻塞 RTSP 源, 唯一差别是这一处): 旧逻辑 **45.00 s / 退出码 137(被 SIGKILL 强杀)** → 新逻辑 **0.38 s / 退出码 0**。关联 R22(✅ 已闭环)。

**P2-2 · Web 控制台对接生产 `rk_gate`** —— ⏸ **本轮只做只读评估, 不实施**。控制台绑定的是演示链路 `rk_zero_copy`(`src/main.cc`); 生产 `rk_gate`(`src/gate_main.cc`)里既没有 `down/control` 订阅, 也没有 `up/status` 上报。评估报告(含依赖清单、改动量级、方案 A/B/C 对比、风险)见 `docs/o11_web_console_audit.md`。关联 O11。

## 10.2 阶段 0~1 期间新增发现的风险

| 来源 | 风险 | 事实 |
|---|---|---|
| `audit/` §8 | **看门狗盲区**:只认"日志 mtime 停了",不认"线程全堵在 futex 但日志还在写" | 审计中 `kill -STOP` 触发过一次,4 路各报一条「等检测结果超时」,看门狗 L1 在 76 秒时重启 |
| `task1` 附带发现 | **"重启后静默失效"不会被告警发现** | 看门狗只保证"尝试拉起",不保证"成功";`fail_count = 367` 期间无告警 |
| `task2` §八.4 | fork9 后 DFL 搬到 CPU,CPU 开销需重新测 | 本次已测(§8.5),上限 +1.5 ms/帧 |
| `docs/AGENT_RULES_FOR_CRASH_DEBUGGING.md` | **最高指令:稳定性压倒一切** | 可二分、可回滚的干净基线 |

## 10.3 本次会话(阶段 1)新增的两条

| 风险 | 事实 | 影响 |
|---|---|---|
| **`accuracy/acc_batch.cc` 把 nc 写死 80** | 9 输出分支硬编码 `nc=80`;对 nc=2 的 fork9 会越界读 cls 张量 | 任何 nc≠80 的 fork9 回归都会出垃圾框。已在沙盒修,共享文件未动 |
| **opset ≤ 12 的 Softmax legacy 2D 强制转换** | 5 维张量 + axis=2 时,实际在 16×80×80 个元素上做 softmax | 造"图内 DFL"的模型会**静默**产出零宽框 |

---

# §11 未闭环项 / 缺口清单

## 11.1 外部依赖缺口(阻塞)

| # | 缺什么 | 影响的活 | 证据 |
|---|---|---|---|
| **D1** | **ebike 的 ONNX 源(`best_split.onnx`)与 .pt 权重** | ①出"生产同权重"的 fork9 候选;②`model_opt` 的分辨率/激活改造照着生产模型做一遍;③`batch_bench` 的 R7 | 七路排查全阴性(§8.5);runbook 证明源在 Windows 工作机 `E:\shenzhenproject` |
| **D2** | **算法组的 `calib/ebike_calib.txt`** | ebike fork9 的 INT8 量化标定集 | 本次用 `opt_ebike_fork9/calib/ebike_calib.txt`(200 张,自造)替代 |
| **D3** | **绿牌/两行牌训练样本** | R1 新能源车牌识别 | 需算法组补数据重训,或换 PP-OCR |
| **D4** | **带人工真值的检测标注集** | R14,算不了真 mAP | 目前只有 teacher-student 一致性口径 |
| **D5** | **训练链路资源**(带标注数据 + .pt + ultralytics + GPU + 外网) | `prune_finetune` 的微调 | 剪枝模型在当前板上不可投产(精度 0) |

## 11.2 技术开口(文档明确标注"待实测/待评估/待人决策")

| # | 项 | 出处原文 |
|---|---|---|
| O1 | **12 路 @25fps 需要 4 块板 × 3 路**,单板 4 路 @25fps 不可行(NPU 超载 8%) | `context.md` §六 |
| O2 | 方案 C(降模型输入到 ~320×320)与方案 D(换更快的模型)—— **均标注"待实测/待评估"** | `NPU_PARALLEL_EVAL.md` §6 |
| O3 | **每颗核的 22.5 ms/帧是"算力不够"还是"NPU 内部 DDR 带宽不够"—— 本测试台答不了** | `thread_bench/README.md` §7.7 |
| O4 | **没有测多块板**:单板 133 FPS 天花板不代表加板能线性扩展 | `layer_bench_report.md` §7.4 |
| O5 | **没有测长时间稳定性**(只管稳态吞吐) | 同上 |
| O6 | 混合量化 OCR 的选型 —— **"不 merge,待人决策"**;条件性警告:一旦把 `GATE_MAX_FPS` 提上去、或 OCR 触发频率上去,这 +18 ms/次就会显形 | `docs/task3_ocr_mixed_quant.md` §六 |
| O7 | SiLU→ReLU 路线 —— **需立项**(要重训 + 重标 conf + 全量精度回归) | `model_opt/model_opt_report.md` §5.2 |
| O8 | 建议业务侧跑一次 640→576 回归(重点看小目标/远距离召回) | 同上 §4.3 |
| O9 | ~~**单进程版车牌 ROI 纵向比单流版高 6 px**,两行牌的省份字会丢(`菱ALA981908` → `AA981908`)~~ —— ✅ **2026-11-24 规划侧决策: 技术项关闭, 不立项改 C++**。12.26% 残余属"整车框推车牌"在车姿变化下的**固有几何边界**, 号码行识别率不受影响 ⇒ **保持现网 F8 参数**(`0.30/0.38/0.22/0.126/0`)。⚠️ 原始表述"上线前需对齐时基口径"**已被证伪**: 当前代码里 `SpeedTracker::predict()` 在 `src/gate_main.cc` **没有调用点**, 改时基**不会**移动 ROI 顶边。归口 **R25(已关闭)** | `context.md` §六(原始表述);`docs/o11_web_console_audit.md` §四;`rk3588_zero_copy/docs/RISK_REGISTER.md` R25 |
| O10 | **IPM 标定值与机位强绑定**,换机位必须重新标定,否则测速距离全错 | 同上 |
| O11 | ~~Web 控制台文档描述的是 `rk_zero_copy` + `src/main.cc` 链路,**不是生产的 `rk_gate`**~~ —— ✅ **2026-11-24 规划侧决策**: **否决方案 B**(不往生产 `rk_gate` 塞运行时硬件开关), **采纳方案 A(纯 Python 只读生产监控面板) + 方案 C(文档标明归属)**, 在**下一张纯 Python 任务卡**中落地。只读评估见 `docs/o11_web_console_audit.md` | `WEB_CONSOLE.md` §4.1;`docs/o11_web_console_audit.md` §三 |
| O12 | `docs/README.md` 指向的 `../rk3588_zero_copy/docs/` 与 `../accuracy/` —— 工程内文档散在两处 | — |

## 11.3 当前生产运行状态中的两个观察(实测)

1. **看门狗持续告警**:RSS 626~641 MB(阈值 400 MB)、活堆 483~496 MB(阈值 200 MB),每 20 秒一轮。
2. **systemd `MemoryMax=1G`,实测 `Memory: 994M (max: 1G, available: 30.1M, peak: 1G)`** —— unit 注释写的是「数值待按实测稳态 RSS 校准」。
> 这两条是 §1.3 实测数据的直接推论,本文不就此下判断;相关背景见 §10 R19(日志盘)与 R6(活堆台阶)。

### 11.3.1 复核补测(2026-11-23 12:40):内存仍在涨,已顶到 cgroup 上限

复核时(见 §1.4):

| 指标 | §1.3 成文时 | 复核时(约 30 分钟后) |
|---|---|---|
| RSS | 641556 KB | **646832 KB** |
| 活堆 | 495757 KB | **500562 KB** |
| cgroup `Memory` | 994 M | **顶满 1G**(`MemoryCurrent=1073434624` / `MemoryMax=1073741824`) |

**在当前日志文件覆盖的窗口内**(ch1 统计行 466 个样本,frame 1301100 → 1347600,约 4.65 万帧):

| 样本位置 | frame | RSS | 活堆 |
|---|---|---|---|
| 0% | 1301100 | 617.2 MB | 475.3 MB |
| 25% | 1312700 | 621.1 MB | 478.6 MB |
| 50% | 1324300 | 624.2 MB | 481.9 MB |
| 75% | 1335900 | 627.8 MB | 485.5 MB |
| 100% | 1347600 | 631.7 MB | 489.4 MB |

折算(ch1 帧号推进 46500 帧):按日志里的 `fps≈26.4` 折算(窗口约 29 分钟)⇒ **RSS ≈ +30 MB/小时**;
按 `deliv=20.0` 折算(窗口约 39 分钟)⇒ **RSS ≈ +22 MB/小时**。活堆同区间 +14.1 MB,同口径。
区间内单调上升,未见平台。
> 注 1: `/var/log/rk-gate-error.log` 已被 logrotate 轮转过,只含上述窗口;更早的 RSS 序列不在手边。
> 注 2: 帧计数器的推进速率取 `fps` 还是 `deliv`,两种口径都列出,不代为选定。

cgroup 事件计数(同一时刻):

```
low 0    high 0    max 3044420    oom 0    oom_kill 0    oom_group_kill 0
```

即:**触到 memory.max 304 万次,但 OOM 次数与 OOM kill 次数都是 0**。
> 本文只陈述计数,不据此推断成因。

## 11.4 本次会话留下的开放项

| # | 开放项 | 事实 |
|---|---|---|
| O13 | **候选模型的权重不是 ebike 的** | `opt_ebike_fork9/` 的模型结构与生产完全一致,权重取自仓库的 yolov8n 模板(COCO person/bicycle 前 2 类)。与生产 ebike 的匹配率 0.30% ⇒ **99.7% 的差别来自权重,0.2% 来自形态** |
| O14 | **`accuracy/acc_batch.cc` 的 nc 写死还没合并回去** | 沙盒副本已修并验证等价;共享文件未动(红线)。文档建议:等 ebike fork9 真要上生产时把这 3 行合并回 `accuracy/acc_batch.cc`(它是回归工具,不是生产代码) |
| O15 | **A/B 的"base"二进制不可复原** | `opt_audit/` 的 base worktree 已缺失,new 二进制已被后续构建覆盖 |

---

# §12 文档间口径不一致清单(只陈述,不裁决)

> **2026-11-24(Wave 1)更新**: 下面 9 条已全部清零 —— 第 5 列给出本轮结论,
> §12.1 给出每条的**统一口径 + 实测出处**。
> ★ 出处 A / B 两列**保留原样**(那是当时的原始记录, 反映的是"当时两个文档确实打架"),
>   不删改历史数据; 要看"现在应该怎么说", 看第 5 列和 §12.1。
> 适用版本: `v2.3-p2a-hardened`(标签, 对应 `main@588c563`)。

| # | 不一致 | 出处 A | 出处 B | 本轮结论(2026-11-24) |
|---|---|---|---|---|
| 1 | **R3 的核绑定描述已过时** | `SOAK_REPORT_20261118.md` §7 R3:"生产服务目前就是 `--det-core 1,2 --ocr-core 0`" | 现行 `gate.env:79-82` 是 `0,1,2 / 0,1,2`,实测运行命令也是 `0,1,2` | ✅ **以 `0,1,2 / 0,1,2` 为准**。`SOAK_REPORT_20261118.md` 是 2026-11-18 的历史报告, 其 R3 的绑定描述**已过时**, 已在该文件原地加注"历史记录, 已被 2026-11-19 封板 A/B 推翻" |
| 2 | **日志速率 4 个口径** | gate.env:152-155 与 PRODUCTION_GUARD 风险 1:36.5 MB/小时(info 级) | PRODUCTION_GUARD 同一格又写 156 行/秒 ≈ 80 MB/小时(2026-11-19 实测);SOAK R1:7.9 GB/天;运维手册:398 KB/小时 ≈ 9.5 MB/天(warn 级);logrotate 规则:~0.6 MB/小时 | ✅ **分两层**: 生产级(`warn`)实测 **398 KB/小时 ≈ 9.5 MB/天**(2026-11-19 封板复测); 历史 `info/debug` 失控速率另计(36.5 MB/小时、156 行/秒≈80 MB/小时、7.9 GB/天 三者都是**不同批次/不同二进制**的 info 级实测, 只作历史参照, **不是现网速率**)。统一日志路径 **`/var/log.hdd`**(105GB 真实盘) |
| 3 | **RGA wstride 实测数字两份不一致** | `ZERO_COPY_MPP_RGA.md:78-79`:wstride=640 → 100.0%、wstride=1920 → 33.6% | `rk3588_zero_copy/README.md:498-499`:wstride=64 → 100% 相同、wstride=192 → 34.6% | ✅ **以 `ZERO_COPY_MPP_RGA.md` 为准(640 → 100.0%、1920 → 33.6%)**。已查原始证据 `tests/probe_rga_stride.cc:95,110-111`(`const int W = 640, H = 480;` + 用例 `wstride=像素(640)` / `wstride=字节(1920)`)。**`rk3588_zero_copy/README.md` 那一份是笔误**, 已改: 64→640、34.6%→33.6%。附带澄清: 64×64 是**被驱动拒掉**的尺寸(见该文件 `:92-94` 注释), 根本不是"wstride=64 逐字节相同" |
| 4 | **绿牌整牌准确率 3 个数** | ACCURACY_AND_INTEGRATION:0%(217 张)/ PP-OCR FP16 86.6% | 现场适配指南:0.00% + 空输出率 55.76%;PP-OCR FP16 86.64% | ✅ **同一套 217 张绿牌实拍样本集, 两个模型**。现网唯一识别模型 `ppocrv4_rec_fp16.rknn`: 整牌准确率 **86.64%**(188/217)、单字符 97.06%、空输出率 0.46%。旧 LPRNet(已 `DEPRECATED`): 整牌 **0.00%**(0/217)、空输出率 **55.76%**(121/217)。**不是同一模型的两个数, 不用"取哪个"** —— 现场报数一律用 PP-OCR 的 86.64% |
| 5 | **README 工程目录树已过时** | `rk3588_zero_copy/README.md:38-73` 只列 9 个 include、8 个 src | 实际有 `gate_main.cc`、`include/rk/`、`config/`、`scripts/systemd/` 等 | ✅ 已按**当前实际目录**重写 `rk3588_zero_copy/README.md` §1 的目录树: 补齐 `src/gate_main.cc`、`include/rk/`(13 个头)、`config/`、`scripts/systemd/`、`scripts/`(30+ 个)、`tools/`、`tests/`、`third_party/`、`model/`、`build*/` |
| 6 | **看门狗"卡死"阈值两套口径** | 现场运维手册:23:日志停 > 5 分钟 / 某路帧号停 > 3 分钟 | `gate_watchdog.sh:70-71`:实际 `STALL_SEC=60`(1 分钟)、`CH_STALL_SEC=180`(3 分钟),检查周期 20 秒 | ✅ **以 `scripts/gate_watchdog.sh` 为准**: `STALL_SEC=60`(L1 整机心跳)、`CH_STALL_SEC=180`(L2 单路帧号)、`SILENT_STALL_SEC=60`(L1.5 日志在写但帧号全不涨)、`FAIL_ALERT_THRESHOLD=3`(连续失败 3 次才升级)、检查周期 **20 秒**。`现场运维手册.md` 的"日志停 > 5 分钟"是错的(漏了 L1.5 与告警升级门限), 已改 |
| 7 | **CPU 调频快照两处不同** | `performance_audit_report.md` §1.3/§4.2 写的是 max 值(1800/2256/1416) | `env_audit.txt` 是 cur 值(1200/1200/1608) | ✅ **两者都不是"max 值", 是 `cur/max` 对, 只是采样时刻不同**。`performance_audit_report.md:64` 记的是**满载时**的快照: policy0 `1800/1800`、policy4 `2256/2256`、policy6 `1416/2256`(policy6 的 cur 已经< max); `audit/out/env_audit.txt:35-37` 记的是**低负载时**的快照: `1200/1200`、`1200/2256`、`1608/2256`。CPU 是 `ondemand` governor, cur 随负载浮动 —— **两个快照都对, 不能互相替换**。已在这两份文档里注明采样条件 |
| 8 | **RISK_REGISTER 编号跨段跳号** | R13~R18 在"低危"表但排在 R19~R22 之前 | 原文未解释跳号原因 | ✅ 已补录原因: R1~R12 是 2026-11-13 首扫的编号; **R19~R23 是 2026-11-18~11-19 做生产守护层时新增的**, 当初插在"中危/低危"表里而没重排编号, 于是 R13~R18(R19 之前)看起来"跳号"。编号**保留不动**(历史报告靠编号索引), 只在 `RISK_REGISTER.md` 里加"编号沿革"说明 |
| 9 | **NPU 频率口径差异** | `layer_bench_report.md`:频率锁 1000 MHz 是"算力天花板"口径 | 生产 `rknpu_ondemand` 会降频,实测单核 25.4 ms vs 23.0 ms(约 10% 差异) | ✅ **两个口径都对, 适用面不同**: `layer_bench_report.md` 的 23.0 ms/133 FPS 是**锁频 `min=max=1000 MHz` 下**的"算力天花板"口径; 生产跑的是 `rknpu_ondemand`(`min=300MHz/max=1000MHz`), 满载时实测 `cur=1000MHz`(`audit/out/env_audit.txt:18-19`), 但**空档期会降频**, 该组 25.4 ms 就是 **900 MHz** 下的数(比值 1000/900 = 1.111, 见 `layer_bench_report.md:302`)。已注明"天花板"是**上限不是保证**。★ 另: `/sys/class/devfreq/fdab0000.npu/load` 在本板**恒显 `100@1000000000Hz`**, 判 NPU 占用只能用 `/sys/kernel/debug/rknpu/load` 的逐核读数(需 root) |

---

## §12.1 统一口径速查(v2.3-p2a-hardened)

> ★ 这是**引用时直接抄这一节**的口径。每一行都标了实测出处与测量条件。

| 主题 | **统一口径** | 实测出处 / 测量条件 |
|---|---|---|
| **NPU 核绑定** | 检测 `0,1,2` + 识别 `0,1,2`(每核 1 worker, i%3 轮询) | `rk3588_zero_copy/config/gate.env:79-82`;2026-11-19 封板 A/B 实测 4 路合计 **78.40 FPS**(旧绑定 `1,2 / 0` 是 68.30 FPS) |
| **日志速率(生产级)** | `warn` 级 **398 KB/小时 ≈ 9.5 MB/天** | `rk3588_zero_copy/docs/现场运维手册.md`(2026-11-19 封板复测: 4 路推理 + 看门狗 + rk-minimal 同时在跑) |
| **日志速率(info/debug, 历史)** | 失控量级, **仅作历史参照, 不是现网速率**: 36.5 MB/小时 ≈ 17 GB/天(2026-11-18 旧二进制) / 156 行/秒 ≈ 80 MB/小时(2026-11-19) / 7.9 GB/天(`SOAK_REPORT_20261118.md` R1) | 各批次条件不同(旧二进制 / 采样窗口不同), 因此三个数互不相等 |
| **日志目录** | **`/var/log.hdd`**(105GB 真实盘, 实测剩 78GB) | 2026-11-23 迁移; `/var/log` 是 47MB zram(写入算真 RAM, 写满 → `write()` ENOSPC → 应用崩) |
| **RGA wstride 单位** | **像素**, 不是字节。RGB888 640×480 纯拷贝: wstride=640 → **100.0%** 逐字节相同; wstride=1920 → **33.6%**(被横向压扁) | `rk3588_zero_copy/tests/probe_rga_stride.cc:95,110-111`;`docs/ZERO_COPY_MPP_RGA.md:78-79` |
| **绿牌识别率** | 现网唯一识别模型 `ppocrv4_rec_fp16.rknn`: 整牌 **86.64%**(188/217)、单字符 97.06%、空输出率 0.46%。旧 LPRNet(已 `DEPRECATED`): 整牌 **0.00%**(0/217)、空输出率 **55.76%**(121/217) | 同一套 **217 张绿牌实拍样本**(`accuracy/data/plates_serial/`);`docs/PROJECT_BRIEFING_20261123.md` §9.3 |
| **看门狗阈值** | `STALL_SEC=60` / `CH_STALL_SEC=180` / `SILENT_STALL_SEC=60` / `FAIL_ALERT_THRESHOLD=3` / 检查周期 **20 秒** | `rk3588_zero_copy/scripts/gate_watchdog.sh:70-74,89` |
| **NPU 频率** | `rknpu_ondemand`, `min=300 MHz / max=1000 MHz`;满载实测 `cur=1000 MHz`。**1000 MHz 锁频的 23.0 ms/帧 是"算力天花板"上限口径**, 生产空档期会降频(900 MHz 时 25.4 ms, 比值 1000/900=1.111) | `audit/out/env_audit.txt:18-19`(实测);`layer_bench_report.md:9,302` |
| **NPU 占用读取** | `/sys/kernel/debug/rknpu/load` 逐核(需 root)。**`/sys/class/devfreq/fdab0000.npu/load` 在本板恒显 `100@1000000000Hz`, 不可用** | `docs/PROJECT_BRIEFING_20261123.md:1333`;`model_opt/model_opt_report.md:137` |
| **CPU 频率** | `ondemand` governor。policy0 max 1800 / policy4 max 2256 / policy6 max 2256 MHz。`cur` 随负载浮动: 满载快照 1800/2256/1416, 低负载快照 1200/1200/1608 | `audit/performance_audit_report.md:64,272`(满载快照);`audit/out/env_audit.txt:35-37`(低负载快照) |
| **10bit HEVC** | 本板 MPP/RGA 硬件路径**不支持** 10bit;降软解交付率 **~10 FPS/路**(软解 9~10 ms/帧 + CPU 色彩转换 9~10 ms/帧), MPP 路径是 20 FPS。生产代码已加位深探测 + 拒收护栏(P2-A1), 不会静默零检出 | `docs/p2a_stability_hardening_report.md` §2.1 / §七-1 |
| **O9 残余** | **12.26%** = 13/106 帧的"ROI 顶边切进省份字行"。是**离线几何指标**(原始检测框, 无跟踪器), 不是跟踪器测量。**归口 R25, 与 R1(模型, 已闭环)无关**。✅ **2026-11-24 决策: 技术项关闭, 不改 C++, 保持现网 F8 参数**(`0.30/0.38/0.22/0.126/0`), 作为**模型几何边界**如实记录 | `opt_ebike_fork9/docs/05_roi_calibration_eval.md:28,90-120`;`opt_ebike_fork9/tools/eval_plate_recognition.py:115-116`;`rk3588_zero_copy/config/gate.env:96-100` |

---

# §13 硬约束与红线

## 13.1 `docs/AGENT_PROJECT_BASELINE.md` 给规划侧的三条硬约束(原文)

1. **禁止改动 `main.cc` 的单路核心逻辑**(已冻结的稳定基线);新功能一律新建文件或加宏开关。
2. **性能结论必须来自板端实测**(`debugfs` / `top` 采样),**禁止写「预计 / 大概 / 估算」**;对比必须单变量(同跑 600 帧、同开 PP-OCR、只关一个变量)。
3. **模型目录 `models/` 只读**(唯一真源);`archive/rk12ch/` 与 `third_party/` 禁止触碰。

## 13.2 目录白名单与权限规则(原文)

| 目录/文件 | 权限 |
|---|---|
| `rk3588_zero_copy/src/` `scripts/` `docs/`、根 `docs/` | 可读可改 |
| `models/` | **只读引用(禁止修改/删除)** |
| `archive/rk12ch/` | **禁止触碰(已冻结)** |
| `.git/` | **禁止触碰** |
| `third_party/` | **禁止触碰** |

## 13.3 数据口径与验证标准(原文)

- **数据真源**:所有性能指标必须来自板端真实运行的 `debugfs` 或 `top` 采样。
- **测试条件**:性能对比必须保持单变量原则。
- **禁止行为**:**严禁在汇报材料和文档中写"预计、大概、估算"的虚假数据。**

## 13.4 Agent 行为规范(原文要点)

- **报错处理**必须按 `现象 → 根因 → 修复步骤 → 验证方法` 四步走,禁止瞎猜。
- **交付命令**必须是可直接复制粘贴执行的完整版,禁止用 `<替换项>` 占位而不说明替换逻辑。
- **环境变更**(修改网段、切代理、锁频等)必须显式告知副作用。
- 代码交付前必须通过本地 `make -j8` 编译**零警告**,且通过 **300 帧冒烟测试**。
- 遇到不确定的硬件行为,必须如实标明风险,禁止强行跑测试。

## 13.5 安全红线(3 条,原文)

1. **绝对不能上传/喂给模型的数据**:内部网络 IP 拓扑、公司敏感数据的原始码流、私有 PAT。
2. **必须人工操作的场景**:断电重启、物理插拔网线/摄像头、重置路由器、操作 GitHub 账户生成 Token。
3. **禁止 Agent 执行的操作**:
   - 禁止 `rm -rf /` 或删除 `.git` 仓库
   - 禁止强制覆写已被标记为稳定的旧代码(如 `main.cc` 单路逻辑)
   - 禁止在未经允许的情况下修改 `models/` 目录下的生产模型

## 13.6 已验证的 7 条实测坑位(规划必须规避,原文)

| # | 现象 | 根因 | 解法 |
|---|---|---|---|
| 01 | 模型载入报错,或推理结果全零 | RKNN Toolkit 版本高于板端 `librknnrt.so` 版本 | 强制用 **v2.3.2** Toolkit 编译,用 `rknn_query` 校验版本哈希一致 |
| 02 | 长时间运行后 RSS 缓慢上升 | `rknn_outputs_get` 未预分配缓冲,且 `AVFrame` 只 unref 不 free | `is_prealloc=1` 预分配;`av_frame_free()` 替代 `av_frame_unref()`;定期 `malloc_trim(0)` |
| 03 | LPR 裁剪阶段偶发段错误,甚至内核 panic | `MppFrame` 被过早 `deinit`,导致悬垂 fd | RAII(`MppFrameGuard`),所有权托管到本帧生命周期结束 |
| 04 | RGA 报错或输出花屏,严重时崩溃 | `rga_buffer_t::wstride` 单位是**像素**,误传成字节 | 传像素值,目标宽度按 16 字节对齐 |
| 05 | PP-OCR 输出 `ANT` 等乱码 | 从 640 检测小图上裁 ROI,像素严重丢失 | 回到 **1080P 原图**裁切再送 OCR |
| 06 | 读取本地文件瞬间崩溃或假死 | 文件读取速度极快,瞬间灌爆 MPP 输入队列 | 引入背压,按源帧率限速(Pacing)或抽帧推理 |
| 07 | 省份简称被丢弃,或识别成字母("苏") | 字典不含汉字,或输入是白字绿底(训练域是黑字白底) | 换 PP-OCRv4 FP16(6625 类汉字),强制灰度反白 |

## 13.7 本次会话(阶段 1)的红线

| 红线 | 状态 |
|---|---|
| 不 merge | ✅ 只新增 `opt_ebike_fork9/`,未动任何已有分支 |
| 不改生产推理逻辑 | ✅ `rk3588_zero_copy/src/**` 一个字节未动(mtime 未变) |
| 不改共享回归工具 | ✅ `accuracy/acc_batch.cc` 一个字节未动,改的是沙盒副本 |
| 用修正后的解码器做精度回归 | ✅ 用 `src/acc_batch_nc.cc`(含 ×stride 与跨步读修正,且已验证与原版等价) |
| 独立目录 | ✅ 全部在 `opt_ebike_fork9/` |
| 板端测完恢复现场 | ✅ `rk-gate.service` 仍在跑(生产模型 + `--max-fps 20`),NPU 仍 1 GHz,`GATE_MAX_FPS=20` 未动 |
| 生产配置不变 | ✅ `git status` 与开工时一致(9 个 M + 未跟踪沙盒) |

---

# §14 术语与文件坐标速查

## 14.1 三种 YOLO 导出形态(本项目核心概念)

| 形态 | 输出 | 含义 | 本项目出处 |
|---|---|---|---|
| **形态 A** | 1 个:`[1, 4+nc, N]` | 标准 ultralytics 单输出,box 与 cls **拼在同一个张量** | `yolo_post.cc` |
| **形态 B** | **9 个**:3 尺度 × `[box(4*DFL_LEN=64), cls(nc), sum(1)]` | 瑞芯微 fork `format='rknn'` 导出。**DFL 留在 CPU 解**。顺序契约 `index = s*3 + {0:box, 1:cls, 2:sum}`,s = 0/1/2 = stride 8/16/32 | `include/yolo_post.h:13-18` |
| **形态 C** | 2 个:`box[1,4,8400]` + `cls[1,nc,8400]` | **生产 ebike 用的形态**。DFL 与 dist2bbox 都在图内 | `EBIKE_MODEL_RUNBOOK.md` |

**形态 B 的解码契约**(`yolo_post.cc` + `acc_batch.cc`):
- `box` = `[1, 64, H, W]` **NCHW**,64 个通道含义 `c = side*16 + bin`;同一个 anchor 的 16 个 bin 在内存里**相隔 grid_len**,地址 `addr(bin) = base + (side*16 + bin) * grid_len`(**跨步读,不是连续读**)。
- `cls` = `[1, nc, H, W]`,**已过 Sigmoid**(概率)。
- `sum` = `[1, 1, H, W]`,`Clip(ReduceSum(cls, axis=1), 0, 1)`,用于快速预过滤。
- DFL:softmax 在 16 个 bin 上做 → 期望值落在 [0,15] = **格点单位** → **×stride 才是像素**。
- anchor 中心 = `((j+0.5)*stride, (i+0.5)*stride)`。
- `nc` 从 cls 张量维度推导:`nc = dims[n_dims-3]`。**形态 C 的 cls 是 `[1,nc,N]`,所以 `nc = dims[n_dims-2]` —— 两个形态别混。**
- 形态 C 的 box 输出是 **(cx, cy, w, h)** 像素,解码时 `x1 = cx - w/2`。
- NMS 是**按类别**的,IoU 阈值生产用 0.45(工程默认 0.60 是给 head/helmet 调的)。

## 14.2 关键文件坐标

| 想了解 | 去哪 |
|---|---|
| 仓库地图 / 瓶颈判定 / 已验证基线 | `context.md` |
| 唯一事实来源(环境/坑位/交付标准/红线) | `docs/AGENT_PROJECT_BASELINE.md` |
| 稳定性最高指令 | `docs/AGENT_RULES_FOR_CRASH_DEBUGGING.md` |
| 生产守护规范(看门狗判据/预检/9 个坑) | `rk3588_zero_copy/docs/PRODUCTION_GUARD.md` |
| 风险登记表(全表) | `rk3588_zero_copy/docs/RISK_REGISTER.md` |
| 现场运维(一条命令体检/日志/令牌桶) | `rk3588_zero_copy/docs/现场运维手册.md` |
| 现场适配(IPM 标定/绿牌/9 项验收) | `rk3588_zero_copy/docs/现场环境适配指南.md` |
| ebike 模型手册(形态 C/喂数/归一化/域) | `rk3588_zero_copy/docs/EBIKE_MODEL_RUNBOOK.md` |
| 零拷贝改造与三个根因 | `rk3588_zero_copy/docs/ZERO_COPY_MPP_RGA.md` |
| NPU 多实例并行评估 | `rk3588_zero_copy/docs/NPU_PARALLEL_EVAL.md` |
| 精度与联调(2026-11-13) | `rk3588_zero_copy/docs/ACCURACY_AND_INTEGRATION_20261113.md` |
| 交付白皮书(正式交付件) | `docs/RK3588边缘AI系统交付白皮书.md` / `.pdf` |
| 各阶段报告 | `docs/p0_fixes_report.md` / `task1_gate_lock_fix.md` / `task2_fork9_ab.md` / `task3_ocr_mixed_quant.md` |
| 算力天花板分层实测 | `layer_bench_report.md` + `npu_layer_bench/` |
| 精度回归工具链 | `accuracy/` + `accuracy/ACCURACY_REPORT.md` |
| 本次新增 | `opt_ebike_fork9/REPORT.md` + `opt_ebike_fork9/docs/0{0,1,2,3}_*.md` |

## 14.3 常用复现命令

```bash
# 编译(板端原生)
cd /home/seeed/rk_deploy/rk3588_zero_copy
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j8

# 单进程 4 路压测 10 分钟 / 两种拓扑公平 A/B
./scripts/run_4ch_gate_single.sh 600 20
./scripts/run_4ch_gate_ab.sh 600 20

# 精度回归全流程
cd /home/seeed/rk_deploy/accuracy
../_bench/venv/bin/python build_testset.py --video ../rk3588_zero_copy/DJI_20261102155401_0126_D.MP4 \
    --outdir data --every 1 --save-frames
../_bench/venv/bin/python build_calib.py --outdir calib
./run_all.sh && ../_bench/venv/bin/python make_report.py

# 模型审计(逐算子 NPU 耗时)
../_bench/venv/bin/python -I audit/model_audit.py models/ebike_int8_rk2332.rknn --json /tmp/a.json

# 本次新增:ebike fork9 全流程
cd /home/seeed/rk_deploy/opt_ebike_fork9
bash scripts/build_ebike_fork9.sh onnx/<源>.onnx <前缀>     # 导出+转换+自检
bash scripts/run_regression.sh                              # 回归 + NPU A/B
```

---

# §15 附录:数据资产与测量口径定义

## 15.1 数据资产(实测)

| 资产 | 数量/大小 | 内容 |
|---|---|---|
| `dataset/` | 2001 帧 `frame_%06d.jpg` | 抽帧 |
| `dataset_mp4/` | 1664 帧 | 抽帧 |
| `accuracy/data/frames/` | **1724** 帧 `f%05d.jpg` | DJI 实拍视频抽帧 —— **检测回归测试集** |
| `accuracy/data/plates_serial/` | 217 张 | 车牌裁剪 —— **识别回归测试集** |
| `accuracy/data/manifests/` | frames_list.txt / frames.csv / plates.csv / plates_serial.csv | 清单 |
| `_bench/calib/` | 24 张 | INT8 校准集(办公桌场景) |
| `_bench/dl/` | yolov8n_optimized.onnx(9 输出 fork9)、yolov8n_{320,416}.onnx 等 | 模型源 |
| `_bench/check{0,1,2,3}_*.onnx` | 仓库根也有 4 个 | ultralytics 导出流程的中间产物 |
| 板上的视频素材 | `testvideo3.mp4`(1280×720/30fps/21351 帧)、`testvideo2.mp4`(1280×720/21.5fps/30200 帧)、`Test2_fixed.mp4`、`电动车素材.mp4` | 生产 4 路源 |

## 15.2 测量口径定义

| 指标 | 定义 |
|---|---|
| `npu` ms | `rknn_run` + `rknn_outputs_get`(真正占着 NPU 核的时间) |
| `detwait` ms | 通道线程从提交任务到拿到结果的**墙钟**;**detwait − npu = NPU 排队时间** |
| `ocrwait` ms | 车牌 ROI 提交到拿到识别结果的墙钟 |
| `deliv` FPS | 交付帧率(累计平均);**实测会虚高 2~4%**,A/B 用帧计数器差分而非它 |
| 活堆 | `mallinfo2().uordblks` |
| 核占用(应用侧) | Σ(该核上所有上下文的 run 墙钟) ÷ (窗口 × 该核上下文数) |
| 核占用(驱动侧) | Σ(`RKNN_QUERY_PERF_RUN`) ÷ 同口径 |
| 核占用(debugfs) | `/sys/kernel/debug/rknpu/load`(需 root)。**devfreq 的 `load` 在本板恒显 100%,不能用** |
| 匹配率 | 匹配框数 ÷ 参考框数,匹配条件 IoU ≥ 0.5,按参考分数降序一对一贪心,**类别单独统计** |
| 类别一致率 | 类别相同的匹配对数 ÷ 匹配数 |
| mAP@0.5 | 伪 mAP:把 FP16 框当真值,11 点插值 |
| 干净窗口 | 空闲核占用 ≤ 8% 且单位耗时 ≤ 最快样本 × 1.10 |

## 15.3 测量污染的三个已知来源(各文档记录)

1. **别的会话在跑压测** —— `model_opt` 第一版脚本因 `npu_layer_bench` 抢 Core0 测出 C1 = 49.8 ms 这种物理上不可能的数据;`prune_finetune` 因此改用 Core1;`batch_bench` 期间 Core0 稳定 ~80%,**绝对吞吐值被系统性压低**(但所有配置在相同外部负载下测量,组间对比有效)。
2. **板上有常驻服务** —— `rk-minimal` 常驻 45% 单核 + 生产 `rk-gate` ⇒ **79.20 FPS 是在负重下测的**;板子安静时同一二进制能跑 77~79 FPS。
3. **停服务必须先让看门狗闭嘴** —— `touch /run/rk-gate/paused` → 停所有 `rk-*.service` → 按进程名杀孤儿 → 核对无残留进程。**服务清单必须动态枚举** `systemctl list-units | grep '^rk-'`(手写清单漏过 `rk-night-soak-monitor`,压测中 CPU 飙到 98%)。

## 15.4 产量的三个数(生产实际口径)

| 口径 | 值 | 出处 |
|---|---|---|
| 生产配置 4 路交付(限帧 20) | **79.20 ~ 80.02 FPS** | SOAK 纯净基线 / opt_audit A/B |
| 解除限帧 4 路全链路 | **95.2 FPS**(旧核绑定)/ **105.02 FPS**(现行) | `layer_bench_report.md` §第 5 层 |
| 单板 NPU 算力天花板 | **133.32 FPS**(L1 9 worker)/ 约 **108 FPS**(L1 三核各 1 worker 的实测均值) | 同上 / `audit/` |

---

**本文档到此结束。**
所有数字均可在上文标注的出处复核;凡本文未写明的,即为该事实未被记录。
