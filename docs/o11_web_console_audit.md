# O11 · Web 控制台 3.0 只读可行性评估

> **文档性质**：**只读调研**。本轮**没有改任何 C++ / 前端代码**，也没有改任何配置。
> **评估对象**：Web 控制台 3.0（`scripts/web_console.py`）与生产服务 `rk_gate`（`src/gate_main.cc`）的衔接可能性。
> **适用版本**：`v2.3-p2a-hardened`（标签，对应 `main@588c563`）
> **成文日期**：2026-11-24
> **对应开放项**：O11（`docs/PROJECT_BRIEFING_20261123.md` §11.3）、P2-2（`rk3588_zero_copy/docs/RISK_REGISTER.md` §五）

---

## 〇、一页结论

| 问题 | 结论 |
|---|---|
| **控制台现在绑在谁身上？** | 绑在**演示链路 `rk_zero_copy`（`src/main.cc`）**上，**不是**生产 `rk_gate`。绑定是**编译期**的：`engine_ctl.cc` / `mqtt_sub.cc` 只进 `rk_zero_copy` 目标（`CMakeLists.txt:233,240`），`rk_gate` 目标里**没有**这两个文件（`CMakeLists.txt:283-298`）。 |
| **生产 `rk_gate` 缺什么？** | 缺 **`down/control` 订阅**与 **`up/status` 上报**两件事。其余(MQTT 上行、结果落盘、硬件负载采样)**都已经在**。 |
| **要接生产，代价多大？** | 只读面板：**≈ 0 行 C++**（复用现成日志与脚本）。三开关全接入：**≈ 200~250 行 `gate_main.cc` + ≈ 30~40 行 `src/rk_base/npu_pool.cc` + 2 行 `CMakeLists.txt`**，且**有一个语义级决策必须先做**（三开关是进程全局的，接进 4 路生产就变成"一刀切 4 路"）。 |
| **风险** | 只读方案**对 `rk_gate` 零风险**（不碰进程）。全接入方案**中风险**：开关切在解码主链路上，4 路同时受影响。 |
| **决策（2026-11-24 规划侧）** | ✅ **采纳方案 A + 方案 C**（纯 Python 只读面板 + 文档标明归属），在**下一张纯 Python 任务卡**中落地；❌ **否决方案 B**（不往生产 `rk_gate` 塞运行时硬件开关）。详见 §3.1。 |
| **O9 残余 12.26%** | 顺带摸底完成：**根因不是跟踪器时基口径**，而是**框→ROI 的几何关系**；而 `SpeedTracker::predict()` 在 `gate_main.cc` 里**根本没有调用点**，所以"改时基"这条路**不会**移动 ROI 顶边。详见 §四。<br>✅ **2026-11-24 决策: 技术项关闭, 不立项改 C++** —— 属"整车框推车牌"在车姿变化下的**固有几何边界**, 号码行识别率不受影响, **保持现网 F8 参数**(`0.30/0.38/0.22/0.126/0`), 仅在文档中如实记录为模型几何边界。归口 `rk3588_zero_copy/docs/RISK_REGISTER.md` R25(已关闭)。 |

---

## 一、问题 1：控制台依赖 `rk_zero_copy` 的哪些具体接口 / 文件 / IPC 协议

### 1.1 板端侧：三条 MQTT 主题 + 一条 TCP 控制通道

控制台与板子之间**只有 MQTT 3.1.1 一种协议**（无 unix socket、无 HTTP、无共享文件）。

| 方向 | 字面主题 | 板端实现位置 | 控制台实现位置 |
|---|---|---|---|
| 网页 → 板（**控制下行**） | `device/<sn>/message/down/control` | `src/main.cc:1698`（默认值）；订阅端 `rk::MqttSubscriber`，`include/mqtt_sub.h` + `src/mqtt_sub.cc`，接线在 `src/main.cc:2463-2523` | `scripts/web_console.py:1975` |
| 板 → 网页（**性能/状态**） | `device/<sn>/message/up/status` | `src/main.cc:1700`；发布端 `rk::StatusPublisher`，`include/engine_ctl.h:85-102` + `src/engine_ctl.cc:296-423`，接线 `src/main.cc:2447-2450` | `scripts/web_console.py:1973` |
| 板 → 网页（**检测结果**） | `device/<sn>/message/up/ivs_result` | `src/main.cc:1693` | `scripts/web_console.py:1974` 附近 |
| 板 → 网页（**硬件负载**） | `device/<sn>/message/up/load` | **不在板端进程里** —— 是外部采样器 `scripts/hw_load_mqtt.py:347`（读内核真值） | `scripts/web_console.py:1974` |

★ **这四条主题里，生产 `rk_gate` 只对得上"结果"那一条的一半**：

| 主题 | `gate_main.cc`（生产） | 说明 |
|---|---|---|
| `device/<sn>/message/up/ivs_result` | ⚠️ **不一样** | 生产是**按通道**的：`"device/" + sn + "/ch" + to_string(index_) + "/message/up/ivs_result"`（`src/gate_main.cc:998-999`），客户端 id `<sn>-ch<N>`（`:997`）。控制台订阅的是**不分通道**的主题。 |
| `device/<sn>/message/up/tripwire` | ✅ **生产独有** | `src/gate_main.cc:1731-1733`。**控制台的契约里没有这条**，所以过线计数在控制台上看不到。 |
| `device/<sn>/message/down/control` | ❌ **不存在** | `gate_main.cc` 的 include 列表（`:71-83`）里**既没有** `engine_ctl.h` 也**没有** `mqtt_sub.h`；全文 grep `MqttSubscriber` / `set_engine` / `down/control` **0 命中**。 |
| `device/<sn>/message/up/status` | ❌ **不存在** | 全文 grep `StatusPublisher` / `PerfSnapshot` **0 命中**。 |

### 1.2 控制指令的报文契约

- 板端解析（`src/main.cc:2489-2503`）：`cmd` 必须是 `set_engine` / `set_engines`，否则**静默忽略**；随后读三个布尔 `mpp` / `rga` / `npu`；在 `src/main.cc:2508-2510` 应用。
- 控制台发出（`scripts/web_console.py:1059-1060`）：`{"cmd":"set_engine","mpp":…,"rga":…,"npu":…}`。
- 状态 JSON 字段（`src/engine_ctl.cc:349-380`）：
  `sn, ts, frames, fps, src_fps, wait_ms, dec_ms, rga_ms, npu_ms, post_ms, draw_ms, total_ms, n_objs, engine{mpp,rga,npu}, decoder, rga_mode, load{npu,npu_core,npu_src,npu_c0..c2,rga,rga_src,vpu,cpu,dmc,gpu}`
  控制台实际消费的是 `engine` / `decoder` / `rga_mode`（`scripts/web_console.py:802-804,814-816`）。

### 1.3 三个开关的语义与执行点

`include/engine_ctl.h:23-27` 定义了**恰好三个**开关：

| 开关 | 语义 | 演示链路里的执行点 | 生产 `rk_gate` 里的对应物 |
|---|---|---|---|
| `mpp` | MPP 硬解 on/off（false = FFmpeg 软解） | `src/main.cc:2804-2820`：翻 `use_mpp_decoder` + `avcodec_flush_buffers(vctx)`（`:2811`）+ `av_bsf_flush(mp4_bsf)`（`:2812`）+ 重新喂 MPP extradata（`:2815`） | `use_mpp_` 是**每通道成员**（`src/gate_main.cc:599`），只在初始化时由 `--no-mpp` 设定（`:1588`）。**运行时切换 = 每通道都要 flush 解码器与 bsf 状态**（对应 `gate_main.cc:867-873,903-913`） |
| `rga` | RGA 硬件缩放 on/off（false = OpenCV CPU） | `src/main.cc:2797-2802` → `rk::rga_set_hw_enabled()`（`src/rga_convert.cc:976`） | 已是**进程全局**（`src/rga_convert.cc:63` 默认 true），`gate_main.cc` 里**没有** `--rga 0\|1` 开关、也没有调用 `rga_set_hw_enabled()` ⇒ **接进去最简单**（约 3 行） |
| `npu` | NPU 推理 on/off（只解码+预处理，不产框） | `src/main.cc:2825-2829` 设局部 `npu_on`，在 `:3156`（推理）与 `:3437`（OCR）处生效 | 推理跑在**共享池 worker**（`src/rk_base/npu_pool.cc`）里，不是通道线程里的局部变量 ⇒ 需要**池级**开关，不是加个局部 flag |

### 1.4 前端：纯 Python 标准库 HTTP 服务

| 项 | 事实 | 出处 |
|---|---|---|
| HTTP 服务 | `http.server.ThreadingHTTPServer`（Python 标准库，零第三方依赖） | `scripts/web_console.py:37,2023,2028` |
| 端口 | `--http-port` 默认 **8080**；绑定默认 `0.0.0.0` | `scripts/web_console.py:1959-1960` |
| Broker | `--host` 默认 `192.168.8.21`、`--port` 默认 `1883` | `scripts/web_console.py:1956-1957` |
| 设备号 | `--sn` 默认 `rk3588-01`（**决定主题**） | `scripts/web_console.py:1958` |
| 静态资源 | **磁盘上一个都没有** —— 整个 UI 是 `web_console.py:1080-1947` 里的一段内嵌 HTML 字符串（`PAGE = r"""…"""`），由 `:868-871` 直接吐。全仓没有任何该控制台的 `.js` / `.css` 文件 |
| 路由（GET） | `/`、`/index.html`（`:868`）、`/api/state`（`:872`）、`/api/rtsp`（`:901`）、`/api/video.mjpeg`（`:911`）、`/api/video/snapshot.jpg`（`:913`）、`/api/health`（`:919`）、`/favicon.ico`（`:921`） |
| 路由（POST） | `/api/rtsp`、`/api/rtsp/test`（`:1003`）、`/api/stress`（`:1006`）、`/api/control`（`:1030`） |
| 轮询 | 浏览器 → `/api/state` **每 1000 ms** 一次（`setTimeout(tick, 1000)`，`:1940`）。板端 → 网页是**推**（MQTT），状态主题默认 1 秒一次（`src/engine_ctl.cc:300,322,336`） |
| 读本地文件 | **只读一个**：`logs/console_stack/rtsp.json`（RTSP 地址簿，`:671-672` 载入、`:687` 保存）。**完全不读 `/var/log.hdd`** |
| 本地探测 | 探的是**网络**不是本机：TCP connect + RTSP `OPTIONS`（`probe_rtsp`，`:288-330`），OpenCV 可选（`HAVE_CV2`，`:42-46`） |

### 1.5 一键栈起什么

`scripts/start_console_stack.sh:55` —— `BIN="$PROJ/build/rk_zero_copy"`。
**它启动的是演示二进制，永远不是 `build/rk_gate`**。
（`scripts/stress_agent.py:189` 同理。）

---

## 二、问题 2：要让控制台只读监控 / 全功能联动生产 `rk_gate`，要改哪些文件、量级多大、风险多大

### 2.1 方案 A：只读监控（**不碰 `rk_gate` 一行代码**）

**思路**：控制台不再问"演示进程"，而是去读**生产服务已经在写的东西**。

生产 `rk_gate` 现成产出的东西：

| 可读的东西 | 位置 | 能拿到什么 |
|---|---|---|
| 业务日志 | `/var/log.hdd/rk-gate-error.log` | 每路帧号 `[chN][frame N]`、各阶段耗时、`objs=`、车牌号、RSS/活堆 |
| stdout | `/var/log.hdd/rk-gate.log` | 启动参数、预检结果 |
| 看门狗告警 | `/var/log.hdd/rk-gate-alerts.log` | 卡死/重启/水位告警 |
| 预检结果 | `/var/log.hdd/rk-gate-preflight.log` | 12 项预检逐项结果 |
| 体检结论 | `scripts/gate_health.sh` | 直接给"正常/可疑/卡死" + 帧号 + 水位 |
| 硬件负载 | `scripts/hw_load_mqtt.py` | NPU/RGA 内核真值（**已在发 `device/<sn>/message/up/load`**，与演示链路同主题 ⇒ **控制台不用改就能收**） |
| 检测结果 | MQTT `device/<sn>/ch<N>/message/up/ivs_result` | 生产的**分通道**结果 |

**改动量**：

| 文件 | 改动 | 量级 |
|---|---|---|
| `scripts/web_console.py` | 加一个"生产模式"：订阅 `device/<sn>/ch<N>/message/up/ivs_result`（分通道）、加一个 `/api/gate_state` 读日志尾 + 调 `gate_health.sh`、把帧号/水位画到已有的画布上 | **≈ 150~250 行 Python**，**0 行 C++** |
| `scripts/web_console.py`（前端） | 复用现有 canvas 画图逻辑，加一个"生产/演示"切换 | ≈ 30~50 行 |

**风险**：**对 `rk_gate` 零风险** —— 不改生产代码、不重启、不加线程。
唯一要小心的是**别把日志读成 IO 瓶颈**：`gate_health.sh` 里已经有教训
（"采样器每次都 grep 整个日志，实测 7 分钟就变成 1.9 秒/次"，`SOAK_REPORT_20261118.md`），
所以只读**日志尾部 8MB**。

### 2.2 方案 B：把 `engine_ctl.cc` 三开关完整接入 `gate_main.cc`

**思路**：让生产 `rk_gate` 也订阅 `down/control`、也发 `up/status`，从而控制台能**真正操作生产服务**。

**A. 可以原样复制的部分（零改动）**

| 文件 | 行数 | 说明 |
|---|---|---|
| `include/engine_ctl.h` + `src/engine_ctl.cc` | 127 + 522 | 三开关原子量 + `PerfSnapshot` + `StatusPublisher` + `read_hw_load()` + mini-JSON |
| `include/mqtt_sub.h` + `src/mqtt_sub.cc` | ≈ 67 + ≈ 400 | MQTT 3.1.1 订阅端 |

> ★ **但"零改动复制"有个前提**：`engine_ctl.cc` 读硬件负载时会碰 `/sys/kernel/debug/rknpu/load`
> （需 root）与 `/sys/class/devfreq/.../load`（本板**恒显 100%，不可用**）。
> 生产服务以 `seeed` 身份跑，读 debugfs 会 **EACCES** —— 需要给 `rk-gate.service` 加
> `SupplementaryGroups` 或改用 `hw_load_mqtt.py` 那个**独立的 root 采样器**（**推荐**，生产链路上它已经在发同一条 `up/load` 主题）。

**B. `gate_main.cc` 里要新写的部分（对照 `src/main.cc` 的镜像）**

| # | 内容 | 镜像位置 | 新增 LOC |
|---|---|---|---|
| 1 | 2 个 include | `src/main.cc:80,85` | **2** |
| 2 | 4 个 CLI 选项 + 帮助文本（`--mqtt-ctl-topic` / `--mqtt-status-topic` / `--no-mqtt-ctl` / `--status-interval-ms`） | `src/main.cc:1624-1625` 等 | **≈ 12** |
| 3 | 主题默认值由 `--sn` 拼出 | `src/main.cc:1698,1700` | **≈ 6** |
| 4 | `StatusPublisher` + 控制线程 | `src/main.cc:2434-2528` | **≈ 85** |
| 5 | `PerfSnapshot` 填充 + `status_pub.update()` | `src/main.cc:3996-4013` | **≈ 18** |
| 6 | 三开关在通道循环里落地 | `src/main.cc:2786-2830` | **≈ 35** |
| | **小计** | | **≈ 160** |

**C. 复制不了、必须新写的部分**

| # | 内容 | 原因 | 新增 LOC |
|---|---|---|---|
| 7 | **每通道的 MPP 开关** | `use_mpp_` 是**每通道成员**（`gate_main.cc:599`），不是全局。切过去要 flush `avcodec` + `av_bsf` + 重喂 extradata，**每通道各来一遍** | **≈ 20~30** |
| 8 | **池级 NPU 开关** | 推理在**共享池 worker**（`src/rk_base/npu_pool.cc`）里跑，不是通道线程的局部变量 ⇒ 要在池里加闸门 | **≈ 20~40** |
| 9 | `--rga 0\|1` 开关 | `gate_main.cc` 现在**没有**这个开关；`rga_set_hw_enabled()` 是进程全局的，很好接 | **≈ 3** |
| 10 | `CMakeLists.txt` 加两个源文件 | `rk_gate` 目标（`:283-298`） | **2** |

> **量级汇总**：
> **≈ 200~250 行 `gate_main.cc` + ≈ 30~40 行 `src/rk_base/npu_pool.cc` + 2 行 `CMakeLists.txt`**
> ＋ 复制 `engine_ctl` / `mqtt_sub` 两个模块（合计 ≈ 1100 行，**逐字复制，不改逻辑**）。

**D. ⚠️ 必须先做的语义决策（不是 LOC 问题）**

`engine_ctl.cc` 的三个开关是**进程全局**的（`src/engine_ctl.cc:29-31`：`g_mpp` / `g_rga` / `g_npu` 三个 `std::atomic<bool>`）。
演示链路 `rk_zero_copy` 是**单流**的 —— 全局 == 单路，没问题。
生产 `rk_gate` 是 **4 路独立通道** ⇒ 一次开关会**同时影响 4 路**。

这不是 bug，是**语义变更**：控制台上按一下"关 NPU"，生产会 4 路一起停。
必须明确：
- (a) 接受"全局一刀切"，在 UI 上写清楚；还是
- (b) 把开关改成**按通道**（`engine_ctl` 要从全局原子量改成 per-channel 数组，工作量再 +50~80 行，且控制报文要带通道号）。

### 2.3 风险评估

| 风险 | 等级 | 说明 | 缓解 |
|---|---|---|---|
| **开关切在解码主链路上** | 🔴 中 | `mpp` 开关要 flush 解码器 + bsf（`src/main.cc:2811-2815`）。在**生产 4 路**上做这件事，一旦 flush 后状态没恢复对，就是该路永久黑屏 | ① 先在 `rk_zero_copy` 单流上验证 flush 逻辑; ② 生产侧给开关加"切完 N 秒内帧号不涨就自动切回"的保险; ③ 开关落地走 `up/status` 回报实际生效状态 |
| **NPU 开关加在共享池** | 🟠 中高 | 池是 4 路共用的。加闸门的位置如果选错（worker 取任务前 vs 后），会出现"某路卡在等结果" | 闸门必须放在**取任务之前**，让任务留在队列里而不是被丢弃 |
| **一次开关 4 路全停** | 🟠 中高 | 见上面 D | 改成 per-channel，或 UI 明确警示 |
| **控制线程是新的失败点** | 🟡 低中 | 新增一个常驻线程 + 一条 MQTT 长连接。MQTT 断连/重连逻辑若不健壮，可能拖垮主进程 | `mqtt_sub.cc` 是零依赖实现，**先在演示链路上跑够时长**（≥ 3 小时长跑）再上生产 |
| **状态上报的 IO 占用** | 🟡 低 | 每秒一发 JSON | 已有令牌桶；`up/status` 与 `up/load` 是两条主题，别合并成一条大包 |
| **`read_hw_load()` 权限** | 🟡 低 | 读 debugfs 需 root | 生产**不要**在 `rk_gate` 里读；用已有的 `hw_load_mqtt.py` |
| **回归测试缺口** | 🟠 中 | `rk_gate` 是 4 路 + 共享池 + 过线计数 + 优雅关闭(P2-A2) | 改完必须重跑：300 帧冒烟 + DJI 10bit 单路 + SIGTERM 优雅退出 + **16 项全量交付验收** + 一次长跑 |
| **对 `rk_gate` 稳定性的净影响** | — | **方案 A: 0**。**方案 B: 中** —— 新增 1 个线程 + 1 条 MQTT 连接 + 3 个可在运行时改变主链路行为的开关 | 见上 |

---

## 三、问题 3：三个方案的对比

| 维度 | **方案 A：只读面板** | **方案 B：三开关完整接入** | **方案 C：不适配，只标明归属** |
|---|---|---|---|
| **做什么** | 控制台读 `/var/log.hdd` + `gate_health.sh` + 订阅生产的分通道 `ivs_result`，画只读面板 | 生产 `rk_gate` 也订阅 `down/control`、也发 `up/status`，三开关可在网页上实时切 | **不动代码**，在 `WEB_CONSOLE.md` / `RISK_REGISTER.md` 里写清"控制台属 `rk_zero_copy` 演示链路" |
| **改动量** | ≈ 180~300 行 Python，**0 行 C++** | ≈ 200~250 行 `gate_main.cc` + 30~40 行 `npu_pool.cc` + 2 行 `CMakeLists.txt` + 复制 ≈1100 行 `engine_ctl`/`mqtt_sub` | **0 行** |
| **对 `rk_gate` 稳定性风险** | **零**（不碰进程、不重启） | **中**（新线程 + 新连接 + 运行时可改主链路的开关 + 全局/按通道的语义决策） | **零** |
| **能不能看到生产真实状态** | ✅ 能（帧号、耗时、水位、告警、结果、负载） | ✅ 能，且比 A 更实时（`up/status` 是进程内直接报，不依赖解析日志） | ❌ 不能（控制台仍只能看演示链路） |
| **能不能在网页上操作生产服务** | ❌ 不能 | ✅ 能 | ❌ 不能 |
| **交付周期** | **短**（纯 Python，无板端重编，不需重跑 NPU 验收） | **长**（要改 C++、重编、重跑全部验收 + 长跑） | **最短**（只改文档） |
| **解决了 O11 的痛点吗** | ✅ 解决了 90% —— "看不到生产服务状态" | ✅ 全解决 | ❌ 没解决，只是把问题写清楚 |
| **主要缺点** | 状态是**解析日志**得来的，比进程内直接报**延迟一点、字段少一点**；帧号/耗时需从日志文本里抽（已有先例：`SOAK_REPORT` 里的采样器就是这么做的） | 风险与工作量都不小；且**三开关的"全局 vs 按通道"语义决策必须先定** | 现场仍然拿不到生产状态 |

### 3.1 规划侧决策（2026-11-24 已拍板）

> ✅ **决策**：**否决方案 B**，**采纳方案 A + 方案 C**，在**下一张纯 Python 任务卡**中落地。
>
> | 项 | 决策 |
> |---|---|
> | **方案 A（纯 Python 只读生产监控面板）** | ✅ **采纳** —— 下一张纯 Python 任务卡落地 |
> | **方案 B（把 `engine_ctl.cc` 三开关接入 `gate_gate`）** | ❌ **否决** —— 不往生产 `rk_gate` 塞运行时硬件开关，**不改任何生产 C++** |
> | **方案 C（文档标明控制台属演示链路）** | ✅ **采纳** —— 随方案 A 一并在下一张任务卡里写进 `WEB_CONSOLE.md` / `RISK_REGISTER.md` |
>
> **否决方案 B 的理由（本评估给出的依据）**：① 三个开关是**进程全局**的，接进 4 路生产就是"一刀切 4 路"，
> 语义变更需要额外决策；② MPP 开关要 flush 解码器 + bsf，是**解码主链路**上的运行时变更；
> ③ NPU 开关要加在**共享池 worker** 里，不是加个局部 flag；
> ④ 需要重跑 300 帧冒烟 + DJI 10bit 单路 + SIGTERM 优雅退出 + 16 项全量验收 + 一次长跑。
> 而"网页上切生产硬件开关"目前**没有明确的现场需求**，方案 A 能以 0 行 C++ 解决
> **真正痛点**（"看不到生产服务状态"）。
>
> **落地时的两条纪律**（见 `rk3588_zero_copy/docs/RISK_REGISTER.md` P2-2）：
> ① **IO 纪律** —— 只读日志**尾部 8MB**（`tail -c 8M`），不要 grep 整个文件
> （`SOAK_REPORT_20261118.md` 已踩过：采样器 7 分钟变成 1.9 秒/次）；
> ② **保持"只读"边界** —— 不要把生产的 `rk_gate` 接到 `down/control` 上。

### 3.2 当时的推荐（已被上节决策取代，保留备查）

**建议：先做方案 A，同时把方案 C 的口径先写进文档（成本几乎为零）。**

理由：

1. **方案 A 解决的是真痛点**。O11 的原始诉求是"Web 控制台看不到生产的 `rk_gate`"，
   而**看**比**切**重要得多 —— 现场排障要的是"现在到底卡在哪一路"。
2. **方案 A 零风险、零板端重编**。不用动 `src/**`（本轮的硬红线），不用重跑 NPU 验收。
3. **方案 B 的"切"目前没有明确需求**。谁会在现场网页上关掉生产的 NPU？
   而它的风险与工作量都不小，还有"一刀切 4 路"的语义问题。
4. **方案 C 作为兜底**：无论 A/B 做不做，`WEB_CONSOLE.md` 都该把"控制台绑定的是演示链路"
   这件事写清楚 —— 现在它没写，是 O11 这个开放项能存在这么久的直接原因。

### 3.3 方案 A 的具体落地清单（供下一张纯 Python 任务卡参考）

| # | 改动 | 文件 | 量级 |
|---|---|---|---|
| 1 | 新增"生产模式"订阅分通道结果 `device/<sn>/ch<N>/message/up/ivs_result` | `scripts/web_console.py` | ≈ 60 行 |
| 2 | 新增 `/api/gate_state`：读 `/var/log.hdd/rk-gate-error.log` **尾部 8MB** 抽各路最后帧号 / 耗时 / `objs=` | `scripts/web_console.py` | ≈ 90 行 |
| 3 | 新增 `/api/gate_health`：调 `scripts/gate_health.sh`，透传它的"正常/可疑/卡死"结论 | `scripts/web_console.py` | ≈ 30 行 |
| 4 | 前端加"生产/演示"切换 + 帧号/水位曲线（复用现有 canvas） | `scripts/web_console.py` | ≈ 60 行 |
| 5 | 在 `docs/WEB_CONSOLE.md` 写清控制台的适用边界（= 方案 C 的口径） | `rk3588_zero_copy/docs/WEB_CONSOLE.md` | ≈ 30 行 |
| 6 | 在 `RISK_REGISTER.md` 把 P2-2 标注为"已按方案 A 落地 / 方案 B 待需求" | `rk3588_zero_copy/docs/RISK_REGISTER.md` | ≈ 10 行 |

> ★ **注意 IO 纪律**：一定要**只读日志尾部**（`tail -c 8M`），
> 不要 `grep` 整个文件 —— `SOAK_REPORT_20261118.md` 里已经踩过这个坑
> （"长跑时日志到几百 MB，采样器自己就被拖死，实测 7 分钟就变成 1.9 秒/次"）。

---

## 四、问题 4（顺手摸底）：O9 残余 12.26% 的确切代码行与最小修复思路

> ★ **本轮只读不改。** 以下每一条都带 `file:line`，可直接核对。

### 4.1 先把 12.26% 这个数说清楚

| 项 | 事实 | 出处 |
|---|---|---|
| 数值 | **12.26%** = **13/106** 帧 | `opt_ebike_fork9/docs/05_roi_calibration_eval.md:28`（"ROI 顶边切进省份字行 **12.26%**"，同格"省份行整行被框住 **66.98%**"） |
| 指标定义 | `o9_top_cut`：ROI 顶边**低于**省份字行顶边（`gy + 0.149*gh`）即计数 | `opt_ebike_fork9/tools/eval_plate_recognition.py:24,115-116` |
| 分母 | `n_frames` = **产生了裁图**的帧数 = 106 | `opt_ebike_fork9/tools/eval_plate_recognition.py:83,96,161` |
| 样本 | 客户路口实拍的两行绿牌 `苏N / A339058` | `opt_ebike_fork9/docs/05_roi_calibration_eval.md:6` |
| ★ **关键性质** | 它在 E6 / F4 / F6 / **F8(推荐)** / F9 五组**不同 ROI 参数**下**全都是 12.26%** | `opt_ebike_fork9/docs/05_roi_calibration_eval.md:92-96` |

最后一条很重要：**12.26% 不随 `roi_y` 变化** ⇒ 它**不是**"ROI 参数没调好"造成的，
而是**一组"框→牌的相对位置本来就对不上"的帧**，任何单一 `roi_y` 都救不了。

### 4.2 跟踪器时基的确切代码行（`context.md:136-138` 说的"根因"）

**生产 `rk_gate`：**

| 关注点 | 位置 | 代码 | 性质 |
|---|---|---|---|
| 时间戳的产生 | `src/gate_main.cc:1136-1138` | `dt.ts_ms = static_cast<int64_t>(duration<double,milli>(steady_clock::now().time_since_epoch()).count());` | ★ 这是**提交时刻**（解码+RGA 已完成、推理尚未开始），**不是采集时刻** |
| 注释声称的语义 | `include/rk/npu_pool.h:59-61` | "ts_ms : 这一帧【被采集到】的时刻(单调时钟毫秒) … 跟踪/测速必须用它自己的时间戳, 不能用『现在』 — 否则 dt 算错" | ★ **实现与注释不符** |
| 传播 | `src/rk_base/npu_pool.cc:474` | `r.ts_ms = task.det.ts_ms;` | |
| `t_now` | `src/gate_main.cc:1157-1158` | `const double t_now = duration<double>(steady_clock::now().time_since_epoch()).count();` | 这是**推理之后**的时刻，**算出来了但没给跟踪器用** |
| **`t_track`** | `src/gate_main.cc:1158` | `const double t_track = static_cast<double>(dr.ts_ms) * 0.001;` | ★ **两条链路的时基分叉点就在这一行** |
| 跟踪器调用 | `src/gate_main.cc:1170` | `tracker_.update(track_boxes, t_track, speed_cfg, calib_, &tracks);` | |

**演示链路 `rk_zero_copy`（单流）：**

| 关注点 | 位置 | 代码 |
|---|---|---|
| 提交时刻戳 | `src/main.cc:3184-3187` | `job.ts_ms = …steady_clock::now()…`（注释写"这一帧的采集时刻"） |
| **处理时刻** | `src/main.cc:3380-3381` | `const double t_now = …steady_clock::now()…`（**在推理块之后**） |
| 选用哪个 | `src/main.cc:3410-3413` | `t_track = (pipe_on && pipe_has_result) ? pipe_ts_ms*0.001 : t_now;` → `tracker.update(track_boxes, t_track, …)` |

⇒ **时基确实不一致**（生产用"提交时刻"，演示单流用"处理时刻"），`context.md` 的描述属实。

### 4.3 ⚠️ 但"改时基"**不会**解决 12.26% —— 三个硬证据

**证据 1：ROI 用的是【检测框】，不是【预测框】**

```
src/speed_track.cc:389   tr.box = dets[i];      // 框始终是当前这一帧的检测结果
src/speed_track.cc:436   r.box  = tr.box;
src/gate_main.cc:1214    rk::plate_roi_from_box(tr.box, nv.width, nv.height, opt_.roi_ratio, …)
```
`speed_track.cc` / `.h` 里**没有任何框平滑或 EMA**。ROI 直接由**原始检测框**算出。

**证据 2：`SpeedTracker::predict()` 在生产链路里【根本没有调用点】**

```
include/speed_track.h:177   void predict(double t);      // 声明
src/speed_track.cc:195       void SpeedTracker::predict(double t) { … }   // 实现
src/main.cc:3406             tracker.predict(t_now);      // ← 唯一的调用点, 在【演示单流】里
```
`src/gate_main.cc` 全文 **grep `predict(` 0 命中** —— 只有 `tracker_.set_predict_enabled(true)`（`:554`），
设了开关却从不调用。
⇒ **生产链路根本没有"运动预测"这一步**，而文档里"dt 不同 → 预测框差几像素 → ROI 差 6px"
这条因果链的**中间那一步在当前代码里不存在**。

**证据 3：12.26% 是在【离线、原始检测框、无跟踪器】的口径下测出来的**

- 数据来源是**原始检测框**的 CSV：`opt_ebike_fork9/out/boxes_ebike_real_fork9.csv`
  （`opt_ebike_fork9/tools/make_plate_crops.py:63,87-92`）。
- 文档自己写明："框的来源 | **原始检测框(离线)** | 跟踪器平滑/预测后的框(生产)"
  （`opt_ebike_fork9/docs/05_roi_calibration_eval.md:147`）。
⇒ **跟踪器时基在这次测量里根本没有参与**，所以它**不可能**是 12.26% 的成因。

**证据 4：两条链路的 ROI 参数差得比"几像素"多得多**

| 参数 | 演示默认 | 生产默认 | 生产现网(`gate.env:96-100`) |
|---|---|---|---|
| `roi_ratio` | 0.72 | 0.164 | **0.30** |
| `roi_y` | 0.70 | 1.0 | **0.38** |
| `roi_height` | 0.16 | 0.40 | **0.22** |
| `roi_shift` | 0.00 | 0.126 | **0.126** |
| `roi_below` | 0.05 | 0.0 | **0** |

（演示 `src/main.cc:465-469`；生产默认 `src/gate_main.cc:233-234`；现网 `config/gate.env:96-100`）
ROI 公式本身两条链路**逐字相同**（`include/rk/plate_roi.h:93-123` ≡ `src/main.cc:1119-1149`），
顶边是 `top = b.y1 + bh*roi_y + bh*below`（`include/rk/plate_roi.h:108`）。
⇒ 相对框的几何参数一变，ROI 移动的量级**远大于**任何 dt 差异能造成的位移。

### 4.4 那 12.26% 到底是什么

文档给的解释是自洽的：

> 13 帧的 GT 牌宽 **192~380 px**（全部 217 帧牌宽中位 200 px）⇒ **不是"远处小牌", 是中等距离的牌**；
> 原因是**框→牌的相对位置随车姿/距离变化**，单一 `roi_y` 覆盖不了；
> 影响是**省份字读不出但号码仍读得出**，所以整牌命中率不受影响。
> —— `opt_ebike_fork9/docs/05_roi_calibration_eval.md:113-120`

⇒ **根因 = 框→ROI 的几何关系**（框是"车/人的框"，牌在框里的位置随距离与姿态漂移），
**不是**跟踪器时基。

### 4.5 最小修复思路（Wave 2 用，本轮不改）

| 方案 | 做法 | 改动量 | 风险 | 预期 |
|---|---|---|---|---|
| **B-1（一行对齐）** | `src/gate_main.cc:1158` 的 `t_track` 改用已经算好的 `t_now`（`:1157-1158`），与演示单流口径一致 | **1 行** | 极低 | ⚠️ **只消除"时基不一致"这个事实**, **不会**移动 ROI 顶边 ⇒ **不解决 12.26%**。但它是**该做**的 —— 它消掉了一个真实存在、且被文档反复引用的不一致 |
| **B-2（真正的方向）** | 改**框→ROI 的几何**: ① 按帧**自适应** `roi_y` / `roi_below`(比如按框高在某个区间内插值); ② 或者改用**牌类检测框**而不是"车框"来裁 ROI(模型有牌类时); ③ 或者在 `SpeedTracker::predict()` 真正接入生产后, 用**预测/平滑后的框**算 ROI(注意当前 `predict()` 未接入, 且 `tr.box` 无平滑) | 中（数十行 + 需要重跑 217 帧离线评估） | 中(动的是**几何**, 会影响识别准确率, 必须重跑精度回归) | 这才是能真正把 12.26% 压下去的方向 |
| **B-3（捕获）** | 把 `predict()` 接入生产时, 同时把 ROI 的框来源显式化(记录"用检测框还是跟踪框"), 避免以后再出现"文档说的根因和代码实际走的路径不是一回事" | 小 | 低 | 提高可观测性 |

> ★ **给 Wave 2 的建议顺序**：**B-1 先做**(1 行, 消掉一个真实不一致, 风险几乎为零)→
> **B-2 用离线工具先验证**(在 `opt_ebike_fork9/tools/eval_plate_recognition.py` 上扫参数,
> 目标是把 12.26% 压到 0, 且**整牌准确率不能掉**)→ 确认有收益再动生产代码。

---

## 五、附：本次评估的核对清单（全部已在本机核对过）

| # | 核对项 | 结果 |
|---|---|---|
| 1 | `main.cc` 的三条主题字面值 | ✅ `src/main.cc:1693,1698,1700` |
| 2 | `gate_main.cc` 的主题字面值 | ✅ `src/gate_main.cc:998-999`（分通道 `ivs_result`）、`:1732`（`tripwire`） |
| 3 | `gate_main.cc` 里**没有** `down/control` 订阅、**没有** `up/status` 上报 | ✅ grep 0 命中；include 列表 `:71-83` 也证实 |
| 4 | `engine_ctl.cc` / `mqtt_sub.cc` 只进 `rk_zero_copy` 目标 | ✅ `CMakeLists.txt:233,240` vs `rk_gate` 目标 `:283-298`（只列了 `src/mqtt_pub.cc`） |
| 5 | 三个开关的语义与执行点 | ✅ `include/engine_ctl.h:23-27`；`src/main.cc:2797-2829` |
| 6 | `rk_gate` 侧 MPP 是**每通道**的、RGA 是**全局**的、NPU 跑在**共享池**里 | ✅ `src/gate_main.cc:599` / `src/rga_convert.cc:63` / `src/rk_base/npu_pool.cc` |
| 7 | 控制台的 HTTP 端口 / 路由 / 轮询间隔 / 静态资源 | ✅ 见 §1.4 |
| 8 | 控制台**不读** `/var/log.hdd` | ✅ 全仓 grep `/var/log.hdd` 不命中 `web_console.py` |
| 9 | `start_console_stack.sh` 起的是 `rk_zero_copy` | ✅ `:55` |
| 10 | O9 跟踪器时基的确切代码行 | ✅ 见 §4.2 |
| 11 | `SpeedTracker::predict()` 在 `gate_main.cc` **无调用点** | ✅ grep 0 命中（唯一调用点 `src/main.cc:3406`） |
| 12 | ROI 来自**检测框**（`tr.box = dets[i]`），无平滑 | ✅ `src/speed_track.cc:389,436`；`src/gate_main.cc:1214` |
| 13 | 12.26% = 13/106，离线原始检测框口径 | ✅ `opt_ebike_fork9/docs/05_roi_calibration_eval.md:28,90-120,147`；`tools/eval_plate_recognition.py:115-116` |
| 14 | 12.26% 不随 ROI 参数变化 | ✅ `opt_ebike_fork9/docs/05_roi_calibration_eval.md:92-96`（E6/F4/F6/F8/F9 全是 12.26%） |

---

## 六、修订记录

| 日期 | 版本 | 变更 |
|---|---|---|
| 2026-11-24 | v1.0 | 初版。**只读评估**，未改任何 C++ / 前端 / 配置。适用 `v2.3-p2a-hardened`（`main@588c563`）。 |
| 2026-11-24 | v1.1 | 规划侧**拍板**: 否决方案 B, 采纳方案 A + C, 转入下一张纯 Python 任务卡(§3.1 新增决策节)。同时记录 O9 技术项关闭的决定(§四)。**本轮仍未改任何 C++ / 前端 / 配置。** |
| 2026-11-25 | v1.2 | **方案 A + C 已落地**(分支 `feat/o11-web-console-readonly`): 新增 `/api/gate_state` + `/api/gate_health`, 页面加【生产监控 / 演示调试】视图切换。**纯 Python, 0 行 C++ 改动**, 线上 `rk_gate` 零侵入。落地清单见 §3.3, 闭环记录见 `rk3588_zero_copy/docs/RISK_REGISTER.md` P2-2。 |
