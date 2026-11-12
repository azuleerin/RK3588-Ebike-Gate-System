# rk12ch —— RK3588 12 路 1080P RTSP 电动车检测 + 车牌识别

Python 版工程脚手架，四层解耦流水线，全链路硬件加速（MPP 硬解 → RGA 零拷贝 → NPU 分核/批处理）。

```
┌── 解码层 ──────────────────────────────────────────────┐
│ 12 个线程: RTSP(自研客户端) → MPP 硬解 → NV12 dma-buf   │
│   抽帧率在这里做(解码后丢帧, 省掉整条 RGA+NPU 的开销)    │
└───────────────┬─────────────────────────────────────────┘
                │ Q1 (有界, 丢最旧)
┌── 预处理层 ────────────────────────────────────────────┐
│ RGA: 裁剪 → 缩放 → 颜色转换 → 直接写进 batch 缓冲的槽位  │
│   ★ 组 batch 放在这一层, NPU 才能零拷贝读              │
└───────────────┬─────────────────────────────────────────┘
                │ Q2 (batch: 一块连续 dma-buf)
┌── 推理层 ──────────────────────────────────────────────┐
│ YoloWorker: core 0_1, batch=N, set_io_mem 零拷贝        │
│ LprWorker : core 2,   int8 量化码 + rknn_inputs_set     │
└───────────────┬─────────────────────────────────────────┘
                │ Q3 / Q6
┌── 业务层 ──────────────────────────────────────────────┐
│ ByteTrack 跟踪 → 车牌 ROI 几何先验 → 车牌匹配 → 上报    │
└───────────────┬─────────────────────────────────────────┘
                │ Q4 (车牌裁剪请求) → 预处理层 → 推理层(LPR)
```

## 文件结构

```
rk12ch/
├── config/
│   └── system.yaml              统一配置(路数/抽帧/模型/阈值/队列/统计)
├── rk12ch/
│   ├── config.py                配置加载 + 启动前校验 + 命令行覆盖
│   ├── app.py                   主程序(四层装配 + 优雅退出)
│   ├── stats.py                 每路FPS/端到端延时/NPU利用率/CPU占用/队列积压
│   ├── hal/                     硬件抽象层(全部 ctypes 直连 .so)
│   │   ├── dmabuf.py            dma-heap 分配 / mmap / cache 同步
│   │   ├── rga.py                librga: rga_buffer_t/im_rect/improcess
│   │   ├── rknn.py               librknnrt: attr/io_mem/core_mask/batch/perf
│   │   ├── mpp.py                librockchip_mpp: MppApi 函数表 / FrameLease
│   │   └── rtsp.py               自研 RTSP over TCP → AnnexB (零外部依赖)
│   ├── pipeline/                四层
│   │   ├── types.py              层间数据结构(含引用计数的帧 lease)
│   │   ├── queues.py             有界队列 + 丢帧策略 + 积压统计
│   │   ├── decode.py             解码层(每路一线程)
│   │   ├── preprocess.py          预处理层(RGA + CPU 兜底)
│   │   ├── infer.py               推理层(YOLO/LPR 两个 worker)
│   │   └── business.py            业务层(跟踪/车牌匹配/上报)
│   └── post/
│       ├── yolo.py               YOLOv8n 双输出形态C 解码 + NMS
│       └── tracker.py            ByteTrack 简化版 + 车牌缓存
├── scripts/
│   └── check_env.py             环境自检(动态库/设备/模型属性/dma-buf)
├── requirements.txt
└── README.md
```

## 跑起来

```bash
cd ~/rk12ch
export LD_LIBRARY_PATH=/home/seeed/rk_deploy/rk3588_zero_copy/third_party/lib:$LD_LIBRARY_PATH

# 1. 先自检(动态库/设备/模型/dma-buf/模型 batch 维)
python3 scripts/check_env.py --config config/system.yaml

# 2. 起流水线
python3 -m rk12ch.app --config config/system.yaml
python3 -m rk12ch.app --config config/system.yaml --set models.yolo.conf_threshold=0.3
python3 -m rk12ch.app --config config/system.yaml --duration 300     # 跑 5 分钟
```

## RKNN API 对齐(Toolkit2 V2.5.0)

全部走 `librknnrt.so` 的 C ABI(ctypes), 用到的官方接口:

| 接口 | 用途 |
|---|---|
| `rknn_init` / `rknn_destroy` | 加载 `.rknn` 模型 |
| `rknn_query(RKNN_QUERY_INPUT_ATTR / OUTPUT_ATTR / IN_OUT_NUM / PERF_DETAIL / PERF_RUN)` | 查模型真实属性(类型/形状/zp/scale) |
| `rknn_set_core_mask(RKNN_CORE_0_1 / RKNN_CORE_2)` | **NPU 分核**: 2 核 YOLO + 1 核 LPRNet |
| `rknn_set_batch_core_num` | 单次 run 用 n 核并行算 batch |
| `rknn_create_mem_from_fd` + `rknn_set_io_mem` | **零拷贝输入**(dma-buf fd 直绑) |
| `rknn_inputs_set` | LPRNet 的 int8 量化码输入 |
| `rknn_run` / `rknn_outputs_get` / `rknn_outputs_release` | 推理与取结果(want_float=1) |

★ **batch 维的硬约束**: 模型转换时必须带 batch 维(`rknn.config(batch_size=N)`, 且 ONNX 的 batch 轴是动态的)。
查询模型输入 attr 的 `dims[0]`: `dims[0] == 1` 说明模型被固化成 batch=1, 喂 batch>1 会被 runtime 拒绝。
本仓现有的两个模型都是 batch=1 —— `check_env.py` 会直接报出来, 想要真 batch 就重新转换。

## 三个"不这么写就崩"的坑(都已写进代码注释)

1. **LPRNet 的输入不能用 uint8**。模型输入张量是 INT8(zp=0, scale=1/127.5), 而 runtime 对 uint8 输入会按
   **92160 字节**(23040×4)去读写你那块 23040 字节的缓冲 → 越界踩坏堆 →
   `malloc(): invalid next size` / 随机崩在 libavcodec / cv::Mat。
   正确喂法(板端 A/B 七种喂法实测唯一解出正确结果的):
   `q = clamp(round((x/255)/scale) + zp, -128, 127)`, `type=INT8`, `size=23040`。
2. **RGA 只碰 dma-buf**。用户态内存会让 librga 走软件 MMU, 缓存的 VA→PA 失效后把输出写到**别的分配**里
   (`RGA_MMU unsupported memory larger than 4G!`)。另外 RGA 在缩放比例不整除时会**多写 1~2 行**,
   目标缓冲必须留余量(本工程 `RGA_PAD_ROWS = 16`)。
3. **MPP 的 frame 必须活到用完**。`mpp_frame_deinit()` 之后再拿它的 fd 去裁剪, 读到的是被复用的内存。
   本工程用 `RefLease` 做引用计数(一个帧可能被多个车牌请求共享)。

## 性能瓶颈与优化点(代码里都标了 ★ / 瓶颈)

| 位置 | 瓶颈 | 优化方向 |
|---|---|---|
| 解码层 RTSP 拆包 | socket→MPP 有一次用户态拷贝 | 换成 `mpp_packet_init_with_buffer` 直接把 buffer 交给 MPP, 零拷贝 |
| 解码层 MPP | 12 路各一个解码线程 | 确认 VPU 核数(RK3588 是 1 个 VPU), 12 路 1080P H.264 已经接近上限, 建议降到 4~6 路或降分辨率/帧率 |
| 预处理层 RGA | RGA 是全局硬件资源, 12 路并发会排队 | 调大 `workers` 不会提升 RGA 吞吐, 只会增加排队延时; 要提升就把 RGA 换到 C++ |
| 预处理层 batch 组装 | 单线程组 batch | 组 batch 的线程是串行瓶颈(每帧一次 RGA ioctl), 可以按通道分片成 2 个组装线程 |
| 推理层 batch | batch 越大吞吐越高、延时越大 | batch=4 时最差延时多约 4 帧间隔; 追求延时就减 batch, 追求吞吐就加 |
| 业务层跟踪 | Python 里的 IoU/NMS | 12 路 x 10fps = 120 次/秒, 目前 numpy 够用; 精度要求高就换 C++ |
| 业务层 MQTT | 阻塞 IO | 已经在业务线程, 不影响推理; 但上报量大时要加批量 |

## 本板(当前 RK3588 板子)的环境限制

`scripts/check_env.py` 会把下面几项直接验出来:

1. **VPU 内核模块没加载**(`/proc/umap` 下没有 vpu 节点) → MPP 硬解不可用, 需要 `modprobe rkvdec`。
   ★ 这条特别危险: VPU 没就绪时调用 MPP 解码会**直接 SIGSEGV**(内部解引用空的驱动句柄),
   Python catch 不住, 整个进程被带走。所以 `hal/mpp.py` 里加了 `mpp_driver_ready()` 前置探测,
   不就绪时解码线程只打印可执行的提示并退避重试(运维加载模块后自动恢复), 绝不碰解码路径。
2. **dma-buf 不可 mmap**(任何用户都 `Permission denied`) → RGA/NPU 的零拷贝路径不可用,
   代码会自动降级到 CPU 兜底(帧率会掉, 功能不丢)。
3. **没有 ffmpeg CLI / PyAV / GStreamer 绑定** → 所以 RTSP 拆包是自己实现的(`hal/rtsp.py`),
   好处是零依赖 + 少一次拷贝 + 能拿 SDP 的 sprop-parameter-sets。
4. `/dev/rknpu` 不存在但 RKNN 推理正常(NPU 走的是别的节点)。

## 配置速查

```yaml
channels:            # 12 路, 每路可单独设 target_fps(抽帧率)
  - {id: 0, name: ch0, url: "rtsp://...", target_fps: 10, width: 1920, height: 1080}

inference:
  yolo_cores: "0_1"   # NPU 核分配(YOLO 2 核)
  lpr_cores:  "2"     # 车牌 1 核
  yolo_batch: 4       # ★ 需要模型重新转换成 batch>1 才生效
  lpr_batch:  8

models:
  yolo: {path: ..., input_w: 640, input_h: 640, conf_threshold: 0.25, nms_iou: 0.45}
  lpr:  {path: ..., feed: int8_code, roi: {ratio: 0.72, y: 0.70, height: 0.16, pad: 0.10}}

business:
  tracker: {type: bytetrack, high_threshold: 0.6, low_threshold: 0.25, max_age: 30}
  output:  {sinks: [stdout, mqtt]}
```
