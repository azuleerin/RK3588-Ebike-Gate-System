# 网页控制台 —— 生产只读监控 + 演示调试(双视图)

> 一句话: **浏览器打开一个地址, 就能看到生产 `rk_gate` 的 4 路实时帧号/耗时/RSS/活堆/
> 最近车牌/看门狗告警(只读); 切到"演示调试"视图, 还能点 MPP / RGA / NPU 三个开关
> 当场把硬解/加速/推理关掉看性能曲线怎么变; 路由器换了网段时, RTSP 地址在网页上
> 现填现测, 不用改代码重编译。**
>
> 面向: 现场运维 / 调性能的人 / 想知道"到底卡在哪"的人。
>
> **适用版本**: `v2.4-p2b-prep` 起(O11 方案 A + 方案 C 落地)

---

## 0. ⚠️ 先读这一段: 两个视图的归属(方案 C 口径)

这个控制台有**两个互不相干的视图**, 页面右上角切换。**它们对接的是两条不同的服务链**,
搞混会得出完全错误的结论 —— 所以先把边界说清楚:

| | **生产监控**(默认视图) | **演示调试** |
|---|---|---|
| **对接的服务** | 生产 **`rk_gate`**(`src/gate_main.cc`, 4 路单进程 + 全局 NPU 线程池) | 演示 **`rk_zero_copy`**(`src/main.cc`, 单流) |
| **数据来源** | ① `config/gate.env` 的 `GATE_LOG_DIR`(现网 `/var/log.hdd`)下 `rk-gate-error.log` 的**尾部 8 MB**; ② 生产 MQTT 的分通道结果主题 `device/<sn>/ch<N>/message/up/ivs_result` 与 `device/<sn>/message/up/tripwire`; ③ `scripts/gate_health.sh` 的结论 | 演示实例的 MQTT 状态主题 `device/<sn>/message/up/status` + `up/load` |
| **MPP/RGA/NPU 三个开关** | ❌ **不可见**(且 JS 层面有护栏, 误点会被挡下) | ✅ 可点, 通过 `device/<sn>/message/down/control` 下发 |
| **压力测试按钮** | ❌ **不可见**(护栏同���) | ✅ 可点 |
| **能不能改生产服务** | ❌ **不能, 整个面板只读** | — |
| **能不能改演示服务** | — | ✅ 能 |

### 0.1 为什么生产服务只能"从日志里读"

生产 `rk_gate` **不上报** `device/<sn>/message/up/status` —— 那是演示链路
`rk_zero_copy` 的状态主题(`src/engine_ctl.cc` 的 `StatusPublisher`,
**只编进 `rk_zero_copy` 目标**, 见 `CMakeLists.txt:233,240` vs `:283-298`)。
生产真正对外发的只有分通道的 `ivs_result` 和设备级的 `tripwire`。
所以"生产实时状态"(帧号 / 各阶段耗时 / RSS / 活堆 / 最近车牌)只能从**业务日志**解析 ——
这就是生产监控视图的由来。

完整评估(含为什么否决"把三个开关接进生产")见 `docs/o11_web_console_audit.md`。

### 0.2 明确的红线

- **生产监控视图【不下发任何指令】**, 也不改任何生产配置、不重启任何服务。
- 三个硬件开关与压力测试**只属于已冻结的 `rk_zero_copy` 演示链路**,
  对生产 `rk_gate` **无效**(生产 `rk_gate` 里既没有 `down/control` 订阅,
  也没有运行时硬件开关)。
- 生产监控视图**只读日志尾部**: `seek(-8MB, SEEK_END)` 定长读取,
  **严禁整文件读** —— 生产日志会长到几百 MB, 整读会把页面拖死
  (教训: `SOAK_REPORT_20261118.md` —— 采样器每次 grep 整个日志,
  实测 7 分钟就变成 1.9 秒/次)。

---

## 1. 它长什么样

```
┌────────────────────────────────────────────────────────────────┐
│ RK3588 流水线控制台  ● MQTT 已连接  设备 rk3588-01  ...        │
├───────────────────────────────────────────┬────────────────────┤
│ 视频源(现场可换地址)                       │  实时画面          │
│  [rtsp://192.168.8.200:8557/h264] 测试连接 应用                │
│  ❌ IP 或端口不可达 — TimeoutError         │  (MJPEG 预览)     │
│  [正在重连…] 已重连 2 次                   │                    │
├───────────────────────────────────────────┴────────────────────┤
│ 硬件引擎开关              │ 实时指标                            │
│  MPP 硬解        [ ●━━ ]  │  FPS 25.02   源 FPS 25.00         │
│  RGA 硬件加速    [ ●━━ ]  │  解码 0.64ms  RGA 2.32ms          │
│  NPU 推理        [ ━━● ]  │  NPU 25.00ms  总耗时 39.97ms       │
├───────────────────────────┴────────────────────────────────────┤
│ FPS 与单帧总耗时   (折线图, 双 Y 轴)                            │
├───────────────────────────┬────────────────────────────────────┤
│ 各阶段耗时(ms)             │ 硬件负载(%)                         │
│  解码 / RGA / NPU / 后处理  │  VPU解码 / CPU / DMC / GPU / RGA   │
└───────────────────────────┴────────────────────────────────────┘
```

---

## 2. 视频源: 网段变了不用改代码(现场最常踩的坑)

### 2.1 为什么要做这个

现场反馈的故障十有八九不是程序挂了, 而是**路由器换了网段**:
路由器从 `192.168.31.x` 换成 `192.168.8.x`, 相机和板子的 IP 全变了,
代码里写死的 `rtsp://192.168.31.200:8557/h264` 就废了, 页面只剩一句
"视频源连接失败" —— 现场根本分不清是**地址错了**、**相机没开机**、
还是**网线松了**。

所以这个地址必须能: **现场手填 → 立刻测通不通 → 自动重连**。

### 2.2 页面上的操作

| 元素 | 作用 |
|---|---|
| 输入框 | 填完整的 RTSP 地址(默认 `rtsp://127.0.0.1:8554/camera1`, 即本地回放) |
| **快捷填入** | 两个按钮: `本地回放` / `真实相机`(现场换源不用手打) |
| **测试连接** | 后端做 TCP 握手(1.5 秒超时)+ 一句 RTSP `OPTIONS`, 立刻给结论 |
| **应用** | 换源: 后端立刻切过去重连, 同时存到后端配置文件和浏览器 LocalStorage |
| 状态徽章 | `已连接` / `连接中…` / `正在重连…` / `不可达` |
| 预览框 | MJPEG 实时画面(装 OpenCV 才有; 没装也能测连通性) |

**测试连接的三种结论**(现场照着这个表就能定位问题):

| 页面上的字 | 含义 | 下一步 |
|---|---|---|
| `✅ 连通 — 127.0.0.1:8554 · 握手 3 ms` | TCP 通, RTSP 服务正常 | 点【应用】就能出画面 |
| `❌ IP 或端口不可达` | TCP 都握不上 | 查网段: 相机 IP 变了吗? 相机开机了吗? 网线通了吗? |
| `❌ 端口通，但不是 RTSP 服务` | 端口开着, 但上面不是 RTSP | 地址/端口填错了 |

### 2.3 假死检测与自动重连

"网线没插紧" 这种情况的特征是: **TCP 能连上, 但一帧码流都收不到**。
所以拉流线程不看"连上了没有", 而看**"最近 5 秒有没有出画面"**:

- **5 秒没收到画面** → 判定假死 → 关掉重开, 退避 1→2→4→8 秒重试;
- 页面徽章变 `正在重连…`, 预览框上盖一层**红字**"视频源已断开，正在重连...",
  并弹一个红色 Toast(停 6 秒, 保证看得见); 恢复后弹 `视频源已恢复`;
- 退避策略分两档:
  - **连上过又断了**(多半是网线松了)→ 快退避(1/2/4/8 秒), 尽快恢复;
  - **一直没连上过**(多半是地址错了)→ 慢退避(5/10/20/30 秒),
    别每秒去敲一次门(会刷爆内核日志, ARP 失败会刷 `detected local route`)。

### 2.4 没有相机时怎么验(本地 RTSP 回放服务器)

现场/实验室经常没有相机可用(相机换了网段、还没接上、或者就是要复现"假死")。
`scripts/rtsp_replay_server.py` 在板子上起一个**协议完全合规**的 RTSP 服务:
完整握手 + RTP/H.264(**真码流**, 不是假帧), 走的是和真相机一样的客户端路径
——OpenCV、FFmpeg、以及本仓库的 `rk_zero_copy` 主程序(MPP 硬解)都能正常拉。

```bash
# 默认: testvideo3.mp4 推到 rtsp://127.0.0.1:8554/camera1
python3 scripts/rtsp_replay_server.py

# 换端口/路径/视频
python3 scripts/rtsp_replay_server.py --port 8555 --path /cam2 \
    --video /home/seeed/电动车素材.mp4

# 假死模式: 握手成功后一直不说话(模拟网线松了), 用来验重连
python3 scripts/rtsp_replay_server.py --stall-only

# 中途恢复: 先静默 N 秒, 然后开始出画
python3 scripts/rtsp_replay_server.py --stall 10
```

然后把控制台指过去:

```bash
curl -s -X POST -H 'Content-Type: application/json' \
     -d '{"url":"rtsp://127.0.0.1:8554/camera1"}' http://127.0.0.1:8080/api/rtsp
```

> ★ 它靠仓库里的 `tools/es_dump` 把 MP4 转成 Annex-B 裸流再发出去
> (板子上没有 H.264 编码器, 所以是"播现成的码流", 不是现场编码)。
>
> ★ `scripts/start_console_stack.sh` **默认就把它起起来**(第 0 个进程),
> 所以一键起完, 控制台的"视频源"面板直接就有画面。
> 换成真相机: `--rtsp rtsp://192.168.8.200:8557/h264 --no-replay`。

### 2.5 地址存在哪

| 位置 | 作用 |
|---|---|
| 浏览器 LocalStorage(`rk3588.console.rtsp`) | 换台电脑打开页面, 还能看到自己上次填的地址 |
| `logs/console_stack/rtsp.json` | 后端持久化 —— 控制台重启、甚至板子重启, 地址都还在 |

`start_console_stack.sh` 的生效顺序:
**命令行 `--rtsp` > `logs/console_stack/rtsp.json` > 默认地址**。

### 2.6 相关的 HTTP 接口

| 接口 | 说明 |
|---|---|
| `GET /api/rtsp` | 当前地址 + 默认地址 + 实时状态(页面首次加载时拉一次) |
| `POST /api/rtsp` | `{"url":"rtsp://..."}` → 换源 + 持久化 |
| `POST /api/rtsp/test` | `{"url":"rtsp://..."}` → 连通性测试(TCP + RTSP OPTIONS) |
| `GET /api/video.mjpeg` | MJPEG 预览流(`multipart/x-mixed-replace`) |
| `GET /api/video/snapshot.jpg` | 单帧 JPEG |

> ★ **OpenCV 是可选的**: 装了才有画面预览; 没装的话连通性测试和假死检测
> 依然可用(走纯标准库的 RTSP 握手 + interleaved 数据活跃度判断)——
> 现场 PC 上没装 opencv 也能用这个功能。

---

## 3. 数据通路

### 3.1 演示调试视图(演示链路 `rk_zero_copy`)

```
  ┌──────────── PC ────────────┐        MQTT         ┌──── 板子 ────┐
  │  web_console.py            │                      │              │
  │   ├─ 网页(HTML+canvas)      │                      │  rk_zero_copy │
  │   ├─ HTTP :8080             │                      │              │
  │   │    GET  /               │                      │  ┌────────┐  │
  │   │    GET  /api/state      │◀─ up/status ────────│  │控制线程│  │
  │   │    POST /api/control    │─ down/control ─────▶│  └────────┘  │
  │   │    POST /api/rtsp/test  │                      │  ┌────────┐  │
  │   │    GET  /api/video.mjpeg│◀── RTSP ──────────▶相机 │ 状态线程│  │
  │   └─ MQTT 客户端            │                      │  └────────┘  │
  └─────────────────────────────┘                      └──────────────┘
```

三个话题(默认 `sn = rk3588-01`):

| 方向 | 主题 | 内容 |
|---|---|---|
| 网页 → 板子 | `device/rk3588-01/message/down/control` | `{"cmd":"set_engine","mpp":true,"rga":false,"npu":true}` |
| 板子 → 网页 | `device/rk3588-01/message/up/status` | `{fps,dec_ms,rga_ms,npu_ms,post_ms,total_ms,wait_ms,engine,decoder,rga_mode,load{...}}` |
| 板子 → 网页 | `device/rk3588-01/message/up/load` | `{npu,npu_core,rga,vpu,cpu,dmc,gpu}`(root 采样器发的, 见 §6) |

### 3.2 生产监控视图(生产 `rk_gate`, 只读)

```
  ┌──────────── PC / 板子 ────────────┐
  │  web_console.py                    │      ① 读日志尾部(定长 8 MB)
  │    GET /api/gate_state ────────────┼──▶ /var/log.hdd/rk-gate-error.log
  │    GET /api/gate_health ───────────┼──▶ scripts/gate_health.sh(后台线程, 每 30s)
  │    MQTT 订阅 ──────────────────────┼──▶ device/<sn>/ch<N>/message/up/ivs_result
  │                                    │      device/<sn>/message/up/tripwire
  └────────────────────────────────────┘
```

★ **注意这里没有 `down/control`** —— 生产视图是**单向**的:
控制台只**读**生产服务, 绝不向它下发任何指令。

| 方向 | 来源 | 内容 |
|---|---|---|
| 控制台 ← 生产 | `rk-gate-error.log` 尾部 8 MB | `[chN][frame M] fps=… deliv=… src=… rga=…ms npu=…ms detwait=…ms ocrwait=…ms objs=… tracks=… rss=…KB heap=…KB` 与 `[chN] [车牌] track=… score=… -> "…"` |
| 控制台 ← 生产 | `rk-gate-alerts.log` 尾部 64 KB | 看门狗告警行 |
| 控制台 ← 生产 | `device/<sn>/ch<N>/message/up/ivs_result` | 分通道检测/识别结果 JSON |
| 控制台 ← 生产 | `device/<sn>/message/up/tripwire` | 过线计数 JSON(`in`/`out`/`total` + 各相机分项) |
| 控制台 ← 生产 | `scripts/gate_health.sh` | 结论(正常/可疑/卡死)+ 退出码 |

---

## 4. 部署步骤

### 4.1 板子: 重新编译(带了控制通道)

```bash
cd /home/seeed/rk_deploy/rk3588_zero_copy
cmake --build build -j4 --target rk_zero_copy     # 或者 ./build_native.sh
```

改了哪些文件:
- 新增 `include/mqtt_sub.h` + `src/mqtt_sub.cc` —— 极简 MQTT 订阅端
- 新增 `include/engine_ctl.h` + `src/engine_ctl.cc` —— 运行时开关 + 状态上报 + 负载采集
- 改 `src/main.cc` —— 接上控制线程 / 状态上报 / 三个开关的切换逻辑
- 改 `src/rga_convert.cc` —— RGA 硬件路径的运行期开关
- 改 `CMakeLists.txt` / `build_native.sh` —— 加新源文件

### 4.2 板子: 启动(加上 `--mqtt-host` 就同时开了控制+上报)

```bash
cd /home/seeed/rk_deploy/rk3588_zero_copy
export LD_LIBRARY_PATH="$PWD/third_party/lib:$PWD/lib:/usr/lib:$LD_LIBRARY_PATH"

./build/rk_zero_copy \
    rtsp://192.168.8.200:8557/h264 \
    ./model/ebike_int8_rk2332.rknn \
    --labels ./model/labels_ebike.txt \
    --track-cls 1 --nms 0.45 \
    --no-show \
    --mqtt-host 192.168.8.21 \
    --sn rk3588-01
```

启动日志应该看到这三行:

```
[解码] MPP 硬解已就绪 (H.264, VPU), 可在网页控制台动态开关
[控制] 状态上报主题: device/rk3588-01/message/up/status (周期 1000 ms)
[控制] 指令订阅主题: device/rk3588-01/message/down/control
```

新命令行参数:

| 参数 | 默认 | 说明 |
|---|---|---|
| `--mqtt-host HOST` | 无 | 开了才走 MQTT(结果上行 + 控制 + 状态上报) |
| `--mqtt-ctl-topic T` | `device/<sn>/message/down/control` | 控制指令订阅主题 |
| `--mqtt-status-topic T` | `device/<sn>/message/up/status` | 状态上报主题 |
| `--no-mqtt-ctl` | 关 | 只上报状态, 不接受控制指令 |
| `--status-interval-ms N` | 1000 | 状态上报周期 |

### 4.3 PC: 启动网页控制台

```bash
cd /home/seeed/rk_deploy/rk3588_zero_copy
python3 scripts/web_console.py --host 192.168.8.21 --sn rk3588-01 --http-port 8080
```

> **不需要 pip install**。只用了 Python 标准库(`http.server` + `socket`),
> 现场 PC 没装 flask/paho 也能直接跑。
> (装了 OpenCV 的话, 还能在网页上看视频源预览。)

浏览器打开: **http://<PC的IP>:8080/**

一键起三件套(控制台 + 板端主程序 + 负载采样器):

```bash
sudo ./scripts/start_console_stack.sh            # 用默认/上次保存的 RTSP 地址
./scripts/start_console_stack.sh --rtsp rtsp://192.168.8.200:8557/h264   # 临时换一个
```

### 4.4 只看生产 `rk_gate`(推荐给现场运维的用法)

```bash
cd /home/seeed/rk_deploy/rk3588_zero_copy
python3 scripts/web_console.py --host <broker IP> --http-port 8080
```

**参数说明**:

| 参数 | 默认 | 说明 |
|---|---|---|
| `--sn` | **空** | 留空 → 自动读 `config/gate.env` 的 `GATE_SN`(现网默认 `RK3588-GATE-01`); 读不到才退回 `rk3588-01`。**两个都会订阅**, 所以现场改了一处也不会收不到数据 |
| `--gate-env PATH` | `<repo>/rk3588_zero_copy/config/gate.env` | 生产配置文件路径。控制台用它取 `GATE_SN` 与 `GATE_LOG_DIR`(**只读解析, 不 source**) |
| `--host` / `--port` | `192.168.8.21` / `1883` | MQTT broker |

> ★ **控制台在 PC 上跑时**, `config/gate.env` 要么放在控制台旁边(同仓库),
>   要么用 `--gate-env` 指到板子上的那份 —— 否则读不到 `GATE_LOG_DIR`,
>   会退回默认的 `/var/log.hdd`(现网就是它, 所以通常不用管)。
> ★ **控制台在板子上跑时**, 默认路径直接就对。

**打开页面后**: 默认就是**生产监控**视图(右上角切"演示调试"才切过去)。
生产视图里能看到:

| 卡片 | 内容 |
|---|---|
| 横幅 | 当前连接生产 `rk_gate`(只读监控模式)+ 日志路径 + 设备号 |
| 健康检查 | `scripts/gate_health.sh` 的结论(正常/可疑/卡死)+ 快照新鲜度 |
| 内存水位 | RSS / 活堆 / 日志文件大小 |
| 4 路实时状态 | 每路的帧号、FPS、交付 FPS、源 FPS、RGA/NPU/排队/OCR 等待(ms)、目标数、轨迹数 |
| 最近车牌识别 | 最近 12 条 `[车牌]` 记录(通道 + score + track + 识别文本) |
| 最近看门狗告警 | `rk-gate-alerts.log` 尾部最近 10 条 |

**新增 HTTP 接口**(都是只读):

| 接口 | 返回 | 耗时(实测) |
|---|---|---|
| `GET /api/gate_state` | 生产实时状态 JSON(见上表) | **30~50 ms**(定长读尾部 8 MB + 定位解析) |
| `GET /api/gate_health` | 健康检查结论 JSON(`verdict` / `exit_code` / `age_sec` / `raw`) | **约 2 ms**(读后台缓存) |

> ★ `gate_health.sh` 自己要隔 6 秒取两次帧号快照(判"帧号在不在推进"),
>   一次完整运行 6 秒多。所以控制台**在后台线程里跑它**(默认每 30 秒一次),
>   `/api/gate_health` 只返回**上一次的结果** + 它有多旧(`age_sec`)。
>   ⇒ 接口永远毫秒级返回, 页面可以每秒轮询。

---

## 5. 怎么用(验收步骤)

### 5.0 生产监控视图(只读)

| 操作 | 预期现象 | 为什么 |
|---|---|---|
| 打开页面 | 默认停在**生产监控**视图, 横幅写明"当前连接生产服务 rk_gate(只读监控模式)" | 默认落生产视图 —— 打开第一眼看到的不该是【对生产无效】的演示开关 |
| 看"4 路实时状态" | 4 行(ch1~ch4), 帧号在涨, FPS/耗时有值 | 帧号来自业务日志尾部, 每 100 帧一行 |
| 帧号**不涨** | 说明那一路真的停了(源断流/相机挂了) | 帧号是逐路独立的 |
| 点右上角"演示调试" | 切到演示链路: 视频源、三个硬件开关、压测按钮、图表都回来了 | 两个视图对接的是两条不同的服务链 |
| 在**生产**视图里找硬件开关 | **找不到** —— 生产视图里根本没有那三个开关和压测按钮 | 它们只属于演示链路; 生产 `rk_gate` 里没有 `down/control` 订阅 |
| `curl /api/gate_state` | HTTP 200, JSON 里有 `channels` / `plates` / `alerts` | 只读接口 |
| `curl /api/gate_health` | HTTP 200, `verdict` 是 正常/可疑/卡死 | 只读接口 |

### 5.1 演示调试视图(开关联动)

| 操作 | 预期现象 | 为什么 |
|---|---|---|
| 点 **NPU 推理** 关 | FPS 基本不变(源就 25fps), `NPU 推理` 变 0ms, 画面上检测框消失, 负载图里 NPU 掉到 0 | 推理是流水线里最大的一块耗时(约 24ms), 去掉之后省下的时间只能用来【等流】—— 源限速时 FPS 不会涨 |
| 点 **MPP 硬解** 关 | `解码` 从 0.6ms 飙到 15ms+, FPS 从 25 掉到 19 左右 | 换成 FFmpeg 软解, CPU 扛 1080p |
| 点 **RGA 硬件加速** 关 | `RGA 预处理` 从 2.3ms 涨到 8ms+, CPU 负载明显上升 | 改走 OpenCV CPU 缩放, 多一次整帧内存拷贝 |
| 再点回去 | 全部指标立刻回到原值 | 切换是运行期的, 不用重启进程 |

板端日志会同步打印每一次切换:

```
[控制] 收到指令: mpp=on rga=on npu=off
[开关] NPU 推理 -> 关 (只跑解码+预处理, 不出检测框)
[开关] 解码后端 -> FFmpeg 软解 (CPU, 会明显变慢)
[开关] RGA 硬件加速 -> 开 (improcess 零拷贝)
```

---

## 6. 硬件负载的来源(为什么有的显示 -1)

| 指标 | 数据源 | 需要 root? |
|---|---|---|
| NPU 单核占用 | `/sys/kernel/debug/rknpu/load` | **要** |
| RGA 占用 | `/sys/kernel/debug/rga/load` | **要** |
| VPU(解码核)占用 | `/proc/mpp_service/load` | 不要 |
| CPU 占用 | `/proc/stat` | 不要 |
| DMC / GPU | `/sys/class/devfreq/*/load` | 不要 |

主程序(普通用户跑)自己就能读 VPU / CPU / DMC / GPU, 报在 `up/status` 里。
**NPU 和 RGA 的内核真值要 root**, 所以另配一个采样器:

```bash
# 在板子上
sudo python3 scripts/hw_load_mqtt.py --host 192.168.31.214 --sn rk3588-01
```

它每秒采一次, 发到 `device/rk3588-01/message/up/load`, 网页端自动合并显示。

> ⚠️ **注意**: `/sys/class/devfreq/fdab0000.npu/load` 这个节点在这块板子上
> **恒等于 100**(空闲时也是), 它不是真的占用率。所以网页端只在拿到
> `rknpu` 真值时才画 NPU 那条线, 否则宁可不画 —— 免得一条假的 100% 盖住别的曲线。

---

## 7. 排错

| 现象 | 原因 / 怎么办 |
|---|---|
| 网页上小圆点是红的 | MQTT 没连上。检查 broker 地址/端口; 控制台终端会打印 `[web] MQTT 连接断开: ...` |
| 开关点了没反应 | ① 板子上的 `--mqtt-host` 没开, 或者主题对不上(板子的 `--sn` 和控制台的 `--sn` 要一致); ② 板端日志里没有 `[控制] 收到指令` |
| 开关点完板端日志没反应, 但网页显示"已下发" | 指令发出去了但板子没收到 —— 多半是两边 `--sn` 不一致, 主题对不上 |
| MPP 开关是灰的/点了没反应 | MPP 初始化就没成功(板端日志有 `[警告] MPP 初始化失败`)。控制台会忽略 `mpp=true` |
| 状态报文里 `npu_ms` 是 0 但 FPS 没变 | 正常: 源是 25fps, 去掉推理省下的时间变成【等流】(`wait_ms` 会变大) |
| 关 MPP 后 RGA 也变慢了 | 正常: 软解出来的帧是 CPU 内存, 不是 dma-buf, RGA 没法 import, 只能走 CPU 兜底 |
| 视频源一直 `正在重连…` | 看输入框下面那行"已重连 N 次 · 原因": `No route to host`/`timed out` = **相机不可达**(网段变了/相机没开机); `OpenCV 打不开` = 地址或端口不对; `5 秒没收到画面` = **相机假死**(网线松了/相机重启中), 系统已自动重连 |
| 换了网段后一直连不上 | 路由器换网段后相机 IP 也变了 —— 在页面上填新的 RTSP 地址, 点【测试连接】确认通不通, 通了再点【应用】。地址会存下来, 以后不用再填 |
| 预览框是黑的 | 没装 OpenCV(只能测连通性, 没有画面), 或者相机当前不可达 |

---

## 8. 兼容性说明

- **MQTT 3.1.1**, 不依赖 paho-mqtt。板子和 PC 两端都是零依赖实现。
- 三条独立的 MQTT 连接(结果上行 / 控制订阅 / 状态上报), 互不干扰 ——
  一条连接上收发混用会让 qos=1 的 PUBACK 等待被对端 PUBLISH 插队。
- 板端控制线程**只写三个原子开关**, 真正的切换动作由主线程在下一帧执行 ——
  控制线程不碰解码器/推理上下文, 没有跨线程的数据竞争。
- 网页端只用标准库 + 原生 canvas 画图, **不引任何 CDN**, 没外网也能正常显示。
- **OpenCV 是可选依赖**: 装了才有视频源预览(MJPEG);
  没装的话连通性测试和假死检测照常工作(纯标准库 RTSP 握手),
  现场 PC 上没装 opencv 也不影响这个功能。
