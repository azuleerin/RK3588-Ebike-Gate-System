# MPP 硬解不生效 —— 排查手册

> 现象：P0 跑到"运行"这一步，日志停在
> ```
> 视频流: 1920x1080  fps=25.00  codec=h264
> [警告] 没有找到 h264_rkmpp 解码器, 退回软件解码
> 解码器: h264
> ```
> 之后**一行都没有**，连 `[解码] 后端 = ...` 都没有，程序直接没了。

---

## 一、先把两个"看起来像"的原因排除掉

### 1.1 不是 `RK_HAVE_MPP` 没定义（**不用改 CMakeLists.txt**）

`CMakeLists.txt` 里这两行在**同一个 if 分支**：

```cmake
if(MPP_LIB AND MPP_INC_DIR)
  target_compile_definitions(rk_zero_copy PRIVATE RK_HAVE_MPP=1)
  message(STATUS "MPP        = ${MPP_LIB} (header ${MPP_INC_DIR})")
else()
  target_compile_definitions(rk_zero_copy PRIVATE RK_HAVE_MPP=0)
  message(WARNING "找不到 librockchip_mpp, ...")
endif()
```

你贴的 cmake 输出是

```
-- MPP = /usr/lib/aarch64-linux-gnu/librockchip_mpp.so (header /usr/include/rockchip)
```

**这正是 true 分支打印的** → `RK_HAVE_MPP=1` 确凿已定义。

反过来看日志也能自证：如果是 `RK_HAVE_MPP=0`，代码会打

```
[警告] 编译时没有 MPP 支持(RK_HAVE_MPP=0), 无法用 MPP 硬解
[解码] 后端 = FFmpeg h264  <== 软解(...)
```

如果是 `mpp_create()` **干净地**返回失败，也会打

```
[警告] MPP 初始化失败: ...
[解码] 后端 = FFmpeg h264  <== 软解(...)
```

**这两组字一条都没出现** → 说明程序根本没走到打印那句的地方。

### 1.2 不是参数解析漏洞

`--decoder` 在 `main.cc` 有正常解析（`opt.decoder = argv[++i]`），
而且条件是 `opt.decoder == "mpp" || opt.decoder == "auto"` —— 就算参数没传进去，
默认的 `"auto"` 也会走进 MPP 分支。所以解析没问题。

### 1.3 也不是 RTSP 的问题

`视频流: 1920x1080 fps=25.00 codec=h264` 这行说明 `avformat_open_input()` +
`avformat_find_stream_info()` 都成功了，码流参数都拿到了。**RTSP 是通的。**

---

## 一又四分之一、★ 2026-11-10 新证据：更可能是**设备权限**，不是版本

软解那条日志里出现了这两行：

```
[警告] dma-buf 不可用, 退化为 CPU 内存输入
failed to open RGA: Permission denied.
Segmentation fault (core dumped)
```

- `failed to open RGA: **Permission denied**` —— 这是 librga 内部去 open `/dev/rga` 被拒。
- `dma-buf 不可用` —— `/dev/dma_heap/*` 也打不开。

**同一台机器上三个硬件设备节点都打不开**：RGA（明确 Permission denied）、dma-buf、MPP。
**最可能的共同根因：当前用户对 Rockchip 设备节点没有权限**（板子缺 udev 规则，
或者用户不在 `video` 组）。

而 MPP 那一侧，旧的 20190626 用户态在 `/dev/mpp_service` open 失败后
**没有正确处理错误就继续解引用** → 段错误 —— 和观察到的现象完全吻合。

### 所以第一件事不是重写代码，是跑这一条

```bash
cd ~/rk3588_zero_copy
sudo ./run_demo.sh --no-show --frames 300 --decoder mpp
```

**如果 sudo 之后 RGA 和 MPP 都正常了，就完全不需要重写 `mpp_decoder.cc`。**

### ★ 另一条"零代码改动"的路子：板子上可能有另一个 ffmpeg

`pick_decoder()` 本来就**优先找 FFmpeg 的 `h264_rkmpp` 解码器**，找不到才退软解。
日志里那句"没有找到 h264_rkmpp 解码器"说明的是 **PATH 里那个 ffmpeg** 没有。
但 Rockchip BSP 常常另外带一个 SDK 版 ffmpeg：

```bash
which -a ffmpeg
ls -la /usr/local/bin/ffmpeg /opt/*/bin/ffmpeg /usr/local/*/bin/ffmpeg 2>/dev/null
for f in $(which -a ffmpeg) /usr/local/bin/ffmpeg; do
    [ -x "$f" ] && echo "--- $f ---" && $f -hide_banner -decoders 2>/dev/null | grep -i rkmpp
done
```

**如果哪个 ffmpeg 带 rkmpp，直接用它就行，一行代码都不用改**：

```bash
export PATH=/usr/local/bin:$PATH
ffmpeg -hide_banner -decoders | grep rkmpp      # 确认有
./run_demo.sh --no-show --frames 300            # 会自动走 h264_rkmpp
```

注意这条路上 FFmpeg 的 h264_rkmpp 拿到的也是 dma-buf，
所以 `[NV12] 数据源: dma-buf fd (RGA import)` 这条验收关键字照样能出现。
（代码里本来就有 `SafeProbe` 保护 opaque 指针解引用，见 `probe_mpp_opaque`。）

### 确认权限现状

```bash
ls -l /dev/rga /dev/mpp_service /dev/dma_heap/ /dev/dri/renderD128 /dev/video0 2>&1
id
groups
```

### 装 udev 规则（Rockchip BSP 标准那套）

```bash
sudo tee /etc/udev/rules.d/99-rockchip.rules >/dev/null <<'EOF'
KERNEL=="rga",           MODE="0666", GROUP="video"
KERNEL=="mpp_service",   MODE="0666", GROUP="video"
KERNEL=="rkvdec",        MODE="0666", GROUP="video"
KERNEL=="dri",           MODE="0666", GROUP="video"
KERNEL=="video[0-9]*",   MODE="0666", GROUP="video"
KERNEL=="media[0-9]*",   MODE="0666", GROUP="video"
KERNEL=="dma_heap/*",    MODE="0666", GROUP="video"
EOF
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=video
sudo usermod -aG video $USER      # 重新登录生效
```

装完不用重登也行，直接 `sudo` 跑一次验证；或者用 `newgrp video` 临时切组。

### 为什么不用 sudo 跑程序

sudo 能跑通只是验证，**长期方案是 udev 规则**（让服务以普通用户跑）。
另外要注意：如果板子上有 systemd 服务，开机时 udev 规则必须已经生效。

---

## 二、真正的原因：进程在 `mpp_dec.init()` 里**崩了**

执行顺序是：

```
pick_decoder()          → 打印 "[警告] 没有找到 h264_rkmpp 解码器..."
avcodec_open2()         → 打印 "解码器: h264"
[2.5 解码后端块]        → 应该打印 "[解码] 后端 = ..."   ← 这里没打印到
   └─ mpp_dec.init()    → ★ 进程在这里没了
```

`[解码] 后端 = ...` 在**成功和失败两个分支都会打印**。它没出现，
说明进程在 `mpp_dec.init()` 内部**异常终止**（SIGSEGV / abort）。

### 最可能的根因：MPP 版本三方不匹配

| 层 | 我们用的 | 板子的 |
|---|---|---|
| 头文件 `/usr/include/rockchip/rk_mpi.h` | Radxa **20190626**（旧 API） | 原本可能是 Rockchip BSP 版 |
| 用户态 `librockchip_mpp.so` | Radxa **20190626** | 原本可能是 BSP 版 |
| **内核驱动 `/dev/mpp_service`** | — | 板子自己的**新**内核 |

旧用户态 + 新内核 = ioctl 协议对不上 = `mpp_create()` 直接段错误。

**而且很可能，是我们装那两个 .deb 时把板子自带的 MPP 降级了** ——
`dpkg -i` 默认允许降级（只是提示），`install_deps.sh` 的"核心库降级保护"
只覆盖 `libc6 / libc6-dev / libgcc-s1 / libstdc++6 / zlib1g`，**不含 MPP**。

---

## 三、请在板子上跑这些命令（把输出贴回来）

新的二进制带 `--mpp-diag`，一条命令就能看清：

```bash
cd $HOME/rk3588_zero_copy
./build/rk_zero_copy --mpp-diag
```

它会打印：运行时**实际加载**的 `.so` 路径（`dladdr` 查的，不是链接期那个）、
内核 MPP/VPU 设备节点在不在。

再手动跑这几条取证：

```bash
# ① 我们装 MPP 之前, 板子有没有自带 MPP? 有没有被降级?
grep -iE "rockchip-mpp" /var/log/dpkg.log | tail -20

# ② 全盘有几个候选 .so? (板子自带的可能不在 dpkg 管理下)
find / -name "librockchip_mpp.so*" 2>/dev/null
ldconfig -p | grep rockchip_mpp

# ③ 原装的 .deb 有没有被 dpkg 缓存在这? (有就能直接装回去)
ls -la /var/cache/apt/archives/ 2>/dev/null | grep -i mpp

# ④ 内核侧设备节点
ls -l /dev/mpp_service /dev/rkvdec /dev/dri 2>&1
dmesg | grep -iE "mpp|rkvdec|vpu" | tail -20
uname -a

# ⑤ 板子上有没有别的 ffmpeg(SDK 版带 rkmpp 解码器)?
which -a ffmpeg
ls -la /usr/local/bin/ffmpeg /opt/*/bin/ffmpeg 2>/dev/null
ffmpeg -hide_banner -decoders 2>/dev/null | grep -i rkmpp
```

---

## 四、按结果对号入座

### 情况 A：`dpkg.log` 显示之前有 BSP 版 MPP，且被我们降级了

```bash
# 卸掉我们装的旧版, 恢复板子原装
sudo dpkg -r librockchip-mpp-dev librockchip-mpp1
# 从 dpkg 缓存里把原装的装回去(若 ③ 找到了)
sudo dpkg -i /var/cache/apt/archives/librockchip-mpp*.deb
```

然后**用板子自己的头文件重编**（CMake 已经支持指定路径）：

```bash
cd $HOME/rk3588_zero_copy && rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
  -DMPP_INC_DIR=/usr/include/rockchip \
  -DMPP_LIB=/usr/lib/aarch64-linux-gnu/librockchip_mpp.so
make -j$(nproc)
```

⚠️ **新版 MPP 的 `MppApi` 还在**（1.5.0 实测：`mpi->size == sizeof(MppApi) == 184`，
`mpi->control` 的偏移也一致），`mpp_create(&ctx, &mpi)` 的用法不用改。
真正变的是 **`MPP_DEC_SET_PARSER_SPLIT_MODE` 的参数语义**：

| | 旧版(20190626) | 新版(1.5.0) |
|---|---|---|
| 传参 | `mpi->control(ctx, cmd, (MppParam)3)` | `mpi->control(ctx, cmd, &mode)` |
| 原因 | 直接当整数用 | 上游 `mpp_dec_set_cfg_by_cmd()` 里是 `*((RK_U32 *)param)`，**会解引用 param** |

**新版还按旧写法传值的话，MPP 会去读地址 `0x3` 上的内存 → 当场 SIGSEGV。**
这是"硬解一启动就段错误"的直接原因，`src/mpp_decoder.cc` 已按新语义改好。

---

## 〇、★ 2026-11-11 结论：三层问题，逐层排掉

板子上的 MPP 在 2026-11-11 09:33 被 `apt` 换成了**系统自带的 BSP 版 1.5.0**
（原先 `p0_mpp_verify.sh` 装的 20190626 已被 purge 掉），
而 `build/` 里的二进制是 **11-10 用 20190626 头文件编的** —— 三者错位：

| 层 | 现象 | 处理 |
|---|---|---|
| ① 头文件 / .so 版本错位 | 旧头文件编译 + 新 .so 运行 → `mpp_create()` 内部 SIGSEGV | ✅ CMake 锁死用系统路径 `/usr/include/rockchip` + `/usr/lib/aarch64-linux-gnu/`，并加了"工程目录里不许有 MPP 头/库"的检查 |
| ② `control` 参数语义变了 | `mpp_create` 过了，**下一步** `control` 又 SIGSEGV | ✅ 改成传指针；`init()` 拆成 5 步，每步单独打返回码 |
| ③ 设备节点权限 | `mpp_init` 返回 -1 | ⏳ 需要 root：见下面"udev 规则" |

② 修完之后，日志长这样（**崩溃消失**，卡点一目了然）：

```
[MPP] 步骤 1/5 mpp_create (上下文创建)      返回码=0 ctx=0x... mpi=0x...
[MPP] 步骤 2/5 control (解析器分割模式)     返回码=0
[MPP] 步骤 3/5 check_support_format (H.264) 返回码=0 -> 支持
mpp[...]: vcodec_service: open vcodec_service /dev/mpp_service failed
[MPP] 步骤 4/5 mpp_init (编码格式设置)      返回码=-1
[警告] MPP 初始化失败: mpp_init 失败
```

`/dev/mpp_service` 是内核 `mpp_class` 驱动建的，权限 **0600 root:root**，
普通用户 open 直接 EACCES。装 udev 规则即可（工程里已经放好了）：

```bash
sudo cp udev/99-rockchip-media.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=mpp_class
```

或者先 `sudo ./run_demo.sh --no-show --frames 300 --decoder mpp` 验证一把。

---

### 情况 B：板子根本没有 MPP，内核也没有 `/dev/mpp_service`

MPP 硬解这条路在当前内核上走不通。可选：

1. 找板子 BSP 里带 `h264_rkmpp` 的 ffmpeg（见取证 ⑤），用 `--decoder auto` 走它；
2. 升级到带 rkvdec 的内核；
3. 先用软解把整条业务链路（检测→测速→车牌→MQTT）跑通，硬解作为独立议题推进。

### 情况 C：运行时加载的 `.so` 不是我们装的那个

说明板子自带的 MPP 抢占了（`LD_LIBRARY_PATH` / `ldconfig` 顺序问题）。
要么改用板子那个（情况 A），要么把我们的放到更靠前的位置。

---

## 五、这一版新增的"保命"改动

新版已经做了四件事，**不会再出现"程序一声不响就没了"**：

1. **`mpp_dec.init()` 被 `SafeProbe` 包住**
   —— MPP 崩溃会被捕获，打印
   ```
   [警告] MPP 初始化过程中崩溃(SIGSEGV/SIGBUS 已捕获), 退回软解
   ```
   然后**自动退回软解继续跑**，后面的检测/测速/车牌/MQTT 链路不受影响。
2. **RGA 调用也被 `SafeProbe` 包住 + 自动降级到 OpenCV 的 CPU 转换**
   —— librga 在 `/dev/rga` 打不开时会**自己段错误**（它先打印
   `failed to open RGA: Permission denied.` 然后直接 SIGSEGV，来不及返回错误码），
   所以我们外层的 `if (!rga_ok(st))` 判断**永远执行不到**，整个进程 core dump。
   现在会打印
   ```
   [RGA] NV12->RGB888 期间 librga 崩溃(SIGSEGV 已捕获)
          最常见原因: 当前用户打不开 /dev/rga (权限不足)
          已自动降级到 CPU 转换(慢一些, 但不会崩)
   ```
   然后用 OpenCV 的 `cvtColor(NV12→RGB)` + `resize` 走 CPU，**流水线照常跑**。
   CPU 兜底路径已经和 Python 参考实现**逐像素对拍通过（最大差 0）**。
   想强制走 CPU 对比：`RK_FORCE_CPU_RGA=1 ./run_demo.sh ...`
3. **新增 `--mpp-diag`** —— 不碰 RTSP、不碰摄像头，只打印 MPP 环境诊断然后退出。
4. **P0 脚本的诊断包扩充** —— 自动带出 `dpkg.log` 历史、全盘 `.so` 清单、内核设备节点。

> ⚠️ 顺带修了个路径错误：板子的用户名是 **`seeed`**（家目录 `/home/seeed`），
> 不是 `seed`。原来文档里的 `/home/seed` 全部改成 `$HOME` 了。

---

## 六、现在就能做的并行验证（不用等硬解）

先确认**除硬解以外**的链路是好的：

```bash
cd $HOME/rk3588_zero_copy
./run_demo.sh --no-show --frames 300 --decoder ffmpeg
```

如果这版能出 `[frame N] fps=... dec=...` 的统计行，说明 RTSP、RGA、RKNN、跟踪、
测速、车牌、MQTT 全部正常，**只剩 MPP 硬解这一个点**。

顺带提醒：工程默认 NMS 是 `0.60`（给 head/helmet 调的），
人/车模型建议 `--nms 0.45`；高速场景 `--match-thresh` 要降到 `0.3~0.5`，
否则每帧开新 track，速度永远攒不够样本。
