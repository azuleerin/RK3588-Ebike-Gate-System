# RK3588 工程化落地与避坑指南

> **副标题**: 从"点亮硬件三件套"到"无人值守长跑" —— 一次把 RK3588 边缘 AI 推理链路做成产品的完整排雷记录
>
> 适用板卡: Seeed Studio Recompute Host (RK3588, 8 核 A55+A76, 三核 NPU, 6 TOPS)
> 项目: 路口电动车检测 + 车牌识别 + 测速 + MQTT 上报
> 记录时间: 2026-11-10 ~ 2026-11-13
> 配套文档: 《系统能力对比表》/《演示流程傻瓜式手册》/《遗留风险清单》

---

## 摘要: 这三天解决了什么

| 阶段 | 问题 | 根因 | 结果 |
|---|---|---|---|
| 点亮 | MPP/RGA/NPU 三件套各自不通 | 版本对齐、dma-buf 契约、输入格式契约 | 全链路跑通, **零拷贝**(一帧图像只存在一份物理内存) |
| 死机 | 跑几百帧就整机硬挂死 | **MppFrame 悬垂**: 帧释放了, RGA 的异步 DMA 还在读它的 fd | RAII 守卫 + 极值保护, **4.4 小时 / 39 万帧零崩溃** |
| 泄漏 | 活堆每帧涨 416 字节, 24h 涨 1GB | `av_frame_unref()` **不释放 AVFrame 结构体** | 降幅 **94%**, 长跑活堆打平 |
| 性能 | 25 FPS 上限, 不知道卡在哪 | 逐段打点 + 硬件负载 | 结论: **上限是相机源(25fps)**, 不是算力 |
| 精度 | INT8 量化损失未知 | 同源 ONNX 受控对比 | **损失 << 2%, 可直接交付** |
| 调度 | NPU 三个核只用了一个 | 单实例默认单核; `CORE_0_1` 对该模型无效 | 双实例双缓冲并行, 检测吞吐 **1.95×** |
| 交付 | 长跑没人盯着 | systemd + 看护 + 自动出报告 | 崩溃 5 秒自愈, 早上来直接看报告 |

**一句话经验**: RK3588 上真正耗时的从来不是"调通", 而是**把每个硬件模块的隐式契约一条条抠出来**。
本文把抠出来的契约全部列在这里, 后面的人不用再踩一遍。

---

## 第 0 章: 最终形态

```
   RTSP(H.264 1080p25)
        │
        ▼  FFmpeg(只做拉流)
   ┌────────────┐
   │ MPP 硬解    │  VPU   0.49 ms/帧
   └────────────┘        │
        │  dma-buf fd     │  ★ 同一块物理内存, 零 CPU 拷贝
        ▼                │
   ┌────────────┐        │
   │ RGA 预处理  │  RGA   2.31 ms/帧   NV12 → RGB888(640x640)
   └────────────┘        │
        │  同一块 dma-buf  │
        ▼                │
   ┌────────────┐        │
   │ RKNN 推理  │  NPU   28.33 ms/帧   YOLO 检测 + LPRNet 识别
   └────────────┘        │
        ▼                │
   ┌────────────┐        │
   │ CPU 后处理  │  CPU   0.46 ms/帧   DFL+NMS+几何过滤+跟踪+测速
   └────────────┘        │
        ▼                │
   ┌────────────┐        │
   │ MQTT 上报   │        │ 臻识 ivs_result 协议, 车牌 Base64
   └────────────┘        ▼
                     整帧 40.02 ms = 源的推送间隔
```

**关键认识**: 整帧耗时 40.02ms **恰好等于相机的推送间隔**。流水线是"刚好跟上", 不是"被算力卡住"。
这个结论只有把每一段都打点、并且同时看硬件负载, 才能确认。

---

## 第 1 章: 点亮硬件三件套

### 1.1 MPP 硬解: 版本对齐是生死线

**坑**: 工程目录里留着旧版 MPP 头文件(20190626), 板子自带的是新版 BSP(1.5.0)。
头文件里定死了结构体布局和 `MppApi` 函数指针表的顺序 —— **编的时候用旧头, 运行时加载新库,
`mpp_create()` 之后的第一次调用就按错偏移跳进野地址 → SIGSEGV**。

**解法**(已固化进 `CMakeLists.txt`):

```cmake
# 头文件只从系统目录找, 断掉"从工程目录捡到旧 MPP"的路
find_path(MPP_INC_DIR rk_mpi.h
          PATHS /usr/include/rockchip /usr/local/include/rockchip
          NO_DEFAULT_PATH)
# 库也一样
find_library(MPP_LIB rockchip_mpp
             PATHS /usr/lib/aarch64-linux-gnu /usr/lib /usr/local/lib
             NO_DEFAULT_PATH)
# 工程目录里只要发现 MPP 头文件, 直接 FATAL_ERROR
```

> **教训**: 第三方头文件/库的版本不一致, 编译期完全看不出来(结构体大小可能碰巧一样),
> 表现为运行期随机 SIGSEGV。**必须在构建系统里做"版本排他检查"**, 而不是靠人记住。

**兜底**: MPP 初始化包在 `rk::SafeProbe::call()` 里(手写 signal handler 捕获 SIGSEGV/SIGBUS),
初始化崩了自动退回软解, 不会带着整机一起挂。

**其他 MPP 细节**:
- `extradata` 不是 Annex-B 时, 部分 rkmpp 版本解析不出 SPS → 启动时告警。
- **10bit HEVC 不支持**(本地 DJI 素材就是 `yuv420p10le`) → 硬件路直接放弃, 走软解 + CPU 颜色转换。
- MPP 解出来的图高度会**对齐到 16**, 用 `frame->height` 当 NV12 的 plane 高度会导致下半屏花屏。
  正确做法: 用 `(data[1] - data[0]) / linesize[0]` 反推真实行数(见 README 坑①)。

### 1.2 RGA 零拷贝: 源和目标**都**必须是真 dma-buf

**坑**: RGA 的软件 MMU 在 **4GB 以上的用户态地址**上会失败, 而且失败时**会写坏堆**
(踩过: 直接把 RGA 目标换成 malloc 内存, 进程随机 abort)。

**解法**: 整条链路的每一块缓冲都用 dma-buf:
- 解码帧: MPP 出来的 dma-buf fd
- RGA 输出: `rk::dma_buf_alloc()` 出来的 fd
- NPU 输入: 同一个 fd, 通过 `rknn_create_mem_from_fd` 绑定给 NPU

**任何一端是普通内存, RGA 就退回 OpenCV CPU 转换**(功能不丢, 但慢 7.6 倍)。

> **教训**: "零拷贝"不是一句口号, 它是一条**端到端的 dma-buf 契约**:
> 解码器 → RGA → NPU, 三段必须共用同一块物理内存, 任何一段断了就退化成拷贝。

### 1.3 NPU 输入契约: 喂 uint8 还是 float32, 看 `type` 不看 `qnt_type`

这是本次**最贵的坑**(花了几个小时才定位)。

```c
// rknn_tensor_type: FLOAT32=0, FLOAT16=1, INT8=2, UINT8=3
const bool is_float = (attr_logic.type == RKNN_TENSOR_FLOAT32 ||
                       attr_logic.type == RKNN_TENSOR_FLOAT16);   // ★ 别漏 FLOAT16
```

| 逻辑输入 type | 该喂什么 | 喂错的后果 |
|---|---|---|
| INT8 / UINT8 | uint8 NHWC **原始像素 [0,255]** | 运行时自己量化, 正常 |
| FLOAT32 / FLOAT16 | float32 NHWC **[0,1]**(即 /255) | 喂 uint8 → 输出全乱, 每帧 6000+ 个假框 |

**两个反直觉的点**:
1. **FP16 模型的逻辑输入是 `FLOAT16(1)`, 不是 FLOAT32**。只判 `type == FLOAT32` 会把 FP16 模型
   当成量化模型 → 喂 uint8 → 输出一片噪声。**这就是"板上编出来的 FP16 .rknn 为什么是坏的"**。
2. **`qnt_type` 不能用来判断**。FP16 模型的 `qnt_type` 依然是 `AFFINE_ASYMMETRIC`,
   按 qnt_type 判断必然错。**判据是逻辑输入的 type**。

配套还有两条:
- **喂 float32 时布局必须跟模型的 `fmt` 一致**(NHWC 还是 NCHW)。填错的话 `inputs_set` 会失败,
  模型拿到全 0 → **每张图输出都一样**, 看着像"模型只输出固定框"。
- **cls 张量已经是概率, 不要再 sigmoid**(README 坑⑧)。

### 1.4 NPU 输入内存要"绑定", 光写进 dma-buf 没用

**坑**: 只把像素写进 dma-buf, 然后 `rknn_run(ctx, nullptr)` → 模型读到的是**没绑过的内存**, 检测框一个都没有。

```c
in_mem = rknn_create_mem_from_fd(ctx, model_fd, model_ptr, model_buf_size, 0);
bind_attr = attr_logic;
bind_attr.pass_through = 0;
bind_attr.type = RKNN_TENSOR_UINT8;
bind_attr.fmt  = RKNN_TENSOR_NHWC;
bind_attr.n_dims = 4;
bind_attr.dims[0] = 1; bind_attr.dims[1] = model_h;
bind_attr.dims[2] = model_w; bind_attr.dims[3] = 3;
rknn_set_io_mem(ctx, in_mem, &bind_attr);      // ★ 这一步不能少
```

用 `RKNN_QUERY_NATIVE_NHWC_INPUT_ATTR` + `pass_through=1` 还能再省一次 NCHW→NHWC 转换。

### 1.5 RKNN 上下文不能跨线程用

在 A 线程 `rknn_init`, 拿到 B 线程 `rknn_run` → **运行时内部死锁**(所有线程卡在 futex)。
本次做 NPU 双核并行时踩到: 300 帧卡了 10 分钟毫无动静。

**解法**: 每个实例的上下文**在它自己的工作线程里创建和使用**。

> **教训**: 异步/硬件 SDK 的"上下文"通常隐含线程亲和性。文档不一定写, 但一用就死。

---

## 第 2 章: 死机排查 (P0-1 / P0-2)

### 2.1 现象与取证

跑几百帧后整机硬挂死, 无 OOM、无应用崩溃日志, 只有内核侧
`rga: irq handler err! INTR[0x2099]` + `soft reset complete`。

### 2.2 排查方法论: 最小化单元 → 无罪 → 引爆

**第一步: 拆成三个只碰一个硬件模块的最小单元**(自研 `rk_minimal`):

| 单元 | 只碰 | 结论 |
|---|---|---|
| `--decoder-only` | MPP 解码 | 无罪 |
| `--rga-only` | RGA 转换 | 无罪 |
| `--npu-only` | RKNN 推理 | 无罪 |

三个都干净 → **必然是多模块交互**。

**第二步: 主动"引爆"**。不靠等它随机崩, 而是写代码**故意制造**悬垂:
用一个小 dma-buf 占住被释放的 fd 号, 逼 RGA 去 DMA 一个已失效的 fd; 并且把引爆点
**挪到 LPR 裁剪之后**(RGA 的 DMA 是异步的, 裁剪那一刻还没真正发出去)。

> **这是本次最有价值的方法论**: 被动等崩溃靠运气, 主动引爆能 100% 复现,
> 而且能把"哪一行之后释放"和"哪一行还在用"精确对齐。

### 2.3 根因: 悬垂的是 MppFrame, 不是 AVFrame

代码里有一句 `av_frame_unref(frame)` 在 LPR 裁剪**之前**, 而 LPR 的 RGA 裁剪用的
是那一帧的 fd —— 帧已经被 unref, fd 可能已被复用, RGA 的异步 DMA 写到了别人的内存。

**但真正的问题比这更隐蔽**: `AVFrame` 里是**空的**(走的 MPP 路径, 像素根本不在 AVFrame 里),
**真正悬垂的是 `MppFrame`**。所以只盯 `AVFrame` 的人会完全看不到问题。

### 2.4 解法: RAII 守卫 + 极值保护

```cpp
// 每轮循环一个守卫, 作用域到循环结束 -> 任何 continue/return/异常展开都会释放
rk::MppFrameGuard mpp_frame_guard;   // 托管 MppFrame(结构+缓冲一起)
rk::AVFrameGuard  av_frame_guard;    // 托管 AVFrame
```

守卫**接管所有权**, 释放路径只有一条, 不可能漏。同时修掉两个连带问题:
- `wstride` 单位错误(应填像素数, 不是字节) —— README 坑②
- ROI 最小尺寸约束(`kMinRoiH=16 / kMinRoiW=32`): 检测框退化时 ROI 会变成
  `780x8` 这种极扁的矩形, 纵向放大 6 倍超出 RGA 缩放器承受范围, **硬件直接报错并软复位 core**。
  实测: 默认参数**每 300 帧必现一次**, 加高后 900 帧 0 次。

### 2.5 验证

**4.4 小时 / 39 万帧零崩溃, RGA IRQ 错误零新增。**

---

## 第 3 章: 内存泄漏

### 3.1 先分清"碎片"和"泄漏"

| 指标 | 含义 | 判据 |
|---|---|---|
| `VmRSS` (from /proc) | 进程占的物理内存 | 会被 glibc 的 arena 保留/碎片干扰, **会假涨** |
| `mallinfo2().uordblks` | **正在使用**的堆字节数 | 只增不减才是真泄漏 ✅ |

```c
struct mallinfo2 mi = mallinfo2();
fprintf(stderr, "... heap=%zuKB ...", (size_t)mi.uordblks / 1024);
```

> **教训**: 排查内存问题, **第一件事是选对指标**。用 RSS 排查泄漏, 会被 malloc 碎片带偏,
> 白花几天时间。

### 3.2 排查方法论(四步, 这次真凶就是这么找到的)

1. **二分法缩小范围**: 同一台板子、同一路流, 只换**一个变量**。
   本次: `--decoder mpp` (+0.049 KB/帧) vs `--decoder ffmpeg` (+0.612 KB/帧)
   → 泄漏在 FFmpeg 分支, RGA/NPU/跟踪/LPR 全部排除。
2. **加探针再二分**: 在流水线里插临时开关(`RK_SKIP=afterdec|afterrga`),
   把"只跑到解码"和"解码+RGA"分别测一遍 → 泄漏 100% 在解码段。
3. **写个最小对照程序**: 把同样的 60 行逻辑抽出来单独跑 → **不漏**。
   差异只剩一处: 主程序每帧 `av_frame_alloc()` 新建帧, 对照程序复用一个。
4. **去读头文件**: `av_frame_unref()` 的文档原话是
   "Unreference all the buffers referenced by frame and reset the frame fields"
   —— **它不释放结构体本身**, 那是 `av_frame_free()` 的活。

### 3.3 真凶

```c
- av_frame_unref(frame_);      // 只放缓冲区, 结构体(416 字节)永远不还
+ av_frame_free(&frame_);      // = unref(缓冲区) + free(结构体)
```

`sizeof(AVFrame) = 416` 字节 —— 和实测的 +0.42 KB/帧 **完全吻合**。

**为什么 MPP 硬解路径一直没暴露?** 因为它走 `MppFrameGuard`, 用 `mpp_frame_deinit()`
把结构和缓冲**一起**释放了。软解路径每帧一个 AVFrame, 所以必漏。

**顺带修掉一个隐藏 bug**: 拷贝构造里 `av_frame_ref(other.frame_, frame_)` 的 dst 是空指针,
`av_frame_ref` 要往 dst 里写 → 段错误。现在先 `av_frame_alloc()` 再 ref。

### 3.4 被证伪的三个假设(省得后人重走一遍)

| 假设 | 实测 | 结论 |
|---|---|---|
| 送包后不立刻 `av_packet_unref`(包所有权交给解码器) | +0.453 KB/帧 ❌ | FFmpeg 文档明写 "ownership remains with the caller", **送包后的 unref 必须保留** |
| `has_b_frames=0` + `LOW_DELAY`(关重排序) | +0.451 KB/帧 ❌ | 无关 |
| 抽干解码器队列释放滞留帧 | **崩溃**(TBB abort), 且斜率涨到 +6.1 KB/帧 ❌ | 释放多余帧破坏了 OpenCV 内部状态 |

### 3.5 残留的"台阶": 跟踪器 vector 的高水位

`SpeedTracker::tracks_` 是 `std::vector<Track>`, 而 `sizeof(Track) ≈ 4.1KB`
(里面有 4 组环形缓冲: 速度历史 128×2、显示样本 96×2、像素轨迹 64×3)。
轨迹数涨上去时 vector 扩容 → **一次台阶 +65KB / +129KB**(容量 16→32→64)。
这是**高水位分配, 不是泄漏**, 但会让"活堆直线"看起来有台阶。

**结论**: 判断泄漏要看**尾段斜率**, 而不是全程斜率(全程会把台阶算成持续上涨)。

### 3.6 内存三件套(工程化收尾)

| 措施 | 作用 |
|---|---|
| `--mem-trim-frames/--mem-trim-sec` + `malloc_trim(0)` | 把 arena 里空闲的还给内核, RSS 不再缓慢爬升 |
| `--log-file` + 每 5s `fdatasync` + logrotate `copytruncate` | 日志脏页不堆 Page Cache(cgroup 限额满会被 OOM 杀掉, **现场表现像内存泄漏**) |
| fd 台账 + 守卫计数接入业务二进制 | 泄漏/未登记 fd 都能在 `[frame]` 行一眼看到 |

> `copytruncate` 是必须的: 应用用 `fopen("a") + dup2` 接管了 stderr, 文件是**打开着的**,
> 默认的 `create` 方式(改名+新建)会让应用继续写老 inode, 新文件永远是空的。

---

## 第 4 章: 性能诊断 —— 瓶颈在源, 不在算力

### 4.1 打点

主循环加 `t_wait`(等流) / `t_decode` / `t_rga` / `t_npu` / `t_post` / `t_total`,
外加 `src_gap`(相邻两帧解码出来的间隔 = 相机真实推送节奏)。

### 4.2 结论

| 路径 | FPS | 等流 | 解码 | RGA | NPU | 后处理 | 我们的活儿 |
|---|---|---|---|---|---|---|---|
| MPP 硬解 | 24.99 | 8.42 ms | **0.49** | **2.31** | 28.33 | 0.46 | 31.59 ms (79%) |
| FFmpeg 软解 | 17.52 | 0.38 ms | 18.42 | 11.42 | 26.37 | 0.48 | 56.69 ms (**99%**) |

- **相机推送 24.98 fps = 40.02 ms**, 整帧耗时也是 40.02 ms → **25 FPS 是源的天花板**。
- 但我们的活儿占 79%, 只剩 21% 余量 → **不是"闲着等源"**。
- 换 30fps 的相机, 最多跑到 ~31 fps(31.6ms/帧)。
- 软解路径是**算力打满**(99%), 而且 RGA 因为拿到的是 CPU 指针而走慢路(11.42 vs 2.31 ms)。

### 4.3 必须同时看硬件负载

光看耗时会误判。用 `/sys/kernel/debug/rknpu/load`(需 root)和 `/proc/mpp_service/load`:

```
NPU: Core0 48%  Core1 0%  Core2 0%      ← 单实例默认只用一个核
VPU: rkvdec 1.9%                          ← 硬解几乎空转
CPU: 21.6%
FPS: 25.05
```

**没有任何硬件单元打满, 流水线却卡在 25 FPS** → 只能是源。

> `/proc/mpp_service/load` 要先 `echo 1000 > /proc/mpp_service/load_interval`。
> `/sys/kernel/debug/rga/load` 在本内核**不存在**(rktop 走别的接口能显示 RGA)。

---

## 第 5 章: 精度回归 (INT8 vs FP16)

### 5.1 能做什么、不能做什么

| 想要的 | 有没有 |
|---|---|
| 同一 ONNX 的 INT8 / FP16 受控对比 | ✅ `_bench/dl/yolov8n_optimized.onnx` 两份都编出来了 |
| 生产模型的 FP16 对照 | ❌ 板上没有它的 ONNX/PT 源模型 |
| 带人工真值的标注集 | ❌ 全盘没有任何标注文件 |

所以只能做**同源 ONNX 的受控对比**, 而且要用**模拟器**跑:
板上两个 .rknn 的预处理喂法不同(INT8 收 uint8, FP16 收 float32/255),
像素经过两次不同的插值, 差异会被混进来。模拟器里两个模型吃的是**同一个 numpy 数组**,
差异 100% 来自量化。

### 5.2 结论

```
类别逐格一致率      97.47%
有把握格子一致率     80.45%   (分数>0.01 的格子, 都在噪声带里, 两边都在乱猜)
最大类别概率平均漂移  0.00037  (0.04 个百分点)
DFL 框坐标平均漂移    0.070 格 (~0.56px @stride8)
```

**INT8 的量化损失 << 2%, 可以直接交付, 不需要重做量化校准集。**

### 5.3 顺带发现的两个真问题

1. **生产 LPRNet 读不出绿牌(新能源)**。真实车牌 `苏N A339058`(字符集里没有汉字,
   预期输出 `A339058`), int8 喂法读成 `1`, float 喂法读成 `E1` ——
   **两种喂法都错, 所以不是量化问题, 是模型域问题**, 要补绿牌样本重训。
2. **板上编出来的 FP16 .rknn 是坏的**: 同一个 ONNX 在模拟器里完全正常, 但在板子上
   输出恒定噪声(每帧 8874 个 class-66 框, 换图结果一样 = 没吃到输入)。
   三种喂法(uint8 / float32 /255 / float16 /255)全是垃圾。

---

## 第 6 章: NPU 多核调度

### 6.1 实测(300 帧, 同一批图, 墙钟计时)

| 策略 | 单帧墙钟 | FPS | Core0 | Core1 | 结论 |
|---|---|---|---|---|---|
| auto(基线) | 30.0~31.8 ms | 31~33 | 38.4% | **0%** | 单实例默认只用一个核 |
| A: YOLO→core0, LPR→core1 | 30.0~31.9 ms | 31~33 | 38.4% | 0% | **串行流水线分核没意义** |
| C: YOLO→`CORE_0_1` | 30.4~30.5 ms | 32.8 | 37.3% | **4.1%** | **双核模式对这个模型无效** |
| B-seq: 两实例**交替** | 30.2~31.3 ms | 31~33 | 18.7% | 18.9% | **不可能更快** |
| **B-par: 两实例双缓冲并行** | **15.4~15.6 ms** | **64~65** | 28.1% | 28.7% | ✅ **1.95×** |

### 6.2 三个必须澄清的概念

1. **"交替推理"在数学上不可能提吞吐**: 单线程轮流跑两个实例, 总时间还是 2×单次时间。
   真正能提吞吐的是**并行**: 两个实例**同时**处理相邻两帧(双缓冲流水线)。
2. **`CORE_0_1` 双核模式 ≠ 把模型拆到两核**。它只是"允许用两个核",
   而驱动发现这个模型(算子并行度不够)拆不动 → Core1 只占 4%。
3. **串行流水线分核是负优化**: 两个模型本来就轮流占 NPU, 分核只是让每个模型
   失去"用多核算自己"的机会(单核跑 YOLO 实测 25ms vs auto 22ms)。

### 6.3 落地

`--npu-pipe 2`(默认 1 = 保持原串行路径)。正确性验证: 同一段确定性输入下,
串行与并行的检测结果 **15/15 个采样点完全一致**。

**现在不建议开**: 相机只有 25fps, 端到端 FPS 不会变, 白加一个线程和一份内存。
**等相机升到 30fps+ 再开**(串行路径在 25fps 已占 69% 帧预算, 30fps 就会丢帧;
并行检测能撑到 64fps)。

### 6.4 性能对比的纪律

第一轮测出"`CORE_0_1` 慢 21%" —— 那是**测量污染**: 那一轮 rktop 抓屏在同时跑, 抢了 CPU。
重跑 3 轮才发现是持平。

> **性能对比绝不能有别的采样进程在旁边跑。** 这条应该写进所有测试规范。

---

## 第 7 章: 端到端联调 (MQTT)

现场勘察发现 PC 上**本来就有 ActiveMQ**(192.168.31.30:1883), 直接用。

```
[MQTT] 连接成功 192.168.31.30:1883, topic=device/RK3588-12CH-01/message/up/ivs_result (QoS=1)
[MQTT] 报文发送成功 (frame 200, ..., 534 字节)
```

300 帧 → 300 条全部投递成功, 订阅端实收 300 条。报文是臻识 `ivs_result` 协议,
车牌 UTF-8 Base64。

**MQTT 断线不崩**: broker 不可达时输出 `[MQTT] 将每 5 秒自动重连, 不影响视频流水线`
(`MqttPublisher::send_all` 失败 → `close_broken()` → `fd_ = -1` → 下一帧走重连分支)。

自己写了一个零依赖的 MQTT→网页看板(`scripts/mqtt_web.py`, 内含 MQTT 3.1.1 客户端),
现场验证"网页端能不能收到真实 JSON"。

---

## 第 8 章: 可复用的排查套路

| 场景 | 套路 | 本次实例 |
|---|---|---|
| **随机硬挂死** | 最小化单元拆分 → 主动引爆 → 对齐"释放点"与"使用点" | P0-1/P0-2 |
| **内存缓慢上涨** | 选对指标(`uordblks` 不是 RSS) → 二分换变量 → 加探针 → 最小对照程序 → 读头文件 | AVFrame 结构体泄漏 |
| **性能不如预期** | 逐段打点 + 同时看硬件负载 → 判断是"源限制"还是"算力限制" | 25fps 瓶颈定位 |
| **怀疑精度问题** | 同源 ONNX 受控对比(模拟器, 同一输入) + 量化损失看"分数漂移/框漂移"而不是只看 mAP | INT8 vs FP16 |
| **硬件能力没用满** | 直接读 `/sys/kernel/debug/rknpu/load` 对照 mask 设置 | NPU 三核只用了一个 |
| **无人值守** | systemd `Restart=always` + `StartLimitIntervalSec=0` + 看护进程(采样/告警/卡死拉起) + 自动出报告 | 夜间长跑 |

**一条通用原则**: 每次排查都要留下**可复现的开关**(环境变量或命令行参数)和**可复现的数据**,
否则结论无法被别人验证, 下一个问题还得从头再来。

---

## 附录 A: 工具清单

| 工具 | 用途 |
|---|---|
| `scripts/heap_slope.py` | 活堆斜率/水平线判定(尾段斜率 + 100MB 红线) |
| `scripts/ab_heap.sh` | 一键测活堆斜率(600 帧) |
| `scripts/hw_load.sh` / `hw_load_chart.py` | NPU/VPU/CPU/FPS 采样 + SVG 监控图 |
| `scripts/quant_compare.py` | 两份检测 JSON 的一致性/mAP/漂移对比 |
| `scripts/sim_quant_compare.py` | 模拟器级 INT8 vs FP16 张量对比 |
| `scripts/mqtt_sub.py` / `mqtt_web.py` | 零依赖 MQTT 订阅端 / 网页看板 |
| `scripts/npu_sched_run.sh` | NPU 调度六策略 + 负载截图 |
| `scripts/night_soak_*.{sh,py}` | 夜间长跑: 包装器 / 看护 / 报告生成 |
| `build/rk_eval` | 检测精度评测(逐图检测框 JSON) |
| `build/rk_lpr_eval` | 车牌评测(自动找绿牌 / int8 vs float) |
| `build/npu_sched_bench` | NPU 调度专项基准(墙钟) |
| `build/rk_minimal` | 三硬件模块最小化单元(排雷用) |
| `build/lpr_test` | 单图车牌识别 |

## 附录 B: 关键标签

| 标签 | 含义 |
|---|---|
| `baseline/pre-crash-debug` | 崩溃前的现场基线 |
| `step/07.1-raii-guards` | RAII 守卫落地 |
| `step/07.2-raii-lifecycle` | 帧生命周期重构(P0-1/P0-2 修复) |
| `step/07.3-errorpath-audit` | 错误路径审计 + fd 台账 |
| `fix/p0-1-raii-verified` | 死机修复验证通过(4.4h/39万帧) |
| `task1/zero-growth-prelongrun` | 内存 Zero-Growth 长跑 |
| `fix/avframe-leak` | AVFrame 结构体泄漏修复 |
| `task2-3-4/bench-and-diagnosis` | 精度回归 + 核调度 + MQTT 联调 |
| `task3/accuracy-bench-mqtt` | 同上(合入 main) |
| `release/v1.0-stable-core` | 稳定版核心 |
