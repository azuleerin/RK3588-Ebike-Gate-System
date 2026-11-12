# 最小化测试单元 · 架构改造方案（待人类确认，尚未动代码）

> 依据：`AGENT_RULES_FOR_CRASH_DEBUGGING.md`（根目录，已建立）
> 目标：把"整机硬挂死"从"不知道哪个模块"变成"锁死到单个模块"，并在崩溃后仍有现场可查。
> 状态：**方案阶段，未写任何业务代码**。

---

## 0. 现状核查结论（决定了本方案怎么排）

### 0.1 工程不在 git 下 —— 规则第五节当前无法执行

```
$ git rev-parse --is-inside-work-tree
fatal: not a git repository (or any of the parent directories): .git
```

规则要求"每次跑长跑前打 Tag、死机后二分回滚"，**当前没有任何版本控制**，无法二分。
→ **step/00 必须先建 git 基线**（见第 5 节），且需要 `.gitignore` 排除大文件：

| 需排除 | 大小 | 原因 |
|---|---|---|
| `_bench/venv/` | ~1.0 GB | python 环境 |
| `_bench/wheel/*.whl` | 36 MB | 二进制包 |
| `_bench/*.onnx` | ~130 MB | 模型 |
| `rk3588_zero_copy/*.MP4` | 141 MB | 测试视频 |
| `_archive/*.tar.gz` | 66 MB | 历史存档 |
| `rk3588_zero_copy/output_*/` | 880+84 个 jpg | 调试产物 |
| `third_party/lib/*.so` | 7.7 MB | 第三方二进制（改为文档说明获取方式） |

保留跟踪：`src/` `include/` `CMakeLists.txt` `docs/` `models/*.rknn`（885 KB）`tools/`

### 0.2 板端状态：已重启，NPU 已恢复

```
uptime                 : up 29 min（NPU PM 回到驱动默认 auto，runtime_status=suspended）
残留 D 态进程          : 无
dmesg 硬挂死关键字命中 : 0（journal 只保留了一次启动记录，历史现场已随重启丢失）
```

### 0.3 硬挂死取证能力：**已有，且够用**（这是好消息）

| 能力 | 状态 | 用途 |
|---|---|---|
| **ramoops** | ✅ 已配好：`ramoops: dmesg-0 0x18000@0x0000000000110000`（96 KB） | 内核 oops 现场**跨重启保留**到 `/sys/fs/pstore/dmesg-0`。硬挂死现场的证据就在这里 |
| **netconsole** | ✅ 模块存在：`/lib/modules/6.1.172-vendor-seeed-rk3588/kernel/drivers/net/netconsole.ko` | 把内核日志实时发到局域网另一台机 —— **SSH 断了但网还通**时的唯一取证通道 |
| **cgroup memory** | ✅ 已启用（cmdline `cgroup_memory=1 cgroup_enable=memory`，`/sys/fs/cgroup/memory/` 存在） | 规则第二节要求的内存上限（v1：`memory.limit_in_bytes` + `memory.memsw.limit_in_bytes`） |
| **UART console** | ✅ `console=ttyFIQ0,1500000n8` + `console=tty1` | 有串口线的话是最佳实时通道 |

**⚠️ 隐患**：cmdline 是 **`loglevel=1`**（内核日志几乎不输出）。
建议改为 `loglevel=7`（需改 cmdline 并重启）。**在拿到 ramoops 现场之前，这个必须先改** —— 否则下一次崩溃可能又什么都留不下。

### 0.4 多路并发开发：`rk12ch/` 冻结

`rk12ch/` 是新的 **12 路 1080P RTSP Python 工程**（4 层流水线：解码 12 线程 → RGA 预处理 → NPU 推理（core 0_1 batch=N + core 2）→ 业务层）。
它通过 **ctypes 直连 .so** 调 MPP/RGA/RKNN。

**处置：冻结，不删、不改、不长跑。**
理由：① 它就是"多路/并发开发"，规则明令停止；② ctypes 没有边界检查、结构体布局对不上就是内存破坏，**在硬挂死排查期引入第二个高风险实现会让归因彻底失效**；③ 它可以作为"业务侧最终形态"的设计参考，但**必须在根因定位之后**才恢复。

---

## 1. 最小化测试单元：架构

### 1.1 核心思路：三级数据源隔离

规则要求"只跑 MPP 解码 → 只跑 RGA 转换 → 只跑 NPU 推理"。
但如果 RGA 单元还要先解码，就仍然混入了 MPP，**二分不成立**。
所以必须用**预录中间产物**把上游彻底摘掉：

```
        ┌──────────────┐   dump NV12    ┌──────────────┐   dump RGB888   ┌──────────────┐
        │ decoder-only │ ──────────────▶ │  rga-only    │ ──────────────▶ │  npu-only    │
        │  MPP/FFmpeg  │   .nv12 帧     │  RGA 转换    │  .int8 640² blob│   RKNN 推理  │
        └──────────────┘                └──────────────┘                 └──────────────┘
             只碰 MPP                     只碰 RGA                          只碰 RKNN
```

**任一单元崩溃 → 立刻锁定模块**，且不需要跑另一个单元。

### 1.2 目标目录结构（全部新增，不动现有业务代码）

```
rk3588_zero_copy/
├── include/rk/                      ← 新增：基础库头（无业务、无硬件依赖）
│   ├── log.h                        分级日志 + 落盘 + 线程ID/函数名/模块
│   ├── mem_guard.h                  受控分配 + canary + 峰值统计
│   ├── fd_ledger.h                  fd 台账（谁开/谁关/漏没漏）
│   ├── stack_guard.h                栈水位 + 心跳看门狗
│   └── dma_guard.h                  DMA-BUF 尺寸记账 + 使用前校验
├── src/
│   ├── rk_base/                     ← 新增：基础库实现（静态库 rk_base）
│   │   ├── log.cc
│   │   ├── mem_guard.cc
│   │   ├── fd_ledger.cc
│   │   ├── stack_guard.cc
│   │   └── dma_guard.cc
│   └── minimal/                     ← 新增：最小化测试单元（新可执行 rk_minimal）
│       ├── main_minimal.cc          唯一入口：模式互斥校验
│       ├── unit_decoder.cc          只做 MPP/FFmpeg 解码
│       ├── unit_rga.cc              只做 RGA 转换
│       ├── unit_npu.cc              只做 RKNN 推理
│       ├── unit_common.cc           帧源/统计/落盘/长跑循环
│       └── dump_format.h            NV12/RGB/INT8 blob 的落盘格式定义
├── tools/visualizer/                ← 新增：规则第三节的独立可视化程序
│   └── visualizer.cc                读视频→推理→画框→存图（不推MQTT/不测速）
└── scripts/                         ← 新增：长跑与取证脚本
    ├── longrun.sh                   24h 长跑守护
    ├── collect_crash.sh             崩溃现场收集（pstore+dmesg+日志尾段）
    └── setup_gitignore.sh
```

**不改动** `main.cc`（108 KB 单体）、`rga_convert.cc`、`mpp_decoder.cc` 等现有文件。
原因：① 规则要求"小步快跑，绝不大段重写"；② 现有代码是 bisect 的对照基线；③ 最小单元必须**不带现有 bug**，否则测的是 bug 而不是模块。

### 1.3 CLI 设计

```
rk_minimal (--decoder-only | --rga-only | --npu-only)     # 三选一，互斥
  --src <spec>          输入源，按模式解释：
                          decoder:  file.mp4 | rtsp://...
                          rga:      nv12:<dir|file>   (推荐，纯 RGA)
                                   mp4:<file>         (可选，含 MPP)
                          npu:      int8:<file>       (640×640×3 uint8 blob)
  --dump <path>         把本单元的输出落盘，供下一单元使用
  --frames N            跑 N 帧后退出
  --duration SEC        跑 SEC 秒后退出（长跑用）
  --loop                到达末尾自动回到开头（长跑用）
  --log <file>          日志路径，默认 logs/minimal_<mode>.log
  --stats-interval SEC  每 SEC 秒 dump 一次统计快照
  --core-mask N         NPU 核掩码（仅 npu-only）
  --batch N             NPU batch（仅 npu-only）
  --save-frame <file>   存一帧 PNG（肉眼验证，规则第三节）
  --mem-limit-mb N      进程内存软上限（配合 cgroup）
  --no-legacy-rga       禁用 RGA 硬件路径（对照实验用）
```

**模式互斥校验**（启动即校验，违反直接退出，绝不允许"顺手把两个都开了"）：

```cpp
int n = (opt.decoder_only?1:0) + (opt.rga_only?1:0) + (opt.npu_only?1:0);
if (n != 1) { LOG_ERROR("必须且只能指定一个模式"); return 2; }
```

### 1.4 三个单元各自的行为边界

| 单元 | 允许调用 | **禁止调用** | 输出判据 |
|---|---|---|---|
| `--decoder-only` | MPP / FFmpeg 解码、fd 台账 | **RGA、RKNN** | 帧数、解码耗时 p50/p99、每帧 fd/stride/尺寸、**fd 泄漏计数**、内存峰值 |
| `--rga-only` | RGA（improcess）、memset 校验 | **MPP（默认）、RKNN** | 每帧转换耗时、RGA 调用次数、**输出缓冲区边界校验结果**、IOMMU 映射建立/销毁次数 |
| `--npu-only` | rknn_init/run/outputs_get | **RGA、MPP** | run 耗时直方图、驱动自报 run_duration、core_mask、输出张量尺寸校验 |

### 1.5 关键设计点：**最小单元不带现有 bug**

审计发现的 4 个 P0 必须在最小单元里按**正确语义**重写（否则测的是 bug 不是模块）：

| 审计项 | 现有位置 | 最小单元里的正确做法 |
|---|---|---|
| P0-1 帧提前释放 | `main.cc:1962` `av_frame_unref` 早于 `:2023/:2071/:2136` | 帧引用由作用域持有，`av_frame_unref` 放在**最后一次使用之后**；需要跨阶段使用时 `av_frame_ref` 加引用 |
| P0-2 MPP 帧过早 deinit | `mpp_decoder.cc:339` | `MppDecoder` 持有 `MppFrame hold_`，提供 `release_frame()`，**调用方用完才 deinit** |
| P0-3 每帧重建 IOMMU 映射 | `rga_convert.cc:515/532` | import handle **按 fd+尺寸缓存复用**；或提供 `--no-legacy-rga` 开关做对照 |
| P0-4 wstride 单位错 | `license_plate_rec.cc:553` | 统一走 `rga_wstride_pixels()` 换算（**像素**），目标缓冲加 pad 行余量；**禁止 fd 与 vir_addr 同时填** |

> 这 4 条修完后，最小单元是"正确版"，若它仍崩 → 排除应用层代码，直指驱动/硬件。

---

## 2. 内存安全基建（规则第二节）

### 2.1 受控分配器 `rk::MemGuard`

```cpp
namespace rk {
// 所有堆分配必须走这里；tag 用于按用途统计与定位
void* MemGuard::alloc(size_t bytes, const char* tag);
void  MemGuard::free(void* p);
size_t MemGuard::peak_bytes();
void  MemGuard::dump_by_tag();      // 按 tag 打印 当前/峰值/次数
bool  MemGuard::check_canaries();    // 遍历所有存活块校验头尾 canary
}
```

- 每个块头尾各 32 B poison（`0xA5`），free 时校验 → **立刻抓到越界写**
- 分配时记录 `size + tag + 调用线程 + 返回地址`，泄漏时能直接定位到调用点
- 全局 current/peak 统计 → 直接喂给 cgroup 的 `MemoryMax` 决策

### 2.2 fd 台账 `rk::FdLedger`

```cpp
int  FdLedger::open_dma_heap(const char* tag, size_t want);
int  FdLedger::dup(int fd, const char* tag);
void FdLedger::close(int fd);
void FdLedger::dump_open();          // 与 /proc/self/fd 对账, 列出未登记的 fd
size_t FdLedger::leaked_count();
```

- 每个 fd 记录 `{tag, 申请线程, 大小(经 fstat), 时间戳}`
- **`dma_buf` 必须 fstat 记录真实大小**，RGA/NPU 每次使用前校验 `fd 有效 && 尺寸足够`
- 定期 `dump_open()` 与 `/proc/self/fd` 对账 → **fd 泄漏在长跑中当场暴露**，而不是靠猜

### 2.3 DMA-BUF 严格校验 `rk::DmaGuard`（规则第二节核心）

针对审计 P0-1/P0-3 的根因（**用已失效的 fd 号去 import → RGA DMA 写进别人的内存 → IOMMU fault → 内核 abort**）：

```cpp
struct DmaBuf {
    int fd = -1;
    void* virt = nullptr;
    size_t size = 0;      // fstat 得到的真实大小
    size_t map_size = 0;
};

// 使用前必须调用；任何一项不满足就拒绝并记 ERROR
bool DmaGuard::validate(const DmaBuf& b, size_t need_bytes);
```

- **禁止 `fd` 与 `vir_addr` 同时填写**（审计 P1-5：软件 MMU 路径下 VA→PA 失配，RGA 会写进别的分配）
- 目标缓冲统一走 dma-buf，**禁止用 `std::vector` 堆内存当 RGA 目标**（审计 P0-4 的 9 倍尺寸失配就是从这来的）
- 目标缓冲按 `stride × (h + kRgaDstPadRows)` 分配，给 RGA 非整数倍缩放留余量

### 2.4 堆栈监控 `rk::StackGuard` + 看门狗

```cpp
void StackGuard::register_thread(const char* name);
size_t StackGuard::used_bytes(thread_id_t tid);   // 已用栈深度
size_t StackGuard::free_bytes(thread_id_t tid);   // 剩余余量
bool  StackGuard::heartbeat_alive();              // 主循环心跳是否在推进
```

- 登记每线程栈基址/大小（`pthread_getattr_np`），采样当前 SP → **栈水位**
- 看门狗线程每 500 ms：检查 ① 栈余量 < 阈值 ② 主循环心跳计数递增 ③ 超限则**立即落盘 ERROR 日志 + `fdatasync`**
- 看门狗**必须落盘不落 stdout**（硬挂死时 stdout 可能已经没了）

### 2.5 cgroup 内存上限（规则第二节要求）

板上是 **cgroup v1**，所以是 `memory.limit_in_bytes` / `memory.memsw.limit_in_bytes`：

```bash
# 先测当前峰值，再定上限（不猜）
systemd-cgtop -m -n 1
# 建议给主程序单独建一个 slice：
#   MemoryMax  →  memory.limit_in_bytes
#   MemorySwapMax → memory.memsw.limit_in_bytes
```

**上限数值待定** —— 需要先跑一次最小单元测出真实峰值（预计 NPU+MPP 稳态 200~400 MB），再留 2~3 倍余量。

---

## 3. 日志与打点（规则第四节）

```cpp
// rk/log.h
LOG_INFO("mpp", "decode", "frame#%u w=%d h=%d fd=%d", n, w, h, fd);
LOG_WARN("rga", "import", "fd=%d size=%zu need=%zu 复用缓存", fd, sz, need);
LOG_ERROR("dma", "validate", "fd=%d 已失效, 拒绝提交", fd);
```

**输出格式**（规则要求的四要素齐全）：

```
[2026-11-12 09:30:12.345][tid=1234][ERROR][dma][validate] fd=57 已失效, 拒绝提交
   └ 时间戳           └线程ID └级别 └模块 └函数  └动作
```

落盘要求：

- `O_APPEND | O_CREAT` 打开 `logs/minimal_<mode>.log`，**每行 `fflush`**
- WARN/ERROR 额外 `fdatasync` —— 保证硬挂死时最后几行真的在盘上
- **轮转**：单文件上限 64 MB，保留最近 5 个。**写满磁盘本身也会拖死系统**，这条不能省

**关键路径强制打点**（规则第四节）：

| 位置 | 打点 |
|---|---|
| 每次内存分配/释放 | `MemGuard::alloc/free` 内 |
| 进入推理前 / 出来后 | `unit_npu.cc` |
| 跨线程传数据前后 | `unit_common.cc` 队列出入口 |
| fd 申请/释放 | `FdLedger` 内 |
| 每帧关键尺寸 | `fd/stride/width/height/format` |

---

## 4. 独立可视化验证程序（规则第三节）

规则明确：**不要用打印日志验证图像算法**，必须能肉眼看。

```
tools/visualizer/visualizer.cc
  读视频 → (可选) RGA → RKNN 推理 → 画框/画 ROI → 存 PNG 或 cv::imshow
  ❌ 不推 MQTT   ❌ 不测速   ❌ 不连串口   ❌ 不写业务库
```

- `--save-dir out/` 存关键帧：原图 / RGA 输出 / 画框结果 / 车牌 ROI
- 人工确认：检测框位置对不对、ROI 裁切对不对、字符识别对不对
- 目的：**排除"日志正确但内存已损坏"** —— 日志说尺寸对，画面却花，就说明内存被踩了

最小单元也提供 `--save-frame`，用于验证解码帧和 RGA 输出本身是否正确。

---

## 5. Git 基线与推进节奏（规则第五节）

### 5.1 基线

```bash
cd /home/seeed/rk_deploy
git init
# .gitignore 排除第 0.1 节列出的 1.3 GB 产物
git add -A && git commit -m "baseline: 崩溃调试前现状"
git tag -a baseline/pre-crash-debug -m "硬挂死排查起点"
```

### 5.2 小步快跑（每步一个 tag，长跑前必打）

| 步骤 | 内容 | 产出 | 长跑 |
|---|---|---|---|
| step/00 | git init + .gitignore + baseline 提交与 tag | 可二分基线 | — |
| step/01 | `rk_base`：日志 + fd 台账 + 内存守卫 + 栈看门狗（**不碰硬件**） | 基础库 | 单测 |
| step/02 | `rk_minimal --npu-only`（最简单，先跑通） | 第一个最小单元 | 24h |
| step/03 | `rk_minimal --decoder-only`（按 P0-2 正确语义重写） | 第二个单元 | 24h |
| step/04 | `rk_minimal --rga-only`（按 P0-3 复用 handle；含 `--no-legacy-rga` 对照） | 第三个单元 | 24h |
| step/05 | 取证加固：netconsole 接收端、ramoops 采集脚本、cgroup 限额、看门狗联动 | 崩溃现场不丢 | — |
| step/06 | `tools/visualizer` 独立可视化程序 | 肉眼验证 | 人工 |
| step/07 | **把定位到的根因修复同步回业务代码** `rk_zero_copy` | 业务侧修复 | 24h |
| step/08 | 根因确认后再解冻 `rk12ch/` | 多路并发恢复 | — |

**每步都必须执行 `git diff` 并把差异贴到对话里供审查**（规则第五节强制要求）。
**每步长跑前打 tag，死机后 `git bisect start` / `git bisect good <上一个tag>` / `git bisect bad <当前tag>`**。

### 5.3 长跑与崩溃收集

```bash
scripts/longrun.sh --mode npu-only --duration 86400
scripts/collect_crash.sh          # 打包: pstore/dmesg 尾段 + run.log 尾 200 行 + fd/内存快照
```

崩溃后**先查**（规则第四节"重启后也能查到断连前最后几秒的日志"）：

1. `logs/minimal_<mode>.log` 尾部 200 行
2. `/sys/fs/pstore/dmesg-0`（ramoops，内核现场）
3. `dmesg -T | tail -100`
4. fd 台账 + 内存峰值快照

---

## 6. 审计结论摘要（本次已完成的只读审查）

对 `rk3588_zero_copy` 全部内存相关代码做了只读审查，**20 项问题**（完整版含 file:line 与修复建议，见对话记录）：

**P0（可导致内核级崩溃）**

| # | 位置 | 问题 |
|---|---|---|
| 1 | `main.cc:1962` ↔ `:2023/:2071/:2136` | `av_frame_unref` 先执行，之后仍在用该帧的 fd 与 CPU 指针 → **fd 号被复用后 RGA 会 import 别人的内存** |
| 2 | `mpp_decoder.cc:339` ↔ `main.cc:1721-1733` | `mpp_frame_deinit()` 在 decode() 内部就丢掉帧引用，fd/ptr 仍交给调用方 → **VPU 与 RGA 可能同时 DMA 同一块内存** |
| 3 | `rga_convert.cc:515/532` ↔ `:488-491` | 每帧 2~6 次 `importbuffer_fd`/`releasebuffer_handle` → **每帧重建/销毁 IOMMU 映射**，与已确认的 `rk_iommu_enable_stall` oops 同构 |
| 4 | `license_plate_rec.cc:553` | `d.wstride = resized_w*3`，librga 的 wstride 单位是**像素** → **行距算大 3 倍，9 倍尺寸失配** |

**P1（内存损坏/越界）**：`main.cc:2118` `dma_buf_map` 未查 `MAP_FAILED`（→ `memset((void*)-1)`）；`main.cc:1426/1619/2120` `aligned_alloc(64,N)` 的 N 非 64 倍数且未查 NULL；`yolo_post.h:48` `OutTensor::at()` 无边界检查；`license_plate_rec.cc:539-540` fd 与 vir_addr 同时填 → 软件 MMU；`main.cc:1868` `dims[4]` 在 n_dims==5 时越界读。

**明确无问题（不要动）**：`dmabuf.cc` 的 fd 生命周期；`rga_convert.cc` 的 stride 单位换算自洽且主链路正确；letterbox 边界计算；NV12 UV plane 偏移；CPU 兜底路径的 NV12 打包；`yolo_post/ipm/speed_track/result_sink/mqtt_pub/led_serial` 无越界；RKNN 的 fd/mem 配对与销毁顺序正确。

**最优先的三处改动**（性价比排序）：
`main.cc:1962` 移 `av_frame_unref` → `mpp_decoder.cc:339` 帧引用交给调用方 → `rga_convert.cc:515/532` 缓存 import handle。

> **注**：按规则"稳定性压倒一切"，**这三处业务代码的修复不在本次动手范围** —— 先用最小单元把根因锁死，再统一修（step/07）。

---

## 7. 需要人类确认的决策点

| # | 决策 | 我的建议 | 影响 |
|---|---|---|---|
| D1 | 最小单元建在 **C++ (`rk3588_zero_copy`)** 还是 **Python (`rk12ch`)**？ | **C++**。ctypes 无边界检查、结构体布局错配即内存破坏，**硬挂死排查期不能引入第二个高风险实现**；且 C++ 侧已有审计过的 stride 语义与 zero-copy 基建 | 决定 step/01~04 的落点 |
| D2 | `rk12ch/` 处置：**冻结**还是 **删除**？ | **冻结**（加 `README.QUARANTINE.md`，不删代码），作为最终业务形态的设计参考，根因定位后再解冻 | 步骤 step/08 |
| D3 | 是否现在建 git 仓库？ | **建**。规则第五节（二分回滚）的前提，且需要 `.gitignore` 排除 1.3 GB 产物 | step/00，且后续所有改动可审查 |
| D4 | 是否改 cmdline `loglevel=1` → `7`？（需重启） | **改**。否则下一次崩溃可能又留不下内核现场。ramoops 是 96 KB 的独立缓冲，不受 console loglevel 影响，但 `dmesg` 实时输出会受影响 | 取证能力 |
| D5 | 是否启用 **netconsole**（需另一台机器做接收端） | **启用**。SSH 断连但网还通时，这是唯一的实时取证通道。板上有串口 console 也可替代 | 取证能力 |
| D6 | cgroup 内存上限设多少？ | **先不拍数**。step/02 跑通后测真实峰值，再定 2~3 倍余量 | step/05 |
| D7 | 业务代码的 3 个 P0 现在就修，还是等最小单元定位后？ | **等**。现在修会破坏"最小化单元 vs 业务代码"的对照关系，干扰归因 | step/07 |
| D8 | 长跑用什么守护方式？ | **systemd service + `WatchdogSec=`**（应用卡死时被杀重启，而不是整机挂死），而不是 nohup 循环 | step/05 |

---

## 8. 确认后的第一个动作

确认 D1~D8 后，我立刻做 **step/00**：

1. 写 `.gitignore`（排除第 0.1 节产物）
2. `git init` → `git add -A` → 提交 `baseline: 崩溃调试前现状` → 打 tag `baseline/pre-crash-debug`
3. **把 `git diff` / `git show --stat` 输出贴到对话里供审查**（规则第五节）

然后 step/01 起，逐步实现 `rk_base` 与三个最小单元。
