# 结案报告：FFmpeg 活堆泄漏 + FPS 瓶颈 + 硬件基准 (2026-11-12)

> 结论先行：
> **泄漏的真凶不是 FFmpeg 的包，是 `AVFrameGuard` 只调了 `av_frame_unref()` 没调 `av_frame_free()`，
> 每帧漏掉一个 416 字节的 AVFrame 结构体。**
> 25 FPS 的上限**确实是相机源**（源推送 24.98 fps = 40.02 ms），但我们的流水线已经吃掉了
> 其中 79% 的时间预算 —— 不是"等源等到闲着"。

---

## 一、任务 1：FFmpeg 活堆泄漏（结案）

### 1.1 现象

| 阶段 | 活堆斜率 (mallinfo2.uordblks) | 24h 外推 |
|---|---|---|
| 修复前 | **+0.612 KB/帧** | ~997 MB/24h |
| 修复后 | **+0.036 KB/帧**（且是阶跃后打平，不是线性） | 见 1.5 |

### 1.2 定位过程（可复现）

1. **二分法**：同一台板子、同一路流，只换解码后端：

   | 解码后端 | 活堆斜率 |
   |---|---|
   | `--decoder mpp`（VPU 硬解） | +0.049 KB/帧 |
   | `--decoder ffmpeg`（软解） | +0.612 KB/帧 |

   → 泄漏在 FFmpeg 这条分支上（RGA/NPU/后处理/跟踪/LPR 全部排除）。

2. **拆段探针**（临时加 `RK_SKIP=afterdec|afterrga` 开关）：
   - 只跑到解码：+0.426 KB/帧 → 泄漏在解码段；
   - 解码 + RGA：+0.423 KB/帧 → RGA 无辜；
   - 结论：泄漏 100% 在「取包 + 送解码器 + 取帧」这一段。

3. **对照实验**：把这段逻辑单独抽成一个 60 行的 C 程序（`dec_loop`），
   同样的 `av_read_frame → avcodec_send_packet → avcodec_receive_frame → unref`，
   **不漏**（+0.03 KB/帧）。差异只剩一处：主程序每帧 `av_frame_alloc()` 新建一个
   AVFrame，而对照程序复用一个。

4. **对拍 FFmpeg 头文件**（`/usr/include/aarch64-linux-gnu/libavutil/frame.h`）：
   > `av_frame_unref()`：Unreference all the buffers referenced by frame and reset the frame fields.
   > —— 它**不释放 AVFrame 结构体本身**；那是 `av_frame_free()` 的活。

   `sizeof(AVFrame) = 416` 字节，和实测的 +0.42 KB/帧 **完全吻合**。

### 1.3 真正的修复

`src/rk_base/av_frame_guard.cc`：

```c
- av_frame_unref(frame_);          // 只放缓冲区, 结构体(416B)永远不还
+ av_frame_free(&frame_);          // = unref(缓冲区) + free(结构体)
```

顺带修掉一个**隐藏 bug**：拷贝构造函数里 `av_frame_ref(other.frame_, frame_)` 的
`dst` 是空指针（`av_frame_ref` 要往 dst 里写），现在改成先 `av_frame_alloc()` 再 ref。

> 为什么 MPP 硬解路径一直没暴露？因为它走 `MppFrameGuard`，用 `mpp_frame_deinit()`
> 把结构和缓冲**一起**释放了。软解路径每帧一个 AVFrame，所以必漏。

### 1.4 被证伪的三个假设（都实测过，斜率一模一样，别再试了）

| 假设 | 实测结果 |
|---|---|
| 送包后不立刻 `av_packet_unref`（方向 1） | +0.453 KB/帧 ❌ |
| `has_b_frames=0` + `AV_CODEC_FLAG_LOW_DELAY`（方向 2） | +0.451 KB/帧 ❌ |
| 抽干解码器队列 | 崩（TBB abort），且斜率反而涨到 +6.1 KB/帧 ❌ |

另外，FFmpeg 7.1 的 `avcodec_send_packet` 文档明确写着
**"Ownership of the packet remains with the caller"** —— 送包后的 `av_packet_unref`
**必须保留**，删掉它不符合 API 语义。

### 1.5 残余：LPR 车牌缓存（已修）

`LprNet::cache_` 是按 `track_id` 建的 map，而 track_id 单调递增、旧 track 再也不回来，
不清理就只增不减。现在每次写入后按 TTL 删掉过期条目。

### 1.6 验收

4000 帧实测曲线：

```
frame    60 -> 1800 :  活堆 21539 -> 21557 KB   (+18 KB, 基本平)
frame  1860        :  一次阶跃 +3066 KB           (帧池高水位, 一帧 1080p NV12)
frame 1860 -> 3960 :  活堆 24623 -> 24622 KB    (完全平, 2100 帧零增长)
```

**结论：活堆不再随帧数线性增长，长跑是一条"偶尔阶跃一次然后水平"的线。**

### 1.7 复现工具

```bash
# 一键测活堆斜率(600 帧)
rk3588_zero_copy/scripts/ab_heap.sh mytag 600

# 只看斜率/判定
python3 rk3588_zero_copy/scripts/heap_slope.py logs/heap_mytag.log
```

`heap_slope.py` 用**尾段斜率**判定（真泄漏是线性的，高水位分配是阶跃后打平），
全程斜率会把两者看成一样。

---

## 二、任务 3：25 FPS 上限是不是视频源的限制？—— **是**

主循环新增打点：`t_wait`(等流) / `t_decode` / `t_rga` / `t_npu` / `t_post` / `t_total`，
外加 `src_gap`（相邻两帧解码出来的间隔 = 相机实际推送节奏）。

### 生产路径（MPP 硬解 + RGA + NPU）

```
流水线 FPS   : 24.99   (帧间隔 40.02 ms)
相机推送 FPS : 24.98   (解码间隔 40.02 ms)   ← 源就是 25 fps
解码等待     : 8.42 ms   ★ 阻塞在取流上
解码         : 0.49 ms   (VPU)
RGA 转换     : 2.31 ms
NPU 推理     : 28.33 ms  (rknn_run 22.57)
后处理       : 0.46 ms
单帧总耗时   : 40.02 ms
我们的活儿   : 31.59 ms  (占帧间隔 79%)
```

**判读**：
- 相机推送间隔 40.02 ms = 精确的 25 fps → **25 FPS 是源的天花板**；
- 但我们自己的活儿占 79%，只剩 21% 的余量 → 不是"闲着等源"；
- 就算把相机换成 30 fps，本流水线最多跑到 ~31 fps（31.6 ms/帧）；
- 最大的单块是 **NPU 推理 28.3 ms**，其次 RGA 2.3 ms。

### 软解路径（对照）

```
流水线 FPS : 17.52    (不是源限制, 是自己跑不动)
解码       : 18.42 ms  (CPU 软解)
RGA 转换   : 11.42 ms  (源是 CPU 指针, RGA 走慢路径)
NPU 推理   : 26.37 ms
我们的活儿 : 56.69 ms  (占帧间隔 99% —— 算力打满)
```

---

## 三、任务 2：硬件宏开关 + 2000 张图片基准

### 3.1 开关

```bash
cmake -B build     -DUSE_MPP=1 -DUSE_RGA=1 -DUSE_NPU=1   # hw
cmake -B build_npu -DUSE_MPP=0 -DUSE_RGA=0 -DUSE_NPU=1   # npu
cmake -B build_cpu -DUSE_MPP=0 -DUSE_RGA=0 -DUSE_NPU=0   # cpu
```

编译宏：`RK_USE_MPP` / `RK_USE_RGA` / `RK_USE_NPU`（默认 1）。
关掉之后**必须还能编、还能跑**（已验证三个构建都通过，CPU 回退路径是完整的）。

### 3.2 跑法

```bash
./build/rk_zero_copy <rtsp_url> model.rknn --labels labels.txt \
    --no-show --decoder mpp --benchmark 2000 \
    --benchmark-images output_debug --benchmark-csv bench.csv
```

- 阶段 A（视频流）：测**解码**耗时；
- 阶段 B（本地图片，循环读入 2000 帧）：测**预处理/推理/后处理**；
  输入放在 **dma-buf** 里，和生产路径一致（否则 RGA 走 CPU 虚拟地址慢路径，
  测出来 7.6 ms 而不是真实的 2.4 ms）。

### 3.3 结果（`docs/bench_20261112.csv`）

```
config  fps    decode_ms  rga_ms  npu_ms  post_ms  total_ms
hw      37.14  0.504      2.410   23.596  0.411    26.922
npu     16.93  16.394    18.356  23.914  0.400    59.064
cpu     46.83  12.647     8.707   0.000   0.000    21.354
```

- **RGA 比 OpenCV CPU 快 7.6 倍**（2.41 ms vs 18.36 ms）→ 零拷贝的价值就在这；
- **软解比硬解慢 32 倍**（16.4 ms vs 0.50 ms）；
- ⚠️ `cpu` 那行**不做检测**（NPU 关了），它的 fps 只代表「取流+解码+预处理」，
  不能和别的行直接比（程序输出里也打了这条提醒）。

---

## 四、任务 4：硬件负载监控

### 4.1 数据源（都要 root）

| 指标 | 路径 | 说明 |
|---|---|---|
| NPU 三核 | `/sys/kernel/debug/rknpu/load` | Core0/1/2 各自占用 |
| VPU | `/proc/mpp_service/load` | 需先 `echo 1000 > /proc/mpp_service/load_interval` |
| RGA | `/sys/kernel/debug/rga/load` | **本内核没有这个节点** |
| CPU | `/proc/stat` | |

### 4.2 实测（单实例，MPP 硬解 + RGA + NPU，30 个采样点）

```
NPU Core0  48.4%   ← 检测模型
NPU Core1   0.0%
NPU Core2   0.0%
VPU rkvdec  1.9%    ← 硬解几乎空转
CPU        21.6%
流水线 FPS 25.05    ← 稳如老狗
```

**判读：没有任何硬件单元打满，流水线却卡在 25 FPS → 上限就是相机源。**
（监控图：`docs/hw_load_monitor.svg` / `.png`）

### 4.3 工具

```bash
rk3588_zero_copy/scripts/hw_load.sh 30 logs/hw_load.csv          # 采样 + ASCII 仪表盘
python3 rk3588_zero_copy/scripts/hw_load_chart.py logs/hw_load.csv out.svg
```

> rktop：`cargo install rktop` 可装（GitHub 上的仓库路径要认证，装 crates.io 的版本）。
> 本报告的监控图是自绘的（纯 python 生成 SVG），不依赖 rktop。

---

## 五、当前状态

- 24h 长跑已重新启动（`scripts/memtest_supervisor.sh`，PID 见 `ps`），
  走的是**软解路径**（就是原来漏的那条），日志 `logs/memtest.log`、采样 `logs/memtest.csv`。
- 本轮改动已在 `main` 分支：4 个 commit + 1 个 tag `fix/avframe-leak`。
