# npu_layer_bench —— RK3588 NPU 分层压测台

> 一个**完全独立**的压测台: 不接入主工程 `rk3588_zero_copy`, 不共用它的任何
> `.cc/.h`, 不改它的 CMakeLists, **一行主工程代码都不改**。
> 它只依赖板上的 `librknnrt.so` / `librga.so` + `rknn_api.h` / `im2d.h`。
>
> 目的: 逐层回答四个问题
> 1. **NPU 的算力天花板到底是多少?**
> 2. **生产链路的 93/95 FPS 是合理的, 还是被某个环节卡住了?**
> 3. **全链路的耗时分布长什么样?**
> 4. **还剩多少优化空间, 分别在哪一环?**

硬件: Rockchip RK3588(3 核 NPU, 6 TOPS; 8 核 CPU = 4×A76 + 4×A55)
运行时: RKNN Toolkit2 **v2.3.2**(`/usr/lib/librknnrt.so`)
模型: `ebike_int8_rk2332.rknn`(YOLOv8n INT8 640×640) + `ppocrv4_rec_fp16.rknn`(PP-OCRv4 rec)

---

## 一、5 分钟上手

```bash
cd npu_layer_bench
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j8

# 一键跑全部(会先停掉板子上的生产服务, 跑完自动恢复)
./scripts/run_layer_bench.sh --duration 30 --repeats 3
#   -> logs/layer_bench/<时间戳>/{results.jsonl, bench.log, layer_bench_report.md}
```

> ★ **板子必须"安静"**。脚本默认会停掉 `rk-gate` / 看门狗 / 长跑看护 等服务,
> 结束时**自动恢复**(含 Ctrl-C 和异常退出)。调试时可用 `--no-quiet` 跳过,
> 但那样测出来的是"负重"数据。

---

## 二、五个分层

| 层 | 测什么 | 在哪测 |
|---|---|---|
| **L1** | 纯 NPU 算力天花板(裸推理, 无前后处理) | 本进程自建流水线 |
| **L2** | 检测 + OCR 双模型纯推理(同线程串行) | 本进程自建流水线 |
| **L3** | RGA 预处理 + 检测推理 | 本进程自建流水线 |
| **L4** | 单路全链路(解码+RGA+检测+OCR+后处理) | **驱动生产二进制 `rk_gate`** |
| **L5** | 4 路全链路极限(解除限帧) | **驱动生产二进制 `rk_gate`** |

### ★ 为什么 L4/L5 是"驱动生产二进制"而不是在压测台里重写一遍

全链路 = 解码(MPP) + RGA 预处理 + NPU 检测 + 跟踪/测速 + NPU 识别 +
CTC 解码 + 结果上报 + MQTT。这条链路上有几十处业务逻辑(车牌两行切分、
候选并发、每核闸门、优先级队列、fd 守卫、帧缓冲组重建…)。

在压测台里重写一遍, 测到的就不是"生产那条链路的真实上限", 那是一个**简化版**,
拿来验证生产链路的 93 FPS 是不成立的。

所以 L4/L5 的做法是: **原样跑生产二进制 `rk_gate`**, 只改命令行参数
(核绑定 / 每路帧率上限 / 时长), 然后解析它自己打的统计行。
主工程代码一行未改, 只把它当黑盒驱动。

---

## 三、指标口径

| 指标 | 定义 | 为什么这么定 |
|---|---|---|
| **run / npu ms** | `rknn_run` + `rknn_outputs_get` | 这才是**真正占着 NPU 核**的时间 |
| **wall ms** | 从开始处理这一帧到结果就绪 | 业务口径, 含排队 |
| **两者之差** | = NPU 排队时间 | `detwait - npu` 就是排队(全链路口径) |
| **核占用(应用侧)** | Σ(该核上所有上下文的 run 墙钟) ÷ (窗口 × 该核上下文数) | 同核多上下文时**除以上下文数**, 上限才是 100% |
| **核占用(驱动侧)** | Σ(`RKNN_QUERY_PERF_RUN`) ÷ (窗口 × 该核上下文数) | 驱动自报的"真正在算"的时间 |
| **核占用(debugfs)** | `/sys/kernel/debug/rknpu/load` | 驱动侧的真实值, **需要 root** |
| **吞吐 FPS** | 完成帧数 ÷ 测量窗口(墙钟) | 并行下"各阶段耗时相加"没意义, 吞吐只能用墙钟算 |
| **CPU%** | 整机口径(8 核归一, 100% = 8 核跑满) | 与主工程 `run_4ch_gate_single.sh` 对齐 |
| **活堆** | `mallinfo2().uordblks` | 内存平线的直接证据(RSS 会被 arena 碎片干扰) |

### 测量窗口

```
t0        : 所有 worker 都建好上下文的那一刻(闸门放行)
warmup_end= t0 + warmup_sec
meas_end  = warmup_end + duration_sec
```

只有**跑完落在 `[warmup_end, meas_end)` 区间内**的帧才计入统计。
`FPS = 计入的帧数 ÷ (meas_end - warmup_end)`。

★ 两段式闸门是必须的: 时间戳要在**所有 worker 都就绪之后**算(否则第一个
worker 会比别的多等一段"建上下文"的时间, 它的第一个帧会被算成慢帧),
又要在 worker 开跑**之前**发布。

---

## 四、四条红线怎么落地的

| 红线 | 落地方式 | 怎么验证 |
|---|---|---|
| **零拷贝** | 输入用 `rknn_create_mem` / `rknn_create_mem_from_fd` 分配 —— 一块"NPU 可直读"的物理内存, 再用 `rknn_set_io_mem` 绑到输入张量。启动时往里写一次图案, **热循环里对输入缓冲的写次数 = 0** | L3 里 RGA 的目标缓冲**就是** NPU 的输入缓冲(同一块 dma-buf), 中间零 memcpy |
| **内存平线** | 启动时一次性分配: 输入 mem、**预分配输出缓冲**(`is_prealloc=1`)、采样数组、模型字节。测量窗口内零 malloc | `mallinfo2().uordblks` 斜率 + RSS |
| **零动态内存** | 采样数组在构造时按容量一次性分配, 热循环里只做"写一个 uint32 + 加一个计数" | 代码里热循环只有 `rknn_run` / `rknn_outputs_get` |
| **纯 C++ 多线程** | 只用 `std::thread` / `mutex` / `condition_variable` / 原子变量, 零第三方依赖 | `include/` 下只有 7 个头文件 |

### 与主工程的**故意**不同(以及为什么)

1. **输出缓冲 `is_prealloc = 1`**(主工程 `npu_pool.cc` 用的是 0)。
   `is_prealloc = 0` 意味着每帧由 runtime 去 malloc 一块输出缓冲 —— 那会直接
   破坏"内存平线"。
2. **`want_float = 1`**(主工程也是 1)。保持和生产的 `npu=` 口径一致,
   这样第 1 层的单帧耗时可以和生产的 `npu=` 直接比。
3. **不调 `rknn_outputs_release`**(主工程也不调)。多一次 runtime 调用就是
   多一次开销, 会让两边的数字对不上。
4. **输入绑定优先走"原生 NHWC uint8" + `pass_through = 1`**(和主工程
   `npu_pool.cc:298-329` 同一套判断), 驱动不做任何布局转换。

---

## 五、踩过的坑(全部来自本板实测)

### ★ RKNN 上下文不能跨线程

在 A 线程 `rknn_init`, 拿 B 线程 `rknn_run`, 运行时内部**死锁** ——
表现是所有线程卡在 futex, 一帧都不动。

所以本测试台把这件事做成**结构性的**: `NpuModel::setup()` 在
**worker 自己的线程**里被调用, 上下文在**本线程**创建、在**本线程**销毁。

### ★ `rknn_set_io_mem` 的绑定属性必须以运行时查出来的 attr 为底

运行时拿 attr 里的 `n_elems` / `size` 去校验内存大小。自己 memset 出来的
attr 这两个字段是 0, 运行时直接拒绝
(`input memory size(N) < model input size(M)`)。

### ★ RGA 的 `wstride` 单位陷阱

`RgaImage::wstride` 是**字节**, librga 的 `rga_buffer_t::wstride` 是**像素**。
NV12 是 1 字节/像素 → 两者数值相同; RGB888 是 3 字节/像素 → 要除以 3。

### ★ fd 有效时【不要】再填 `vir_addr`

两者都填, librga 会走"软件 MMU"那条路, 它会缓存 VA→PA 映射,
分配复用之后 PA 对不上 —— 堆被随机破坏。

### ★ librga 在一个进程里是串行的

每次调用都要 `importbuffer_fd` 注册缓冲, 那张全局 handle 表是
**进程内共享 + 加锁**的。多线程同时转就在这把锁上排队。
L3 的 `rga_3w` 用例专门量这个排队。

### ★ librga 打不开 `/dev/rga` 时会 SIGSEGV, 而不是返回错误码

`rga_init()` 是第一次调 `imcheck`/`improcess` 时才触发的, 它内部
`open("/dev/rga")`; open 失败时它只打印一行然后**继续解引用没初始化的
全局句柄**。所以必须**自己先 `open()` 一次**, 把原因查清楚再决定要不要
碰 librga。`RgaConverter::available()` 用信号探针兜住了这一段。

### ★ `/proc/stat` 和 debugfs 的解析都要跳过前缀

`/proc/stat` 第一行是 `cpu  12345 ...`, 直接 `sscanf(line, "%llu ...")`
会在 `"cpu"` 上就失败, 八个变量全是 0 → CPU% 永远是 0。

debugfs 的 NPU load 是一行 `NPU load:  Core0: 72%, Core1: 72%, Core2: 70%,`,
所以不能拿整行去 `sscanf("Core%d: %d%%")` —— 要在行里**逐个找 "Core"**。

### ★★ 锁 NPU 频率: `target_freq` 是只读的, 得用 `min_freq`/`max_freq`

实测: 这台板子(`/sys/class/devfreq/fdab0000.npu`)上

| 节点 | 可写? |
|---|---|
| `user_freq` | **不存在** |
| `target_freq` | **只读**(写会报 Permission denied) |
| `governor` | 可写, 但 `performance` 在这个 BSP 上不生效 |
| `min_freq` / `max_freq` | **可写** ← 正确的锁法 |

所以 `governor=userspace + target_freq=1GHz` 这套是**假的**: 看起来锁住了,
其实根本没锁。实测抓到过 NPU 实际跑在 900MHz(单帧推理 25.4ms 而非 23.0ms,
正好是 1000/900 = 1.111 的比), 而数据表面上完全看不出问题。

**正确做法**: `min_freq = max_freq = 1000000000`。
不管 governor 怎么想, NPU 都下不去。

★ 而且本工具把**测量窗口内的实际频率区间记进每一条结果**
(`npu_freq_min_mhz` / `npu_freq_max_mhz`), 报告里可以直接核对这一组数
是在什么频率下测的 —— 锁没生效的话一眼就能看出来。

### ★★ 停服务必须【先让看门狗闭嘴】

看门狗(`gate_watchdog.sh`)判断"要不要拉起 rk-gate"看的是单元的 `Result`:

- `Result == success` → 认为"人为停机", 尊重运维意图, 不拉起
- `Result != success` → 认为"崩了", 退避后**自动拉起**

而我们为了清掉孤儿进程会 `pkill rk_gate` —— 那会让 systemd 把单元标成
`Result=exit-code`, 看门狗于是**把我们刚停掉的服务又拉起来**。
实测: 压测跑到一半 rk_gate 自己回来了, 三颗核全被占, 那一组数据全废。

**顺序必须是**:

1. `touch /run/rk-gate/paused`(它自带的维护窗口开关)→ 看门狗闭嘴
2. 停所有 `rk-*.service`
3. 按进程名杀孤儿(`setsid` 起的子进程, 父脚本被 stop 后会被 init 收养)
4. **核对**没有残留进程

★ 而且服务清单要**动态枚举** `systemctl list-units | grep '^rk-'`,
不能手写 —— 手写的清单漏掉过 `rk-night-soak-monitor`(它会把别的服务拉起来),
结果压测跑到一半 CPU 突然飙到 98%。

### ★★ 怎么判断"这一组数据有没有被别的任务污染"

这块板子可能被别的会话同时用来跑压测(实测遇到过: 另一个会话的 RKNN
量化导出任务会**占用 NPU**)。最可靠的判据是**驱动侧逐核占用**:

> **我没有任何上下文的那颗核, 如果占用不是 0 —— 那就是别的任务在抢它。**

实测对照:

```
干净的 core0 样本:  load=[84,  0,  0]     ← 我只在 Core0 上跑
被污染的 core0 样本: load=[95, 70, 70]     ← Core1/Core2 上有别人
```

所以每个样本都记了 `ctx_per_core`(我在每颗核上挂了几个上下文)和
`npu_load`(驱动侧逐核占用), 报告时据此筛掉被污染的样本
(`scripts/render_report.py` 的 `foreign_load()` + `clean_window()`)。

★ 对"三颗核全占满"的用例没有空闲核可用, 退化为耗时判据
(单位耗时 > 最快样本 × 1.25 就剔除)—— 因为干扰只会让事情变慢, 不会变快。

---

## 六、参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--layer <spec>` | `all` | 跑哪几层, 逗号分隔: `all,L1,L2,L3,L4,L5,EX` |
| `--det-model <path>` | 必填 | 检测模型 |
| `--ocr-model <path>` | 空 | 识别模型(L2 必填) |
| `--gate-bin <path>` | 空 | 生产二进制 `rk_gate` 的路径(L4/L5 必填) |
| `--ch <spec>` | 空 | 一路: `name:src:near|high[:IPM四点]`(可重复) |
| `--fullchain-max-fps <n>` | `0` | 传给 `rk_gate` 的 `--max-fps`(0 = 解除限帧) |
| `--duration <sec>` | `30` | 每组测量窗口 |
| `--warmup <sec>` | `5` | 每组预热 |
| `--repeats <n>` | `3` | 每组重复次数 |
| `--workers <list>` | `1,2,4,6,9` | 第 1 层扫描的 worker 数 |
| `--src-w` / `--src-h` | `1920` / `1080` | 第 3 层模拟的 NV12 源尺寸 |
| `--res-model-640/576/512 <p>` | 空 | 分辨率扫描用的模型 |
| `--out <path>` | 空 | JSONL 结果输出路径 |

---

## 七、目录

```
npu_layer_bench/
├── CMakeLists.txt              # 独立构建, 不挂主工程
├── README.md
├── include/nlb/
│   ├── bench_types.h           # 配置 / 结果 / 指标口径
│   ├── harness.h               # 多 worker 测量骨架
│   ├── stat.h                  # 预分配的分位数采样器
│   ├── npu_model.h             # RKNN 零拷贝封装
│   ├── rga_pre.h               # RGA 预处理
│   ├── dma_buf.h               # 自包含的 dma-buf 分配器
│   ├── sys_probe.h             # CPU / RSS / 活堆 / NPU load 采样
│   └── layers.h                # 五层的入口
├── src/
│   ├── main.cpp                # CLI + 编排
│   ├── harness.cpp             # 测量骨架实现
│   ├── layer1_npu.cpp  layer2_dual.cpp  layer3_rga.cpp
│   ├── layer_full.cpp          # L4/L5: 驱动生产二进制
│   ├── layer_extra.cpp         # 分辨率扫描 + 算子回落统计
│   ├── npu_model.cpp  rga_pre.cpp  dma_buf.cpp
│   ├── stat.cpp  sys_probe.cpp  json.cpp
└── scripts/
    ├── run_layer_bench.sh      # 一键跑全部(含停服务/锁频/恢复)
    ├── render_report.py        # JSONL -> Markdown
    ├── make_res_models.sh      # 生成分辨率扫描用的 RKNN 模型
    └── resize_onnx.py          # 改 ONNX 的输入尺寸
```

---

## 八、复现与排错

```bash
# 单组快速验证
./build/npu_layer_bench --layer L1 --det-model ../models/ebike_int8_rk2332.rknn \
    --duration 10 --warmup 3 --repeats 1 --workers 1,3

# 只跑全链路(需要生产二进制 + 视频源)
./build/npu_layer_bench --layer L5 \
    --det-model ../models/ebike_int8_rk2332.rknn \
    --ocr-model ../models/ppocrv4_rec_fp16.rknn \
    --gate-bin ../rk3588_zero_copy/build/rk_gate \
    --ch "ch1:/home/seeed/testvideo3.mp4:near" ... \
    --fullchain-max-fps 0 --duration 30
```

| 现象 | 原因 | 处理 |
|---|---|---|
| 单核推理比 26 ms 慢一大截 | 板子上有别的进程在抢 NPU | 用 `run_layer_bench.sh`(它会停服务); 或 `ps aux \| grep rk_gate` |
| `npu_load` 全是 -1 | debugfs 需要 root | 用 `sudo` 跑; 或看应用侧核占用 |
| `rknn_set_io_mem 失败` | 绑定属性没以运行时查出来的 attr 为底 | 见 §五 |
| 推理线程全卡死 | RKNN 上下文跨线程了 | `NpuModel::setup()` 必须在 worker 线程里调 |
