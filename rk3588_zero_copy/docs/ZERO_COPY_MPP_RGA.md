# 零拷贝改造：MPP → RGA → NPU

> 目标：解码出来的 NV12 帧**不再拷到 CPU 内存**，而是把 DMA-BUF fd 直接交给
> RGA 做格式转换/缩放；RGA 的输出再以 DMA-BUF fd 直接喂给 RKNN NPU。
> 整条链路上，一帧像素从头到尾只存在**一份物理内存**里。

---

## 1. 改造后的数据流

```
MPP 硬解 ──> dma-buf fd ┐
                        ├─> RGA (importbuffer_fd) ──> RGB888 dma-buf
(软解)  ──> CPU 指针 ───┘        │                        │
   (CPU 兜底, OpenCV)             │                        v
                                 │              rknn_create_mem_from_fd
                                 └──────────────────────> NPU
```

| 环节 | 改造前 | 改造后 |
|---|---|---|
| MPP → RGA | 取虚拟地址，RGA 再自己找 fd | `mpp_frame_get_fd` 直接拿 fd → `importbuffer_fd` 注册 |
| RGA → NPU | `rknn_inputs_set` 从 `cv::Mat.data` 拷 | `rknn_create_mem_from_fd` + `rknn_set_io_mem` |
| 帧描述 | `AVFrame` / `cv::Mat` 在模块间传递 | 轻量结构体 `Nv12Frame{fd,w,h,format,stride}` |

---

## 2. 改了什么

| 文件 | 改动 |
|---|---|
| `include/nv12_frame.h` | **新增**。轻量帧描述 `Nv12Frame`：fd + 尺寸 + stride + 格式 + 数据来源 |
| `include/mpp_decoder.h` / `src/mpp_decoder.cc` | `decode()` 改为直接填 `Nv12Frame`；新增 `mpp_frame_get_fd_compat()`（dlsym 探测 `mpp_frame_get_fd_with_caller` / `mpp_frame_get_fd`，旧版 MPP 退回 `mpp_buffer_get_fd`） |
| `include/rga_convert.h` / `src/rga_convert.cc` | 新增 `importbuffer_fd` 源缓冲注册；**wstride 单位修正为像素**；新增 `RK_RGA_SRC` / `RK_ENABLE_RGA_DMA_BUF` 开关；新增 `[RGA] 输入来源:` 日志；新增 `pack_yuv420_to_nv12()`（诊断用） |
| `src/dmabuf.cc` | **修 bug**：ioctl 用错（见 §3.1）；优先用非 cache 的堆（见 §3.3） |
| `src/main.cc` | 只改**管道**部分：MPP 分支改用 `Nv12Frame`；新增 `[NPU] 输入来源:` 日志；新增诊断模式 `RK_RGA_STAGE` |
| `tests/probe_rga_stride.cc` | **新增**。实测 librga 的 wstride 单位（见 §3.2） |
| `src/dmabuf.cc` | 修掉 `dma_buf_backend()` 返回悬垂指针的隐患（改成固定 char 缓冲） |
| `tests/test_rga_fd_import.cpp` | **新增**。`importbuffer_fd` vs 老路逐字节对比 + 颜色探针 + 耗时 |
| `tests/measure_zero_copy.sh` | **新增**。零拷贝前后 A/B 实测（`rga=` 耗时 + `top -b` 的 %CPU） |

`src/yolo_post.cc` 和 `src/main.cc` 的**业务逻辑**（检测/后处理/跟踪/测速/上行）**一行未动**。

---

## 3. 过程中挖出来的三个根因（都是"静默失败"型，最费时间）

### 3.1 dma-buf 分配用的是**错的那个 ioctl**

原代码用上游标准 `DMA_HEAP_IOCTL_ALLOC`（magic `'H'`），但这块板子的内核
（6.1.172-vendor-seeed）用的是 **Rockchip 自己的 `rk-dma-heap`**：

```c
// /usr/src/linux-headers-*/include/uapi/linux/rk-dma-heap.h
#define RK_DMA_HEAP_IOC_MAGIC   'R'
#define RK_DMA_HEAP_UNCACHED     (1UL << 0)
struct rk_dma_heap_allocation_data {
    __u64 len; __u32 fd; __u32 fd_flags; __u64 heap_flags;   // ← fd 在第二个字段!
};
```

三个坑叠在一起：
1. **magic 错**：`'H'` → `'R'`
2. **字段顺序错**：原代码把 `fd` 放在最后，内核读到的 `heap_flags` 是垃圾 → 一律 `EINVAL`
3. **UNCACHED 位错**：`1<<1` → `1<<0`

现象：日志里一直是 `[警告] dma-buf 不可用, 退化为 CPU 内存输入`，而 RGA 又要求源和目标
都必须是真 dma-buf → **整条 RGA 路径永远走不到**。

修好之后：`模型输入缓冲: dma-buf fd=14 (dma_heap(/dev/dma_heap/system-uncached,cached))`

### 3.2 librga 的 `wstride` 单位是**像素**，不是字节

内核 RGA 驱动的尺寸校验是 `size = wstride * hstride * bpp(format)`。实测
（`tests/probe_rga_stride.cc`，RGB888 640×480 纯拷贝）：

| wstride | 与源逐字节相同 |
|---|---|
| 640（像素） | **100.0%** |
| 1920（字节） | 33.6%（被横向压扁） |

> 附带发现：缓冲**太小时 RGA 会直接拒掉**。64×64 的纯拷贝报
> `no core match` / `RGA_MMU unsupported memory larger than 4G`，0 字节写入；
> 640×480 正常。生产路径上的最小缓冲是 640×640（模型输入），不受影响。

原代码写的是 `mdst.wstride = model_w * 3`（=1920 字节），驱动直接拒绝：

```
rga: Only get buffer 1232896 byte from dma_buf_ptr, but current image required 3686400 byte
rga: memory param: w = 1920, h = 640, f = RGB888(0x2)
```

> README §6「坑 ② RGA 的 `wstride` 单位是字节」**是错的**，已更正。

现在 `RgaImage.wstride` 仍然用**字节**（CPU 兜底路径要按字节 memcpy），
在 `rga_convert.cc` 的边界上用 `rga_wstride_pixels()` 换算成像素再交给 librga。

### 3.3 这块板子的内核**没有实现 dma-buf 的 cache 同步 ioctl**

`RK_DMA_BUF_IOCTL_SYNC` 一律返回 `ENOTTY`。所以从 cached 堆（`system` / `reserved`）
拿到的内存，RGA 用 DMA 写完之后，**CPU 再读会读到自己的旧 cache** —— 看起来就像
"RGA 明明转换成功了，数据却没写进去"。

对策：`dma_buf_alloc()` 优先申请 `/dev/dma_heap/system-uncached`（本身非 cache）。

---

## 4. 运行时开关

| 开关 | 作用 |
|---|---|
| `RK_RGA_SRC=auto` | **默认**。有 dma-buf fd 就用 `importbuffer_fd`，否则 CPU |
| `RK_RGA_SRC=import` | 强制 `importbuffer_fd` |
| `RK_RGA_SRC=legacy` | 强制老路（`rga_buffer_t` 里直接填 fd）。实测与 import **输出逐字节一致**，但快 ~0.6ms/帧（省掉每帧的 PRIME import/export） |
| `RK_RGA_SRC=cpu` | 强制 OpenCV CPU 转换 |
| `RK_FORCE_CPU_RGA=1` | 强制 CPU（原有开关，保留） |
| `RK_RGA_STAGE=1` | **诊断模式**（见 §6）：把软解帧重排进 dma-buf，好让 RGA 走 fd 路径 |
| 编译期 `-DRK_ENABLE_RGA_DMA_BUF=0` | 编译期把 import/legacy 两条路整个编掉，只留 CPU |

> 编译期开关是为了"没有 dma-buf 的机器"做回归，**CPU 路径任何时候都保留**，
> 不会因为改了零拷贝路径就失去兜底。

---

## 5. 关键日志（判断零拷贝到底成没成）

```
[MPP] 硬解出帧: 1920x1080 stride=1920x1088 dma-buf fd=23
[NV12] 数据源: dma-buf fd (RGA import)
[RGA] 输入来源: DMA-BUF fd (fd=23, importbuffer_fd 注册为 RGA 源缓冲)
[NPU] 输入来源: DMA-BUF fd (fd=14, 与 RGA 输出同一块物理内存, 零 CPU 拷贝)
```

三行都是绿的 = 整条链路零拷贝。

对照（CPU 兜底）：

```
[RGA] 输入来源: CPU 映射地址 (OpenCV CPU 转换, 有一整帧的 CPU 拷贝)
[NPU] 输入来源: CPU 内存 (rknn_inputs_set, 有一整帧 RGB 的 CPU 拷贝)
```

---

## 6. 实测结果

### 6.1 `importbuffer_fd` 机制自测（`tests/test_rga_fd_import.cpp`）

```
[Q1] dma-buf 分配: src fd=4 (dma_heap(/dev/dma_heap/system-uncached,cached))   [ OK ]
[Q2] importbuffer_fd + improcess                                              [ OK ]
     legacy vs import  不同字节=0/1228800 (0.00%)   ← 两条路【逐字节一致】
[Q4] 颜色探针:
     红色矢量 (Y=81 U=90 V=240) -> R=254 G=0 B=0     ← 正好是 BT.601 limited 的红
     灰色矢量 (Y=128 U=V=128)  -> R=130 G=130 B=130 ← 中性灰
[Q5] 单帧耗时(50 次最好值):
     RGA importbuffer_fd      : 2.15 ms
     RGA 老路 rga_buffer_t.fd : 1.53 ms
     CPU(OpenCV)              : 8.60 ms      ← RGA 快 4.0x
```

> `importbuffer_fd` 比老路慢约 0.6ms/帧：每帧都要做一次 PRIME import/export。
> 两条路**输出逐字节一致**，所以 `RK_RGA_SRC=legacy` 随时可以切回去换那 0.6ms。

> 附带发现：这块板子的 librga/驱动**不支持 BT.709**
> （`IM_YUV_BT709_LIMIT_RANGE` 会报 `Unsupported full csc mode!`），
> 所以源固定用默认的 BT.601 limited。

### 6.2 整条流水线 A/B（`tests/measure_zero_copy.sh`，1920×1080 → 640×640，300 帧）

| | CPU 转换 | RGA `importbuffer_fd` | 变化 |
|---|---|---|---|
| `rga=` | 16.01 ms | **2.79 ms** | **快 5.7x** |
| `dec=` | 21.29 ms | 31.15 ms | +9.9ms（诊断模式的 CPU 重排，见下） |
| `npu=` | 24.06 ms | 25.11 ms | — |
| `total=` | 61.99 ms | **59.67 ms** | **快 4%** |
| **FPS** | 16.45 | **17.37** | **+6%** |
| **%CPU**（`top -b`） | **90 ~ 150%** | **36 ~ 70%** | **降约 55%** |

> 多次重跑，`rga=` 和 `%CPU` 会在几个百分点内浮动（网络抖动 + RTSP 抖动缓冲影响 `dec=`），
> 但 **`rga=` 稳定在 2.5~2.8ms、`%CPU` 稳定在 CPU 路的一半左右** 这个结论是稳定的。

**%CPU 下降约 45%** 是这次改造最直接的收益：格式转换从 CPU 挪到了 RGA 硬件。

> ⚠️ 上表 B 组的 `dec=` 里多了 ~10ms，是**诊断模式**的 CPU 重排（把平面 yuvj420p
> 重排成 NV12 再塞进 dma-buf）。这条路径是为了在**没有 MPP 权限**的机器上单独验证
> 「DMA-BUF fd → RGA」这段，不是零拷贝。
> **真正走 MPP 硬解时没有这次重排** —— `dec=` 会回到 ~21ms，`total=` 会再降 ~10ms
> （应该能到 50ms 上下），而且 `%CPU` 还会继续降（重排本身就是纯 CPU 活）。

### 6.3 300 帧长跑

两种模式都跑满 300 帧、退出码 0、无崩溃，检测结果正常（`objs=6`）。

---

## 7. MPP 硬解的设备权限（已解决）

MPP 的 HAL 必须打开 `/dev/mpp_service`，而它默认是 `0600 root:root`：

```
mpp: vcodec_service: open vcodec_service /dev/mpp_service failed
mpp: hal_h264d_api: mpp_dev_init failed ret: -1
mpp: mpp_dec_init could not init hal
```

MPP 没有 DRM 替代路径（已确认库里有 `allocator_drm`，但它只是缓冲分配器，
替代不了 `/dev/mpp_service`），所以必须改权限。工程里已经备好了 udev 规则：

```bash
sudo cp udev/99-rockchip-media.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=mpp_class
# 或者临时验证:
sudo chmod 666 /dev/mpp_service
```

之后日志里会出现：

```
[MPP] 硬解出帧: 1920x1080 stride=1920x1088 dma-buf fd=23
[NV12] 数据源: dma-buf fd (RGA import)
```

**这一步做完，整条链路就是完整的 MPP → DMA-BUF fd → RGA → DMA-BUF fd → NPU 零拷贝。**

### ✅ 已验证跑通（2026-11-11 19:47）

权限放开之后的实际日志，四行全绿：

```
[MPP] 硬解出帧: 1920x1080 stride=1920x1088 dma-buf fd=21
[NV12] 数据源: dma-buf fd (RGA import)
[RGA] 输入来源: DMA-BUF fd (fd=21, importbuffer_fd 注册为 RGA 源缓冲)
[NPU] 输入来源: DMA-BUF fd (fd=16, 与 RGA 输出同一块物理内存, 零 CPU 拷贝)
```

300 帧长跑（`--frames 300`）：退出码 0，无崩溃。

| | 软解 + CPU 转换 | MPP 硬解 + RGA fd |
|---|---|---|
| `dec=` | 19~21 ms | **0.8 ms** |
| `rga=` | 13~16 ms | 8.5 ms |
| `npu=` | 24 ms | 24 ms |
| `total=` | 57~62 ms | **42.3 ms** |
| **FPS** | 16.5~18 | **24.5** |

---

## 8. 复现命令

```bash
# 1) 编译
cmake --build build -j$(nproc)

# 2) 机制自测（不需要 RTSP、不需要 MPP）
g++ -O2 -std=c++17 -o build/tests/test_rga_fd_import \
    tests/test_rga_fd_import.cpp src/dmabuf.cc \
    -Iinclude -Ithird_party/include/rga \
    $(pkg-config --cflags --libs opencv4) -Lthird_party/lib -lrga -ldl
LD_LIBRARY_PATH=third_party/lib ./build/tests/test_rga_fd_import

# 3) wstride 单位探针
g++ -O2 -std=c++17 -o build/tests/probe_rga_stride \
    tests/probe_rga_stride.cc src/dmabuf.cc \
    -Iinclude -Ithird_party/include/rga -Lthird_party/lib -lrga -ldl
LD_LIBRARY_PATH=third_party/lib ./build/tests/probe_rga_stride

# 4) 整条流水线 A/B（含 top -b 的 %CPU 采样）
./tests/measure_zero_copy.sh 300
```
