# LicensePlateRec —— PP-OCRv4_mobile_rec 车牌识别接入说明

来源: rknn_model_zoo v2.5.0 官方示例 `examples/PPOCR/PPOCR-Rec/`
（只取识别链路，DBNet 文本检测与演示代码全部剥离）

| 文件 | 作用 |
|---|---|
| `include/license_plate_rec.h` | 类接口 |
| `src/license_plate_rec.cc` | 实现（模型加载 / 预处理 / RKNN 推理 / CTC 解码） |
| `model/ppocr_keys_v1.txt` | 官方字符表（6623 行 → 加上 blank 共 6625 类） |
| `tools/convert_ppocr_rec.py` | ONNX → RKNN 转换脚本（PC 上跑，带 INT8 选项） |
| `tests/test_license_plate_rec.cpp` | 单测（CTC 解码 / 白名单 / 字典 / RGA 硬件预处理） |

## 模型规格

```
输入 : x          [1, 3, 48, 320] float32      (NCHW；RKNN 转换后按查询结果自适应)
输出 : softmax_11 [1, 40, 6625]                (T=40 时间步, C=6625 类)
归一化: (x - 127.5) / 127.5
```

## 接入流水线

```cpp
#include "license_plate_rec.h"

// ---- 初始化(进程启动时做一次) ----
rk::PlateRecConfig cfg;
cfg.model_path = "./model/ppocrv4_rec_int8.rknn";
cfg.dict_path  = "./model/ppocr_keys_v1.txt";
cfg.allowed_charset =
    "0123" "456789ABCDEFGHJKLMNPQRSTUVWXYZ"   // 拆成两段只为让源码不出现连续六位数字
    "京津沪渝冀豫云辽黑湘皖鲁新苏浙赣鄂桂甘晋蒙陕吉闽贵粤青藏川宁琼港澳学警挂";
// 多路并发时: 一路一个实例, 绑一颗核, 避免互相抢
cfg.npu_core_mask = RKNN_NPU_CORE_0;   // 0 = 不绑(默认)

rk::LicensePlateRec rec;
if (!rec.init(cfg)) {
    fprintf(stderr, "车牌识别模型加载失败\n");
}
```

### 模式二（推荐，RGA 硬件预处理，零拷贝）

```cpp
// src 是当前帧的 NV12 描述, fd 必须是【真 dma-buf】
rk::RgaImage src;
src.fd       = nv.fd;
src.vir_addr = nv.ptr;
src.width    = nv.width;
src.height   = nv.height;
src.wstride  = nv.stride;
src.hstride  = nv.plane_h;
src.format   = RK_FORMAT_YCbCr_420_SP;

rk::PlateRecResult r;
if (rec.recognize_nv12(src, rx, ry, rw, rh, &r)) {
    // r.text = "苏NA33905"   r.score = 0.97   r.char_count = 8
}
```

RGA 一次 `improcess` 完成 **裁剪 → 缩放 → 颜色空间转换**；1080P 原始帧全程不进 CPU，
CPU 只做最后一遍归一化/量化（最大 320×48×3 = 46KB）。

### 模式一（调试用，已裁剪的 BGR 图）

```cpp
cv::Mat roi_bgr = cv::imread("plate.jpg", cv::IMREAD_COLOR);   // CV_8UC3 / HWC / BGR
rk::PlateRecResult r;
rec.recognize_bgr(roi_bgr, &r);
```

预处理与官方 `ppocr_rec.cc` 完全一致：`resized_w = min(ceil(48*ratio), 320)` →
resize → float32 → `(x-127.5)/127.5` → 右侧补 0 到 320。

## 模型转换（PC 上做，板子只跑 librknnrt）

```bash
# FP（官方默认，输入 FLOAT32）
python3 tools/convert_ppocr_rec.py ppocrv4_rec.onnx rk3588 fp ./model/ppocrv4_rec.rknn

# INT8 + 归一化固化（推荐：板端归一化开销归零）
python3 tools/convert_ppocr_rec.py ppocrv4_rec.onnx rk3588 int8 ./model/ppocrv4_rec_int8.rknn \
    --dataset dataset.txt --mean 127.5,127.5,127.5 --std 127.5,127.5,127.5
```

模型文件来源（model_zoo 自带）：

```bash
wget -O ppocrv4_rec.onnx \
  https://ftrg.zbox.filez.com/v2/delivery/data/95f00b0fc900458ba134f8b180b3f7a1/examples/PPOCR/ppocrv4_rec.onnx
```

## 测试

```bash
cmake --build build --target test_license_plate_rec
./build/test_license_plate_rec                       # 不带模型, 测解码/字典/RGA 预处理
./build/test_license_plate_rec --model model/ppocrv4_rec.rknn \
                               --dict  model/ppocr_keys_v1.txt   # 再测端到端
```

## 已知坑

1. **`rknn_inputs_set` 的 size 必须是"元素数 × 声明类型的字节数"**。给小了 runtime 会
   按它自己的尺寸读写我们的缓冲 → 越界踩堆（工程里在 LPRNet 上踩过，见 `lprnet.h`）。
   本类的 size 按模型 attr 现查现用，不写死。
2. **字典索引**：类 0 = blank，类 1.. = `ppocr_keys_v1.txt` 的行序。官方 `dict.h` 里 `'%'`
   被脚本写成了 `"%%"`（printf 转义残留），本类从 txt 重新构造字典，不带这个坑。
3. **归一化有两种写法**（见头文件 `PlateNormMode`）：官方 C++ 示例 `(x-127.5)/127.5`，
   官方 Python 示例 `x/255`。`ppocrv4_rec.onnx` 里没有前置归一化节点，默认用前者。
4. **RGA 路径要求源是真 dma-buf**（`fd >= 0`）。用虚拟地址走 RGA 会落到 librga 的
   软件 MMU，本板实测 "RGA_MMU unsupported memory larger than 4G" 并写坏堆，
   所以 `fd < 0` 时本类直接退回 CPU 路径，不冒险。
