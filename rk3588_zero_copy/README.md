# RK3588 零拷贝视频推理链路（已合并 AI 算法组板端推理代码）

RTSP(H.264) → FFmpeg 硬解 → NV12 → RGA 缩放/转 RGB888 → RKNN YOLOv8 → OpenCV 画框显示

后处理部分移植自 `rknn_cpp/rknn_yolov8_infer.cc`（AI 算法组），支持 9 输出 fork 格式。

---

## 📑 交付文档索引（2026-11-13）

| 文档 | 适合谁 | 内容 |
|---|---|---|
| **[《NPU 多核并行收益评估报告》](docs/NPU_PARALLEL_EVAL.md)** | 架构 / 排产 | `--npu-pipe` 真实收益实测(1/2/3 实例)、带宽争抢代价、12 路 300FPS 能不能一块板扛 |
| **[《网页控制台使用说明》](docs/WEB_CONSOLE.md)** | 现场演示 / 调性能 | 浏览器里看 MPP/RGA/NPU 实时负载与 FPS, 点开关动态切换硬解/加速/推理 |
| **[《RK3588 工程化落地与避坑指南》](docs/RK3588_ENGINEERING_GUIDE.md)** | 技术负责人 / 接手的人 | 点亮硬件、死机排查、内存泄漏、性能诊断、精度回归、NPU 调度的完整排雷记录与避坑清单 |
| **[《系统能力对比表》](docs/SYSTEM_CAPABILITY_TABLE.md)** | 决策者 / PPT | 三种配置的 FPS、逐段耗时、硬件加速比、负载、温度、内存、精度、调度能力汇总 |
| **[《演示流程傻瓜式手册》](docs/DEMO_RUNBOOK.md)** | 现场演示的人 | 从上电到网页看到真实车牌 JSON，每步命令 + 期望日志 + 故障速查 |
| **[《遗留风险清单》](docs/RISK_REGISTER.md)** | 项目经理 / 客户对接 | TODO 扫描结果 + 18 项真实风险(分级/证据/处置建议) |
| [《结案报告》](docs/RESOLUTION_20261112.md) | 追溯 | 泄漏真凶、FPS 瓶颈、硬件基准、负载监控 |
| [《精度回归与联调》](docs/ACCURACY_AND_INTEGRATION_20261113.md) | 追溯 | INT8 vs FP16、车牌真值、MQTT 联调 |
| [《NPU 调度专项》](docs/NPU_SCHED_REPORT.md) | 追溯 | 六种绑核策略实测 + 负载截图 |
| [《夜间长跑报告》](docs/NIGHT_SOAK_REPORT.md) | 验收 | 无人值守长跑: 时长/帧数/重启/活堆/温度(每 10 分钟自动刷新) |

---

## 1. 最终工程目录

```
rk3588_zero_copy/
├── CMakeLists.txt
├── build.sh                        # 交叉编译(PC/SDK)
├── build_native.sh                 # 板端原生编译(只要 g++, 不需要 cmake)
├── deploy.sh                       # 连通性→自检→传源码/模型→板端编译→运行
├── README.md
├── TROUBLESHOOTING.md              # ★ 上板编译的 5 个高频错误 + 解决思路
├── cmake/
│   └── toolchain-rockchip.cmake    # Rockchip aarch64 交叉编译工具链
├── include/
│   ├── dmabuf.h                    # dma-buf 分配 / mmap / cache 同步 / 按地址反查 fd
│   ├── nv12_frame.h                # ★ 轻量帧描述(fd+尺寸+格式), MPP->RGA 的交接单
│   ├── ipm.h                       # 逆透视标定 + 鸟瞰投影（GroundPlane / BevView）
│   ├── mpp_compat.h                # MPP_BUFFER → dma-buf fd 的版本兼容层
│   ├── rga_convert.h               # RGA NV12→RGB888（improcess）
│   ├── safe_probe.h                # 受控崩溃探测（保证降级不崩）
│   ├── speed_track.h               # ★ 跟踪 + 测速骨架（Med5 / ByteTrack-lite / 速度估计）
│   ├── stats.h                     # 各阶段耗时统计
│   ├── engine_ctl.h                # ★ 运行时开关(MPP/RGA/NPU) + 状态上报 + 硬件负载采集
│   ├── mqtt_sub.h                  # ★ 极简 MQTT 订阅端(收控制指令)
│   └── yolo_post.h                 # YOLOv8 后处理（形态A / 形态B 9 输出 DFL）+ 几何过滤
├── src/
│   ├── main.cc                     # 主流程
│   ├── dmabuf.cc
│   ├── engine_ctl.cc               # ★ 三个开关的原子状态 + 状态上报线程 + 负载采样
│   ├── ipm.cc                      # ← 移植 speed_demo.py 的 GroundPlane / BevView
│   ├── mqtt_sub.cc                 # ★ MQTT 订阅端实现(零依赖)
│   ├── rga_convert.cc
│   ├── safe_probe.cc
│   ├── speed_track.cc              # ← 移植架构组伪代码的模块 A/C/D/E
│   └── yolo_post.cc                # ← 移植 AI 算法组的 post_process_A / post_process_B
├── scripts/
│   ├── web_console.py              # ★ 网页控制台(PC 端, 零依赖): 实时负载 + 动态开关
│   ├── start_web_console.sh        # ★ 一键启动网页控制台
│   ├── hw_load_mqtt.py             # ★ 板端 root 负载采样器(NPU/RGA 内核真值)
│   └── mqtt_sub.py / mqtt_web.py   # 抓包/看板小工具
├── docs/
│   ├── WEB_CONSOLE.md              # ★ 网页控制台使用说明(部署 + 验收步骤 + 排错)
│   ├── ZERO_COPY_MPP_RGA.md        # ★ 零拷贝改造(MPP->RGA->NPU): 改动/根因/实测
├── tests/                          # ★ 与 Python 基线的对拍测试
    ├── test_speed.cpp              # C++ 侧(只依赖 ipm/speed_track/yolo_post, 无需 RKNN)
    ├── py_reference.py             # Python 侧(直接 import speed_demo.py 的真实实现)
    ├── compare.py                  # 带容差的逐行对比
    ├── run_parity.sh               # 一键跑对拍(用 docker 编译 C++)
    └── stubs/                      # FFmpeg/OpenCV/MPP 的最小 stub, 用来做语法检查
```

复用同级目录 `../yolov8-helmet-rk3588-multithread` 里的 `rknn_api.h` /
`librknnrt.so` / `librga.so`(1.9.1)，用 `-DRK_DEMO_ROOT=` 指定。

---

## 2. 从 AI 算法组代码合并了什么

| 来源 | 内容 | 去向 |
|---|---|---|
| `rknn_yolov8_infer.cc` | `OutTensor` 统一访问器（float 直读 / int8 现场反量化） | `include/yolo_post.h` |
| `rknn_yolov8_infer.cc` | `post_process_A` 单输出 `[1, 4+nc, num_anchors]` | `src/yolo_post.cc` |
| `rknn_yolov8_infer.cc` | `post_process_B` 9 输出 fork + CPU DFL 解码 | `src/yolo_post.cc` |
| `rknn_yolov8_infer.cc` | `nms_per_class` 分类别贪心 NMS | `src/yolo_post.cc` |
| `rknn_yolov8_infer.cc` | `rknn_query` 取每个输出的 shape / zp / scale | `src/main.cc` |
| `test_rknn_infer.py` | 9 输出布局约定、DFL 通道顺序 `b*16+i`、anchor 中心 `(j+0.5)*stride` | `src/yolo_post.cc` |
| `test_rknn_infer.py` | "cls 已是概率，不要再 sigmoid" 的实证结论 | 已在两个 post_process 里遵守 |

**预处理默认改成"拉伸缩放"**（和 AI 算法组板端代码一致），不是 letterbox。
`--letterbox` 可以切回等比 letterbox，两种模式的框坐标映射都做了正确处理。

9 输出 fork 布局（每尺度 3 个，stride 依次 8/16/32）：

```
out[3s+0] box = [1, 4*DFL_LEN=64, H, W]   原始 DFL logits, CPU 上要解
out[3s+1] cls = [1, nc, H, W]              概率（fork 里已 sigmoid）
out[3s+2] sum = [1, 1, H, W]               各类别概率和, 用于快速预过滤
```

DFL 解码：`dist[b] = Σ_i softmax(logits[b*16..b*16+15])[i] * i`，
框中心 `cx=(j+0.5)*stride, cy=(i+0.5)*stride`，框为 `l,t,r,b`。
（实现里对 softmax 减了最大值做数值稳定处理，与不减的写法数学等价。）

---

## 3. FFmpeg 兼容性：三级降级，保证不崩

`frame->buf[0]` 的 opaque 是不是 `MPP_BUFFER`，各 FFmpeg 分支并不统一。
强行当 MPP_BUFFER 解引用会**段错误**。现在的处理：

```
① opaque 探测
   ├─ 先做指针合理性检查（非空 / 8 字节对齐 / 用户态地址范围）
   ├─ 再在 SafeProbe 保护下调 mpp_buffer_get_ptr()
   │    └─ 崩了？SIGSEGV handler 里 siglongjmp 跳回 → 判定"不是 MPP_BUFFER"
   └─ 校验 mpp_buffer_get_ptr() 的返回值 == frame->data[0]
        └─ 通过 → mpp_buffer_get_fd() 拿 dma-buf fd

② /proc 反查（不碰 opaque，绝对安全）
   /proc/self/maps   找到包含 frame->data[0] 的映射 → 拿设备路径 + inode
   /proc/self/fd/*   readlink 对上设备路径
   /proc/self/fdinfo/*  "ino:" 对上 inode  → 这就是那个 dma-buf fd

③ 都没有 → 用 frame->data[0] 的 CPU 映射地址
   RGA 直接 DMA 读这块内存（不产生 CPU 内存拷贝，只是少了一次 fd import 优化）
```

三种模式都会在启动时打印一次：

```
[NV12] 数据源: dma-buf fd (RGA import)
[NV12] 数据源: CPU 映射地址 (RGA DMA 直读)
```

环境变量开关：

| 变量 | 作用 |
|---|---|
| `RK_NO_MPP_OPAQUE=1` | 完全跳过 ①（你怀疑 FFmpeg 版本对不上时用） |
| `RK_NO_MPP_FD_LOOKUP=1` | 跳过 ②，只走 ③ |

> `SafeProbe` 用 `sigsetjmp/siglongjmp` 捕获 SIGSEGV，只在单线程的解码路径上调用，
> 被探测函数无锁无副作用。真实崩溃（探测之外）不会被吞掉，进程仍然会退出。

---

## 4. 交叉编译

### 4.1 前置检查（在编译机上执行）

```bash
# 1) 工具链
ls /path/to/rk-linux/toolchain/aarch64-rockchip-linux-gcc/bin/aarch64-rockchip-linux-g++
# 或者 SDK 里的：
ls $SDK/rk-linux/toolchain/aarch64-rockchip-linux-gcc/bin/

# 2) sysroot 里必须有带 rkmpp 的 FFmpeg
ls $TOOLCHAIN/sysroot/usr/lib/aarch64-linux-gnu/libavformat.so*
ls $TOOLCHAIN/sysroot/usr/include/libavformat/avformat.h

# 3) OpenCV (aarch64)
ls $TOOLCHAIN/sysroot/usr/include/opencv4/opencv2/core.hpp
ls $TOOLCHAIN/sysroot/usr/lib/aarch64-linux-gnu/libopencv_core.so*

# 4) RKNN / RGA 头文件和库 (从同级 rockchip demo 目录复用)
ls ../yolov8-helmet-rk3588-multithread/include/rknn_api.h
ls ../yolov8-helmet-rk3588-multithread/include/librknnrt.so
ls ../yolov8-helmet-rk3588-multithread/include/3rdparty/rga/RK3588/lib/Linux/aarch64/librga.so
```

### 4.2 编译命令（完整、显式）

```bash
cd /path/to/rk3588_zero_copy

export TOOLCHAIN=/path/to/rk-linux/toolchain/aarch64-rockchip-linux-gcc
export RK_DEMO_ROOT=/path/to/yolov8-helmet-rk3588-multithread

rm -rf build/aarch64 && mkdir -p build/aarch64 && cd build/aarch64

cmake ../.. \
  -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-rockchip.cmake \
  -DTOOLCHAIN_ROOT=$TOOLCHAIN \
  -DRK_DEMO_ROOT=$RK_DEMO_ROOT \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/path/to/rk3588_zero_copy/install

make -j8
```

等价的简写：`TOOLCHAIN=... RK_DEMO_ROOT=... ./build.sh`

**FFmpeg 不在 sysroot 里时**（自己编的 FFmpeg 装在别处）追加：

```bash
  -DCMAKE_PREFIX_PATH=/opt/ffmpeg-rk/lib \
  -DFFMPEG_INCLUDE_DIRS=/opt/ffmpeg-rk/include \
  -DFFMPEG_LIBRARIES="/opt/ffmpeg-rk/lib/libavformat.so;/opt/ffmpeg-rk/lib/libavcodec.so;/opt/ffmpeg-rk/lib/libavutil.so"
```

**OpenCV 的 cmake config 找不到时**追加：

```bash
  -DOpenCV_DIR=$TOOLCHAIN/sysroot/usr/lib/aarch64-linux-gnu/cmake/opencv4
```
（CMakeLists 里已经带了一套手工找 `libopencv_*.so` 的兜底，通常不用管。）

### 4.3 链接的库
| 库 | 用途 | 来源 |
|---|---|---|
| `librockchip_mpp.so` | MPP（拿解码 dma-buf fd） | SDK `/usr/lib` |
| `librga.so` | RGA 2D 缩放/格式转换 | `RK_DEMO_ROOT/include/3rdparty/rga/RK3588/lib/Linux/aarch64/` |
| `librknnrt.so` | RKNN runtime（NPU 推理） | `RK_DEMO_ROOT/include/` |
| `libavformat/libavcodec/libavutil` | 拉流 + 硬解 | SDK sysroot（**必须带 rkmpp 解码器**） |
| `libopencv_core/imgproc/highgui/imgcodecs` | 画框显示 | SDK sysroot |
| `libdl` | `dlsym` 探测 `mpp_buffer_get_fd` | 系统 |

FFmpeg 必须是带 rkmpp 的版本，编译时：

```bash
./configure --prefix=/usr \
  --enable-cross-compile --cross-prefix=aarch64-linux-gnu- \
  --enable-avcodec --enable-avformat --enable-swscale \
  --enable-decoder=h264_rkmpp --enable-decoder=h265_rkmpp \
  --enable-ffmpeg-rkmpp --disable-debug
```

### 4.4 板端原生编译（推荐走这条）

工程是**自包含**的：头文件（`rknn_api.h` / `im2d.h`）和运行期库
（`librknnrt.so` / `librga.so`）都在 `third_party/`，模型和标签在 `model/`，
**整个目录拷到板子上就能直接编**，不用管 SDK 路径。

板子上（Armbian / Debian 系）一条命令搞定：

```bash
cd /path/to/rk3588_zero_copy
chmod +x board_setup.sh run_demo.sh
./board_setup.sh        # 环境自检 + 装依赖 + cmake 编译
./run_demo.sh           # 跑起来
```

`board_setup.sh` 做的事：

1. **环境自检**：系统版本、NPU/RGA/MPP/dma_heap 设备节点、系统里的 Rockchip 库、
   工具链、**FFmpeg 有没有 rkmpp 硬解码器**、OpenCV
2. **装依赖**（见下面的 apt 清单）
3. **cmake 编译**：`cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)`

### 4.5 交叉编译还是板端原生？

**板端原生编译**，理由：

| | 板端原生 | 交叉编译 |
|---|---|---|
| 工具链 | apt 装 `build-essential` 就有 | 要 Rockchip SDK 的 `aarch64-rockchip-linux-gcc` |
| 依赖 | 系统 apt 装 OpenCV/FFmpeg 开发包 | 要把 SDK sysroot 里的库都带过去 |
| 迭代 | 改一行 `scp` 一下就重编 | 同样要传，但工具链还得先配好 |
| 产物 | 天然是 aarch64 | 需要指定 sysroot / toolchain.cmake |
| 适合 | 调试、验证 | CI / 出包 / 量产 |

本工程 7 个源文件，板端全量编译约 30~60 秒（8 核 A76），完全够用。
交叉编译那条路（`build.sh` + `cmake/toolchain-rockchip.cmake`）保留着，
需要出包时再用。

---

## 5. 板端依赖清单（apt）

```bash
sudo apt update
sudo apt install -y \
    build-essential \      # g++ gcc make
    cmake \
    pkg-config \
    libopencv-dev \        # opencv2/core.hpp, libopencv_core/imgproc/highgui
    libavformat-dev \      # libavformat/avformat.h
    libavcodec-dev \       # libavcodec/avcodec.h
    libavutil-dev \        # libavutil/error.h (av_err2str)
    libswscale-dev \       # 备用
    ffmpeg \               # 命令行(自检用), 带 rkmpp 解码器的另说
    libdrm-dev             # DRM GEM dma-buf 备用路径
```

**不需要 apt 装的**（工程自带）：

| 东西 | 来源 | 放哪 |
|---|---|---|
| `rknn_api.h` | AI 算法组 demo | `third_party/include/` |
| `im2d.h` / `rga.h` 等 | librga 1.9.1 | `third_party/include/rga/` |
| `librknnrt.so` | AI 算法组 demo | `third_party/lib/` |
| `librga.so` | librga 1.9.1 | `third_party/lib/` |
| `best.rknn` + `labels_list.txt` | 你的模型 | `model/` |

### ⚠️ 关于 FFmpeg 硬解（h264_rkmpp）

**系统 apt 的 FFmpeg 一律没有 `h264_rkmpp` 解码器**（Debian/Ubuntu 不带
Rockchip MPP 支持）。所以第一次跑：

- 程序会打印 `[警告] 没有找到 h264_rkmpp 解码器, 退回软件解码`
- **流水线照样跑通**（FFmpeg → NV12 → RGA → RKNN → 画框），只是：
  - NV12 源在 CPU 内存里，HUD 显示 `NV12:cpu-ptr` 而不是 `NV12:dma-buf`
  - CPU 占用高一些（软解 1080p H.264 大约 15~25ms/帧）
  - RGA→NPU 那段仍然是零拷贝（这块不受影响）

要真硬解，需要从 Rockchip SDK 额外拷两样东西到板子：

```bash
# SDK 里: <sdk>/rk-linux/lib/librockchip_mpp.so
#        <sdk>/usr/lib/libavcodec.so 等(带 rkmpp 的 FFmpeg)
sudo cp librockchip_mpp.so /usr/lib/
sudo cp libavcodec.so libavformat.so libavutil.so /usr/lib/
# 确认
ffmpeg -decoders | grep rkmpp
```
拷完再跑，日志里应该出现 `[NV12] 数据源: dma-buf fd (RGA import)`。

### 5.0 板子没有外网？离线装依赖（Windows 侧一条命令搞定）

板子无外网时 `apt install` 必然失败，用两个脚本走离线通道：

```powershell
# Windows 上: 解析 Ubuntu Ports 索引 -> 解依赖闭包 -> 下载全部 arm64 .deb
cd E:\shenzhenproject
powershell -ExecutionPolicy Bypass -File .\download_deps.ps1
# 产物: E:\shenzhenproject\board_deps\  (几百个 .deb + manifest.txt)

# 传到板子
scp -r board_deps seeed@192.168.31.77:$HOME/

# 板子上装
ssh seeed@192.168.31.77 'cd $HOME && chmod +x install_deps.sh && sudo ./install_deps.sh board_deps'
```

实测体积（Ubuntu 26.04 "resolute" / arm64）：

| 组合 | 包数 | 体积 |
|---|---|---|
| 全量（含 OpenCV + 编译工具链） | 564 | 430 MB |
| 不含 OpenCV | 316 | 249 MB |
| 不含 OpenCV 和工具链（板子上已有 gcc/cmake） | 235 | 149 MB |

板子上大概率已经有 gcc/cmake/OpenCV（KDE 桌面系统），所以推荐：

```powershell
powershell -ExecutionPolicy Bypass -File .\download_deps.ps1 -NoOpenCV -NoToolchain
```

`download_deps.ps1` 做的事：拉 `main/universe/multiverse` 的 `Packages.gz` → 解析成
包表 → **从你要的包做依赖闭包 BFS** → curl 并行下载 → 按索引里的 Size 逐个校验 →
失败的换备用源重下 → 生成 manifest.txt。**只收 `Architecture: arm64/all` 的包**，
绝不可能下成 x86 的。

> `install_deps.sh` 装之前会逐个检查 .deb 架构，出现非 arm64 立刻中止；
> 板子无外网时**不要**用 `apt-get -f install`（它会去网上拉依赖），
> 缺什么就把对应的 .deb 再下一份放进 `board_deps/` 重跑即可。

### 5.1 从笔记本传到板子

工程是自包含的，整个目录传过去就行（约 12MB）：

```bash
# 笔记本上(Windows Git Bash)
IP=192.168.31.77
cd /e/shenzhenproject
tar czf /tmp/rk3588_zero_copy.tgz -C . rk3588_zero_copy
scp /tmp/rk3588_zero_copy.tgz seed@$IP:$HOME/
```
板子上：
```bash
cd ~ && tar xzf rk3588_zero_copy.tgz
cd rk3588_zero_copy
chmod +x *.sh tests/*.sh
./board_setup.sh
```

### 5.2 运行

```bash
# 默认：RTSP + 模型 + 标签
./run_demo.sh

# 不显示窗口，纯跑性能（SSH 场景推荐）
./run_demo.sh --no-show --frames 300

# 打开测速（IPM 模式，参数是 0124 素材的标定值，相机流要先重新标定）
./run_demo.sh --no-show --frames 300 \
  --ipm-points "300,622;1500,643;1450,877;550,819" \
  --ipm-width-m 7.0 --ipm-fov 84 --speed-limit 25
```

也可以直接调可执行文件：

```bash
export LD_LIBRARY_PATH=./third_party/lib:$LD_LIBRARY_PATH
./build/rk_zero_copy \
    rtsp://192.168.31.200:8557/h264 \
    ./model/best.rknn \
    --labels ./model/labels_list.txt \
    --conf 0.25 --nms 0.60
```

### 5.3 板端测试（192.168.31.77）

```bash
IP=192.168.31.77
scp build/aarch64/rk_zero_copy root@$IP:/usr/local/bin/
scp $RK_DEMO_ROOT/model/best.rknn        root@$IP:/root/model/
scp $RK_DEMO_ROOT/model/labels_list.txt  root@$IP:/root/model/
```

### 5.1 上传

```bash
IP=192.168.31.77
BIN=/path/to/rk3588_zero_copy/build/aarch64/rk_zero_copy

scp $BIN root@$IP:/usr/local/bin/rk_zero_copy
scp $RK_DEMO_ROOT/model/best.rknn        root@$IP:/root/model/best.rknn
scp $RK_DEMO_ROOT/model/labels_list.txt  root@$IP:/root/model/labels_list.txt
```
（`mkdir -p /root/model` 先在板上建好）

或者直接：`IP=192.168.31.77 ./deploy.sh`

### 5.2 板端环境自检

```bash
ssh root@$IP '
  uname -a
  ls /dev/dma_heap/                      # 需要有 cma / system, 否则会退化成 CPU 内存
  ls -l /usr/lib/librknnrt.so /usr/lib/librga.so /usr/lib/librockchip_mpp.so
  ffmpeg -decoders 2>/dev/null | grep rkmpp    # 需要看到 h264_rkmpp
  cat /sys/kernel/debug/rknpu/version 2>/dev/null
'
```

### 5.3 跑

```bash
ssh root@$IP '
  export LD_LIBRARY_PATH=/usr/lib:$LD_LIBRARY_PATH
  /usr/local/bin/rk_zero_copy \
      rtsp://192.168.31.200:8557/h264 \
      /root/model/best.rknn \
      --labels /root/model/labels_list.txt \
      --conf 0.25 --nms 0.45 \
      --no-show --frames 300
'
```

带窗口显示（板子接了显示器/X11 时）：

```bash
  /usr/local/bin/rk_zero_copy rtsp://192.168.31.200:8557/h264 /root/model/best.rknn \
      --labels /root/model/labels_list.txt
```

排查 opaque 崩溃时：

```bash
  RK_NO_MPP_OPAQUE=1 /usr/local/bin/rk_zero_copy ... --no-show --frames 100
```

### 5.4 参数

| 参数 | 说明 |
|---|---|
| `--size WxH` | 模型输入尺寸（默认从 RKNN 属性读） |
| `--conf 0.25` | 置信度阈值 |
| `--nms 0.45` | NMS IoU 阈值 |
| `--labels f.txt` | 类别名，每行一个 |
| `--letterbox` | 等比 letterbox（默认拉伸缩放，与 AI 算法组板端代码一致） |
| `--int8` | 取原始量化输出（需要 AFFINE_ASYMMETRIC），不让驱动转 float |
| `--no-show` | 不开窗口，纯跑性能 |
| `--disp-width N` | 显示窗口最大宽度（默认 1280） |
| `--frames N` | 处理 N 帧后退出 |

---

## 6. 实现要点（踩过的坑）

**① NV12 的 plane 高度不能直接用 `frame->height`**
MPP 会把解码图像高度对齐到 16。用 `frame->data[1] - frame->data[0]` 除以
`linesize[0]` 反推真实行数填进 RGA 的 `hstride`，否则下半屏绿屏/花屏。

**② RGA 的 `wstride` 单位是【像素】, 不是字节**（2026-11-11 实测更正）

内核 RGA 驱动的尺寸校验是 `size = wstride * hstride * bpp(format)`，
所以 `wstride` 必须填**像素数**: RGB888 宽 640 → `wstride = 640`。
填字节数(1920)驱动会直接拒绝:
`rga: Only get buffer 1232896 byte from dma_buf_ptr, but current image required 3686400 byte`

实测见 `tests/probe_rga_stride.cc`: wstride=64 时纯拷贝与源**逐字节 100% 相同**,
wstride=192 只有 34.6% 相同。

本工程里 `RgaImage.wstride` 仍然用**字节**(CPU 兜底路径按字节 memcpy),
在 `rga_convert.cc` 的边界上换算成像素(`rga_wstride_pixels()`)。

> ⚠️ NV12 是例外: luma 一行本来就是 1 字节/像素, 像素数 == 字节数, 两种写法一样。

**③ librga 1.9.1 没有 `imcvt`**
只有 `improcess` / `imcvtcolor` / `imresize` / `immakeBorder`。
缩放+格式转换用 `improcess(src, dst, pat, srect, drect, prect, fence, fence, opt, usage)`，
指定 `drect` 就能把缩放结果写进目标缓冲的子区域（letterbox 靠这个）。

**④ librga 有两个"成功"值**
`imcheck_t` 返回 `IM_STATUS_NOERROR(2)`，`improcess` 返回 `IM_STATUS_SUCCESS(1)`。
只判 `== IM_STATUS_NOERROR` 会把成功当失败。

**⑤ RKNN 零拷贝要用 `RKNN_QUERY_NATIVE_NHWC_INPUT_ATTR`**
rknpu2 v1.x 里 NPU 内部输入是 NCHW。`RKNN_QUERY_INPUT_ATTR` 拿到的是对外的
NCHW 视图，按它绑内存驱动会自己做一次 NHWC→NCHW 转换（等于多一次拷贝）。
查原生 NHWC 属性 + `pass_through=1` 绑定，NPU 直接吃 RGA 产出的 packed RGB，
整条链路一帧图像只存在一份物理内存。

**⑥ `rknn_create_mem_from_fd` 的内存必须是 dma-buf**
普通 malloc 内存不行，驱动要 import。`dma-heap` / DRM GEM 出来的都是 dma-buf。

**⑦ OpenCV 是 BGR，RGA 输出的是 RGB**
显示那一路直接让 RGA 输出 `RK_FORMAT_BGR_888`，OpenCV 零成本直接用；
送 NPU 那一路必须输出 `RK_FORMAT_RGB_888`。

**⑧ cls 已经是概率，不要再 sigmoid**
两种导出形态都是。套两层 sigmoid 会把分数压到 [0.5, 0.731]，阈值完全失效。

**⑨ 形态 A 的输出是 CHW**
`out[c * num_anchors + a]`，**不要** reshape 成 `[num_anchors, 4+nc]`，
reshape 会把框和分数全部串位。

**⑩ letterbox 的 padding 要填 114**
RGA 只写中间的图像区域，四周自己填 114 灰，否则检测框位置偏。

---

## 7. 排错

| 现象 | 原因 | 处理 |
|---|---|---|
| `h264_rkmpp` 找不到 | FFmpeg 没编 rkmpp 解码器 | 重编 FFmpeg（见 4.2） |
| 解码器打开失败 / 花屏 | extradata 不是 Annex-B | RTSP 一般是 Annex-B；过 MP4 要先 `h264_mp4toannexb` |
| `dma_buf_alloc` 返回 -1 | 没有 `/dev/dma_heap` | 用 DRM GEM 路径，或自研分配；程序自动退化成 CPU 内存 |
| RGA `imcheck` 报错 | wstride/hstride/format 不对 | 对照坑 ①② 排查 |
| RGA 转换耗时很高 | RGA3 对 YUV→RGB 缩放有格式限制 | 两步走：NV12→RGB888(原尺寸) 再 RGB888→RGB888(缩放) |
| 颜色偏红/偏蓝 | 颜色空间不匹配 | RGA `color_space_mode` 改 `IM_YUV_BT709_LIMIT_RANGE` |
| 检测框整体偏移 | 预处理和后处理的缩放方式不一致 | 确认默认是拉伸缩放；用 `--letterbox` 时 padding 必须是 114 |
| 检测结果全空 / 很差 | 模型归一化和训练时不一致 | 转换模型时 `mean_values=[0,0,0], std_values=[255,255,255]` |
| 框和分数串位 | 形态 A 的输出被 reshape 了 | 见坑 ⑨，本工程是按 CHW 直接索引的 |
| `rknn_set_io_mem` 失败 | rknnrt 版本太老 | 升级 rknpu2 到 2.x；或退回 `rknn_inputs_set` |
| 延迟 2 秒左右 | RTSP 抖动缓冲 | `rtsp_transport=tcp` + `max_delay=500000` + `avioflags=direct` |
| 段错误在取 opaque 那里 | FFmpeg 的 rkmpp 内部实现变了 | `export RK_NO_MPP_OPAQUE=1`（现在已经不会崩了，会自动降级） |
| 日志出现 `[RGA] RGA 不可用，原因：权限不足…` | `/dev/rga` 的权限当前用户打不开 | `sudo chmod 666 /dev/rga`（或把用户加进节点所属的组）；`RGA_DEVICE=/dev/xxx` 可换节点 |
| 日志出现 `[RGA] RGA 不可用，原因：设备节点不存在…` | 内核没编 RGA 驱动 | `ls /dev/rga* ; dmesg \| grep -i rga`；DRM 模式下用 `RGA_DEVICE=/dev/dri/renderD128` |
| 日志出现 `[RGA] RGA 不可用，原因：rga_init() 内部崩溃…` | 设备能打开，但 librga 和内核驱动对不上 | 换与内核匹配的 librga；注意 librga 1.9.1 固定 open("/dev/rga")，不认 `RGA_DEVICE` |

### 7.1 RGA 不可用时的判断顺序

`src/rga_convert.cc` 里的降级判断分三步，三种原因在日志上是三行不同的文字：

| 步骤 | 检查什么 | 日志 |
|---|---|---|
| ① | 自己 `open()` 设备节点（**不碰 librga，绝不会崩**） | `[RGA] 设备节点 /dev/rga 可以打开` / `[RGA] RGA 不可用，原因：权限不足…` |
| ② | 源/目标是不是真 dma-buf（fd ≥ 0） | `[RGA] RGA 不可用，原因：源/目标不是 dma-buf(fd<0)…` |
| ③ | 才让 librga 初始化（带日志 + SafeProbe 兜底） | `[RGA] 进入 rga_init() 探测` / `[RGA] rga_init() 正常返回…` / `[RGA] RGA 不可用，原因：rga_init() 内部崩溃…` |

设备节点路径从环境变量 **`RGA_DEVICE`** 读，默认 `/dev/rga`。
（`RGA_DEVICE` 也是 librga ≥ 1.10 自己认的变量名；本工程带的 librga 是 1.9.1，它固定 open("/dev/rga")。）

回归测试：`./tests/run_rga_init_guard.sh`（用真的 librga 和一个故意崩溃的 stub 各跑一遍）。

---

## 8. 模型元数据（已从 best.rknn 里解出来核对过）

`yolov8-helmet-rk3588-multithread/model/best.rknn`：

```
magic: RKNN  ver 6
toolkit: rknn-toolkit2 2.3.2 (e045de294f@2025-04-07)   target: rk3588

输入  images  [1, 3, 640, 640]   逻辑 layout=nchw
      原生 layout=NHWC  dtype=int8
      mean=[0,0,0]  std=[255,255,255]  rgb2bgr=False
      → 必须喂 uint8 RGB NHWC，不要自己再除 255

输出（9 个，每尺度 [box, cls, sum]）:
  idx0  271                     [1, 64, 80, 80]   box (4*DFL_LEN=64)  stride 8
  idx1  onnx::ReduceSum_277     [1,  2, 80, 80]   cls (nc=2)          stride 8
  idx2  282                     [1,  1, 80, 80]   sum                 stride 8
  idx3  287                     [1, 64, 40, 40]   box                 stride 16
  idx4  onnx::ReduceSum_293     [1,  2, 40, 40]   cls                 stride 16
  idx5  297                     [1,  1, 40, 40]   sum                 stride 16
  idx6  302                     [1, 64, 20, 20]   box                 stride 32
  idx7  onnx::ReduceSum_308     [1,  2, 20, 20]   cls                 stride 32
  idx8  312                     [1,  1, 20, 20]   sum                 stride 32
  全部 dtype=int8, 逐层 asym 量化(min/max 见文件内 quant_tab)
```

三条直接结论：

1. **输出顺序就是 `[box, cls, sum] × 3 尺度`**，与 `post_process_fork_9out` 的
   `outs[s*3+0/1/2]` 假设完全一致，nc=2。
2. **原生输入是 NHWC int8** → 必须用 `RKNN_QUERY_NATIVE_NHWC_INPUT_ATTR` +
   `pass_through=1` 绑定，否则驱动会自己转一次 NCHW，零拷贝就断了。
3. **类别数 2** → `model/labels_list.txt` 内容是 `head` / `helmet`
   （注意文件里是 CRLF，加载代码会剥掉 `\r`）。

---

## 9. 测速骨架移植（架构组 speed_stabilization_cpp.md）

按 `docs/speed_stabilization_cpp.md` 移植了 5 个模块，其中三条硬约束的落实位置：

| 约束 | 实现位置 | 说明 |
|---|---|---|
| **① 几何过滤必须前置** | `yolo_post.cc: is_bad_box()/filter_bad_boxes()`，在 `main.cc` 里**紧跟后处理、之前交给跟踪器** | 宽/高/面积比例 + "又宽又扁" |
| **② 抛弃 EMA，改用真实单调时钟** | `stats.h` 的 FPS 从 EMA 改成滑动窗口平均；`main.cc` 用 `std::chrono::steady_clock` 取时间戳 | 不用 frame_idx/fps 推时间 |
| **③ 地面点 5 帧滑动中值** | `speed_track.h: Med5`，x/y **分开**滤波 | 见下面"两个易错点" |

模块对应关系：

| 模块 | 内容 | 落点 |
|---|---|---|
| A 几何先验过滤 | `is_bad_box()` | `yolo_post.h/cc` |
| B NMS IoU 0.70→0.60 | `--nms` 默认改成 0.60 | `main.cc` |
| C 地面点中值 | `Med5`（环形缓冲+插入排序，K=5，零堆分配） | `speed_track.h` |
| D IPM 空间先验 | `GroundPlane` + `BevView`（含矩形修正） | `ipm.h/cc` |
| E 速度估计 + 显示中位数 | `speed_from_hist_chord/lsq` + `display_median` | `speed_track.cc` |

### 板端运行

```bash
# IPM 模式（推荐，0124 素材的标定参数）
rk_zero_copy rtsp://192.168.31.200:8557/h264 /root/model/best.rknn \
    --labels /root/model/labels_list.txt \
    --ipm-points "300,622;1500,643;1450,877;550,819" \
    --ipm-width-m 7.0 --ipm-fov 84 \
    --speed-limit 25

# 单点比例尺模式（没有 IPM 标定时）
rk_zero_copy <url> <model> --p1 "200,640" --p2 "1400,640" --real-m 7.0
```

新增参数：

| 参数 | 默认 | 说明 |
|---|---|---|
| `--no-speed` | 关 | 只做检测，不跟踪不测速 |
| `--ipm-points "x,y;x,y;x,y;x,y"` | 无 | 标定梯形 4 点（图像坐标） |
| `--ipm-width-m` | 3.5 | 标定区横跨宽度（米） |
| `--ipm-fov` | 0 | 水平 FOV（度）；给了就用它算焦距，不给则由两个灭点反解 |
| `--ipm-ppm` | 100 | 鸟瞰图 px/m |
| `--p1/--p2/--real-m` | 无 | 单点比例尺模式 |
| `--speed-limit` | 25 | 超速阈值 km/h |
| `--estimator chord\|lsq` | chord | 速度估计器（chord 与 Python 口径一致） |
| `--bad-w-ratio` | 0.70 | 模块 A：框宽/画面宽 上限 |
| `--ground-median` | 5 | 模块 C：地面点中值窗口（帧） |
| `--match-thresh` | 0.80 | 关联 IoU 阈值 |

### 两个易错点（都已在对拍测试里覆盖）

1. **`Med5` 丢的是"最旧的"不是"最小的"**
   Python 的 `deque(maxlen=5)` 按时间保留最近 5 个。只维护一个有序数组会丢掉
   时间顺序 → 丢的变成最小值 → 中位数算错。实现必须用环形缓冲保留插入顺序。
2. **窗口没满时也必须保持有序**
   否则 `v[n/2]` 取到的不是中位数。

### 关联阈值要按目标速度调

`--match-thresh 0.80` 是 IoU 阈值。**高速目标相邻帧位移大，IoU 会掉到阈值以下
→ 丢轨 → 速度永远攒不够样本**。例：框宽 60px、每帧移 50px 时 IoU 只有 0.09，
跟踪器会每帧都开新 track。实测请先看 HUD 上的 `tracks` 计数是否稳定，
掉轨就把它调到 0.3 左右。

### 与 Python 基线对拍（已通过）

```bash
cd tests && ./run_parity.sh
```
用 `speed_demo.py` 的**真实实现**（直接 import）生成基线，和 C++ 逐行对比：

```
[PASS] 43 行全部一致 (容差 0.0001)
```

覆盖：IPM 解算（焦距/归一化长度/矩形误差）、BEV 投影与梯形包含判定、
矩形修正、几何过滤的 8 个用例、Med5 的 7 步序列、跟踪+测速的合成序列
（0.5 m/s 期望 18 km/h，实测 3.600000 km/h 精确吻合）。

---

## 10. 业务层接口（MQTT 上行 / 串口控制卡）

### 现状与设计

`main.cc` 现在把业务层做成了**可插拔的 sink**，而不是把 MQTT/串口写死在推理循环里：

```
推理线程  ── 只做入队 ──▶  有界队列(32)  ──▶  后台线程 ──▶ ResultSink
                                                              ├─ StdoutSink  打印每帧结果
                                                              ├─ MqttSink     发臻识 ivs_result 协议
                                                              └─ LedSink      驱动 C5HE 控制卡(临显+语音)
```

**硬约束**：MQTT 的 TCP 连接和串口写都是阻塞操作，放在推理线程里会让 NPU 等 IO、
帧率直接崩。所以推理线程只 `push()`，真正的回调在后台线程执行；队列满了丢旧帧
（宁可丢业务数据也不拖慢视频流水线）。

接口在 `include/result_sink.h`：

```cpp
struct DetectResult { int track_id, cls_id; float score, speed_kmh;
                      bool speed_valid, over_speed; std::string plate; ... };

class ResultSink {
    virtual void on_result(const DetectResult& r) {}                 // 单条
    virtual void on_frame(int idx, const std::vector<DetectResult>&) // 一帧(MQTT 用这个)
    virtual void flush() {}
};
```

### MQTT 上行：直接对接现有 edge_controller

本工程的 C++ 可以**替代臻识相机**的角色，把检测结果按同一套协议发出去，
现有 `edge_main.py` / `edge_controller.py` 一行都不用改：

| 项 | 值 |
|---|---|
| Broker | `127.0.0.1:1883`（EMQX），QoS 0 或 1 |
| Topic | `device/<sn>/message/up/ivs_result`（订阅方用 `device/+/message/up/ivs_result`） |
| Payload | `{id,sn,name,version,timestamp,payload.AlarmInfoPlate.result.PlateResult{license,plates[],is_encrypted,confidence}}` |

> ★ **编码陷阱**：`license` 字段在协议里可能是 GBK Base64 也可能是 UTF-8 Base64，
> 接收端 `edge_controller.decode_license` 是**先试 UTF-8 再退 GBK/GB18030**。
> 本工程统一发 **UTF-8 Base64**（`base64_encode()`），与接收端的首选一致。

MQTT 客户端是自己实现的（`src/mqtt_pub.cc`，只做 CONNECT/PUBLISH/PUBACK），
**零第三方依赖** —— 板子上不用装 paho-mqtt-c。

### 串口控制卡（C5HE）

协议移植自同项目的 `led_controller.py`：

- 通用帧：`AA 55 | 流水号 | 地址(0x64) | 保留(2) | 命令字 | 长度(2,高在前) | 数据 | CRC16(2,高在前) | AF`
  - CRC16 = 标准 Modbus（poly 0xA001, init 0xFFFF），**校验位参与计算**（用 00 00 占位），
    **高字节在前**
- 语音帧（0xFD 开头，控制卡原样转发给 SYN6658）：
  `FD | 数据区长度(2,高在前) | 0x01=语音合成 | 0x01=GBK | GBK文本`
  - 这类帧**不要** AA55 包头、**不要** CRC、**不要** AF 结束符
- 临显（命令字 0x27）：`行号 | 时长(秒) | 颜色(1红2绿3黄) | 0 | GB2312文本(≤60字节)`
- 串口默认 9600 8N1，UTF-8→GBK 用 iconv 转换

### 启用方式

```bash
# 只打印结果
./run_demo.sh --sink-stdout

# MQTT 上行(EMQX 在板子上跑, 订阅 device/+/message/up/ivs_result 就能看到)
./run_demo.sh --sink-stdout \
  --mqtt-host 127.0.0.1 --mqtt-port 1883 --mqtt-qos 1 \
  --sn rk3588-01 --ipaddr 192.168.31.77

# 串口控制卡
./run_demo.sh --serial-port /dev/ttyUSB0 --serial-baud 9600 \
  --speak-text "欢迎回家" --display-text "业主车辆" --display-duration 10
```

| 参数 | 说明 |
|---|---|
| `--sink-stdout` | 每帧结果打到 stderr |
| `--mqtt-host/--mqtt-port` | 非空则启用 MQTT 上行 |
| `--mqtt-topic` | 默认 `device/<sn>/message/up/ivs_result` |
| `--mqtt-client-id` | 默认用 `--sn` |
| `--mqtt-qos` | 0 或 1（1 会等 PUBACK） |
| `--serial-port/--serial-baud` | 非空则启用串口 |
| `--speak-text/--display-text/--display-duration` | 控制卡文案（有车牌时用车牌代替） |
| `--sn/--device-name/--ipaddr` | 报文里的设备信息 |

> 当前模型是 head/helmet（不产车牌），所以 `PlateResult.license` 为空、
> `plates[]` 为空数组 —— 接收端会解析成"没识别到目标"而忽略。
> 等 LPRNet 接进来后，把解码出的车牌填进 `DetectResult::plate` 即可，
> 上行协议不用改。

---

## 11. 硬解：MPP 原生解码器（不依赖 ffmpeg-rockchip）

系统自带的 FFmpeg **没有 `h264_rkmpp` 解码器**（Debian/Ubuntu 不打包 Rockchip MPP 支持），
但 `librockchip-mpp` 的 .deb 是能离线下载的。所以本工程让 **FFmpeg 只做 RTSP 拉流，
解码交给 MPP 自己**（`src/mpp_decoder.cc`）：

```
mpp_create → mpi->control(MPP_DEC_SET_PARSER_SPLIT_MODE, ALL) → mpp_init(MPP_CTX_DEC, AVC)
           → mpi->decode_put_packet → mpi->decode_get_frame
           → mpp_frame_get_buffer → mpp_buffer_get_fd()  ← 真正的 dma-buf fd
```

| 参数 | 说明 |
|---|---|
| `--decoder auto\|mpp\|ffmpeg` | 默认 auto：MPP 可用就用 MPP，否则退回 FFmpeg（软解） |

**判断硬解是否生效（两个关键字）**：

```
[解码] 后端 = Rockchip MPP 硬解 (H.264, VPU)   ← 成功
[NV12] 数据源: dma-buf fd (RGA import)         ← 零拷贝路径打通
```

失败时会是 `[解码] 后端 = FFmpeg h264  <== 软解(...)` 或
`[警告] 编译时没有 MPP 支持(RK_HAVE_MPP=0)`。

编译期确认：`grep MPP build/CMakeCache.txt` 应有
`MPP_INC_DIR:PATH=/usr/include/rockchip` 和
`MPP_LIB:FILEPATH=/usr/lib/aarch64-linux-gnu/librockchip_mpp.so`。

---

## 12. PP-OCR 车牌识别接入（取代旧的 LPRNet）

YOLO 检出车牌框 → **裁出车牌 ROI 并按比例缩到高 48** → PP-OCRv4 rec →
CTC 解码 → 填进 `DetectResult::plate` → **MQTT 上行报文的 `license` 字段自动带上车牌**。

```bash
./build/rk_zero_copy ./video.mp4 ./model/ebike_int8_rk2332.rknn \
    --decoder mpp --no-speed --no-show \
    --ppocr ./model/ppocrv4_rec_fp16.rknn --plate-cls 1 \
    --roi-y 0.95 --roi-height 0.50 --roi-ratio 0.187 --roi-shift 0.128 \
    --ppocr-pad 0 --ppocr-invert 1
```

| 参数 | 默认 | 说明 |
|---|---|---|
| `--ppocr <model.rknn>` | 无 | 给了就启用（PP-OCRv4 rec，320×48） |
| `--plate-cls 0,2` | 无 | 哪些类别当车牌（类别 id，逗号分隔） |
| `--ppocr-dict PATH` | 模型同目录 `ppocr_keys_v1.txt` | 字典文件（一行一个字符） |
| `--ppocr-charset "0123..."` | 空（不过滤） | 字符白名单，见下"白名单的两个坑" |
| `--ppocr-min-score` | 0.30 | 车牌**检测框**的最低分 |
| `--ppocr-pad` | 0.10 | 车牌框外扩比例（框一般偏紧） |
| `--ppocr-ttl` | 2000ms | 同一条 track 的识别结果缓存（车牌不会每帧变） |
| `--ppocr-invert 0\|1` | 0 | 车牌图**灰度反白**（白字深色底 → 黑字白底） |
| `--ppocr-deskew 0\|1` | 0 | 车牌图纠偏（把斜着的文字行摆正） |
| `--ppocr-no-baked` | — | 模型**没有**固化归一化时用（板端自己做 `(x-127.5)/127.5`） |
| `--ppocr-no-rows` | — | 关掉两行车牌支持（默认开，见下"两行车牌"） |
| `--ppocr-split 0.45` | 0.45 | 两行车牌的切分点（占 ROI 高度的比例） |

> **老命令行兼容**：`--lprnet-invert` / `--lprnet-deskew` 仍然认（等价于
> `--ppocr-invert` / `--ppocr-deskew`）；`--lprnet` 和 `--plate-prefix`
> 会直接报错并告诉你换成什么 —— 因为 LPRNet 的模型喂给 PP-OCR 只会读出乱码。

### 12.0 为什么换掉 LPRNet

| | LPRNet（旧） | PP-OCRv4 rec（现） |
|---|---|---|
| 字符集 | 34 个 ASCII（`0-9A-Z`） | 6625 类，**含全部省份汉字** |
| 能否输出「苏」 | **结构上不可能** | 能（实测 `苏A`，conf 0.50~0.62） |
| 车牌前缀 | 只能在程序里**硬拼**「深圳」 | 直接吐画面里读到的明文，**不拼前缀** |
| 输入 | 硬拉成 160×48（3.3:1，字被压扁） | 按比例缩到高 48 + 右侧补零（官方 `resize_norm_img`） |
| 整牌准确率 | **0%**（217 张） | **86.6%**（FP16，217 张） |

「深圳」前缀以前是**编的**，不是从画面里读出来的 —— LPRNet 认不出省份汉字，
只能在识别结果前面硬接一个常量。现在这个常量连同它的命令行参数一起删掉了。

### 12.1 白名单的两个坑（`--ppocr-charset`）

1. **白名单必须包含 CTC blank**。CTC 靠 blank 分隔字符，而 40 个时间步里
   真正出字符的只有 7~8 个。白名单模式是在**每个时间步**取 argmax，
   把 blank 挡在外面 → 模型想吐 blank 的时间步被逼吐一个白名单字符 →
   结果变成 `LECTNACTCLN` 这种长串垃圾（实测真牌「苏A」读成 10 个字，置信度 0.01）。
2. **不能用 `ch.size() == 1` 判断"是不是一个字符"**。那是 ASCII 的写法；
   汉字在 UTF-8 里是 **3 个字节**，`size()==3` → 所有省份汉字都被过滤掉了，
   白名单里明明写了「苏」，结果永远读不出「苏」（实测：白名单开着读 `A`，
   关掉读 `苏A`）。正确做法是按 UTF-8 码点切开白名单，再按**整串**比较。

### 12.2 两行车牌（中国的电动车牌就是两行的）

PP-OCRv4 rec 是**单行**文字识别模型（输入 320×48 一行）。两行牌整块喂进去
读出来是乱码（实测那块绿牌整块 → `wm` conf 0.09；反白后 → `S` conf 0.07），
而把**上行单独裁出来** → `苏A`（conf 0.51），**下行单独裁出来** → `A981908`
（真值 `A98190B`，7/8 对）。

所以 ROI 宽高比小于 **2.5**（单行蓝牌约 3~4:1，两行绿牌约 1.3:1）时，
程序把这块**在源帧里**上下切开，各裁一次、各识别一次，再拼起来 ——
上行是省份+字母，下行是序号，拼起来正好是完整车牌。

上行对裁剪位置很敏感（切分点差 5%，结果就在 `A` 和 `苏A` 之间跳），
所以上行会试 **3 个切分点**（`--ppocr-split` ±0.05），取**认出字符最多**的那个。
注意**不能按置信度挑**：认对的 `苏A`(0.532) 反而比认错的 `A`(0.549) 分数低
—— 「苏」这个字本身就难认，孤零零一个 `A` 反而很自信。

### 12.3 模型选型：**必须用 FP16**

同一批裁剪图（真牌「苏A」上行）实测：

| 模型 | 整牌准确率(217张) | 读「苏A」的结果 |
|---|---|---|
| `ppocrv4_rec_fp16.rknn` | **86.6%** | `苏A`（conf 0.50~0.62）✅ |
| `ppocrv4_rec_int8.rknn`（calib200） | 77.4% | `A` / 空 ❌ |
| `ppocrv4_rec_int8.rknn`（calib24） | 80.7% | `A` / 空 ❌ |

**量化到 INT8 会把省份汉字直接丢掉**（校准集里汉字太少，量化时那几个类
被压没了）。要认省份汉字就用 FP16。

### 12.3b 识别前的高斯去噪（**必须开**）

车牌上的「苏」只有 **13×17 px**，PP-OCR 对它极其敏感 —— 实测同一块牌：

| ROI 预处理 | 读出 | 置信度 |
|---|---|---|
| 原样 | `A` | 0.447 |
| 高斯模糊 3×3 | `苏A` | 0.481 |
| **高斯模糊 5×5（默认）** | **`苏A`** | **0.611** |
| 高斯模糊 7×7 | `苏A` | 0.603 |
| 双边滤波 5 | `苏A` | 0.412 |
| 中值滤波 3 | `A` | 0.488 |

这么小的字上，传感器噪声和压缩痕迹是致命的。所以识别之前先抹一层
（`--ppocr-denoise 5`，默认开；`--ppocr-denoise 0` 关掉）。

### 12.4 车牌 ROI 走 CPU 转换，不走 RGA 硬件路径

RGA 硬件路径和 CPU 路径的 YUV→RGB **口径不一样**。同一块 NV12 实测：

| 路径 | 输出像素 |
|---|---|
| RGA 硬件 | `(63,63,63)` —— 亮度按 BT.601 limited 展开，**色度被压掉**，图偏暗偏灰 |
| CPU / OpenCV | `(0,133,0)` —— 色度保留 |

车牌上的小字（「苏」这种笔画细的汉字）对这点差别很敏感：同一块绿牌，
RGA 路径读成 `AL`（省份字丢了），CPU 路径读成 `苏A`。车牌 ROI 很小
（一百多像素宽），CPU 转换几十微秒，相对一次 NPU 推理（20ms）可以忽略，
所以车牌这条路专门走 CPU（`cpu_crop_to_rgb888()`），拿不到 CPU 可读的源时
再退回 RGA 硬件路径。检测那条主路仍然是 RGA 硬件路径（零拷贝），没动。

### 12.5 ROI 几何标定（testvideo3 的绿牌）

`testvideo3.mp4` 里唯一清晰的车牌是**两行绿色电动车牌 苏A / A98190B**
（约 116×86 px，在前方电动车的尾部）。检测框落在它的**储物箱**上，
框底离牌还有 200 多像素，所以 ROI 参数要重新标。

**板端实测参数**：

```bash
--roi-y 0.95 --roi-height 0.50 --roi-ratio 0.187 --roi-shift 0.128 \
--ppocr-pad 0 --ppocr-invert 1
```

标定方法：`output_debug/frame_*.jpg` 里画着**橙框(检测)**和**绿框(ROI)**；
先量出牌的真实像素位置，反推回车体框的比例参数，再在真实帧上离线穷举
（切分点 × ROI 高度），用真实模型逐个验证哪个组合能读出「苏A」。

**实测结果**（RK3588，testvideo3.mp4，300 帧）：

```
[PlateRec] 就绪: 输入 3x48x320 (NHWC) 输出 T=40 C=6625 字典=6625类 白名单=0 字符序=BGR
[PPOCR] 已启用: 牌类=1 缓冲=dma-buf(零拷贝) 最低分=0.30 外扩=10% 输入=320x48
        字典=./model/ppocr_keys_v1.txt 白名单=关(不过滤) 反白=关
[车牌] track=3 cls=1 score=0.32 box=621x146@(50,434) roi=116x72@(382,580) 裁剪=174x48 两行 -> "苏AA981908"
```

真值是 `苏A / A98190B`（上行省份+字母，下行序号）：**上行 `苏A` 完全正确，
下行 `A981908` 7/8 正确**（末位 `B` 被读成 `8`）。

MQTT 上行报文（臻识协议 + 自定义字段都在）：

```json
{"id":"rk3588-01","name":"ivs_result","plate_no":"苏AA981908",
 "payload":{"AlarmInfoPlate":{"result":{"PlateResult":{
     "license":"6IuPQUE5ODE5MDg=", "plates":[{"license":"6IuPQUE5ODE5MDg="}]}}}}}
```

`license` 是 `苏AA981908` 的 UTF-8 Base64（解出来还是 `苏AA981908`），
**没有「深圳」前缀** —— 前缀这个常量已经连同 `--plate-prefix` 一起删掉了。

⚠️ **两个要知道的限制**：

- **检测框本身不可靠**：`ebike_int8_rk2332.rknn` 在这个场景打的是前景杂物
  （蓝色储物箱、骑手），不是车体。框和牌的比例关系逐帧在变，所以 ROI 只能
  做到「大部分帧盖住牌」。要根治得换检测模型或上专用车牌检测器。
- **两行牌的上下行只能各读到一次**：上行「苏A」只有 ~17px 高，
  笔画细，识别对裁剪位置非常敏感（见 12.2）。

### 12.6 单图测试：`lpr_test`（拿一张照片单独试模型）

不跑整条流水线，直接拿一张图 → 预处理 → 送模型 → 打印识别结果。
换模型**只改命令行参数**，不用改代码。

```bash
# 编译（已经编过就跳过）
cd build && cmake .. && make lpr_test

# 跑（默认模型 model/lprnet_finetuned_int8.rknn + 默认图片 test_plate.jpg）
./run_lpr_test.sh --prefix 深圳

# 换图片 / 换模型
./run_lpr_test.sh 我的车牌.jpg
./run_lpr_test.sh 我的车牌.jpg --model model/xxx.rknn --charset-file dict.txt
```

输出：

```
[模型] model/lprnet_finetuned_int8.rknn
[图片] test_plate.jpg  (原图 400x120)
[预处理] -> 160x48  stretch  颜色=RGB  归一化=无
[模型输入] name=input type=INT8 fmt=NHWC dims=[1,48,160,3,] n_elems=23040 zp=0 scale=0.00784314
[模型输出] name=output type=INT8 fmt=NHWC dims=[1,20,35,]  -> T=20 C=35
[喂法] int8   [字符集] 34 个, 偏移=1
[耗时] 推理 1.25 ms
[识别结果] 深圳B12345
[置信度] 0.783
```

| 参数 | 默认 | 说明 |
|---|---|---|
| `--model PATH` | 必填 | rknn 模型 |
| `--image PATH` | 必填 | 待识别图片 |
| `--width/--height` | 自动 | 覆盖模型输入尺寸（默认从模型 attr 查） |
| `--feed int8\|float\|uint8` | 自动 | 覆盖喂法；默认按模型 attr 的 type 选，失败会自动换一种重试 |
| `--charset STR` | LPRNet 34 字符 | 字符集 |
| `--charset-file PATH` | 无 | 从文件读字符集（PP-OCR 那种几千字的字典用这个） |
| `--charset-offset N` | 自动 | 类别 k → 字符集[k-N]；默认按 `C == len+1` 自动判（=1） |
| `--prefix 深圳` | 空 | 车牌前缀（LPRNet 只出数字字母，前端要带省份就加这个） |
| `--crop x,y,w,h` | 无 | 先裁一块再缩放 |
| `--resize stretch\|letterbox\|center` | stretch | 缩放方式（整图用 letterbox/center） |
| `--color rgb\|bgr` | rgb | 通道顺序 |
| `--mean a,b,c` / `--std a,b,c` | 无 | 归一化（**模型里已固化就别给**，重复归一化输出全是垃圾） |
| `--min-score S` | 0 | 置信度低于 S 判为不可信 |
| `--save-preprocessed P` | 无 | 存出预处理后的图（肉眼核对有没有裁歪） |
| `--json` | 无 | 输出 JSON |
| `--repeat N` | 1 | 重复推理 N 次取平均耗时 |

**换模型的 checklist**（只要改参数）：

1. `--model` 指到新模型；
2. 输入宽高 / 布局(NHWC|NCHW) / dtype / 量化参数(zp, scale) **全部自动从模型里查**，
   不用管；
3. 字符集变了 → `--charset` 或 `--charset-file`；
4. 模型要求归一化 → `--mean/--std`（没要求就别给）；
5. 输出不是 `[1,T,字符集个数+1]` 这种 CTC 形状 → 工具会**打印警告**，
   不会给你一串没意义的字。

**板子上没有真实车牌照片**（摄像头对着办公桌），所以附了个合成图生成器：

```bash
python3 tests/gen_test_plate.py -o test_plate.jpg -t B12345
```

合成样式是调出来的：这个模型吃 **白底黑字 + 宽字符间距**（实测蓝底白字的整牌渲染
它只认出一个字母；白底黑字 + 0.4 字宽间距能全认出来，置信度 0.78~0.93）。
真照片直接丢进去即可，不用这个脚本。

**踩到的两个坑**（工具里已经处理掉）：

- 这个 runtime（RKNN api 2.3.2）**要求一次性把全部输出都取出来**，只取第 0 个会报
  `param output index(...) is out of range(N)`（`main.cc` 也是按这个约定写的）；
- int8 喂法要给**量化码个数**（不是字节数×4），喂错 size runtime 会越界读写缓冲，
  把堆踩坏。

---

## 13. 怎么验证"确实没有 CPU 拷贝"

1. 跑 `--no-show`，看各阶段耗时：RGA 那一步应该在 1~3ms（1080p→640x640），
   如果 15ms 以上，基本可以断定走的是 CPU 路径。
2. `cat /proc/<pid>/status | grep VmRSS`：稳定后 RSS 不应随帧数增长。
3. `cat /proc/<pid>/fdinfo/* | grep -A2 drm`：能看到 dma-buf fd。
4. 启动日志里 `[NV12] 数据源:` 那一行直接告诉你走的是 fd 还是 CPU 指针路径。
