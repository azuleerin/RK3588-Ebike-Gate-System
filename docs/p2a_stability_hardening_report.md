# P2-A 稳定性排雷与工程债收口 —— 报告

> 日期: 2026-11-24　　板子: RK3588 (recomputer-rk3588-devkit)
> 基线: `main` @ `ca179e3`(标签 `v2.2-p1-fork9-live`, 16/16 全绿)
> 分支: `feat/p2a-stability-hardening`　　封版标签: `v2.3-p2a-hardened`(本地, **不 push origin**)
> 备份: `/home/seeed/rk-gate-backup-p2a-20261124_094643/`

---

## 〇、一页结论

| 子项 | 结论 |
|---|---|
| **P2-A1** 10bit 静默零检出 | **根因不是 stride, 是位深**。DJI 是 HEVC Main10(10bit); MPP 给的是 10bit 紧密打包数据, 而管线全程硬编码 8bit NV12 → RGA 转出满屏噪点 → `cls_max=0.0036 / objs=0`, **零报错零告警**。已修: 位深探测 + 拒收护栏 + 10bit 改走软解。`cls_max 0.0036 → 0.2416`, `objs` 恒 0 → 1~3 |
| **P2-A2** 优雅关闭 | `AVFormatContext::interrupt_callback` + `sigaction`(不设 `SA_RESTART`)。A/B(阻塞 RTSP 源): 旧 **45.00 s / 退出码 137** → 新 **0.25 s / 退出码 0** |
| **P2-A3** 运维加固 | `GATE_IPADDR=auto`(实测板子早就是 `192.168.8.77`, 配置还写着 `192.168.31.77`)+ 预检 10b 错配即拒启 + 看门狗"连续拉起失败"升级告警(堵 `fail_count` 静默盲区) |
| **P2-A4** LPRNet 收口 | `rk_gate` 构建/运行链路**零 lprnet 依赖**(只编 `rk_zero_copy` / `rk_lpr_eval`, `nm` 复核符号数 0)。`accuracy/` 默认回归锁定 PP-OCRv4 FP16, LPRNet 改 `DEPRECATED` 且需显式 `--include-legacy-lprnet` |
| 构建 | 全新构建 **warning 0 / error 0**, `rk_gate` 与 `rk_zero_copy` 均通过; 干净构建与增量构建**逐字节一致** |
| 冒烟 | 4 路 300 帧 / DJI 10bit / SIGTERM 优雅退出 —— **全绿**; 预检 `FAIL=0 退出码 0` |
| 部署 | 停服 **0.691 s**, `Result=success / ExecMainStatus=0`(非 137), `NRestarts=0`, `--ipaddr 192.168.8.77` 已生效, exe 路径无 `(deleted)` |
| 稳态(36.1 min) | 帧差分 **79.96 FPS**(门槛 ≥79.5); fd 波动 1; cgroup max/oom/oom_kill 全 0; alerts 零新增 |
| 稳态(3 h) | RSS 斜率 **+3.92 KB/min ≤ 10** ✅、活堆 **+1.54 KB/min ≤ 5** ✅; 后 1/3 窗口 RSS **+0.00 KB/min (R²=1.0000)** |
| 内核 NPU | 4 次采样 Core0/1/2 均值 **41.5 / 42.5 / 42.5%**(区间 38~47%) |
| 16 项验收 | **16/16 全绿, 0 项失败**; 三服务 active |
| 附带修复 | `gate_health.sh` 的**既有假信号**(把看门狗 20 s 前的快照当基准)→ 改用两次快照判活; 验收因此从 15+WARN 变成 16/16 |

---

## 一、现象

### 1.1 P2-A1:10bit 素材走生产链路 → 全程静默零检出

`rk3588_zero_copy/DJI_20261102155401_0126_D.MP4` 走生产 MPP → RGA 零拷贝链路:

```
[MPP] 硬解出帧: 1920x1080 stride=2816x1088 dma-buf fd=47 buf=5514854 字节
      (8bit NV12 应为 4595712 字节)  ★ 大小对不上 8bit NV12
[cls空间] 形态B(fork 9输出): cls_max=0.0036 -> 概率 空间
[ch1][frame 100] ... objs=0 tracks=0 ...
```

**零报错、零告警、零崩溃**。现场看过去像"模型坏了", 实际是位深对不上。

### 1.2 P2-A2:阻塞读场景下停服要拖到被 SIGKILL

RISK_REGISTER 的 R22 记录:阻塞读不可中断 → `join()` 永久阻塞 → 等满
`TimeoutStopSec=30` 被 SIGKILL → **退出码 137**。

### 1.3 P2-A3:上报 IP 与本机网卡错配

```
$ ip -4 -o addr show scope global
3: end0    inet 192.168.8.77/24 ...
$ grep GATE_IPADDR config/gate.env
GATE_IPADDR=192.168.31.77          ← 与本机网卡完全不是一个网段
$ tr '\0' ' ' < /proc/<pid>/cmdline | grep -o '\-\-ipaddr [^ ]+'
--ipaddr 192.168.31.77              ← 线上进程一直带着错地址上报
```

### 1.4 P2-A3(另一处):看门狗"连挂 367 次"却没人知道

`fail_count` 只在每行流水账里出现一次("连续第 N 次"), 而退避是指数的
(60 → 300 s 封顶) —— 失败越多, 告警间隔越长。"已经连挂 367 次"这件事,
在告警日志里只是又一行流水账。

### 1.5 P2-A4:废弃模型占着报告主项位

`accuracy/aggregate.py` 与 `make_report.py` 把 LPRNet 排在**第一位**且标成
**"在用模型"**, 于是它那 **0.00%** 的整牌准确率长期占着报告的主项位置 ——
而现网识别模型其实是 **PP-OCRv4 FP16**。

---

## 二、根因

### 2.1 P2-A1:位深被硬编码成 8bit(不是 stride/对齐问题)

任务书的假设是 "RGA `wstride` 单位/对齐"。**实测否掉了这个假设**:
`rk_minimal --rga-only` 跑那块 10bit 缓冲, **RGA 转换成功、零失败、哨兵零损坏** ——
说明 RGA 本身能处理这个 stride。

真正的根因是**位深**:

| 证据 | 取得方式 | 结果 |
|---|---|---|
| 码流位深 | 解析 MP4 的 `hvcC` 参数集 | `profile_idc=2`(**Main10**), `bit_depth_luma_minus8=2` → **10bit 4:2:0** |
| MPP 实际给的格式 | `mpp_frame_get_hor_stride()` ÷ `mpp_frame_get_hor_stride_pixel()` | `2816 字节/行 ÷ 2252 像素/行 = 1.25 字节/采样` = **10bit 紧密打包** |
| 缓冲大小 | `mpp_buffer_get_size()` | 5,514,854 字节 ≠ 8bit NV12 应有的 4,595,712 字节 |
| 解码出来的像素 | 按 2816 行距渲染成图 | **满屏绿/品红噪点**(见 `opt_ebike_fork9/docs/samples_p2a_dji/p2a_dji_before_after.png` 左半) |

而 `src/mpp_decoder.cc` 里:

```cpp
out->format = RK_FORMAT_YCbCr_420_SP;      // ← 硬编码, 从不检查 MPP 实际给了什么
out->source = (out->fd >= 0) ? ... ;
```

⇒ 10bit 字节被当 8bit 解释 → RGA 转出噪点 → 检测器什么都检不到 → **全程零错误码**。

**为什么不能靠参数救**:
试过 `mpi->control(ctx, MPP_DEC_SET_OUTPUT_FORMAT, MPP_FMT_YUV420SP)`(两种参数形式都试了:
按值 / 按指针, 指针形式还会把指针地址当成格式值)。**返回码都是 0, 但输出几何一点没变**
(仍是 2816/2252) ⇒ 本板子的 MPP 不支持这一路 10→8 降位深。

**顺带挖出两个一直藏着的坑**(只有被迫走软解才暴露):

1. `gate_main.cc` 的 `extract_nv12()` **没有 `AV_PIX_FMT_YUV420P10LE` 分支** ——
   10bit 帧会落到"什么都没配"里(`planar=false` / `u_plane=nullptr` / `bit_shift=0`)。
2. **bsf 建在 `avcodec_open2()` 之后**: 解码器上下文挂的是原始 AVCC codecpar
   (extradata 是 hvcC, 长度前缀格式), FFmpeg 的 HEVC parser 就按长度前缀切 NAL,
   而送进去的包已被 bsf 转成 Annex-B(`00 00 00 01`) —— 对不上。
   实测症状: 软解路径 **10828 条** `Invalid NAL unit size (30414822 > 28510)`.
   以前一直藏着, 因为软解只是"MPP 失败时的兜底", 没人真的走过。

### 2.2 P2-A2:`signal()` 隐含 SA_RESTART, 内核把 read() 自动重启

glibc 的 `signal()` 等价于 `sigaction(..., SA_RESTART)`: 被信号打断的系统调用会被
**内核自动重启**。于是 SIGTERM 到了以后, 阻塞在 `av_read_frame()` 的 `read()` 上的线程
被"醒了"又立刻睡回去 —— 而 FFmpeg 侧又没有 `interrupt_callback`, 它根本不知道
该中断。

> 注: 现网 4 路都是**本地 MP4 文件**, `read()` 几乎不阻塞, 所以 journal 里
> 历史上每次 stop 都是 1 秒完成 —— **"卡满 30 秒"从未在现网出现过**。
> 真正的暴露条件是**会真阻塞的源**(RTSP / 假死的源)。本次用仓库自带的
> `scripts/rtsp_replay_server.py --stall-only` 造了这样一个源, 才把症状复现出来。

### 2.3 P2-A3:写死的 IP 不会自己过期, 过期了也不会报错

现场网卡地址会变(换网段/换路由器/DHCP 续租)。`gate.env` 里写死的 IP 过期后,
程序**不会报错**, 只是发给后端的设备地址是错的 —— 现场表现为
"设备在线但数据看不到", 极难定位。板子早已从 `192.168.31.77` 换到 `192.168.8.77`
(`docs/PROJECT_BRIEFING_20261123.md:749` 有记载), 配置却一直没跟上。

### 2.4 P2-A4:`rk_gate` 其实早就没有 lprnet 依赖, 问题在报告口径

`CMakeLists.txt:237` 的 `src/lprnet.cc` 属于 `rk_zero_copy` target(冻结的单流基线),
`:389` 的属于 `rk_lpr_eval` target(独立评测工具); `rk_gate` target(`:283`)的源文件列表里
**没有**它。真正的问题是 `accuracy/` 的报告口径把废弃模型摆在主项位。

---

## 三、修复步骤

### 3.1 P2-A1(`abc19e5`)

| 文件 | 改动 |
|---|---|
| `include/nv12_frame.h` | `Nv12Frame` 新增 `bit_depth` 字段(8/10/12/16) |
| `src/mpp_decoder.cc` | ① 用 `hor_stride` ÷ `hor_stride_pixel` 反解位深写进 `bit_depth`; ② **非 8bit 的帧直接拒收并明确报错**(宁可少一帧, 也不要静默错检); ③ 帧几何自检(行距/缓冲大小)不自洽就丢帧+报错; ④ 首帧日志补上 buf 大小与"8bit NV12 应有字节数", 对不上时点明; ⑤ `MPP_DEC_SET_OUTPUT_FORMAT` 显式指定(作为兜底, 实测本板不生效) |
| `src/gate_main.cc` | ① `extract_nv12()` 补 `AV_PIX_FMT_YUV420P10LE` / `YUV420P12LE` 分支(planar + `bit_shift=2/4`, 与 `src/main.cc:931` 同一套口径), 并补 `uv_stride`; ② **bsf 改到 `avcodec_open2()` 之前建**, 解码器改用 bsf 的 `par_out` 初始化; ③ **HEVC Main10 的源不走 MPP 硬解**, 改走 FFmpeg 软解(明确打日志说明原因) |

### 3.2 P2-A2(`2f4e9b4`)

| 文件 | 改动 |
|---|---|
| `src/gate_main.cc` | ① 新增 `avio_interrupt_callback()` —— `g_quit` 一置位就让 FFmpeg 中断 io; ② 回调在 `avformat_alloc_context()` 之后、**`avformat_open_input()` 之前**就挂上, 连**打开过程本身**也可打断(RTSP 握手阶段就能卡好几秒); ③ `signal(SIGINT/SIGTERM)` → `sigaction()`, **刻意不设 `SA_RESTART`** |
| `src/gate_main.cc` | ④ 修 `parse_chan_spec()`: 以前按冒号切完取 `parts[1]/parts[2]`, **带端口的地址(`rtsp://host:8554/cam`)会被切碎**, 而 RTSP 恰恰是本程序明确支持的源(`src_is_live_` 有判定) ⇒ 命令行里根本没法写 RTSP 地址。改成从末尾倒着认 `near/high`, 中间整体当源地址 |
| `opt_ebike_fork9/tools/` | 新增 `sigterm_graceful_test.sh`(本地文件源)与 `sigterm_blocking_test.sh`(阻塞读, 用 `rtsp_replay_server.py --stall-only`) |

> `NpuPool` 的回收顺序**本来就对**(上下文在各 worker 线程内销毁, 跨线程销毁会死锁),
> 见 `src/rk_base/npu_pool.cc:796` 的注释 —— 未改动。

### 3.3 P2-A3(`f71f22e` + `631fda1`)

| 文件 | 改动 |
|---|---|
| `config/gate.env` | `GATE_IPADDR` → `auto` |
| `scripts/resolve_ipaddr.sh`(新增) | 解析逻辑的**唯一一份**: 先问 `ip route get` 的 `src`, 退到第一个 scope global 非回环地址, 都取不到就退 `127.0.0.1` 并打 WARN |
| `scripts/run_gate_service.sh` | 改为 source 共享文件, `--ipaddr` 传解析后的值 |
| `scripts/preflight_gate.sh` | 新增 **10b 项**: 上报地址必须在本机网卡上, 不匹配直接 **FAIL(退出码 78 拒绝启动)** |
| `scripts/gate_watchdog.sh` | 新增 `FAIL_ALERT_THRESHOLD`(默认 3)/`FAIL_ALERT_REPEAT`(默认 5): `fail_count` 达阈值就往 `${GATE_LOG_DIR}/rk-gate-alerts.log` 写【升级告警】, 带 `ActiveState`/`Result`/`fail_count`, 并按阈值/每 5 次防刷屏; 恢复健康时补一条「已恢复」记录 |
| `opt_*/ab_harness.py`(3 个) | 写死的 `--ipaddr 192.168.31.77` → 参数化(`GATE_IPADDR` 环境变量, 默认 auto) |
| `opt_ebike_fork9/tools/render_gate_args.sh` | 接上同一份解析 —— 之前沙盒渲染传出的是字面量 `auto`, 而生产跑出来的是真实 IP, **冒烟测的就不是同一个东西** |

### 3.4 P2-A4(`10f46f8`)

| 文件 | 改动 |
|---|---|
| `accuracy/run_all.sh` | 旧 LPRNet 的识别推理改为**默认不跑**, 要跑得显式加 `--include-legacy-lprnet`(只作历史对照); 未知参数立刻报错(参数解析前移到脚本开头) |
| `accuracy/aggregate.py` / `make_report.py` | `PPOCR_FP16` 标成「现网基准」排第一, LPRNet 标 `DEPRECATED 废弃模型` 排最后, 并在报告里写明"现网识别模型是 PP-OCRv4 FP16" |
| `rk3588_zero_copy/docs/RISK_REGISTER.md` | R12 / R22 两行标记为已闭环 |

> `CMakeLists.txt` **未改动** —— 核查证据: `rk_gate` target(`:283`)不含 `src/lprnet.cc`;
> `nm -C build/rk_gate | grep -ci lprnet` = **0**。`models/lprnet_finetuned_int8.rknn`
> 一字未动(只读目录红线)。

---

## 四、验证结果

### 4.1 构建

```
$ cmake -B build_p2a -DCMAKE_BUILD_TYPE=Release && cmake --build build_p2a -j8
BUILD_EXIT=0
warning: 0    error: 0
[100%] Built target rk_gate
[100%] Built target rk_zero_copy
```

**可复现性**: 干净构建与增量构建**逐字节一致**

| 目标 | 全新构建 `build_p2a` | 增量构建 `build` |
|---|---|---|
| `rk_gate` | `bf7068cdb5a6a27190344261ed2af0b4` | `bf7068cdb5a6a27190344261ed2af0b4` ✅ |
| `rk_zero_copy` | `3fa26e699dda1737a76634c4638595c6` | `3fa26e699dda1737a76634c4638595c6` ✅ |

新二进制含 P2-A 运行时字符串(旧备份二进制里没有):
`帧不可用` / `非 8bit` / `输出格式=8bit NV12` —— 各 1 处。

### 4.2 三项冒烟(NPU 空闲条件下)

**① 4 路生产同参 300 帧**

| 检查项 | 结果 |
|---|---|
| 退出码 | **0** |
| 逐路帧号 | ch1~ch4 = **300 / 300 / 300 / 300** |
| `[车牌]` 裁剪宽(ch1, n=4) | min 166 / **median 247** / max 273, **<160px: 0/4** |
| MPP / RGA / EMFILE / 自检 / 崩溃 | **各 0** |

**② DJI 10bit 单路**

| 检查项 | 修复前 | 修复后 |
|---|---|---|
| 解码后端 | MPP 硬解 | 软解(日志明写原因) |
| `cls_max` | **0.0036** | **0.2416** |
| `objs` | **恒 0** | **1~3**(frame 600 / 1100) |
| 200 s 内解码帧数 | — | **1200** |
| 位深护栏误触发 | — | **0** |

**③ SIGTERM 优雅退出**

| 场景 | 修复前 | 修复后 |
|---|---|---|
| 阻塞 RTSP 源(真阻塞) | **45.00 s / 退出码 137** ✗ | **0.25 s / 退出码 0** ✅ |
| 4 路生产同参(本地文件) | 0.65 s / 0 | **0.63 s / 0** ✅ |

> A/B 的唯一差别是优雅关闭这一处 —— 为此专门编了一个"只改了 `--ch` 解析器"的
> 旧逻辑二进制(`/tmp/p2a/rk_gate_oldshutdown`),以确保测的确实是关闭逻辑。

**④ 预检**: `FAIL=0  WARN=1(MQTT 未配置, 预期)  退出码 0`, 新增 10b 项
`GATE_IPADDR=auto -> 启动时自动取到 192.168.8.77` ✅
**负向验证**: 临时把 `GATE_IPADDR` 改回 `192.168.31.77` → 10b 报 FAIL → **退出码 78(拒绝启动)**;
改回 `auto` → 退出码 0。

### 4.3 看门狗沙盒测试(`opt_p0/tests/watchdog_fail_alert_test.sh`)

| 场景 | 期望 | 结果 |
|---|---|---|
| A 正常态(一直 active) | 告警日志零新增 | **0 行** ✅ |
| B 连续拉起失败 | 必须有升级告警 + 现场信息 | `★★ 服务连续拉起失败 3 次(ActiveState=inactive Result=exit-code) —— 看门狗自愈【无效】, 需要人工介入` ✅ |
| B 防刷屏 | 阈值后每 5 次才再告 | 第 3 次告, 第 4/5/6/7 次**不告** ✅ |
| C 恢复健康 | 必须有「已恢复」记录且计数清零 | `服务已恢复健康(此前连续拉起失败 N 次) —— 升级告警解除`, `fail_count=0` ✅ |

既有的 `opt_p0/tests/watchdog_silent_stall_test.sh` 复跑仍通过。

### 4.4 部署实测

| 检查项 | 结果 |
|---|---|
| systemd 停机耗时 | **0.691 s**(旧二进制对照: 0.788 s) |
| `Result` / `ExecMainCode` / `ExecMainStatus` | **success / 1 / 0** —— 不是 137(SIGKILL) |
| `NRestarts`(rk-gate) | **0** |
| `NRestarts`(rk-gate-watchdog) | **0** |
| exe 路径 | `/home/seeed/rk_deploy/rk3588_zero_copy/build/rk_gate` —— **无 `(deleted)`** |
| `--ipaddr` | **192.168.8.77** ✅(auto 自动解析已生效) |
| 看门狗 | 13:35:55 重启(脚本 mtime 10:32:00)→ **新告警逻辑已加载** |

> 旧二进制那次停机也是 0.788 s —— 因为现网 4 路都是本地 MP4, `read()` 不阻塞。
> "卡满 30 秒"只在真阻塞源上出现, 那个 45.00 s → 0.25 s 的 A/B 才是这条修复的真正证据。

### 4.5 生产稳态观测(30 分钟)

### 4.5 生产稳态观测(30 分钟, 墙钟锚定)

观测窗口: 2026-11-24 13:37:35 → 14:13:46, 32 个样本(间隔 60 s), 覆盖 **36.1 分钟**。
观测进程 PID 2751970 **全程未变**(无重启)。完整留证:
`/home/seeed/rk-gate-backup-p2a-20261124_094643/acceptance_30min.txt`。

| §4.1 门槛 | 实测 | 判定 |
|---|---|---|
| 帧计数器差分总吞吐 ≥ 79.5 FPS | **79.96 FPS**(4 路各 43,300 帧 / 36.1 min, 分析速率 1199.4 帧/分/路) | ✅ PASS |
| 活堆尾段斜率 ≤ 5 KB/min | **+3.08 KB/min**(R²=0.011, 第 10→30 分钟) | ✅ PASS |
| 活堆平台期斜率 | **−0.65 KB/min**(R²=0.000, 第 30 分钟之后) | ✅ PASS |
| RSS 尾段斜率 ≤ 10 KB/min | **+27.34 KB/min**(R²=0.811, 第 10→30 分钟) | ⚠️ **字面不达标** —— 见下 |
| RSS 平台期斜率 | **+0.00 KB/min, R²=1.000**, 取值集合去重后**只有 1 个值 (232.906 MB)** | ✅ PASS |
| 活堆静态基线 83~86 MB | **82.6 → 82.5 MB**(全程 81.8~84.1 MB) | ✅ 与预期带重合, 远低于 200 MB 告警线 |
| fd 恒定(波动 ≤2) | **170 → 171, 波动 1** | ✅ PASS |
| cgroup `memory.events` | max=0 / oom=0 / oom_kill=0 | ✅ PASS |
| `memory.current` / `peak` / `max` | **199.9 MB / 255.8 MB / 512 MB** | ✅ PASS |
| `NRestarts` / PID | **0 / 全程未变** | ✅ PASS |
| `/var/log.hdd/rk-gate-alerts.log` | 自 2026-11-24 00:58:49 起**零新增** | ✅ PASS |
| 活日志 MPP/RGA/EMFILE/自检/崩溃 | **0 命中** | ✅ PASS |

#### 4.5.1 RSS 尾段那一条:为什么判定为"非 P2-A 引入"

RSS 在进程启动后 ~30 分钟内会**阶梯式**涨约 3~4 MB(每级 256~528 KB),然后走平。
这个现象**与二进制无关**, 两轮同口径对照:

| 口径 | P1 旧二进制 | P2-A 新二进制 |
|---|---|---|
| 各自启动后第 10→30 分钟: RSS 斜率 | +8.47 KB/min(**过**) | +27.34 KB/min(**不过**) |
| 各自启动后第 10→30 分钟: 活堆斜率 | +6.04 KB/min(**不过**) | +3.08 KB/min(**过**) |
| 各自启动后头 30 分钟(同帧号跨度): RSS 总变化 | 228.1 → 232.5 MB(**Δ+4.34 MB**) | 229.7 → 232.9 MB(**Δ+3.25 MB**) |
| 第 30 分钟之后的 RSS | 平台期 232.5 MB 不动(7 h 52 min 全程) | **+0.00 KB/min, R²=1.0, 取值唯一 232.906 MB** |

⇒ 两轮**互有胜负**: P1 那轮 RSS 过、活堆不过; P2-A 这轮活堆过、RSS 不过。
说明这个窗口量到的是**进程启动后的预热沉降尾巴**, 而不是泄漏;
同口径下 P2-A 的 RSS 反而涨得**更少**(+3.25 MB vs +4.34 MB),
且第 30 分钟后 RSS 走成一条**只有一个取值**的直线。

**结论: RSS 尾段那条按字面未达标, 但已用同口径基线对照排除"是 P2-A 引入"这一可能,
且平台期实测为零增长 —— 无泄漏。** (活堆那条与 RSS 那条在两轮之间互换, 这本身
就说明单看一条会误判。)

### 4.6 16 项全量交付验收

### 4.6 16 项全量交付验收 + 内核 NPU 负载

#### 4.6.1 内核 `/sys/kernel/debug/rknpu/load` 逐核采样(4 次)

| 采样 | Core0 | Core1 | Core2 |
|---|---|---|---|
| 1 | 41% | 42% | 42% |
| 2 | 40% | 42% | 42% |
| 3 | 38% | 39% | 40% |
| 4 | 47% | 47% | 46% |
| **均值** | **41.5%** | **42.5%** | **42.5%** |

整体均值 **42.2%**(区间 38~47%)。任务书给的是 "~43~49%" —— 这 4 次采样**均值比带的下沿还低
0.8 个点**(方向上是有利的: NPU 比预期更闲), 但**如实说它没落在那个带里**。
参照 P1 同模型同配置的 7 h 52 min 稳态实测 48/47/46%, 两轮读数在同一量级;
38~47% 的跨度更像是采样点正好压在验收的重编/重启附近。

#### 4.6.2 16 项验收结果

**16 项全部通过, 0 项失败。** 三服务 `rk-gate` / `rk-gate-watchdog` / `rk-kernel-log` 均为 `active`。

> ★ 首轮验收拿到的是 **15 项通过 + 1 项 WARN**(【6】健康检查判成"可疑"),
> 差一项。已定位为 `gate_health.sh` 的**既有假信号**(与本次改动无关):
> 它拿"活日志最新帧号"比"看门狗上次轮到的帧号", 而看门狗每 20 s 才轮一次、
> 应用每约 5 s 才写一行统计 ⇒ 两者在"看门狗刚轮完、应用下一行还没写出来"的窗口里
> **必然相等** ⇒ 被误报成"停滞 2 秒 / 4 秒"。实测连跑 12 次: 10 次正常、2 次可疑。
> 更糟: 看门狗每次轮询都刷新 `ch*_ts`, `now - ch*_ts` 永远 ≤ 20 s,
> 那个"停滞 > 180 s → 卡死"的分支**永远走不到** —— 该脚本宣称的"与看门狗同源"此前没实现。
>
> **处置**(`6339473`): 改成**自己隔 6 s 取两次帧号快照**判活。
> 连跑 12 次真实环境 **12/12 正常**; 并新增负向测试
> `opt_p0/tests/gate_health_stall_test.sh` 证明它没变成橡皮章:
> 帧号推进 → **正常** ✓ / 帧号不动+日志在写 → **可疑** ✓ / 帧号久不动+日志停写 → **卡死** ✓

#### 4.6.3 gate_health.sh 长跑复测(约 3 小时)

【人工】复测: `gate_health.sh` 连续运行约 **3 小时**、至 **207 300 帧/路**,
`exit_code=0`(结论"正常"), **RSS 233 MB、活堆 83 MB、零新增告警**。

我从活日志独立复算了这次长跑的斜率(按帧号回退切分进程段, 留证
`$BK_DIR/longrun_3h_segments.txt`):

| 进程段 | 时长 | RSS 斜率 | 活堆 斜率 | 后 1/3 窗口 RSS / 活堆 |
|---|---|---|---|---|
| **段9(本次 3 小时跑)** | **177.9 min / 8542 统计行** | **+3.92 KB/min**(R²=0.199) | **+1.54 KB/min**(R²=0.156) | **+0.00 KB/min (R²=1.0000)** / +0.19 KB/min (R²=0.0003) |
| 段6(150.8 min) | 150.8 min | −227.70 KB/min(回落) | +1.69 KB/min | +0.00 KB/min (R²=1.0000) / — |
| 段7(本次 30 分钟观测那轮) | 55.1 min | +23.80 KB/min | +4.97 KB/min | +0.00 KB/min (R²=1.0000) / +3.20 KB/min |

⇒ **在长跑窗口(与 P1 那轮 7 h 52 min 同量级)上, §4.1 的两条斜率门槛【直接达标】**:
RSS **+3.92 KB/min ≤ 10** ✅、活堆 **+1.54 KB/min ≤ 5** ✅, 且后 1/3 窗口 RSS 斜率
**+0.00 KB/min、R²=1.0000**(取值唯一)⇒ 彻底走平。
§4.5 里那 30 分钟窗口的 RSS +27.34 KB/min 至此可确认是**预热沉降的尾巴**,
在有意义的稳态窗口上不存在。

### 4.7 红线核查

| 项 | 结果 |
|---|---|
| `rk3588_zero_copy/src/main.cc` | **0 改动** ✅ |
| `archive/rk12ch/` | **0 改动** ✅ |
| `third_party/` | **0 改动** ✅ |
| `models/`(全部 7 个模型) | **0 改动**, 只新增已授权的 `ebike_real_fork9_int8_rknn` ✅ |
| `config/gate.env` 里的 `export` | **0** ✅ |
| 明文密码(真实密码口径) | **0 处** ✅(字面命中 2 处, 全在 `archive/` 的 OCR 字符集字面量, 按禁碰红线冻结备案) |
| `CMakeLists.txt` | **0 改动** ✅ |

---

## 五、产物清单

| 类别 | 路径 |
|---|---|
| 本报告 | `docs/p2a_stability_hardening_report.md` |
| 位深探测与护栏 | `rk3588_zero_copy/src/mpp_decoder.cc`、`include/nv12_frame.h` |
| 优雅关闭 | `rk3588_zero_copy/src/gate_main.cc`(`interrupt_callback` + `sigaction`) |
| IP 解析 | `rk3588_zero_copy/scripts/resolve_ipaddr.sh`(新增)、`run_gate_service.sh`、`preflight_gate.sh` |
| 看门狗升级告警 | `rk3588_zero_copy/scripts/gate_watchdog.sh` |
| 看门狗沙盒测试 | `opt_p0/tests/watchdog_fail_alert_test.sh`(新增) |
| 健康检查沙盒测试 | `opt_p0/tests/gate_health_stall_test.sh`(新增, 负向) |
| SIGTERM 沙盒测试 | `opt_ebike_fork9/tools/sigterm_graceful_test.sh`、`sigterm_blocking_test.sh`(新增) |
| 命令行渲染 | `opt_ebike_fork9/tools/render_gate_args.sh` |
| 前后对比图 | `opt_ebike_fork9/docs/samples_p2a_dji/p2a_dji_before_after.png` |
| 精度口径收口 | `accuracy/run_all.sh`、`accuracy/aggregate.py`、`accuracy/make_report.py` |
| 备份现场 | `/home/seeed/rk-gate-backup-p2a-20261124_094643/` |

## 六、MD5 与 commit hash

| 对象 | MD5 |
|---|---|
| `rk3588_zero_copy/build/rk_gate`(P2-A 新二进制) | `bf7068cdb5a6a27190344261ed2af0b4` |
| `rk3588_zero_copy/build/rk_gate`(P2-A 之前的基线, 备份) | `dddc85a30c7106441c2759dc31d3d176` |

| commit | 说明 |
|---|---|
| `ca179e3` | 基线 `main`(标签 `v2.2-p1-fork9-live`) |
| `abc19e5` | P2-A1 10bit 静默零检出 |
| `2f4e9b4` | P2-A2 优雅关闭 |
| `f71f22e` | P2-A3 IP 自动解析 + 看门狗升级告警 |
| `10f46f8` | P2-A4 识别基准锁定 PP-OCRv4 FP16 |
| `631fda1` | IP 解析逻辑抽成共享文件 |
| `9b8c9e5` | 前后对比图留证 |
| `e6b1b90` / `3c27800` | 报告草稿 + 30 分钟稳态结果 |
| `6339473` | `gate_health.sh` 假信号修复(让 16 项验收能从 15+WARN 变成 16/16) |
| `f4f4608` | **合入 `main` 的 merge 提交**(`--no-ff`), 封版标签 `v2.3-p2a-hardened` |

## 七、遗留项(需单独立项, 本次不做)

1. **10bit 素材只能走软解** —— 本板子的 MPP 不支持 10→8 降位深
   (`MPP_DEC_SET_OUTPUT_FORMAT` 返回 0 但输出几何不变)。
   代价: 1080p10bit 软解只有 **~10 FPS 交付率**(软解 9~10ms/帧 + CPU 色彩转换 9~10ms/帧),
   而 MPP 路径是 20 FPS。**若现场要接 10bit 相机, 这条必须先解决**
   (换 MPP/固件, 或评估 10bit 下的吞吐是否够用)。
   另: 软解有 **58 条** FFmpeg `Error constructing the frame RPS`(缺参考帧),
   60 s 与 200 s 两次跑**都是 58 条**而解码帧数从 500 涨到 1200 ⇒ 是**启动瞬间的固定突发**
   (首个 IDR 前的参考帧还没进 DPB), 不是稳态丢帧率。现网 4 路全是 8bit, 走 MPP, 不受影响。
2. **O9 残余 12.26%**(P1 遗留): 省份字在中等距离帧仍被顶边切, 根因是跟踪器时基口径, 需代码改动。
3. **`--ch` 仍不支持 IPM 标定点里带冒号的地址** —— 本次只修了源地址含冒号的情形。
4. **10bit 素材的 CPU 色彩转换开销** —— 软解路径的 `rga=9~10 ms` 全在 CPU 上做
   (10bit 降位深 + 缩放), 这是它只能交付 ~10 FPS 的直接原因。若现场要接 10bit 相机,
   可以考虑在 CPU 上做一次性的 10→8 预降位深(把像素挪到独立的 8bit 缓冲),
   后续 RGA/NPU 就能回到硬件快路径 —— 但那是另一个改动, 本次未做。
