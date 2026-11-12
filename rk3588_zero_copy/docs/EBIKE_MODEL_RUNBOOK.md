# 双输出 ebike 模型上板手册 —— 形态 C

> 模型：**`model/ebike_int8_rk2332.rknn`**（4.6MB）
>
> ⚠️ 板子的 RKNN 运行时是 **2.3.2**（`9f97ae6b@2025-04-09`）。
> 算法组原始的 `ebike_int8.rknn` 是 toolkit **2.5.0** 编的（`74ea630ce@2026-09-29`），
> 容器版本虽然同为 ver 6 能装上，但算子级兼容性没有保证。
> `ebike_int8_rk2332.rknn` 是本仓用 **toolkit 2.3.2** 从同一个
> `best_split.onnx` + 同一份标定集重编的，编译器哈希 `e045de294f@2025-04-07`
> 与板上已验证能跑的 `best.rknn` **完全一致**。
> 实测两个版本检测框等价（同样 3 个框，分数差 ≤0.03，坐标差 ≤0.8%）。
> 类别：`0=person` `1=ebike`（来自训练用的 `dataset_win.yaml`）
> 输出：**2 个**
> - `输出0 /model.22/Mul_2_output_0` = box `[1, 4, 8400]`，cx,cy,w,h，**单位是 640 输入图的像素**
> - `输出1 /model.22/Sigmoid_output_0` = cls `[1, nc, 8400]`，**已经是概率**
>
> 输入：逻辑 `[1,3,640,640]`/nchw，**原生 NHWC int8**，mean=0 / std=255 / rgb2bgr=False
> → 板端喂 **uint8 RGB NHWC**，归一化已经固化在模型里，**不要再除 255**。

---

## 一、为什么必须用「双输出」而不是标准单输出

Ultralytics 标准导出把框和类别概率拼在同一个输出张量里：

```
output0 = Concat([box(cx,cy,w,h 0~640 像素), cls(sigmoid 0~1 概率)], axis=1)
```

RK3588 的 int8 只支持 w8a8，**一个输出张量只有一个量化 scale**，而它由量级大的 box 决定
（实测 ≈2.548）→ cls 的 0~1 全被量化到码 0 → **类别通道恒为 0，一个框都检不出来**。

坑的是这时余弦相似度仍有 0.9998，**光看余弦完全发现不了**。

拆成两个输出后各自量化（实测值域，来自 RKNN 模拟器）：

| 张量 | 实测值域 | 含义 |
|---|---|---|
| box | 2.548 ~ 634.550 | 像素坐标（scale ≈2.5，1~2px 误差） |
| cls | 0.000 ~ 0.867 | 概率（scale ≈1/255，精度充裕） |

---

## 二、上板步骤

P0（`p0_mpp_verify.sh`）跑通之后，模型和标签已经在交付包里了，直接换模型跑：

```bash
cd $HOME/rk3588_zero_copy

# 人/车模型（双输出形态C）
MODEL=./model/ebike_int8_rk2332.rknn \
LABELS=./model/labels_ebike.txt \
./run_demo.sh --no-show --frames 300 \
              --decoder mpp \
              --track-cls 1 \
              --nms 0.45
```

| 参数 | 值 | 为什么 |
|---|---|---|
| `--track-cls 1` | 只跟 ebike | **必填**。跟人的话地面取点落在骑手脚上，IPM 假设 y2 是轮胎接地点 → 速度系统性偏高；且人框车框重叠会让 track_id 来回跳 |
| `--nms 0.45` | NMS IoU | 与算法组参考实现和 Ultralytics 训练默认值一致（工程默认是 0.60，是给 head/helmet 调的） |
| `--conf 0.25` | 默认即可 | 模拟器实测门禁相机域最高分 0.79~0.87 |
| `--decoder mpp` | 硬解 | P0 已验证 |
| `--plate-cls 1` | 配 LPRNet 时 | 车牌 ROI 从 **ebike 车体框**切，不是从 person 框切 |

启用车牌识别（模型在 `/home/seeed/rk_deploy/models/lprnet_finetuned_int8.rknn`）：

```bash
MODEL=./model/ebike_int8_rk2332.rknn LABELS=./model/labels_ebike.txt \
./run_demo.sh --no-show --frames 300 --decoder mpp --track-cls 1 --nms 0.45 \
  --lprnet /home/seeed/rk_deploy/models/lprnet_finetuned_int8.rknn --plate-cls 1 \
  --roi-ratio 0.72 --roi-y 0.70 --roi-height 0.16
```

启动日志里应该看到这一行（**没看到就是没走形态 C**）：

```
输出形态: 形态C 双输出 box[1,4,num_anchors] + cls[1,nc,num_anchors]
```

---

## 二又二分之一、★★ 输入必须喂原始 0~255 像素（双重归一化坑）

**症状**：绝大多数帧 `objs=0`，但日志全绿、不报错、`[自检] cls_max` 那一行看着"有值"。

模型是用 `config(mean_values=[[0,0,0]], std_values=[[255,255,255]])` 转的，
**归一化（除 255）已经固化在模型图里**（有一个 Div255 节点）。
所以板端**必须喂原始 0~255 像素**，再除一次 255 就是**双重归一化**。

RKNN 模拟器同一个 build 的三路对拍（`tools/rknn/probe_input_convention.py`）：

| 喂法 | box.max | cls_max |
|---|---|---|
| (a) uint8 NHWC 原始[0,255] | 634.550 | **0.867202** |
| (b) float32 NCHW 原始[0,255] | 634.550 | **0.867202**（与 a 完全一致） |
| (c) float32 NCHW **已 /255** | 639.647 | **0.019018** ← 检测被毁 |

cls_max 从 0.867 掉到 0.019 → 全部低于 conf 0.25 → **一个框都出不来**。

**修法**：按输入张量的量化类型区分（量化模型 `qnt_type == AFFINE_ASYMMETRIC`），
量化模型走 uint8 NHWC 原始像素；只有**真浮点**模型才按 ONNX 契约喂 `[0,1]`。

顺带把那段 122 万次除法挪出了 NPU 计时器 —— 之前它被算成"npu 耗时"，
让人误以为模型变慢了（实测能虚增十几毫秒）。

---

## 二又二分之一之二、★★ 预处理用【拉伸缩放】，不是 Letterbox

算法组说"训练参数是 `rect:false`，所以推理必须 Letterbox"。**实测不支持这个结论**。

原图分辨率下两者等价（9 张训练图，cls_max 均值 stretch 0.855 vs letterbox 0.855），
但**监控画面里的小目标**就完全不是一回事（把训练图贴进 1920×1080 画布，
`tools/rknn/probe_small_object.py`）：

| 目标占画面高度 | stretch | letterbox |
|---|---|---|
| 15% | 0.0076 → 0 框 | 0.0456 → 0 框（都检不出） |
| 25% | **0.7037 → 4 框** | 0.4488 → 2 框 |
| 40% | **0.5895 → 7 框** | 0.2130 → **0 框** |

原因：1920×1080 做 letterbox 会先缩成 640×360 再上下补 140px 灰边，
**目标只占输入高度的 56%**，像素更少更难检出；拉伸让目标保住满幅高度。
固定监控相机拍到的目标本来就小，所以这里**必须用拉伸**。

默认保持 **拉伸缩放**，`--letterbox` 保留供竖构图/大目标场景使用。

---

## 三、C++ 改了哪几处

| 文件 | 改动 |
|---|---|
| `include/yolo_post.h` | 新增 `post_process_dual_output()` 声明 + 形态 C 说明 |
| `src/yolo_post.cc` | 新增 `post_process_dual_output()` 实现；`yolov8_postprocess()` 加 `n_out == 2` 分支 |
| `src/main.cc` | 输出形态打印支持形态 C；新增 `--track-cls`；跟踪前按类过滤；未跟踪类用暗色画框 |
| `model/labels_ebike.txt` | 新增，`person` / `ebike` |
| `model/ebike_int8.rknn` | 新增 |

**`nc` 取的维度不一样，这是最容易踩的地方**：

- 形态 A（单输出 `[1, 4+nc, N]`）：`nc = dims[n_dims-2] - 4`
- 形态 B（9 输出，cls 是 `[1, nc, H, W]`）：`nc = dims[n_dims-3]`
- **形态 C（cls 是 `[1, nc, N]`）：`nc = dims[n_dims-2]`**

---

## 四、离线验证结果（不上板就能做的）

### 4.1 模型元数据（不用 rknn-toolkit，直接读 .rknn 头）

```powershell
python tools/rknn/peek_rknn_meta.py ebike_int8.rknn
```

```
images                    shape=[1,3,640,640] layout=nchw mean=[0] std=[255,255,255] rgb2bgr=False
/model.22/Mul_2_output_0  idx=0 shape=[1,4,8400]      <- box
/model.22/Sigmoid_output_0 idx=1 shape=[1,2,8400]     <- cls
```

### 4.2 RKNN 模拟器实跑（用源 ONNX 重建，同一份 config）

```bash
docker run --rm --mount "type=bind,source=E:/shenzhenproject,target=/work" --entrypoint bash rknn_env:2.5.0 -c '
cd /work && python3 tools/rknn/dump_dual_outputs.py \
  --onnx detect_exp/exp02_person_ebike_gpu2/weights/best_split.onnx \
  --calib calib/ebike_calib.txt --prefix /work/_dual_out/img \
  images/train/10.jpg images/train/1.jpg images/train/100.jpg'
```

### 4.3 C++ vs Python 数值对拍

```bash
# C++ 侧(rkbuild:24 镜像有 g++)
g++ -O2 -std=c++17 -Wall -Wextra -o /tmp/td tests/test_dual_output.cpp src/yolo_post.cc -Iinclude
/tmp/td /work/_dual_out/img_10 0.25 0.45

# Python 侧
python3 tests/py_dual_reference.py /work/_dual_out/img_10 0.25 0.45
```

**结果：12/12 组（6 图 × 2 组阈值）逐字节完全一致，累计 12 个框。**

> 注意：模拟器不支持 `load_rknn`，必须给 `--onnx` 从 `best_split.onnx` 重新 build，
> 参数与转换时一致（mean0/std255/w8a8/normal/channel/fp16/level3）。
> 另外 `build(dataset=)` **不认 `#` 注释行**，标定清单要先洗一遍。

---

## 五、⚠️ 必须提前知道的一件事：模型的域

模拟器在 6 张图上的实测：

| 图 | 尺寸 | 最高 cls 分 | conf=0.25 检出 |
|---|---|---|---|
| `images/train/10.jpg` | 500×666 | **0.867** | 1 个 ebike |
| `images/train/1.jpg` | 480×640 | **0.860** | 1 个 |
| `images/train/100.jpg` | 1170×724 | **0.787** | 3 个 |
| `images/val/frame_17730.jpg` | 1920×1080 | 0.034 | **0 个** |
| `images/val/frame_17800.jpg` | 1920×1080 | 0.068 | **0 个** |
| `images/val/frame_17900.jpg` | 1920×1080 | 0.008 | **0 个** |

`images/val/` 是**行车记录仪视角**的街景，`images/train/` 是**门禁相机视角**。
模型在门禁域上分数很高（0.79~0.87），在行车记录仪域上几乎不响应
（低阈值下右侧出的是噪声框，最高分 0.034）。

**所以：上板后第一件事是确认 `192.168.31.200` 的画面属于哪个域。**
如果是行车记录仪视角，检出率会很低，这是模型问题，要找算法组补数据/再训练，
不是 C++ 的问题。调试期建议先跑 `--sink-stdout` 看每帧的 objs 数量。

---

## 六、★★★ 2026-11-11 LPRNet 崩溃排查（三个坑叠在一起）

**症状**：加了 `--lprnet` 之后跑没几帧就

```
[LPRNet] ⚠️ 输入不是 UINT8(type=2), 说明 mean/std 没固化进模型!
terminate called after throwing an instance of 'cv::Exception'
  what(): OpenCV(4.10.0) error: (-215:Assertion failed) 0 <= roi.x && 0 <= roi.width
         && roi.x + roi.width <= m.cols ... in function 'Mat'
Aborted (core dumped)
```

**结论：那个 OpenCV 异常只是"受害者"，真正的元凶有三个，叠在一起。**

### 坑 1：CPU 兜底路径把 OpenCV 的输出尺寸当成 NV12 的行数（就是那条 -215）

`rga_convert.cc` 的 CPU 兜底原来是：

```cpp
cv::Mat yuv(sh * 3 / 2, sw, CV_8UC1, nv12.data());
... copyMakeBorder(补成偶数) ...
cv::cvtColor(yuv_aligned, rgb, cv::COLOR_YUV2RGB_NV12);
rgb = rgb(cv::Rect(0, 0, yuv.cols, yuv.rows)).clone();   // ← 元凶
```

OpenCV 的 NV12 转换：**输入是 (h*3/2, w)，输出是 (h, w)** —— 输出行数只有输入的 2/3。
那段 ROI 裁剪拿【NV12 缓冲的行数 1.5*h】当高度，去裁【已经转成 RGB 的 (h, w) 图】，
只要 w 或 h 是奇数（走过 copyMakeBorder 分支）就必然 `h > rgb.rows` → 抛 -215 → 没人 catch → core dump。

车牌 ROI 来自浮点几何计算，宽高有一半概率是奇数 → 所以是"跑几帧才崩"。
RGA 能用时走 RGA 不触发；RGA 挂掉落到 CPU 兜底就必崩。

**修法**：一开始就把裁剪框对齐到偶数（chroma 本来就是 2x2 下采样），
OpenCV 转换后不需要再裁一刀。

### 坑 2：LPRNet 的输入喂法让 RKNN 运行时越界读写

模型输入 attr 是 `type=INT8, fmt=NHWC, dims=[1,48,160,3], zp=0, scale=0.007843(=1/127.5)`，
但 **runtime 的 `rknn_inputs_set` 实际要 92160 字节**（=23040×4）：

```
E RKNN: rknn_inputs_set, param input size(23040) < model input size(92160)
```

原来喂的是 `type=UINT8 + size=23040` → runtime 按 92160 字节去读写我们那块
23040 字节的缓冲 → **越界踩坏堆** → 后面随便一次 malloc / cv::Mat 就炸，
落点随机（malloc / libavcodec / cv::Mat 都可能）。

**板端实测（合成车牌图 A/B 七种喂法，只有这一种解出正确结果）**：

| 喂法 | size | 解码结果 |
|---|---|---|
| uint8 原值（原来的写法） | 23040 | 乱码 + 越界 |
| float32 x/255 | 92160 | 乱码 |
| float32 (x/255-0.5)/0.5 | 92160 | 乱码 |
| **int8 量化码 `q=round((x/255)/scale)+zp`** | **23040** | **✅ 正确** |

**修法**：`q = clamp(round((x/255) / scale) + zp, -128, 127)`，
`inputs[0].type = RKNN_TENSOR_INT8`，`size = 23040`（正好等于量化码个数）。
量化参数一律从 `rknn_query(RKNN_QUERY_INPUT_ATTR)` 现查，不写死。
（`--lprnet-feed float` 保留 float32 路径，板端实测效果不如 int8 量化码。）

### 坑 3：RGA 在非 dma-buf 缓冲上会写坏堆

这块板子的 dma-buf 分配不出来（`[警告] dma-buf 不可用`），于是 librga 走
**软件 MMU**（自己把用户态 VA 映射成物理地址跑 DMA）：

```
RGA_MMU unsupported memory larger than 4G!
scheduler core[4] unsupported mm_flag[0x8]!
ID[13]: job buffer map failed!
```

软件 MMU 会缓存 VA→PA：缓冲 free 后 VA 被复用，缓存里的 PA 还是老的，
RGA 就把输出写到了**别的分配**里 → 堆随机被破坏。

另外 RGA 在**缩放比例不是整数倍**时（1920×1080 → 640×640，sy=1.6875）
会往目标缓冲**多写 1~2 行**（实测 +3840 字节），目标缓冲按精确大小分配就踩坏堆。

**修法**：
1. RGA **只在源和目标都是 dma-buf(fd ≥ 0) 时**才用，否则一律走 CPU 兜底
   （CPU 路径实测 16~18fps，功能不丢）；
2. 所有 RGA 目标缓冲多分配 `kRgaDstPadRows = 16` 行余量，让那 1~2 行刷在余量里。

### 附带修掉的一个大 bug：整帧颜色全错

软解出来的帧是 **`yuvj420p`（平面 YUV420，U/V 分开，全范围）**，
而代码一律按 NV12（半平面、UV 交织）去读 → **整帧品红/绿**，
灰度结构对、颜色全错。修正前后：

| | cls_max（检出置信度） |
|---|---|
| 修正前（颜色错） | 0.247 ~ 0.323 |
| 修正后（颜色对） | **0.521 ~ 0.677** |

修法：`RgaImage` 增加 `u_plane / v_plane / uv_wstride / planar_yuv / full_range`，
打包成 NV12 时按平面/半平面分别取色度；全范围(yuvj*)先折成 limited
（`Y*219/255+16`，`C:(C-128)*224/255+128`）再交给 OpenCV 转换。
代价：转换多约 8ms/帧（16.2fps vs 17.9fps）。

### 还剩的问题：机位不对

当前 `rtsp://192.168.31.200:8557/h264` 是**室内货架/桌面**画面，根本没有电动车，
所以 `--roi-*` 切出来的是桌面（"ebike" 框是误检，800+ 像素宽），识别结果自然是乱码。
**要验证车牌识别，得换一个真正有车流/车牌的机位**，再调 `--roi-ratio / --roi-y / --roi-height`。
