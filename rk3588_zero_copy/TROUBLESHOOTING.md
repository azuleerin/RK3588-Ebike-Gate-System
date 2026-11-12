# 上板编译/运行 排错手册

针对本工程（RK3588 + FFmpeg/rkmpp + RGA 1.9.1 + rknpu2 v1.x + OpenCV）在板端
实机编译时最可能出现的 5 个编译错误，按出现概率排序。

---

## 0. 上板前能先做的两类自检（不用板子）

本工程已经在 x86 容器里做过这两件事，能提前挡掉大部分低级错误：

```bash
# A. 语法/类型检查（用 tests/stubs 里的 FFmpeg/OpenCV/MPP 最小 stub，
#    配合真实的 rknn_api.h 和 im2d.h）
docker run --rm -v "$(pwd -W):/work" -v "<rockchip demo 目录>:/sib" \
  --entrypoint bash ubuntu/squid:latest -c "
    apt-get update -qq >/dev/null 2>&1; apt-get install -y -qq g++ >/dev/null 2>&1
    cd /work
    for f in src/*.cc; do
      g++ -fsyntax-only -std=c++17 -Wall -Wextra -Wno-unused-parameter \
          -Iinclude -Itests/stubs -I/sib/include \
          -I/sib/include/3rdparty/rga/RK3588/include \$f || echo \"FAIL \$f\"
    done
  "

# B. 与 Python 基线对拍（测速逻辑）
cd tests && ./run_parity.sh
```

A 这套检查在本次移植中抓出了 4 个真 bug（都是板上才会暴露的那种）：
- `dma_heap_allocation_data` 少定义了 `__u64 fd` → **ioctl 号对不上**，
  dma-buf 分配会静默失败，一直退化成 CPU 内存
- `Med5` 窗口没满时没排序 → 取到的不是中位数
- `Med5` 丢的是"最小值"而不是"最旧的" → 中位数算错
- `ground_median_k` 声明成 `float` 却用 `%d` 打印 → varargs 读到垃圾

---

## #1 `fatal error: rknn_api.h: No such file or directory`

**概率：最高。** 板子上装了 `librknnrt.so` 运行时，但**没有开发头文件**。

### 诊断
```bash
ls /usr/include/rknn_api.h              # 大概率 Not found
find / -name "rknn_api.h" 2>/dev/null
ls -l /usr/lib/librknnrt.so
```

### 原因
- 板子镜像只打包了运行时 `.so`，没打包 `.h`。
- 或者 `RK_DEMO_ROOT` 指向了板子上不存在的路径。

### 解决
把 PC 上的头文件和库一起传上去（本工程就是从同级 rockchip demo 目录复用的）：

```bash
# PC 上
scp ../yolov8-helmet-rk3588-multithread/include/rknn_api.h  root@192.168.31.77:/root/include/
scp ../yolov8-helmet-rk3588-multithread/include/librknnrt.so root@192.168.31.77:/root/lib/
# 板子上
cd /root/rk3588_zero_copy
RKNN_INC=/root/include RKNN_LIB=/root/lib/librknnrt.so ./build_native.sh
```

### ⚠️ 顺带提醒
同级项目 `yolov8-helmet-rk3588-multithread/CMakeLists.txt` 里写的是
```cmake
set(RKNN_API_PATH ${CMAKE_SOURCE_DIR}/../../runtime/RK3588/${CMAKE_SYSTEM_NAME}/librknn_api)
```
**这个路径在本工作区里根本不存在**（那是 Rockchip SDK 原始的目录结构）。
本工程用的是 `RK_DEMO_ROOT/include`，别照抄那一行。

---

## #2 `fatal error: libavformat/avformat.h: No such file or directory`

**概率：很高。** 板子上装了 `libavformat.so` 但没装 `libavformat-dev`。

### 诊断
```bash
ls /usr/include/libavformat/avformat.h
ls /usr/lib/aarch64-linux-gnu/libavformat.so*
ffmpeg -version 2>&1 | head -1
```

### 解决 A：板子能上网
```bash
apt-get update && apt-get install -y libavformat-dev libavcodec-dev libavutil-dev
```
> ⚠️ apt 装的版本**大概率没有 rkmpp 解码器**。程序会退化成软解码并打印
> `[警告] 没有找到 h264_rkmpp 解码器, 退回软件解码` —— 能跑但不是零拷贝。

### 解决 B：离线板子（推荐）
从 PC 上把 **带 rkmpp 的** FFmpeg 头文件和库打包传上去：
```bash
# PC 上(FFmpeg 编译时带 --enable-decoder=h264_rkmpp --enable-ffmpeg-rkmpp)
tar czf ffmpeg-dev.tgz include/libavformat include/libavcodec include/libavutil lib/libavformat.so lib/libavcodec.so lib/libavutil.so
scp ffmpeg-dev.tgz root@192.168.31.77:/root/
# 板子上
tar xzf /root/ffmpeg-dev.tgz -C /root
cd /root/rk3588_zero_copy
FFMPEG_INC=/root/include ./build_native.sh
```
> 只传 `.so` 不传头文件是编不过的；反过来只传头文件不传 `.so` 会链接失败。

---

## #3 `fatal error: opencv2/core.hpp: No such file or directory` / `Could NOT find OpenCV`

### 诊断
```bash
ls /usr/include/opencv4/opencv2/core.hpp
ls /usr/include/opencv2/core.hpp
pkg-config --exists opencv4 && echo "pkg-config 有" || echo "pkg-config 没有"
ldconfig -p | grep opencv_core
```

### 解决
```bash
apt-get install -y libopencv-dev        # 能上网时
# 或者只传需要的 4 个库 + 头文件:
#   include/opencv4/  libopencv_core.so libopencv_imgproc.so libopencv_highgui.so libopencv_imgcodecs.so
```
CMakeLists 里已经有"手工找 `libopencv_*.so`"的兜底，只要头文件和 `.so` 都在就能过。

> 本工程用到 OpenCV 的模块只有 **core / imgproc / highgui / imgcodecs**，
> 不用整套 OpenCV，传这 4 个就够。

---

## #4 `error: too few arguments to function 'rknn_init'` / `'rknn_init' was not declared in this scope` / `no member named 'virt_addr' in 'rknn_tensor_mem'`

**这个是版本不匹配，最容易被忽略。**

### 诊断
```bash
grep -n "int rknn_init" /usr/include/rknn_api.h
grep -n "phys_addr\|virt_addr" /usr/include/rknn_api.h | head
grep -n "rknn_set_io_mem" /usr/include/rknn_api.h
```

### 原因
本工程按 **rknpu2 v1.x** 写的（和你仓库里 `yolov8-helmet-rk3588-multithread/include/rknn_api.h` 一致）：

| | v1.x（本工程） | v2.x |
|---|---|---|
| `rknn_init` | 5 个参数 `(ctx, model, size, flag, extend)` | 4 个参数 `(ctx, model, size, flag)` |
| `rknn_set_io_mem` | `(ctx, rknn_tensor_mem*, rknn_tensor_attr*)` | `(ctx, rknn_tensor_mem*, rknn_tensor_mem*)` |
| `rknn_tensor_mem` | 有 `virt_addr/phys_addr/fd/offset/size/flags` | 改成 `index/type/virt_addr/phys_addr/fd/size/stride/width/height/...` |

板子上 `/usr/include/rknn_api.h` 如果是 v2.x，编译期就会报上面那些错。

### 解决（推荐：强制用 v1.x 头文件）
```bash
# 只把 v1.x 的头文件单独放一个目录, 避免和 /usr/include 里的 v2 混
mkdir -p /root/include_v1
scp ../yolov8-helmet-rk3588-multithread/include/rknn_api.h root@192.168.31.77:/root/include_v1/
cd /root/rk3588_zero_copy
RKNN_INC=/root/include_v1 RKNN_LIB=/usr/lib/librknnrt.so ./build_native.sh
```

### 解决（改成 v2.x API）
如果板子只有 v2.x 头文件，改 `src/main.cc` 三处：
```cpp
// 1) rknn_init: 去掉最后一个 nullptr
- ret = rknn_init(&rctx, model_data, (uint32_t)model_size, RKNN_FLAG_PRIOR_HIGH, nullptr);
+ ret = rknn_init(&rctx, model_data, (uint32_t)model_size, RKNN_FLAG_PRIOR_HIGH);

// 2) rknn_set_io_mem: 第 3 个参数从 rknn_tensor_attr* 变成 rknn_tensor_mem*
- rknn_set_io_mem(rctx, in_mem, &bind_attr);
+ rknn_tensor_mem io_mem = {};
+ io_mem.index = 0;
+ io_mem.type   = RKNNA_TENSOR_INPUT;
+ io_mem.fd     = model_fd;
+ io_mem.virt_addr = model_ptr;
+ io_mem.size   = model_buf_size;
+ io_mem.stride = model_w * 3;
+ io_mem.width  = model_w;
+ io_mem.height = model_h;
+ io_mem.elemsize = 3;
+ rknn_set_io_mem(rctx, in_mem, &io_mem);

// 3) v2 里没有 rknn_create_mem_from_fd, 直接自己填 rknn_tensor_mem
```

---

## #5 `/usr/bin/ld: cannot find -lrknnrt` / `cannot find -lopencv_highgui` / `file format not recognized`

这是两个不同的错误，分开看：

### 5a `cannot find -lxxx`
**原因**：板子上只有 `libxxx.so`，没有 `libxxx.so -> libxxx.so.X` 的开发软链接，
或者库在 `/usr/local/lib` 而不在链接器默认搜索路径里。

**解决**：直接用**全路径**链接（`build_native.sh` 已经这么做了）：
```bash
RKNN_LIB=/usr/lib/librknnrt.so RGA_LIB=/usr/lib/librga.so ./build_native.sh
```
或者建软链接：
```bash
ln -sf /usr/lib/librknnrt.so /usr/lib/librknnrt.so.2
```

### 5b `file format not recognized: file format not recognized`
**原因**：**交叉编译没生效**，编出来的是 x86 的，但链接的是 aarch64 的 `.so`。

最常见的原因是 **build 目录是旧的** —— CMake 一旦在某个目录里配置过一次，
再改 `-DCMAKE_TOOLCHAIN_FILE` 是**不生效**的，它会继续用缓存里的编译器。

**诊断**：
```bash
grep CMAKE_CXX_COMPILER:FILEPATH build/aarch64/CMakeCache.txt
file build/aarch64/rk_zero_copy
```
**解决**：
```bash
rm -rf build/aarch64        # ← 关键, 必须删干净
cmake -B build/aarch64 -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-rockchip.cmake ...
```
或者直接在板子上原生编译，绕开这个问题（`build_native.sh`）。

---

## 附：不是编译错，但一定会遇到的运行时问题

| 报错 | 原因 | 处理 |
|---|---|---|
| `error while loading shared libraries: librknnrt.so` | 运行时找不到库 | `export LD_LIBRARY_PATH=/usr/lib:/root/lib:$LD_LIBRARY_PATH` |
| `[警告] 没有找到 h264_rkmpp 解码器` | FFmpeg 没编 rkmpp | 换带 rkmpp 的 FFmpeg（见 #2） |
| `[警告] dma-buf 不可用, 退化为 CPU 内存输入` | 没有 `/dev/dma_heap` | `ls /dev/dma_heap/`；没有就只能用 CPU 路径（仍能跑） |
| `[NV12] 数据源: CPU 映射地址` | 拿不到解码器的 dma-buf fd | 设 `RK_NO_MPP_OPAQUE=1` 试；正常应显示 `dma-buf fd` |
| `rknn_init 失败: -x` | 模型版本和 runtime 不匹配 | 模型是 rknn-toolkit2 2.3.2 导出的，runtime 太老会失败 |
| 段错误 | 多半是 rknn_api.h 版本不匹配 | 见 #4 |

---

## 最快的一条路（不想踩坑）

如果只是为了**先验证后处理对不对**，用 AI 算法组已经验证过的单文件程序，
它只依赖 librknnrt + OpenCV，不用 FFmpeg/RGA/MPP：

```bash
scp rknn_cpp/rknn_yolov8_infer.cc root@192.168.31.77:/root/
scp ../yolov8-helmet-rk3588-multithread/model/best.rknn       root@192.168.31.77:/root/
scp ../yolov8-helmet-rk3588-multithread/model/labels_list.txt root@192.168.31.77:/root/
ssh root@192.168.31.77 '
  g++ -O2 -std=c++14 rknn_yolov8_infer.cc -o rknn_yolov8_infer \
      -I/root/include -L/root/lib -lrknnrt \
      -lopencv_core -lopencv_imgproc -lopencv_imgcodecs -lopencv_highgui
  export LD_LIBRARY_PATH=/root/lib:$LD_LIBRARY_PATH
  ./rknn_yolov8_infer /root/best.rknn /root/test.jpg
'
```
用它先确认「9 输出 DFL 后处理 + best.rknn」这条链路本身是通的，
再回头排本工程的 FFmpeg/RGA 部分，问题会好定位很多。
