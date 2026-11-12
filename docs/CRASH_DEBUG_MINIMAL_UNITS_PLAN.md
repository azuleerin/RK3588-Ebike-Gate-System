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
| **netconsole** | ⚠️ 模块可加载且配置成功，但**本厂商内核上不发包**（详见下方 0.3.1） | 备用通道，已配置并持久化；主通道改用**本地 dmesg 落盘** |
| **cgroup memory** | ✅ **cgroup v2 统一层级**（`/sys/fs/cgroup` = `cgroup2fs`，`/proc/self/cgroup` = `0::/user.slice/...`；v1 memory 控制器**未挂载**）。可用控制器：`cpuset cpu io memory hugetlb pids rdma misc` | 规则第二节要求的内存上限 → systemd 259 用 **`MemoryMax=`**（v2 语义） |
| **UART console** | ✅ `console=ttyFIQ0,1500000n8` + `console=tty1` | 有串口线的话是最佳实时通道 |

**⚠️ 隐患**：cmdline 是 **`loglevel=1`**（内核日志几乎不输出）。
建议改为 `loglevel=7`（需改 cmdline 并重启）。**在拿到 ramoops 现场之前，这个必须先改** —— 否则下一次崩溃可能又什么都留不下。

### 0.3.1 netconsole 实测结论（2026-11-12）

**配置格式踩坑**（已固化进 `scripts/setup_netconsole.sh`）：

```
netconsole=[src-port]@[src-ip]/[src-dev],[tgt-port]@<tgt-ip>/[<tgt-macaddr>]
本板实测可用: netconsole=6666@192.168.31.77/end0,4444@192.168.31.214/14:ac:60:ad:8a:bb
```

三个必须同时满足的条件（任一不满足都报 `couldn't parse config`）：

1. `modprobe` 的参数值**不能**再带 `netconsole=` 前缀（否则内核解析到 `netconsole=4444`）
2. **源端必须带本机设备名**（`/end0`），只写 `4444@<ip>` 必失败
3. **目标端必须带 MAC**（`/<mac>`），只写 `4444@<ip>` 或 `4444@<ip>:port` 必失败

**但功能上仍不可用**：模块加载成功、`console [netcon0] enabled`、
`netconsole: network logging started`、源/目标端口与地址全部正确，
**却没有任何 UDP 包上线**。用 AF_PACKET 原始套接字在 end0 上抓包验证：
抓包器本身工作正常（同窗口抓到 78 帧、74 TCP + 4 ICMP），
但发往 `:4444` 的 UDP 包为 **0**。判定为**本厂商内核 6.1.172 的 netconsole 发送路径失效**。

**因此把取证重心改为本地通道**（D5 批复的兜底指示："着重依赖 ramoops"）：

| 通道 | 状态 | 覆盖场景 |
|---|---|---|
| **本地 `dmesg -w` 落盘** | ✅ 已启用（`rk-kernel-log.service`，`logs/kernel.log`，logrotate 限 64MB×5） | 整机挂死前的最后几秒，**不依赖网络**，比 netconsole 更可靠 |
| **ramoops** | ✅ 已配好（`0x18000@0x110000`） | 内核 oops 现场，跨重启保留 |
| netconsole | ⚠️ 已配置并持久化，但本内核不发包 | 若换官方内核可自动复活 |

> 结论：**本板取证不依赖 netconsole**。本地 dmesg 通道覆盖同样的场景且更稳。

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

### 1.5 关键设计点：**最小单元必须"带病"**（D7 批复：先复现，再定位，最后修）

> ⚠️ 本节在 2026-11-12 被 D7 批复推翻过一次。原方案写的是"最小单元按正确语义重写、不带 bug"，
> **这是错的** —— 提前修好 P0-1，万一直机不复现，就不知道是哪个模块救了命，整个排查作废。
> 现在改为：**忠实复现可疑行为**，把"哪个模块会崩"这个问题交给隔离法回答。

| 审计项 | 现有位置 | 最小单元里的做法（带病） | 隔离价值 |
|---|---|---|---|
| P0-2 MPP 帧过早 deinit | `mpp_decoder.cc:339` | **照抄现有时序**（decode() 内即 deinit），不改语义 | 若此单元崩 → 锁定 MPP/VPU |
| P0-3 每帧重建 IOMMU 映射 | `rga_convert.cc:515/532` | **照抄每帧 import/release**；另提供 `--no-legacy-rga` 做对照 | 若此单元崩 → 锁定 RGA/IOMMU |
| P0-1 帧提前释放 | `main.cc:1962` | 放在**全链路复现器**里（单独模式），照抄释放顺序 | 复现"fd 号被复用"的必要条件 |
| P0-4 wstride 单位错 | `license_plate_rec.cc:553` | **不在最小单元路径上**；但必须在 `rga-only` 里做 A/B 对照验证（见 7.2） | 确认尺寸失配的真实影响 |

**判定标准**：带病最小单元若崩 → 说明该模块可疑；若**不崩** → 说明该模块单独跑不崩，
需要全链路复现器 + 逐步加模块来二分。

> 注意：带病复现 ≠ 放弃内存安全基建。`MemGuard` 的 canary、`FdLedger` 的台账、
> `DmaGuard` 的使用前校验**照常全上** —— 它们只观测不改变时序，是取证工具而非修复。
> 唯一例外：`DmaGuard` 在 fd 失效时**记录 ERROR 但不阻断**（阻断会改变行为，掩盖复现路径）。

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

板上是 **cgroup v2 统一层级**（`/proc/self/cgroup` 为 `0::/user.slice/...`，v1 memory 控制器并未挂载，
尽管 cmdline 里还留着 `cgroup_memory=1` 的旧参数）。所以用 systemd 259 的 **v2 语义**：

```ini
[Service]
MemoryMax=2G          # → /sys/fs/cgroup/<slice>/memory.max
MemorySwapMax=0       # → memory.swap.max
MemoryAccounting=yes  # → memory.peak / memory.events 可查
```

手工查看：

```bash
cat /sys/fs/cgroup/user.slice/memory.current   # 当前
cat /sys/fs/cgroup/user.slice/memory.peak      # 峰值
cat /sys/fs/cgroup/user.slice/memory.events    # OOM 次数（排查"疑似泄漏"最直接的证据）
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

## 7. 决策批复（2026-11-12 人类已批）

| # | 决策 | 批复结论 | 落地动作 |
|---|---|---|---|
| D1 | 最小单元建在 C++ 还是 Python？ | **C++**。排查底层内存和硬件死锁绝不能有 Python ctypes 黑盒，必须看到最底层的指针和内存布局 | 全部落 `rk3588_zero_copy/src/minimal/` + `src/rk_base/` |
| D2 | `rk12ch/` 处置 | **冻结**（不删）。集中精力解决单路稳定性 | `rk12ch/README.QUARANTINE.md` 已建立，解冻条件写死 |
| D3 | 建 Git 仓库 | **立刻建**，1.3G 大文件入 .gitignore | ✅ step/00 完成：153 文件 / 23MB，tag `baseline/pre-crash-debug` |
| D4 | cmdline `loglevel=1` → `7` | **强烈同意改**，改完顺手重启一次 | ✅ `/boot/armbianEnv.txt` 的 `verbosity=7` 已改（原文件备份 `armbianEnv.txt.bak-20261112`），待重启生效 |
| D5 | netconsole | **有条件就开；找不到接收端就着重依赖 ramoops** | ✅ 已装：接收端指向工作站 `192.168.31.214:4444`（由 `scripts/netconsole_receiver.py` 接收）；同时启用本地 `dmesg -w` 落盘 + ramoops 三通道 |
| D6 | cgroup 内存上限 | **暂不限制**，先测正常峰值再定 | `MemoryMax=2G` 仅为占位，测出峰值后回来改 |
| D7 | P0 何时修 | **暂不修**。核心是"复现死机"——提前修好 P0-1，万一直机不复现，就不知道是哪个模块救了命。必须先用**带病代码**复现，再用隔离法锁定模块，最后修 P0 | 见 1.5「带病复现原则」 |
| D8 | 长跑守护 | **systemd 守护**，程序崩溃自动拉起，不拖死整个系统 | `scripts/longrun.sh` + `scripts/systemd/rk-minimal.service`（`Restart=always` + `WatchdogSec=300`） |

### 7.1 带病复现原则（D7 的工程化落地）⚠️ 关键

**最小单元必须忠实复现可疑行为，不得顺手修好。** 否则死机不复现，整个排查作废。

| 单元 | 必须保留的"病" | 绝不能做的事 |
|---|---|---|
| `--decoder-only` | 复现 `mpp_decoder.cc:339` 的帧引用释放时序（P0-2） | 不要把帧引用改成"调用方持有" |
| `--rga-only` | 复现 `rga_convert.cc:515/532` 的**每帧 import/release**（P0-3）；同时提供 `--no-legacy-rga` 做对照 | 不要提前把 handle 改成缓存复用 |
| `--npu-only` | 复现业务链路的 `rknn_set_io_mem` + fd 绑定路径 | 不要只跑裸 `rknn_run` |
| 全链路复现器 | 复现 `main.cc:1962` 的 `av_frame_unref` 早于使用（P0-1） | 不要移动 `av_frame_unref` 的位置 |

**唯一例外是 P0-4（`license_plate_rec.cc:553` wstride 单位错误）**：
它不在最小单元的路径上（属 LPR ROI 裁剪），且**必须在 `rga-only` 里做参数对照验证** ——
因为 `rga-only` 恰好就是用来暴露这个问题的单元。做法见 7.2。

### 7.2 rga-only 里的 wstride 对照验证（回应人类批注）

`rga-only` 单元在转换后，对**输出缓冲区**做以下对照，用数据判定 P0-4 的影响：

| 对照组 | `dst.wstride` 取值 | 预期现象 |
|---|---|---|
| A（字节语义，疑似错误） | `resized_w * 3` | **写入行距是实际宽度的 3 倍** → 越界写 → canary 被破坏 / 邻块被踩 / 内核 abort |
| B（像素语义，librga 正确语义） | `resized_w` | 正常 |

判定手段（三条独立证据）：

1. **canary 校验**：`MemGuard` 在目标缓冲头尾各放 32B poison，A 组必然被破坏
2. **邻块哨兵**：目标缓冲后紧跟一个已知模式的哨兵块，A 组会被覆写
3. **边界数学**：断言 `wstride_pixels * bpp * height <= 分配字节数`，A 组在断言处直接失败

本工程已有 `tests/probe_rga_stride.cc`（实测 RGB888 64×64：`wstride=64` 逐字节相同、`wstride=192` 被压扁），
rga-only 单元应复用它作为**参数基准**，避免重复踩坑。

---

## 8. 已完成的 step/00

1. ✅ `.gitignore`：排除 `_bench/venv`、`_bench/wheel`、`_bench/dl`、`_bench/calib`、`*.onnx`、
   `*.MP4`、`_archive/`、`build*/`、`output_*/`、`__pycache__`、`logs/`
2. ✅ `git init` → `git add -A` → 提交 `482f95a baseline: 崩溃调试前现状` → 打 tag `baseline/pre-crash-debug`
3. ✅ `git show --stat` 已贴出供审查：**153 files changed, 27356 insertions(+)**，无任何功能性改动
4. ✅ 保留 `third_party/lib/librknnrt.so`（7.4MB，精确版本 2.3.2 是复现的前提）

**基线原则**：基线提交 = 崩溃时正在跑的那份代码，**一行业务代码都没动**。
`rk12ch` 的冻结标记、`loglevel` 改动等都不混进基线，避免污染二分起点。



---

## 9. 隔离排查结果（2026-11-12 执行完毕）

### 9.1 三个最小化单元的结论：**单个模块都无法搞挂整机**

| 单元 | 规模 | 结果 |
|---|---|---|
| `--npu-only` | 37,894 帧 / 900s / 42.10 FPS | ERROR=0 WARN=0，**无死机** → **NPU 推理环排除** |
| `--decoder-only` | 22,500 帧 / 900s / 25.00 FPS | ERROR=0，fd 复用事件=0，**无死机** → **MPP 解码环排除** |
| `--rga-only` | 300,101 次转换 / 900s / 333 次/s | ERROR=0，哨兵损坏=0，**无死机** → **RGA 单独跑排除** |

长跑记录: tag `longrun/20261112_101413/npu-only`、`longrun/20261112_103805/decoder-only`、
`longrun/20261112_110056/rga-only`；日志在 `logs/minimal_*.log`。

**结论**: 硬挂死**不是任何单一模块**造成的，必须是多模块交互才会触发。
这把嫌疑指向"只有全链路才存在"的缺陷:
- **P0-1**（`main.cc:1962` `av_frame_unref` 早于使用）—— 只有在解码帧与
  RGA/NPU 交错时才成立
- **P0-4**（`license_plate_rec.cc:553` wstride 单位错误）—— 只有跑 LPR ROI
  裁剪路径才会进入
- 二者叠加时：LPR 的 RGA 读的是**已经被 unref 释放、fd 号可能已被复用**的帧

### 9.2 P0-4 已被实证坐实（不只是代码审查）

`--wstride-mode` A/B 对照（`pixels` 正确 vs `bytes` 复现 bug）：

| 模式 | dst.wstride | RGA 预期写入 | 缓冲应有 | 首个越界字节 |
|---|---|---|---|---|
| pixels（正确） | 640 px | 1,228,800 B | 1,228,800 B | **无**（哨兵损坏 0 次） |
| bytes（bug） | 1920 px | 3,686,400 B | 1,228,800 B | 偏移 **1,232,640** |

**机理可精确反推**：错误 wstride 让行距从 1920 字节变成 5760 字节，于是
- 第 213 行起点 = 213×5760 = 1,226,880，正好落在应有数据尾部内；
- 第 214 行起点 = 214×5760 = **1,232,640** —— 这就是实测到的第一个越界字节；
- 1,228,800~1,232,640 之间那 3840 字节是行距填充，RGA 没写，所以没被检出。

即：**图像被撕裂成"每行跳 5760 字节"的散列，同时冲出缓冲 2,457,600 字节**。
在 `license_plate_rec.cc` 的场景里，目标是 `std::vector` 堆内存
（`d.fd = -1`），这 2.4 MB 越界写**直接踩烂相邻堆对象**。

### 9.3 P0-3（每帧 import/release）无性能代价

`--import-mode` 对照：cached 1.654 ms vs per-frame 1.619 ms（噪声内）。
每帧 import/release 不慢；但它是否触发 IOMMU 死锁，长跑答案是**否**
（300,101 次转换、60 万次 import/release 全部正常）。

### 9.4 顺手挖出的两个非计划问题

1. **systemd 看门狗误杀**：最初没实现 `sd_notify(WATCHDOG=1)`，`WatchdogSec=300`
   到点无条件 SIGABRT 应用（`status=6/ABRT`），看起来像崩溃其实是我们自己没喂狗。
   已在 `rk_base` 里手写 sd_notify 修掉（不依赖 libsystemd-dev）。
2. **本机 DJI 那个 MP4 是 HEVC Main10（10-bit，profile_idc=2）**，RK3588 VPU 不支持，
   业务二进制同样解出 0 帧 —— 这不是代码问题。step/03 因此改用 RTSP 相机
   （8-bit H.264）作源。

### 9.5 下一步：全链路复现器（step/05）

三个隔离单元都干净 ⇒ 必须跑**全链路带病**才能复现。最小单元的价值在于
**证明单模块无罪**，剩下的就是让它们**按业务顺序协作**。

计划（不改业务代码，直接用现成的 `rk_zero_copy`）：
- 以 systemd 守护跑业务二进制（`Restart=always` + `WatchdogSec`），让它自己反复重启
- 开 `--lprnet`（进入 P0-4 的 ROI 裁剪路径）与 `--show`（进入显示转换路径）
- 同时开三个取证通道：ramoops（跨重启）、本地 `dmesg -w` 落盘、fd/内存巡检
- 一旦硬挂死 → `cat /sys/fs/pstore/dmesg-0` 取内核现场


---

## 10. step/05 引爆测试结果（2026-11-12）

### 10.1 已证实的机制

| 结论 | 证据 |
|---|---|
| `mpp_destroy` 会**立即关闭**导出的 dma-buf fd | `tools/fd_close_probe`：销毁前 `fd=14 有效=1 inode=430` → 销毁后 `仍有效=0` |
| 悬垂 fd/指针能**可复现地**打崩业务进程 | 引爆开关 `--mpp-recreate-ms` → **21 次 `status=11/SEGV` + core-dump** |
| RGA 硬件错误可**按需复现** | 正常跑 85,620 帧才 1 次；引爆后随手就有 `INTR[0x2099]` + RGA3_core0 软复位 |

```
rga: irq handler err! INTR[0x2099], HW_STATUS[0xa0000], CMD_STATUS[0x10000]
rga: RGA3_core0[0x1] soft reset complete.
rga: rga intr error[0x2099]! / request commit failed! / submit failed!
```

### 10.2 关键认知：RGA 的 DMA 是异步的

`improcess` 返回时 DMA **还在飞**。所以引爆点必须放在"import 之后、DMA 期间"：

| 引爆点位置 | 结果 |
|---|---|
| 裁剪**之前** | `Fail to get dma_buf from fd[21], ret[-9]` → RGA 优雅失败 → 进程 SEGV，系统不挂 |
| 裁剪**之后**（DMA 在飞时销毁解码器） | **`INTR[0x2099]` + RGA core 软复位** → 这才是硬挂死的前兆 |

### 10.3 被 RGA 驱动挡住的路径（重要负面结论）

RGA 驱动比预想的防御性强，两道校验都拦住了"悬垂 fd"：

```
rga: Fail to get dma_buf from fd[21], ret[-9]                      # fd 已失效(EBADF)
rga: Only get buffer 65536 byte from dma_fd=0x16, but required 3133440 byte   # 尺寸不够
```

即：**"fd 被内核回收分配给别的设备"这条最直观的死法走不到内核 fault**，
只会让 RGA 优雅失败 → 业务代码的 CPU 兜底读到已释放的 CPU 指针 → 用户态 SEGV。

### 10.4 现状与剩余问题

- 累计：21 次 SEGV、13 次 EBADF、9 次尺寸拒绝、4 次 RGA core 软复位
- **板子始终没挂**：pstore 空、无 `internal error`、无 `external abort`
- 说明 **RGA 的软复位自救路径目前是有效的**

**剩下要回答的问题**：反复的 RGA core 软复位（每次都发生在 VPU 正在解码、
IOMMU 有在途翻译的时刻）会不会把 IOMMU 状态搞坏到**无法自救**？
这需要长时间浸泡来回答 —— 引爆服务已就绪（`rk-ignition.service`），
`rk-crash-archive.service` 已武装（下次开机自动抢收 pstore + fsync 日志）。

> ⚠️ 板上**没有硬件看门狗**（`/dev/watchdog` 不存在），硬挂死后只能人工断电重启。
> 因此浸泡期间建议有人盯着，或至少约定好检查间隔。


---

## 11. Step 07：P0-1 / P0-2 的 RAII 修复与验证（2026-11-12）

### 11.1 修复内容

| 步骤 | 内容 | tag |
|---|---|---|
| 07.1 | `rk_base` 新增 RAII 守卫 `AVFrameGuard` / `MppFrameGuard`（只写基建，不碰业务代码） | `step/07.1-raii-guards` |
| 07.2 | `main.cc` 生命周期改 RAII；`MppDecoder::decode()` 增加所有权转移出参；RGA 加极端比例保护 | `step/07.2-raii-lifecycle` |
| 07.3 | 异常路径逐条审计（13 个退出点）；fd 台账与守卫计数接入业务二进制 | `step/07.3-errorpath-audit` |
| 07.4 | 2 小时挂机压力测试 | `fix/p0-1-raii-verified` |

### 11.2 修复的关键认知

**真正悬垂的不是 AVFrame，是 MPP 帧。**
生产的 MPP 路径里 `av_read_frame` 拿到的 AVFrame 其实是**空的**
（`extract_nv12` 只在软解路径调用），`av_frame_unref` 它是空操作。
真正持有解码像素的是 MPP 的 `MppFrame`，而 `MppDecoder::decode()` 在
**函数内部**就 `mpp_frame_deinit()` 了 —— 缓冲随即归还给 MPP buffer group，
调用方却还拿着它的 fd/ptr 继续用（LPR 裁剪在 unref 之后才用）。

修复方式：`decode()` 增加 `void** out_frame` 出参做所有权转移，
调用方用 `rk::MppFrameGuard` 托管到**本轮循环结束**，
于是 NPU 推理 / LPR 裁剪 / 显示转换都发生在帧还活着的时候。

### 11.3 挂机压力测试结果（`--lprnet --plate-cls 0,1 --decoder mpp`）

systemd 守护（`rk-pipeline.service`，`Restart=always`），采样器 `scripts/soak_monitor.sh`（30s/次）。

| 指标 | 结果 | 判定 |
|---|---|---|
| 时长 / 帧数 | **4.40 小时 / 396,360 帧**（约 25.5 fps） | — |
| 进程重启 | **0 次** | ✅ 全程没崩过一次 |
| RGA IRQ 错误 | **0 次新增**（修复前同配置每 300 帧必现） | ✅ |
| 内核 oops / external abort | **0** | ✅ |
| pstore | 空（无 panic） | ✅ |
| RSS | 105,252 ~ 108,684 KB，跨度 3,432 KB，**中段斜率 0.52 KB/小时** | ✅ 实质平线 |
| fd 数 | **34 ~ 34 恒定** | ✅ |
| 守卫计数 | 构 398,460 / 析 398,459 / 存活 1 | ✅ 严格 1:1 |

（存活=1 是因为统计行打在最后一帧守卫析构之前；该帧结束时即回到 0。）

### 11.4 一个必须记住的教训：修复前的"1 次/85k"是假基线

修复前那轮 85,620 帧的日志里，`[RGA] RGA 不可用` **只出现 1 次** ——
LPR 的 RGA 路径在第一次失败后就被自动禁用，剩下 85,000 帧全走 CPU 兜底
（而且还在读已释放的帧指针）。

也就是说：**生产系统早就悄悄退化成了"日志正常、内存已损坏"的状态，
而那条路径实际上已经死了很久。** 那个"1 次/85,620 帧"的漂亮数字，
测的是一条已经死掉的路径。

修复让 LPR 的 RGA 路径复活，也让它固有的竞态显现出来 —— 这才有了本次的定性定位。

### 11.5 仍然保留的隐患（未修，供后续）

- `license_plate_rec.cc:553` 的 wstride 单位错误仍在（该路径只在独立 LPR 测试工具里用，
  不在主链路）；`rga-only` 单元已能 A/B 复现其后果（越界 2,457,600 字节，首个越界字节在 1,232,640）。
- ROI 退化（扁 ROI）目前靠 CPU 兜底规避，更彻底的修法是给 ROI 加最小高度约束。
- `rk12ch` 仍处冻结状态，待根因彻底闭环后再解冻。


---

## 12. 收尾（2026-11-12）与发布

### 12.1 扫清遗留隐患

| 项 | 处理 |
|---|---|
| `license_plate_rec.cc:572` wstride 单位错误（P0-4） | 改为 `d.wstride = resized_w`（librga 的单位是**像素**）。同时修掉同函数里源缓冲 fd/vir_addr 混填（软件 MMU 的 PPA 错配风险）、源 wstride 的字节→像素显式换算、目标缓冲补 `kPadRows=16` 余量行 |
| ROI 退化（扁 ROI 逼疯 RGA） | `main.cc` 加 `kMinRoiH=16 / kMinRoiW=32` 约束：不够就扩（纵向向上扩保住底边——车牌在车尾），再夹回画面。源高 ≥16 时放大到 48 只需 3 倍，落在 RGA 舒适区 |

> **一个诚实的补充**：独立 LPR 测试（`test_license_plate_rec`）的断言**改前改后都通过**——
> 因为它的目标缓冲是精确大小，驱动自己的尺寸校验会挡住越界请求。
> P0-4 只在**目标缓冲足够大、能满足错误 wstride 的尺寸校验**时才发作，
> 这正是 `rga-only` 单元 A/B 实证的那条路径。**测试抓不到，不等于修得不彻底。**

验证：修复后 300 帧 RGA 错误 0 次，且不再出现"超出 RGA 缩放器承受范围"——
即 RGA 路径完全恢复，不再依赖 CPU 兜底（`rga=2.26ms`）。

### 12.2 资源清理

| 服务/进程 | 处理 |
|---|---|
| `rk-pipeline.service`（挂机复现器） | **停用 + 停止** |
| `rk-ignition.service`（引爆测试） | **停用 + 停止** |
| `soak_monitor.sh`（采样器） | **已杀** |
| `rk-kernel-log.service`（内核日志取证） | 停止但保留 enabled —— 下次开机自动恢复；它几乎不占资源，却是硬挂死时唯一的实时取证通道 |
| `rk-crash-archive.service`（开机自动抢收） | 保留 enabled（one-shot，运行期零开销） |

清理后：可用内存 5.1 GiB，load 回落。

### 12.3 最终交付物

| 类别 | 内容 |
|---|---|
| 基建 | `rk_base`：分级日志 / fd 台账 / canary 守卫 / 栈水位看门狗 / **AVFrameGuard + MppFrameGuard** |
| 最小化单元 | `rk_minimal`：`--decoder-only` / `--rga-only` / `--npu-only` 三种互斥模式 |
| 调查工具 | `tools/`：es_dump（码流录制）、sps_flip（SPS 就地改写）、gop_group、ts_flip（自封装 MPEG-TS）、au_feed（不经 FFmpeg 直喂 MPP）、**fd_close_probe**（验证解码器销毁即关 fd）、kern_log（每条 fsync 的内核日志） |
| 长跑/取证 | `scripts/`：longrun.sh、collect_crash.sh、soak_monitor.sh、pipeline_watchdog.sh、setup_netconsole.sh、crash_archive.sh |
| 文档 | 本文件（排查全过程与数据）+ `docs/YOLOV8_RK3588_PERF_CHECKLIST.md`（性能优化清单） |

### 12.4 版本 Tag

- `baseline/pre-crash-debug` —— 排查起点（带病代码，一行未改）
- `step/07.1-raii-guards` / `step/07.2-raii-lifecycle` / `step/07.3-errorpath-audit` —— 修复三步
- `fix/p0-1-raii-verified` —— 2 小时挂机验证通过
- **`release/v1.0-stable-core`** —— 稳定版发布点（当前 HEAD）

### 12.5 后续建议（不阻塞业务联调）

1. **`rk12ch` 解冻前，先把 `rk_base` 的观测能力接进去**（fd 台账 / 守卫 / 栈水位），
   否则 12 路并发一旦出事，又只能"日志正常、内存已损坏"。
2. **ROI 最小高度只是止血**，更彻底的做法是让检测框本身就带最小尺寸约束，
   避免退化到扁 ROI 再靠 CPU 兜底。
3. **长期挂机建议用 systemd 守护 + `rk-crash-archive`**，比人工盯着可靠。
4. 板上**没有硬件看门狗**，硬挂死只能人工断电；若要做无人值守，
   建议外接串口看门狗或改用带 RTC + PMIC 复位能力的方案。
