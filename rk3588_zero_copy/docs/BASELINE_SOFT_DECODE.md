# 基线版本：软解 + CPU 转换（2026-11-10）

> 这是**第一个在真板子上端到端跑通、且零崩溃**的版本。
> 硬解（MPP）和硬转（RGA）都还没解决，**不影响业务演示**。

---

## 一、实测结果

板子：`recomputer-rk3588-devkit`（`seeed@192.168.31.77`，Ubuntu 26.04 / Armbian，无外网）
相机：`rtsp://192.168.31.200:8557/h264`（1920×1080 @ 25fps）

| 指标 | 实测值 |
|---|---|
| FPS | **26.4** |
| NPU 推理耗时 | **12 ms** |
| 检测 | 正常检出目标（`objs` 非 0） |
| 崩溃 | **0**（300 帧全程无 core dump） |
| RTSP | 正常（软解 H.264） |
| 跟踪 / 测速 / 车牌 / MQTT | 代码路径已就位（待接标定参数与业务验收） |

---

## 二、固化下来的配置

三条开关，缺一不可：

| 开关 | 原因 |
|---|---|
| `RK_FORCE_CPU_RGA=1` | 板子上 librga 1.9.1 的 `rga_init()` **会自己段错误**（打开 `/dev/rga` 之后、打印版本号 `rga_api version 1.9.1_[4]` 那一行之后崩）。强制走 OpenCV 的 CPU 转换（NV12→RGB），完全不碰 RGA。 |
| `--decoder ffmpeg` | 系统 ffmpeg 没有 `h264_rkmpp`，只能软解。MPP 硬解目前会在 `mpp_create()` 里段错误。 |
| `--no-show` | 演示 / 长跑不弹窗口。 |

已固化成脚本：

```bash
./run_baseline.sh                    # 基线(旧 head/helmet 模型)
./run_baseline.sh --ebike            # 换双输出人/车模型
./run_baseline.sh --frames 600       # 多跑几帧
./run_baseline.sh --show             # 带显示窗口
```

---

## 三、这条链路上什么能用、什么被禁用

| 环节 | 状态 | 说明 |
|---|---|---|
| RTSP 拉流 | ✅ | FFmpeg 拉 H.264 |
| 硬解 (MPP/VPU) | ❌ 禁用 | `mpp_create()` 段错误。旧版 20190626 用户态 vs 板子新内核 |
| 硬转 (RGA) | ❌ 禁用 | librga 1.9.1 的 `rga_init()` 段错误。改用 OpenCV CPU 转换 |
| dma-buf 零拷贝 | ❌ 不可用 | `/dev/dma_heap/*` 打不开（权限/驱动），退化为 CPU 内存 |
| CPU 预处理 | ✅ | OpenCV `cvtColor(NV12→RGB)` + `resize`，与 Python 参考**逐像素一致** |
| RKNN 推理 | ✅ | 12ms，9 输出 / 2 输出两种形态都支持 |
| 后处理 | ✅ | 形态A 单输出 / 形态B fork 9输出 / **形态C box+cls 双输出** |
| 几何先验过滤 | ✅ | 前置在跟踪之前 |
| 跟踪 + 测速 | ✅ | 需配 IPM 标定参数才出速度 |
| 车牌 (LPRNet) | ✅ 代码就位 | 需 `--lprnet` + `--plate-cls`，ROI 比例待标定 |
| MQTT / 串口 | ✅ 代码就位 | 需配 `--mqtt-host` 等 |

---

## 四、两个"会段错误"的坑（都已在代码里兜住）

### 1. RGA

```
[NV12] 数据源: CPU 映射地址 (RGA DMA 直读)
rga_api version 1.9.1_[4]      ← librga rga_init() 打印的版本号
Segmentation fault
```

`rga_init()` 是**第一次调 `imcheck`/`improcess` 时才触发**的，它打开 `/dev/rga` 失败后
**没有干净返回**，而是继续解引用未初始化的全局句柄 → SIGSEGV。

**只把 `improcess` 包进 SafeProbe 是不够的** —— `imcheck` 排在它前面，是第一个触发
`rga_init` 的调用，崩溃发生在它里面，根本走不到 `improcess`。

现在的做法：第一次用 RGA 前先在 SafeProbe 里做一次**探测性 `imcheck`**
（`ensure_rga_ready()`），把 `rga_init` 的崩溃一次性兜住；失败就永久标记 RGA 不可用，
全程走 CPU 转换。

> 回归测试：`tests/stubs/native/rga_crash_stub.cpp` 是个**故意在 imcheck 里段错误**的
> librga stub，在 PC/容器里精确复现板子故障，验证修复后进程退出码 0、不 core dump。

### 2. MPP

```
解码器: h264
Segmentation fault         ← 崩在 mpp_dec.init() 里
```

`[解码] 后端 = ...` 在成功和失败两个分支都会打印，它没出现 → 进程在
`mpp_dec.init()` 内部异常终止。已用 SafeProbe 包住，崩了会打印
`[警告] MPP 初始化过程中崩溃(SIGSEGV/SIGBUS 已捕获), 退回软解` 并自动继续。

---

## 五、换模型不需要改框架

**代码一行都不用改。** 形态C（box/cls 双输出）的后处理、`--track-cls` 类别过滤、
标签文件都已经在包里了。换模型只改三个环境变量：

```bash
MODEL=./model/ebike_int8_rk2332.rknn \
LABELS=./model/labels_ebike.txt \
./run_baseline.sh --ebike
```

等价于：

```bash
RK_FORCE_CPU_RGA=1 \
./run_demo.sh --no-show --frames 300 --decoder ffmpeg \
              --track-cls 1 --nms 0.45 \
  <rtsp> ./model/ebike_int8_rk2332.rknn --labels ./model/labels_ebike.txt
```

| 模型 | 输出 | 类别 | 必带参数 |
|---|---|---|---|
| `model/best.rknn` | 9 输出（形态B） | `head` / `helmet` | 无 |
| `model/ebike_int8_rk2332.rknn` | **2 输出（形态C）** | `person` / `ebike` | `--track-cls 1 --nms 0.45` |

**`--track-cls 1` 为什么必须带**：跟踪器原本没有类别逻辑，跟人的话地面取点会落在
**骑手脚上**，而 IPM 假设 y2 是轮胎接地点 → 速度**系统性偏高**；人框车框互相重叠
还会让 track_id 在人/车之间来回跳。

---

## 六、⚠️ 模型版本必须与板子运行时匹配

板子（和我们随包发的 `third_party/lib/librknnrt.so`）是 **RKNN runtime 2.3.2**
（`9f97ae6b@2025-04-09`）。

| 模型 | 编译器版本 | 能在这台板子上加载吗 |
|---|---|---|
| `model/best.rknn` | 2.3.2 `e045de294f@2025-04-07` | ✅ 已实测 |
| 算法组原始 `ebike_int8.rknn` | **2.5.0** `74ea630ce@2026-09-29` | ⚠️ 容器版本都是 ver 6，能装；但算子级兼容性未知 |
| `model/ebike_int8_rk2332.rknn` | **2.3.2**（本仓用 2.3.2 重编） | ✅ 与运行时同版本 |

重编方法（离线可复现）：

```bash
docker run --rm --mount "type=bind,source=E:/shenzhenproject,target=/work" \
  --entrypoint bash rknn_env:2.5.0 -c '
cd /work
pip uninstall -y rknn-toolkit2 -q
pip install --no-deps -q rknn-toolkit2==2.3.2 -i https://pypi.tuna.tsinghua.edu.cn/simple
python3 onnx2rknn_int8.py \
  --onnx detect_exp/exp02_person_ebike_gpu2/weights/best_split.onnx \
  --dataset calib/ebike_calib.txt \
  --output ebike_int8_rk2332.rknn'
```

> 转换脚本会自动清洗标定清单里的 `#` 注释行 —— rknn 的 `build(dataset=)` **不认注释行**，
> 带上注释会报 `Unsupport file #!`。

---

## 七、后续待办（都不影响演示）

| 项 | 说明 |
|---|---|
| MPP 硬解 | 需要板子自己的 MPP 头文件/库，或匹配的新版用户态 |
| RGA 硬转 | 同上；`/dev/rga` 权限 + librga 与内核版本匹配 |
| dma-buf 零拷贝 | `/dev/dma_heap/*` 权限 |
| IPM 标定 | 测速要出数，需要用实拍机位标定 `--ipm-points` |
| 车牌 ROI | `--roi-*` 比例需用实拍帧标定 |
| MQTT 闭环 | 开发机 `1883` 已有 broker，板端加 `--mqtt-host` 即可 |
