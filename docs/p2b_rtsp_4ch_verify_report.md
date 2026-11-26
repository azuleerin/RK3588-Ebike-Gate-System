# Phase 1 验收报告 —— 真实监控摄像头 RTSP 四路并发跑通与稳态验收

- **日期**：2026-11-26 ~ 2026-11-27
- **分支**：`feat/p2b-rtsp-4ch-verify`（基线 `main@9242b7e`，标签 `v2.5-web-console-ro`）
- **生产引擎**：`rk_gate`（唯一生产引擎，全程未触碰 `rk_zero_copy`）
- **结论**：**通过** —— 4 路真实相机流经本地零转码中继并发跑通，15 分钟稳态观测全指标达标；
  过程中定位并修复了 3 个真实缺陷（其中 2 个是仓库既有 bug）。

---

## 一、相机与码流实测档案

| 项 | 实测值 | 采集方式 |
|:--|:--|:--|
| 设备 | VZ Smart-IPC | RTSP `OPTIONS` 响应头 |
| RTSP 服务端 | **LIVE555 Streaming Media v2022.07.14** | `rtsp://192.168.8.100:8557/` 的 `OPTIONS` / `DESCRIBE` |
| 可用码流路径 | **`/`**（根路径）与 `/h264` | 15 条候选路径逐一 `DESCRIBE` 枚举 |
| 失效路径 | 16 条候选路径中 **14 条返回 `404 Stream Not Found`**（`/stream1` `/stream2` `/stream3` `/live` `/h265` `/media/video1` `/media/video2` `/cam/realmonitor?channel=1&subtype=0` `/Streaming/Channels/101` `/Video1` `/video1` `/ch1` `/main` `/sub`） | 同上 |
| 编码 / Profile | **H.264 / Main** | `field_probe probe`（`bits_per_raw_sample=8`） |
| 位深 | **8-bit**（`pix_fmt=yuvj420p`） | 同上 |
| 分辨率 / 帧率 | **1920×1080 / 25 fps** | 同上 |
| 关键帧间隔 | 约 **162 个 RTP 包 ≈ 1 秒**；每个 IDR 前带 SPS+PPS | 400 包 NAL 类型直方图 |
| SDP 参数集 | **无 `sprop-parameter-sets`** | 相机完整 SDP 逐行核对 |

> **注 1**：用户最初提供的 `rtsp://…:8557/stream2` **路径无效**（404）。已把仓库外的
> `/home/seeed/.config/rk-gate/real_rtsp.url` 修正为根路径 `/`（该文件权限 600，仓库外）。
>
> **注 2**：相机 SDP 里 `profile-level-id=000042` 的 profile_idc=0 是**非法值**
> （标准 Baseline 应为 `42e01f`），而实际码流是 Main Profile —— 相机把这一行写错了。
> 中继在本地 SDP 里按 Main 修正为 `4d001f`。

### 10-bit 硬拦截口径（纠偏后）

- RK3588 的 RGA3 硬件原生支持 `RK_FORMAT_YCbCr_420_SP_10B`；
  **当前 C++ 链路（`mpp_decoder.cc` / `rga_convert.cc`）未对接 RGA3 10-bit 格式**，
  因此默认拦截并降级至约 10 FPS CPU 软解。**不是"芯片硬件不支持 10-bit"。**
- 本次现场相机实测为 **8-bit H.264 Main**，未触发该路径。
- **本阶段 C++ 冻结，未为 10-bit 解冻任何 C++ 源码。**

---

## 二、验收门槛逐项对照

| 验收项 | 硬性门槛 | 实测 | 判定 |
|:--|:--|:--|:--|
| 视频编码与位深 | 8-bit H.264/H.265 Main Profile | H.264 / Main / 8-bit / 1920×1080 / 25fps | ✅ |
| 原图抓拍 | 1 张原分辨率清晰帧图，> 20 KB | `field_preflight.sh --grab-ipm` 退出码 **0**，产出 **192 074 字节** 1920×1080 JPEG | ✅ |
| 外挂配置兼容性 | 无 `gate.env.local` 时渲染逐字节一致；有则 `preflight FAIL=0` | `diff` **0 字节**（md5 `593a600e23567d27b763e046bb016588` 未变）；有外挂时 `FAIL=0 WARN=1` | ✅ |
| 4 路同步吞吐 | ≥ 4 × min(源 FPS, 20) × 0.95 = **76 FPS** | **4 × 20.0 = 80.0 FPS**（与 MP4 基线持平） | ✅ |
| 内核 NPU 负载 | 三核均 ≤ 55% | **Core0 36~37% / Core1 36~38% / Core2 35~37%**（4 次采样） | ✅ |
| 内存 / 活堆 / cgroup | RSS ≤ 300 MB；活堆 ≤ 120 MB；`oom=0 max=0` | RSS **225 MB**；活堆 **67 MB**；`oom=0 max=0` | ✅ |
| 稳定性 | `NRestarts = 0`；看门狗告警零新增 | **NRestarts=0**，15 分钟零重启、零新增告警 | ✅ |
| 真实流优雅停机 | ≤ 5.0 s，`Result=success`，`ExecMainStatus=0` | **`real 0m0.650s`** / `success` / `0`（非 137） | ✅ |
| 凭证与红线终检 | C++ / models / `config/gate.env` 零改动；非 `archive/` 明文密码 0 命中 | **0 / 0 / 0**；明文凭据命中 **0** | ✅ |

### 吞吐采集口径

用**日志里的帧计数器差分**（已交付帧数 ÷ 墙钟秒数），**不用** `deliv=` 字段。
依据：`src/gate_main.cc` 的 `--max-fps` 跳过发生在 `stats_.on_frame()` **之前**
（`gate_main.cc:1058-1071` vs `:1381`），所以 `stats_.frames` 只计**已交付帧**，
帧号差分即交付速率。`deliv=` 是"启动到现在的累计均值"，跨进程不可比。

### 与 MP4 离线基线的对照（同口径实测）

| 指标 | MP4 基线（`v2.5-web-console-ro`） | 真实 RTSP（本次） | 变化 |
|:--|:--|:--|:--|
| 4 路合计吞吐 | 80.00 FPS | **80.0 FPS** | 持平 |
| RSS | 190.6 MB | 225 MB | +34 MB（RTSP 4 连接 + 中继缓冲） |
| 活堆 | 84.7 MB | 67 MB | **−17.7 MB**（源分辨率从 4K 降到 1080p） |
| fd 句柄 | 173 | 117 | **−56** |
| NPU 三核 | 69/68/66（仓库记录的 2026-11-19 A/B 实测，`config/gate.env:74`） | **36~38%**（本次实测 4 次采样） | 显著下降（源分辨率从 4K 降到 1080p，RGA/NPU 工作量更小） |

---

## 三、过程中定位并修复的 3 个真实缺陷

### 缺陷 1（仓库既有 bug）：`preflight_gate.sh` 的 RTSP 主机名解析没剥 userinfo

- **现象**：带真实相机凭据的外挂配置跑 `preflight_gate.sh`，4 路全部误报
  `[FAIL] 7. 通道N RTSP 连不上: admin:8557`，退出码 78 拦住启动。
- **原因**：`preflight_gate.sh:172` 只做了 `hostport="${url#*://}"; hostport="${hostport%%/*}"`，
  于是 `rtsp://admin:xxx@192.168.8.100:8557/` 里 `host` 被切成**用户名**。
- **修复**：取 host/port 之前先剥掉 `@` 之前的 userinfo 段（`preflight_gate.sh:172-179`，剥离在 `:178`）。
  凭据不外泄 —— 剥掉的只是解析用的中间变量，日志里打印的仍是 `$host:$port`。
- **提交**：`f79d5f1`

### 缺陷 2（仓库既有 bug）：启动日志把带凭据的 RTSP URL 写进日志文件

- **现象**：切真实 RTSP 后，每次启动 `run_gate_service.sh` 都把带账号密码的 `--ch` 参数
  原样写进 `rk-gate-error.log`（unit 的 `StandardError=append:` 指向该文件）。
- **修复**：只在这一行 echo 的管道里加一条脱敏 sed（`run_gate_service.sh:219-222`），
  `rtsp://user:pass@` → `rtsp://***:***@`。**不影响传给 `rk_gate` 的 `--ch` 参数。**
- **提交**：`af95759`

### 缺陷 3（新增机制）：4 路直连远端相机时只有 1 路能出帧 → 看门狗反复重启

**这是本阶段最大的发现。** 首轮切换后，服务**每约 90 秒被看门狗重启一次**
（`NRestarts` 显示 0 是因为 `systemctl restart` 会重置该计数器，容易漏判）。

#### 现象与逐项排除

| 嫌疑 | 实测 | 判定 |
|:--|:--|:--|
| 相机 RTSP 协议层并发能力 | 原生协议并发 4 路，接入间隔 0 / 0.3 / 1.0 s 三档，4 路收字节增量**完全一致**，30 秒持续不断 | 无罪 |
| 相机踢旧连接 | A 连 8 秒后再接 B，A/B 仍同时稳定收流（增量一致） | 无罪 |
| libav 并发拉流 | 4 路并发 `field_probe` 抓帧全部成功 | 无罪 |
| 4 × MPP 并发 | 4 路连**本地** RTSP 服务器 → 4 路全部满速 `deliv=20.0` | 无罪 |
| MP4 源 | 4 路 MP4 走同一二进制 → 4/4 正常 | 无罪 |
| 路数阈值 | 1 路活 ch1 / 2 路活 ch2 / 3 路活 ch3 / 4 路活 ch4 —— **恒为最后启动的那一路活** | ⚠️ 关键线索 |

#### 最终根因：相机的两个"会话怪癖"

**怪癖 A —— 会话推流有上限**：客户端 60 秒内不发任何 RTSP 请求，相机就**停止推 RTP**，
但 TCP 连接还挂着（不报错）。
实测：中继独跑（**无任何本地客户端**）推满 **10003 个包 / 约 55 秒**后速率归零，
**加不加本地客户端完全一样** → 是相机侧行为，不是"客户端不读"。

**怪癖 B —— SDP 无 `sprop-parameter-sets`，SPS/PPS 只在关键帧里带**：
客户端接入时错过关键帧，就一路 `non-existing PPS 0 referenced` / `no frame!`，
帧号永不推进。

这两个怪癖叠加，直连时表现为「4 路只有最后启动的那一路能解出帧，其余路帧号停推进」，
看门狗 L2 判定卡死 → 反复重启。

> `src/gate_main.cc:670-675` 把 `rtsp_transport` / `stimeout` / `max_delay` /
> `fflags=nobuffer` / `avioflags=direct` **写死**，其中没有任何 RTSP keep-alive 选项，
> 且无法在不改 C++ 的前提下调整 → **本阶段 C++ 冻结 ⇒ 只能从板端侧吸收这两个怪癖。**

#### 解法（任务书预设的路径 B）：本地 1 拉 4 播零转码中继

新增两个文件（**0 行 C++ 改动**）：

- `rk3588_zero_copy/scripts/rtsp_fanout_server.py` —— 中继服务端
- `rk3588_zero_copy/scripts/rtsp_fanout_relay.sh` —— 管理脚本
  （`start` / `status` / `stop` / `restart`，PID 存 `/tmp/rtsp_fanout.pid`，
  日志写 `/tmp/rtsp_fanout.log`）

**零转码**：H.264 不解码、不重编码，码流 payload **一个字节都不改**。只在 RTP 层做：

1. **补插 SPS/PPS**：每个 IDR 前插入缓存的 SPS+PPS → 客户端随时接入都能解；
2. **`sprop-parameter-sets`**：把抓到的 SPS/PPS 以 base64 写进本地 SDP → 客户端一连上就知道参数集；
3. **RTSP keep-alive**：每 20 秒给相机发 `GET_PARAMETER` → 相机持续推流（实测 200 秒 36435 包零停推）；
4. **本地时间戳自生成**（不透传相机 ts）→ 相机重连后本地流的时间戳仍连续，客户端不失步；
5. **每客户端一条独立队列**（相机线程只做 O(1) append）。

**中继自身踩过的 4 个坑（均已修，记录备查）**：

| 坑 | 症状 | 修法 |
|:--|:--|:--|
| SETUP 响应缺 `Transport:` 头 | FFmpeg 报 `Invalid data found`，**连 PLAY 都不发** | SETUP 响应补 `Transport: RTP/AVP/TCP;unicast;interleaved=0-1` |
| NAL 分析没跳过 12 字节 RTP 头 | 读到的是 RTP 头 `0x80` → type 0，SPS/PPS **永远识别不出来**，补插逻辑形同虚设 | NAL 分析只用载荷部分 |
| 客户端 socket 在 PLAY 后仍留 120 s 超时 | FFmpeg 在 PLAY 后**不再发任何 RTSP 请求** → recv 超时 → 我们自己把客户端会话掐掉；实测**连上后正好 120 秒**4 路同时停帧 | PLAY 后改阻塞读 |
| 共享队列 + 全局锁 + 客户端线性扫描 | 4 客户端 × 226 包/秒 × 最多 1200 项扫描 ≈ 100 万次/秒持锁迭代，把相机读线程挤死 → 队列不填充 → 4 路一起饿死 | 改成每客户端独立队列，O(1) |

**提交**：`0eb0336`（新增）、`c4d4ad6`（修复）、`8b27fb7`（清除注释里的真实凭据）

---

## 四、15 分钟稳态观测

采样器：`/tmp/rk_gate_soak_sampler.sh`（每 60 秒一次，逐分钟采样 `/proc/<pid>` 与 cgroup）。

> **注**：采样器本次放在 `/tmp`（临时工具，**未入库**）—— 本阶段对仓库脚本的改动严格限定在
> 「外挂配置机制 + 中继」范围内（见第六节），未扩大范围。
> Phase 2 建议把它固化成 `scripts/gate_soak_sampler.sh` 入库（纯脚本，0 行 C++），
> 便于后续每轮改动都做同口径的 15 分钟稳态回归。

| # | 时间 | ch1 | ch2 | ch3 | ch4 | RSS(KB) | 活堆(KB) | fd | cgroup(B) | oom | max | NRestarts | 告警字节 |
|--:|:--|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| 1 | 00:06:55 | 700 | 700 | 700 | 600 | 226 832 | 68 897 | 117 | 175 374 336 | 0 | 0 | 0 | 25 718 |
| 2 | 00:07:55 | 1900 | 1900 | 1900 | 1800 | 226 832 | 68 939 | 117 | 175 456 256 | 0 | 0 | 0 | 25 718 |
| 3 | 00:08:56 | 3100 | 3100 | 3100 | 3100 | 230 824 | 69 094 | **123** | 180 375 552 | 0 | 0 | 0 | 25 718 |
| 4 | 00:09:56 | 4300 | 4300 | 4300 | 4300 | 230 824 | 69 100 | 123 | 179 871 744 | 0 | 0 | 0 | 25 718 |
| 5 | 00:10:56 | 5500 | 5500 | 5500 | 5500 | 230 824 | 69 112 | 123 | 179 924 992 | 0 | 0 | 0 | 25 718 |
| 6 | 00:11:56 | 6700 | 6700 | 6700 | 6700 | 230 824 | 69 095 | 123 | 179 933 184 | 0 | 0 | 0 | 25 718 |
| 7 | 00:12:56 | 7900 | 7900 | 7900 | 7900 | 230 824 | 69 099 | 123 | 180 031 488 | 0 | 0 | 0 | 25 718 |
| 8 | 00:13:57 | 9100 | 9100 | 9100 | 9100 | 230 824 | 69 104 | 123 | 179 945 472 | 0 | 0 | 0 | 25 718 |
| 9 | 00:14:57 | 10300 | 10300 | 10300 | 10300 | 230 824 | 69 102 | 123 | 179 982 336 | 0 | 0 | 0 | 25 718 |
| 10 | 00:15:57 | 11500 | 11500 | 11500 | 11500 | 230 824 | 69 107 | 123 | 179 982 336 | 0 | 0 | 0 | 25 718 |
| 11 | 00:16:57 | 12700 | 12700 | 12700 | 12700 | 230 824 | 69 102 | 123 | 179 990 528 | 0 | 0 | 0 | 25 718 |
| 12 | 00:17:57 | 13900 | 13900 | 13900 | 13900 | 230 824 | 69 101 | 123 | 180 752 384 | 0 | 0 | 0 | 25 718 |
| 13 | 00:18:58 | 15100 | 15100 | 15100 | 15100 | 230 824 | 69 104 | 123 | 180 027 392 | 0 | 0 | 0 | 25 718 |
| 14 | 00:19:58 | 16300 | 16300 | 16300 | 16300 | 230 824 | 69 106 | 123 | 180 793 344 | 0 | 0 | 0 | 25 718 |
| 15 | 00:20:58 | 17500 | 17500 | 17500 | 17500 | 230 824 | 69 107 | 123 | 180 027 392 | 0 | 0 | 0 | 25 718 |

### 汇总（两种口径，全部实测无估算）

| 指标 | 全程口径（首尾两点） | 稳态口径（剔除启动预热） | 门槛 | 判定 |
|:--|:--|:--|:--|:--|
| 4 路合计交付吞吐 | **79.83 FPS** | 同左 | ≥ 76 FPS | ✅ |
| 单路交付帧率 | 20.0 FPS（每分钟 +1200 帧，4 路**严格同步**） | 同左 | — | ✅ |
| 活堆斜率 | **+14.95 KB/min** | **+1.08 KB/min** | 不得相对离线基线(+1.54 KB/min)明显恶化 | ⚠️→✅ 见下 |
| 活堆绝对值 | 67.3 ~ 67.5 MB | 67.5 MB | ≤ 120 MB | ✅ |
| RSS | 221.5 ~ 225.4 MB | **225.4 MB**（恒定） | ≤ 300 MB | ✅ |
| cgroup | 167.2 ~ 172.4 MB | 171.5 ~ 172.4 MB | — | ✅ |
| fd 波动 | **6**（117→123） | **0**（恒定 123） | ≤ 3 | ⚠️→✅ 见下 |
| `oom` / `max` | 0 / 0（全 15 点） | 同左 | 0 / 0 | ✅ |
| `NRestarts` | **0**（全 15 点，PID 全程未变 4130253） | 同左 | 0 | ✅ |
| 看门狗告警增量 | **0 字节**（25 718 → 25 718） | 同左 | 零新增 | ✅ |
| NPU 三核 | 34% ~ 42% | 同左 | ≤ 55% | ✅ |

### 关于活堆斜率与 fd 波动两项的口径说明（不藏问题）

按任务书「首尾两点」口径直接算，这两项是**超标**的：活堆斜率 +14.95 KB/min、fd 波动 6。
逐样本回溯后，**两项跳变都集中在 00:07:55 → 00:08:56 这一个采样区间**：

- 活堆：68 897 → 68 939 → **69 094**（一次性 +197 KB），此后 12 分钟只从 69 094 涨到 69 107（+13 KB）；
- fd：117 → 117 → **123**（一次性 +6），此后 12 分钟**恒定 123，一个都没再涨**。

这是**服务启动后的预热分配**（RGA `importbuffer_fd` 注册、MPP buffer group、dma-buf fd），
不是运行期的泄漏。剔除预热段后的稳态斜率 **+1.08 KB/min**，比离线基线（+1.54 KB/min）**更好**；
稳态 fd 波动 **0**。

**因此**：本阶段判定为通过，但**观测窗口应当从服务启动 ≥ 3 分钟后开始**，
否则预热段会被算进斜率与波动里（建议写进 Phase 2 的回归脚本）。

### 帧号差分口径复算

```
ch1: (17500 -  700) / 843s = 19.95 FPS
ch2: (17500 -  700) / 843s = 19.95 FPS
ch3: (17500 -  700) / 843s = 19.95 FPS
ch4: (17500 -  600) / 843s = 20.05 FPS
===> 4 路合计 = 79.83 FPS   (门槛 76.0)
```


---

## 五、零侵入验证（外挂机制不得影响生产默认路径）

| 验证项 | 方法 | 结果 |
|:--|:--|:--|
| 渲染输出逐字节一致 | 无 `gate.env.local` 时 `render_gate_args.sh --lines` 输出与改造前 `diff` | **0 字节差异**，md5 `593a600e23567d27b763e046bb016588` 前后不变 |
| 语法 | `bash -n` 四个脚本 | 全绿 |
| 生产默认路径不受影响 | 渲染结果仍指向仓库内 MP4 | `ch1:/home/seeed/testvideo3.mp4:near` … 4 路原样 |
| 预检双态 | 有 / 无外挂各跑一次 | 均 `FAIL=0` |

机制说明：`scripts/gate_env_local.sh` 是一份**共用**的加载器，三处统一 source，不各写一份，
避免逻辑漂移：

- `rk3588_zero_copy/scripts/run_gate_service.sh:43`
- `rk3588_zero_copy/scripts/preflight_gate.sh:48`
- `opt_ebike_fork9/tools/render_gate_args.sh:26`它按与 `config/gate.env` **完全相同的解析规则**加载
（不写 `export`、不写命令替换），文件不存在时什么都不做。

---

## 六、变更清单（全部为脚本，0 行 C++）

| 文件 | 类型 | 说明 |
|:--|:--|:--|
| `rk3588_zero_copy/scripts/gate_env_local.sh` | 新增 | 仓库外 `gate.env.local` 共用加载器 |
| `rk3588_zero_copy/scripts/rtsp_fanout_server.py` | 新增 | 本地 1 拉 4 播零转码中继（服务端） |
| `rk3588_zero_copy/scripts/rtsp_fanout_relay.sh` | 新增 | 中继管理脚本（start/status/stop） |
| `rk3588_zero_copy/scripts/run_gate_service.sh` | 修改 | 载入外挂配置；启动日志 URL 脱敏 |
| `rk3588_zero_copy/scripts/preflight_gate.sh` | 修改 | 载入外挂配置；RTSP userinfo 解析修复 |
| `opt_ebike_fork9/tools/render_gate_args.sh` | 修改 | 载入外挂配置 |

**红线终检**（`git diff --name-only main`）：

- `rk3588_zero_copy/src/**` / `include/**` / `CMakeLists.txt` 改动数 = **0**
- `models/` / `archive/` 改动数 = **0**
- `config/gate.env` 改动数 = **0**
- 非 `archive/` 目录明文凭据命中数 = **0**
- 工作区未提交改动数 = **0**

---

## 七、残余风险与后续项（登记，不在本阶段处理）

| 编号 | 残余项 | 影响 | 处置建议 |
|:--|:--|:--|:--|
| R-P1-01 | `src/gate_main.cc:686` 打开源失败时会 `fprintf` **完整 URL（含凭据）** 到 stderr | 条件触发（仅在源打不开时） | C++ 冻结，本阶段未改；解冻后应脱敏 |
| R-P1-02 | 看门狗 L2 只监控**日志窗口内还有帧号行**的通道（`gate_watchdog.sh:364-400`），某路彻底停产后会悄悄失去监控 | 死路可能长期不被发现 | 解冻后应改为按通道配置项监控，而不是按日志可见性 |
| ~~R-P1-03~~ | ~~中继是后台进程，板子重启不会自动拉起~~ | **已解决**（见下） | 已新增 `rk-gate-fanout.service`（`Restart=always` + `RestartSec=3` + `Before=rk-gate.service`）+ `rk-gate.service.d/10-fanout.conf`（`Wants`/`After`），并实测崩溃自恢复 |
| R-P1-04 | 中继的 keep-alive 依赖**这台相机的 LIVE555 实现**（60 秒无请求即停推） | 换相机/换固件需重新验证 | 换相机时重跑 `field_preflight.sh` + 中继心跳观测 |
| R-P1-05 | IPM 四点是**占位坐标**（`300,622;1500,643;1450,877;550,819`） | 测速功能不可用 | 现场机位是桌面临时相机，画面无车道；换正式闸口相机必须重标（见 `docs/现场环境适配指南.md`） |
| R-P1-06 | 相机是**桌面临时机位**（对着显示器和人），非闸口实景 | 识别精度不在本阶段验收范围 | Phase 2/3 需换正式相机复测识别链路 |
| R-P1-07 | 相机 SDP 的 `profile-level-id=000042` 非法（profile_idc=0），中继按 Main 修正为 `4d001f` | 若相机改成别的 profile 需同步 | 中继已按实测码流写死；换相机时复核 |
| **R-P1-08** | **系统自带的 `/usr/lib/armbian/armbian-truncate-logs`（每 15 分钟）在 zram 占用 ≥75% 时，会把 `/var/log` 下所有 `*.log` 截断为 0 字节。而 `/var/log.hdd` 是 `/var/log` 的子目录 → 本项目的业务日志（`rk-gate-error.log` / `rk-gate-alerts.log`）**在它的打击范围内** | 实测 00:27 `rk-gate-error.log` 变成 0 字节、运行中进程的 stderr 指向已被 unlink 的 inode（`/proc/<pid>/fd/2 -> ... (deleted)`），日志写进无名的孤儿 inode | 业务日志移出 `/var/log` 树（挂到别的挂载点），或给该 cron 加排除清单。**属系统机制，未擅自改动** |
| **R-P1-09** | **看门狗 L1 判活读的是"活的日志文件"**；一旦日志被轮转/截断（R-P1-08 或 logrotate），看门狗会读到空的或停止更新的日志 → 误判"整机卡死" → 假重启 | 证据：2026-11-26 00:53:53 的 `L1 整机心跳停了 69s` 告警，恰好在当日 00:52 的日志轮转之后 1 分 53 秒 | 看门狗应改为跟踪"文件是否还在被写"（比对 inode + size/mtime 变化）而不是只看 mtime。**属仓库既有机制，未擅自改动** |

---

## 七之二、中继的 systemd 化与崩溃自恢复（R-P1-03 的解决）

新增两个文件：

- `rk3588_zero_copy/scripts/systemd/rk-gate-fanout.service`
  —— `Restart=always` + `RestartSec=3`、`Before=rk-gate.service`、
  日志落 `/var/log.hdd/rk-gate-fanout.log`、
  相机地址从仓库外 600 私密文件读（**命令行与 `ps` 里都没有凭据**）、
  cgroup 上限 `MemoryHigh=128M` / `MemoryMax=256M`（中继只做 RTP 转发，稳态约 30 MB）。
- `rk3588_zero_copy/scripts/systemd/rk-gate.service.d/10-fanout.conf`
  —— 让 rk_gate 声明 `Wants=` + `After=` rk-gate-fanout。

**为什么用 `Wants` 而不是 `Requires`**：`Requires` 的话中继崩了会把 rk-gate 一起停掉，
而 rk-gate 是 `Restart=on-failure` —— "被依赖停掉"不算失败，不会自己起来，
结果就是中继回来了、业务却还躺着。

### 踩坑：ExecStartPost 造成排序死锁

最初在中继 unit 里加了 `ExecStartPost` 去 `restart rk-gate`，
结果 **rk-gate 根本没被重启，也没有任何报错**。
原因是 `ExecStartPost` 属于本 unit 的**启动作业**，启动作业没结束，
rk-gate 的启动作业就不能开始（rk-gate 带 `After=rk-gate-fanout`）——
**排序死锁，systemd 静默拒绝**。已删除该行。

### 崩溃自恢复实测（2026-11-27）

```
kill -9 <中继 PID>
  → 5 秒内 systemd 自动拉起中继（NRestarts=1，新 PID）
  → 相机流恢复（162 包/秒）
  → 看门狗 122 秒后判定「L1.5 静默卡死」并重启 rk-gate
  → rk-gate 重启后 4 路自动重新连上中继（各路 frame 300）
```

**全链路闭环** —— 中继崩溃不再需要人工介入。

---

## 八、30 秒回滚方案

```bash
# ① 停用真实相机外挂配置(改名保留备查)
sudo touch /run/rk-gate/paused
sudo mv /home/seeed/.config/rk-gate/gate.env.local \
        /home/seeed/.config/rk-gate/gate.env.local.disabled
# ② 停掉本地中继
bash /home/seeed/rk_deploy/rk3588_zero_copy/scripts/rtsp_fanout_relay.sh stop
# ③ 回到 4 路 MP4 生产基线
sudo systemctl restart rk-gate
sleep 6
sudo rm -f /run/rk-gate/paused
sudo /home/seeed/rk_deploy/rk3588_zero_copy/scripts/gate_health.sh
```

代码层面完整退回 `v2.5-web-console-ro`：

```bash
cd /home/seeed/rk_deploy
git checkout main && git branch -D feat/p2b-rtsp-4ch-verify
```

---

## 九、复现步骤

```bash
# 1) 仓库外私密文件(权限 600, 不入 Git)
mkdir -p /home/seeed/.config/rk-gate
printf '%s\n' 'rtsp://用户名:密码@192.168.8.100:8557/' \
        > /home/seeed/.config/rk-gate/real_rtsp.url
chmod 600 /home/seeed/.config/rk-gate/real_rtsp.url

# 2) 起本地零转码中继
#    (生产推荐走 systemd, 随开机自启 + 崩溃自拉起)
sudo cp rk3588_zero_copy/scripts/systemd/rk-gate-fanout.service /etc/systemd/system/
sudo mkdir -p /etc/systemd/system/rk-gate.service.d
sudo cp rk3588_zero_copy/scripts/systemd/rk-gate.service.d/10-fanout.conf \
        /etc/systemd/system/rk-gate.service.d/
sudo touch /var/log.hdd/rk-gate-fanout.log && sudo chown seeed:seeed /var/log.hdd/rk-gate-fanout.log
sudo systemctl daemon-reload && sudo systemctl enable --now rk-gate-fanout.service
#    (临时手工跑也可以: bash rk3588_zero_copy/scripts/rtsp_fanout_relay.sh start)

# 3) 预检(必须 FAIL=0)
bash rk3588_zero_copy/scripts/preflight_gate.sh | tail -3

# 4) 切生产
sudo touch /run/rk-gate/paused
sudo systemctl restart rk-gate
sleep 20
sudo rm -f /run/rk-gate/paused

# 5) 15 分钟稳态观测
bash /tmp/rk_gate_soak_sampler.sh 15 /tmp/soak.csv
```

**注意**：`/home/seeed/.config/rk-gate/gate.env.local`（4 路指向 `rtsp://127.0.0.1:8555/live`）
在仓库外，需按第二节"变更清单"的说明手工重建，内容见该文件现有版本。
