# PRODUCTION_GUARD.md —— RK3588 大门系统的生产守护规范

> 适用范围：`rk_gate`（单进程 4 路大门推理），生产现场无人值守 7×24。
> 设计目标：**让它一直跑下去**，以及**崩了能自己回来**。
> 性能指标（80 FPS KPI）不在本文范围，见《RK3588 边缘 AI 系统交付白皮书》。

---

## 1. 守护架构

```text
multi-user.target
├── rk-gate.service            主服务: 4 路推理
│   ├── ExecStartPre = preflight_gate.sh     环境断言, 不过就拒绝启动
│   └── ExecStart     = run_gate_service.sh   exec 启动 rk_gate(flock 互斥)
├── rk-gate-watchdog.service   看门狗: 卡死自愈 / failed 自愈 / 水位告警
└── rk-collect-crash.service   崩溃取证: 由主服务的 OnFailure= 触发
```

配套文件：

| 文件 | 作用 |
|---|---|
| `config/gate.env` | 生产配置（唯一需要现场改的文件） |
| `scripts/preflight_gate.sh` | 启动前 12 项环境断言 |
| `scripts/run_gate_service.sh` | 启动包装（exec + flock + 托管标记检查） |
| `scripts/gate_watchdog.sh` | 看门狗（四条判据，见第 4 节） |
| `scripts/gate_health.sh` | **一条命令体检**：正常/可疑/卡死（只读，无副作用） |
| `scripts/install_gate_service.sh` | 安装 / 卸载（`--uninstall`） |
| `scripts/systemd/*.service` | 三个 unit 的源文件 |
| `scripts/logrotate/rk-gate` | 日志轮转（**必须 copytruncate**） |

> 现场日常运维看 **`docs/现场运维手册.md`**（极简版）；本文是完整规范。

---

## 2. 安装与卸载

```bash
cd /home/seeed/rk_deploy/rk3588_zero_copy

sudo scripts/install_gate_service.sh              # 安装 + 启动 + 开机自启
sudo scripts/install_gate_service.sh --no-start   # 只安装不启动
sudo scripts/install_gate_service.sh --uninstall  # 卸载(彻底清理)
sudo scripts/install_gate_service.sh --uninstall --purge-logs   # 连日志一起删
```

**安装做的事**（全部幂等，可反复跑）：

1. 预建 6 个日志文件并把属主设成服务用户（`seeed:0640`）
2. 拷 3 个 unit 到 `/etc/systemd/system/`
3. 拷 logrotate 规则到 `/etc/logrotate.d/rk-gate`
4. 建 `/etc/rk-gate/systemd_managed` 标记 → 禁止手动再起一个实例
5. 建 `/var/run/rk-gate.lock`（0666）→ flock 互斥锁
6. `daemon-reload` + `enable` + `restart`

**卸载做的事**（**不留残留**）：停服务并取消自启 → 删 3 个 unit → 删 logrotate 规则
→ 删 `/etc/rk-gate/`（含托管标记）→ 删锁文件与 `/run/rk-gate/` → `reset-failed` +
`daemon-reload`。**日志默认保留**（是证据，要删用 `--purge-logs`）。

> 装完 service 是 `enabled` 的 → **上电自启**，不用再做任何事。

---

## 3. 配置

全部配置在 `config/gate.env`，改完执行 `sudo systemctl restart rk-gate`。

| 变量 | 说明 | 默认值 |
|---|---|---|
| `GATE_MODEL` / `GATE_LABELS` | 检测模型 / 标签 | `models/ebike_int8_rk2332.rknn` / `labels_ebike.txt` |
| `GATE_OCR` / `GATE_OCR_DICT` | 车牌识别模型 / 字典 | `models/ppocrv4_rec_fp16.rknn` / `ppocr_keys_v1.txt` |
| `GATE_CH1..CH4` | 4 路视频源 | 仓库现有 MP4 素材 |
| `GATE_CH*_KIND` | `near`(近景/车牌) 或 `high`(高位/测速) | 见文件 |
| `GATE_CH2_IPM` / `GATE_CH4_IPM` | 测速标定点 `"x1,y1;x2,y2;x3,y3;x4,y4"` | 见文件 |
| `GATE_DET_CORES` / `GATE_OCR_CORES` | 核绑定 | `1,2` / `0` |
| `GATE_DET_WORKERS` / `GATE_OCR_WORKERS` | worker 数 | `3` / `3` |
| `GATE_MAX_FPS` | 每路分析帧率上限 | `20` |
| `GATE_MQTT_HOST` | broker，留空 = 不外发 | 空 |
| `GATE_TRIPWIRE_CH1..CH4` | 过线虚拟线，留空 = 不启用 | 空 |
| `GATE_LOG_DIR` | 日志目录 | `/var/log` |
| `GATE_LOG_LEVEL` | 应用日志级别 → `RK_LOG_LEVEL` | `warn` |
| `GATE_SERVICE_USER` | 服务运行用户 | `seeed` |

**现场换相机**：只改 `GATE_CH1..CH4` 的值成 `rtsp://IP:PORT/path`，
`GATE_CH*_KIND` 选 near/high，high 通道必须给 IPM 标定点（分辨率变了要重标）。

> `models/` 是只读真源，配置里只**引用**不拷贝。

---

## 4. 看门狗的判据（重点）

看门狗每 **20 秒**跑一轮，依次判定四件事。**它只在这四种情况下动手，其余一律只记不碰。**

### 4.1 L1 —— 整机心跳（日志 mtime 停滞）

| 项 | 值 |
|---|---|
| 判据 | `max(mtime(stdout日志), mtime(stderr日志))` 距今 > **60 秒** |
| 为什么用 mtime | `rk_gate` 的统计行是 `[ch1][frame 100] fps=… rss=… heap=…`，**行内没有时间戳**（加时间戳要改业务代码，本方案坚持不改）。mtime 就是"最后一行被写下的时刻" |
| 为什么取两个文件的较大值 | 业务输出全走 stderr，stdout 只有包装脚本的 echo；只看 stdout 会误判 |
| 阈值依据 | 4 路每 100 帧打一行统计 → 约 5 秒一行；60 秒留了 10 倍余量 |
| 前置条件 | **必须先确认 MainPID 还活着**。进程死了是 systemd 的活，看门狗不插手，避免两边打架 |
| 动作 | `systemctl restart rk-gate` |

### 4.2 L2 —— 单路心跳（帧号是否还在推进）

| 项 | 值 |
|---|---|
| 判据 | 解析日志里的 `[chN][frame M]`，某路帧号停滞 > **180 秒** |
| 为什么需要 L2 | L1 只发现"**整机**不出日志了"。若 4 路里卡死 1 路、另外 3 路还在跑，日志照样在刷 —— L1 看不出来 |
| 为什么不用改 C++ | 帧号本来就在日志行里 |
| 帧号回退 | 帧号变小 = 进程重启过 → 重新计时，不误判 |
| **区分"卡死"和"变慢"** | 先数**还有几路在推进**：<br>• **≥2 路在推进** → 整机是活的，这路只是**变慢/源断流** → **只告警**，连续 **3 轮**（≥9 分钟）仍不恢复才重启<br>• **其它路也停了** → 整机性问题 → 直接重启 |
| **重启冷却窗口** | 检测到 **MainPID 变了**（不管是被谁重启的）→ 跳过 L2 判定 **60 秒** |
| 动作 | `systemctl restart rk-gate` |

> **为什么要区分"卡死"和"变慢"**：2026-11-18 实测踩过 —— 板子上有别的任务把 NPU 压到 96%，
> ch4 从 20fps 掉到 1~2fps，100 帧要 50~100 秒，帧号停滞超过阈值，被误判成"该路卡死"
> 并**重启了整机**。生产上这种误报会反复中断业务，所以必须先看整机是否还活着。

> **为什么要重启冷却窗口**：新进程要跑满 `--stats-interval` 帧（约 5~10 秒）才打出第一行统计，
> 在那之前日志里"最后一次出现"的帧号还是**旧进程**的值，拿它跟旧记录比会误判成停滞。
> 判据用 MainPID 变化，三种重启来源（崩溃自动拉起 / 看门狗重启 / 运维手工重启）全覆盖。

### 4.3 服务未在运行 → 带指数退避拉起

| 项 | 值 |
|---|---|
| 判据 | `ActiveState != active` **且** `Result != success` |
| 为什么不用 `is-failed == "failed"` | **那个判据有致命缺陷**：StartLimit 命中后 unit 变成 `inactive/dead`，`is-failed` 返回 `"inactive"` 而不是 `"failed"` —— 判据永远不再成立，看门狗只自愈一次就彻底卡死（2026-11-18 实测踩过） |
| 怎么区分"人为停机" | systemd 原生语义：`systemctl stop` → `Result=success`；启动失败/被限流 → `Result=exit-code`。两者互斥 |
| 退避节奏 | 60s → 120s → 240s → 300s（封顶） |
| 为什么先 `reset-failed` | 不清 StartLimit 计数器的话，这次 `start` 会被 systemd 以 "start request repeated too quickly" 拒掉 |
| 清零条件 | 连续健康运行 600s 后退避计数清零 |

### 4.4 水位告警（只告警，不重启）

| 指标 | 阈值 | 数据来源 |
|---|---|---|
| RSS | 400 MB | `/proc/<MainPID>/status` |
| 活堆(heap) | 200 MB | 日志里的 `heap=NNNKB` |
| 日志盘剩余空间 | 8 MB | `df -Pk` |

### 4.5 MQTT 探针（可选，默认关）

`GATE_MQTT_PROBE=1` 时启用：订阅 topic，`MQTT_PROBE_SEC` 秒内收不到消息就告警。

**默认关的理由**：① broker 抖动/断网会让一个**健康**的进程被误判卡死并重启；
② 要多起一个 python 进程；③ 没配 broker 时毫无意义。

### 4.6 维护窗口

```bash
sudo touch /run/rk-gate/paused    # 看门狗闭嘴，什么都不做
sudo rm   /run/rk-gate/paused    # 恢复正常看护
```

`/run` 是 tmpfs，重启即失效 —— 维护暂停不会跨重启残留。

---

## 5. 启动前预检（12 项）

`preflight_gate.sh` 在 `ExecStartPre` 阶段运行。**任一 FAIL 项 → 退出码 78 → 拒绝启动**，
把"启动了但业务全挂"提前变成"启动失败 + 明确原因"。结果写 `$GATE_LOG_DIR/rk-gate-preflight.log`。

| # | 检查项 | 级别 | 判据 |
|---|---|---|---|
| 1 | 二进制存在且可执行 | FAIL | `build/rk_gate` |
| 2 | 模型/标签/OCR模型/OCR字典可读 | FAIL | 4 个路径逐个检查 |
| 3 | **librknnrt 版本 == 2.3.2** | FAIL | 解析 `librknnrt version: X.Y.Z`（坑位 01：toolkit 版本必须匹配） |
| 4 | librga / librockchip_mpp 就位 | FAIL | `third_party/lib` + `/usr/lib/aarch64-linux-gnu` |
| 5 | **NPU 设备节点** | FAIL | `/dev/dri/renderD12[89]`\|`renderD130`（**不是 `/dev/rknpu`，本板没有那个节点**） |
| 6 | dma_heap 可用 | FAIL | `/dev/dma_heap/system` |
| 7 | 4 路源可用 + high 通道有 IPM | FAIL | 本地文件查可读；`rtsp://` 查 TCP 连通 |
| 8 | 日志文件可写 | FAIL | 判据是**目标日志文件可写**，不是目录可写（`/var/log` 属 root） |
| 9 | 日志盘剩余空间 | FAIL(<8MB) / WARN(<32MB) | `df -Pk` |
| 10 | 核心配置项齐全 | FAIL | 核掩码、帧率、SN 等 |
| 11 | MPP 服务节点 | WARN | `/dev/mpp_service` |
| 12 | MQTT broker 连通 | WARN | `/dev/tcp` |

---

## 6. 运维手册

### 日常查看

```bash
systemctl status rk-gate.service              # 主服务
systemctl status rk-gate-watchdog.service     # 看门狗
tail -f /var/log/rk-gate-error.log            # 业务日志(stderr)
cat /var/log/rk-gate-preflight.log | tail -30 # 最近一次预检
cat /var/log/rk-gate-alerts.log               # 看门狗告警流水
```

### 停 / 启

```bash
sudo systemctl stop rk-gate        # 人为停止 —— 看门狗【不会】把它拉回来
sudo systemctl start rk-gate
```

> `systemctl stop` 之后 `Result=success`，看门狗据此判断"这是有意停的"，会尊重运维意图。
> 但如果服务此前正处在失败状态（`Result=exit-code`），看门狗仍会把它拉回来。
> **要彻底闭嘴请用 `/run/rk-gate/paused` 标记。**

### 改配置

```bash
vim /home/seeed/rk_deploy/rk3588_zero_copy/config/gate.env
sudo systemctl restart rk-gate
```

### 排障速查

| 现象 | 查什么 | 常见原因 |
|---|---|---|
| 服务起不来 | `cat /var/log/rk-gate-preflight.log \| tail -30` | 预检 FAIL 项（模型路径、库版本、NPU 节点） |
| 服务反复重启 | `journalctl -u rk-gate -n 50` | 看退出码；78 = 配置错 |
| 卡死被重启 | `cat /var/log/rk-gate-alerts.log` | L1/L2 判据触发，看当时的日志 |
| 双实例 | `ls -l /proc/*/fd/9 \| grep rk-gate` | flock 互斥应保证不会出现 |
| 日志不涨 | `ls -la /var/log/rk-gate*.log` | 看门狗会报"L1 心跳停了" |
| 内存高 | `cat /proc/<pid>/status \| grep VmRSS` | 对照告警线 400MB |

---

## 7. 踩过的坑（都是板端实测撞出来的，不是推演）

守护层看起来简单，实际写下来踩了 6 个坑。**每一个都会让守护链"看起来在工作、其实在做错的事"**，
所以记在这里，新人接手时先读这一节。

### 坑 1：`exec 9>"$LOCK" 2>/dev/null` 吞掉全部业务日志

`exec` **不带命令**时，重定向会**永久**作用于当前 shell。于是 `2>/dev/null` 把 wrapper 的
stderr 永久换成 `/dev/null`，后面 `exec` 出去的 `rk_gate` 继承它 —— **业务日志一行都不出来**。

症状极隐蔽：stdout 一切正常、只有 stderr 静默，像是"程序没打日志"；
systemd 那边 `StandardError=append:` 也确实指向了正确文件，看不出任何配置错误。

> 正确写法：先用普通命令试探可写性（`( : >> "$LOCK" ) 2>/dev/null`），再单独 `exec 9>"$LOCK"`。

### 坑 2：`OnFailure` 放错段

必须在 `[Unit]`，放 `[Service]` 里 systemd 报 `Unknown key 'OnFailure' in section [Service],
ignoring.` 然后**静默忽略** —— 崩溃取证等于没配，而且 journal 里只有一行不起眼的警告。

### 坑 3：看门狗用 `is-failed == "failed"` 判据 → 只自愈一次就卡死

StartLimit 命中后 unit 变成 `inactive/dead`，此时 `systemctl is-failed` 返回的是
**`"inactive"` 而不是 `"failed"`** —— 判据永远不再成立，看门狗就再也不管了。

> 改用 `(ActiveState != active) && (Result != success)`：
> 人为 `systemctl stop` → `Result=success` → 尊重运维；启动失败 → `Result=exit-code` → 拉起。

### 坑 4：重装后配置不生效

`install` 用了 `systemctl start`，而 `start` 对**已经在跑**的服务是空操作。
改了 `gate.env` 重装，配置根本没生效 —— 排查时白费功夫。

> 改用 `systemctl restart`（对没跑的服务等价于 start）。

### 坑 5：L2 把"变慢"误判成"卡死" → 反复重启整机

NPU 被别的任务压到 96% 时，某一路从 20fps 掉到 1~2fps，100 帧要 50~100 秒，
帧号停滞超过阈值 → 被判定"该路卡死" → **重启整机**。生产上这种误报会反复中断业务。

> 先数还有几路在推进：≥2 路在推进 → 只告警，连续 3 轮不恢复才动手。

### 坑 6：跨重启时帧号取 max → 连环重启（最隐蔽）

日志是 `append` 的，重启**不截断**。于是：

1. 帧号取日志尾部的 **max** → 旧进程的大帧号（如 14700）永远盖住新进程的小帧号
2. 重启后 `rm` 掉了 `ch*_ts` 状态文件 → `_lt=0` → `_stuck = now(epoch 秒)` → 远超阈值
3. 两者叠加 → 每一轮都判"帧号停在 14700" → **连环重启**

实测 2026-11-19 00:29~00:39：**12 次连环重启，每次间隔精确的 3 分 25 秒**（= 180s 阈值 + 20s 检查周期）。

> 修法两条，缺一不可：
> ① 帧号取**最后一次出现**的值（不是 max）→ 新进程的行在日志末尾
> ② **不删**状态文件 → 让"帧号回退 = 进程重启过"的分支自然把计时刷新
> ③ 再加**重启冷却窗口**：MainPID 变了就跳过 L2 判定 60 秒

---

## 8. 已知限制与风险（生产前必读）

| # | 风险 | 现状 | 处置 |
|---|---|---|---|
| 1 | **日志盘容量** | 本板 `/var/log` 是 **47MB 的 zram 内存盘**（`/var/log/syslog` 10MB + `kern.log` 7MB 已占 18MB）。逐帧 guard 日志在 `RK_LOG_LEVEL=info` 下实测 **36.5 MB/小时 ≈ 17 GB/天**，写满后 `write()` 返回 ENOSPC 会让应用崩 | ① `GATE_LOG_LEVEL=warn`（默认）关掉逐帧日志，速率降两个数量级；② **生产建议 `GATE_LOG_DIR=/var/log.hdd`**（真实磁盘，本板 journald 就是软链过去的） |
| 2 | **journald 是 volatile** | `Storage=volatile`，重启即丢 | 所有日志走 `append:` 落文件 + logrotate，不依赖 journal |
| 3 | **阻塞读不可中断** | `gate_main.cc:683` 的 `av_read_frame` 在 RTSP 断流时不可中断 → SIGTERM 后通道线程卡住 → `join()` 永久阻塞 → systemd 等满 `TimeoutStopSec=30` 后 SIGKILL → RKNN 上下文未在 worker 线程内销毁 | 下一轮改 `AVFormatContext::interrupt_callback` + `sigaction`（约 40 行）。当前靠 `TimeoutStopSec=30` 兜底 |
| 4 | **硬挂死无法自愈** | 整机失去响应时看门狗自己也跑不了 | 靠 `ramoops` + `tools/kern_log`（每条 fsync）+ netconsole 取证，这套链**不能删** |
| 5 | `RestartPreventExitStatus=78` 不覆盖 ExecStartPre | 实测：preflight 失败时 systemd 仍会按 `RestartSec` 重试 | 已被 `StartLimitIntervalSec=300 / Burst=5` 限流（5 次后停），再由看门狗带退避接管 |
| 6 | 看门狗与服务双重重试 | 看门狗拉起前 `reset-failed` 会清掉 StartLimit 计数器，systemd 又有 5 次重试机会 | 实际周期 = systemd 的 5 次重试（约 50s）+ 看门狗退避，不会成风暴 |

---

## 9. 变更记录

| 日期 | 内容 |
|---|---|
| 2026-11-18 | 首版。systemd 托管 + 预检 + 看门狗 + 崩溃取证 + 日志轮转。板端实测通过：kill -9 自动拉起、预检阻断、限流后看门狗自愈、日志追加 |
