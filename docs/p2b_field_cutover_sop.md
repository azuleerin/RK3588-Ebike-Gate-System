# P2-B 现场实景切换与回滚 SOP

> **适用版本**：`v2.3-p2a-hardened`（标签，对应 `main@588c563`）
> **适用对象**：现场实施 / 远程支持
> **目标**：把生产服务从「仓库本地 MP4 素材」切到「现场真实 RTSP 相机 + MQTT Broker + 现场 IPM 标定」，每一步都有**可判定的验收判据**，任何一步不达标就**不要往下走**。
> **回滚目标**：一条命令切回本地 MP4，服务恢复到我方可控的已知状态。

---

## 〇、总则与红线

| # | 规矩 |
|---|---|
| 1 | **不要跳步**。第 2 步预检没有 0 FAIL，第 3 步就不用开始 —— 现场最贵的时间是"切了发现不对再切回来"。 |
| 2 | **每次只改一个变量**。切视频源、切 IPM、切 MQTT 是三件事，分三次做、分别验收。 |
| 3 | **先备份**。第 1 步的备份是回滚的唯一依据 —— 没备份就不要往下走。 |
| 4 | **切生产前先停看门狗**。`sudo touch /run/rk-gate/paused`，否则切配置的那几秒会被看门狗当成卡死重启。切完记得 `sudo rm /run/rk-gate/paused`。 |
| 5 | **不要动 `config/gate.env` 以外的任何配置文件**。现场只需要改 `gate.env` 一个文件。 |
| 6 | **不要在 `gate.env` 里写 `export`**。这份文件被 systemd 的 `EnvironmentFile=` 和 `run_gate_service.sh` 的 `source` 读两遍，写了 `export` 会让 systemd 那一遍整份加载失败（服务起不来）。 |

**当前基线状态**（切之前先确认服务是好的）：

```bash
cd /home/seeed/rk_deploy/rk3588_zero_copy
systemctl is-active rk-gate rk-gate-watchdog rk-kernel-log   # 期望: active active active
sudo scripts/gate_health.sh                                   # 期望: [正常]
```

---

## 一、第 1 步：备份现网配置（必做）

```bash
cd /home/seeed/rk_deploy/rk3588_zero_copy
sudo cp -av config/gate.env config/gate.env.bak.local_mp4
echo "备份完成: config/gate.env.bak.local_mp4"
```

> ★ 这份备份就是**回滚用的那一份**。回滚命令见第 9 步。
> ★ 备份名里带 `local_mp4` 是有意的：它必须记录的是"当前正在跑本地 MP4 素材"这个已知状态，
>   而不是"上次有人改过的版本"。回滚要回到**能跑**的状态，不是回到**最近**的状态。

---

## 二、第 2 步：现场预检（只读，不改系统）

预检脚本**不写任何系统配置、不重启服务、不碰 NPU**，只做「打开网络连接 + 读流信息 + 往指定目录写图」。

### 2.1 装模板 + 填视频源

```bash
cd /home/seeed/rk_deploy/rk3588_zero_copy
cp config/gate.env.field.example config/gate.env.field
vim config/gate.env.field
```

只改这 8 行（其余保持模板原样）：

```bash
GATE_CH1=rtsp://<近景相机IP>:8554/<近景路径>
GATE_CH1_KIND=near
GATE_CH2=rtsp://<高位相机IP>:8554/<高位路径>
GATE_CH2_KIND=high
GATE_CH3=rtsp://<近景相机IP2>:8554/<近景路径2>
GATE_CH3_KIND=near
GATE_CH4=rtsp://<高位相机IP2>:8554/<高位路径2>
GATE_CH4_KIND=high
```

### 2.2 跑预检

```bash
cd /home/seeed/rk_deploy/rk3588_zero_copy
bash scripts/field_preflight.sh --config config/gate.env.field --timeout 15
```

### 2.3 ★ 验收判据

| 判据 | 要求 |
|---|---|
| **退出码** | `0`（用 `echo $?` 确认） |
| **`FAIL` 计数** | **必须为 0** |
| **每一路** | 都出现 `[ OK ] ... 连通  编码=... 位深=...bit 帧率=...fps` |
| **位深** | 必须是 **8bit**。出现 `10bit HEVC Main10` 的红色 FAIL = 不达标 |
| **帧率** | 不低于 5 fps（低于此会 WARN） |

### 2.4 常见 FAIL 怎么处置

| 现象 | 根因 | 处置 |
|---|---|---|
| `连不上 / 读不出流信息` | 地址/端口/账号密码/网段不对 | 板子上 `ping <相机IP>`、`nc -vz <相机IP> 8554`；确认 RTSP 服务端口不是 80/554 |
| `10bit HEVC Main10`（红 FAIL） | 相机/NVR 输出了 10bit HEVC | **本板 MPP/RGA 硬件路径不支持 10bit HEVC，降软解仅约 10 FPS**。请在相机/NVR 后台切换为 **8bit H.264** 或 **8bit H.265 Main Profile**，改完重跑预检 |
| `编码是 xxx(不是 h264/hevc)` | 相机出了 MJPEG/AV1 等 | 改相机编码设置 |
| `宽度只有 Npx(< 640)` | 用了子码流 | 换主码流 |
| MQTT `connack_rc=4`（用户名或密码错误） | broker 账号不对 | 核对 broker 的账号密码 |
| MQTT `tcp_error=... Connection refused` | broker 地址/端口不对 | 核对 `GATE_MQTT_HOST` / `GATE_MQTT_PORT` |

---

## 三、第 3 步：单路冒烟（只切 ch1，不碰 MQTT）

**目的**：在动生产服务之前，先用**同一个二进制、同一套参数**证明「这一路真实相机能跑通、能出帧、能识别」。

### 3.1 停生产 + 停看门狗

```bash
sudo touch /run/rk-gate/paused      # 先让看门狗闭嘴
sudo systemctl stop rk-gate
```

> ★ 必须先停 `rk-gate`：双实例会抢 NPU 核、抢 MQTT 连接、抢锁。

### 3.2 跑 60 秒单路

```bash
cd /home/seeed/rk_deploy/rk3588_zero_copy
. config/gate.env          # 注意: 这里 source 的是模板 gate.env.field, 不是 gate.env
export LD_LIBRARY_PATH="$GATE_LD_LIBRARY_PATH"

CH1="rtsp://<近景相机IP>:8554/<近景路径>"
./build/rk_gate \
  --model "$GATE_MODEL" --labels "$GATE_LABELS" \
  --ppocr "$GATE_OCR" --ppocr-dict "$GATE_OCR_DICT" \
  --det-core "$GATE_DET_CORES" --ocr-core "$GATE_OCR_CORES" \
  --det-workers "$GATE_DET_WORKERS" --ocr-workers "$GATE_OCR_WORKERS" \
  --conf "$GATE_CONF" --nms "$GATE_NMS" --max-fps "$GATE_MAX_FPS" \
  --roi-ratio "$GATE_ROI_RATIO" --roi-y "$GATE_ROI_Y" \
  --roi-height "$GATE_ROI_HEIGHT" --roi-shift "$GATE_ROI_SHIFT" \
  --roi-below "$GATE_ROI_BELOW" \
  --sn "$GATE_SN" --ipaddr 127.0.0.1 --duration 60 \
  --ch "ch1:${CH1}:near" 2>&1 | tee /tmp/w1_smoke_ch1.log
```

> ★ 这一步**故意不带 `--mqtt-host`**：先把视频链路验通，再单独验 MQTT（第 6 步）。
>   带着 broker 冒烟的话，测试数据会当真的检测结果发到后端。
> ★ `--ipaddr 127.0.0.1`：冒烟阶段不上报真实地址，避免半成品数据进后台。

### 3.3 ★ 验收判据

| 判据 | 要求 |
|---|---|
| 进程退出码 | `0`（不是 137 / 139） |
| 日志里有 `[ch1][frame N]` 统计行 | **有**，且 N 在**持续增长** |
| `objs=` | 视现场画面而定 —— 画面里有人/车就该有值；**恒为 0 是异常**，回去查是不是 10bit 被当 8bit 解释了（P2-A1） |
| 日志里**没有** `10bit` / `Main10` | 没有就对了；有的话说明第 2 步的预检漏了 |

---

## 四、第 4 步：IPM 标定（只影响测速通道）

### 4.1 抓原始分辨率静态图

```bash
cd /home/seeed/rk_deploy/rk3588_zero_copy
bash scripts/field_preflight.sh --config config/gate.env.field --grab-ipm /tmp/ipm --timeout 15
```

会产出 `/tmp/ipm/ch2_ipm.jpg`、`/tmp/ipm/ch4_ipm.jpg`（**原始分辨率**，不缩放 —— IPM 四点必须按原始像素量）。

> ★ 只抓 `KIND=high` 的通道（ch2 / ch4）。near 通道不需要 IPM。
> ★ 如果这一路的分辨率和当初标定时不一样，**必须重标**（R5：IPM 标定值与实际机位强绑定）。

### 4.2 量 4 个点

用图片工具打开 `/tmp/ipm/ch2_ipm.jpg`，量出**地面车道**的 4 个角点（原图像素坐标）：

| 点 | 位置 |
|---|---|
| 1 | 车道左边界**最近**处 |
| 2 | 车道右边界**最近**处 |
| 3 | 车道右边界**最远**处 |
| 4 | 车道左边界**最远**处 |

同时量一下**车道实际宽度（米）**。

### 4.3 填进配置

```bash
# config/gate.env.field
GATE_CH2_IPM="<x1>,<y1>;<x2>,<y2>;<x3>,<y3>;<x4>,<y4>"
GATE_CH4_IPM="<x1>,<y1>;<x2>,<y2>;<x3>,<y3>;<x4>,<y4>"
GATE_IPM_WIDTH_M=<车道实际宽度,米>
```

> ★ IPM 的值里有分号，**必须加双引号**，否则 `source` 时会被当成命令分隔符。
> ★ 4 个点的顺序固定：近左 → 近右 → 远右 → 远左（逆时针围出梯形）。

### 4.4 ★ 验收判据（用已知参照物核对）

挑一辆**已知速度**的车从过线计区域通过（比如让同事开 20 km/h 的车通过），看日志里报的速度：

| 判据 | 要求 |
|---|---|
| 车速误差 | 与实际车速相差 **≤ 15%** |
| 4 路都是 high 时 | 每路的 `GATE_CH<n>_IPM` 都已填，且没有漏填 |

> ★ 车速全错（差 2~3 倍）= IPM 四点量错了（最常见是**近/远搞反**）或者车道宽度填错。
>   不会崩，但结果全错 —— 这是 R5，也是现场最常见的坑。

---

## 五、第 5 步：MQTT 联通（单独验）

### 5.1 预检脚本验（推荐）

```bash
cd /home/seeed/rk_deploy/rk3588_zero_copy
bash scripts/field_preflight.sh --config config/gate.env.field --publish-test --timeout 15
```

### 5.2 抓包看真实报文（可选，但更直观）

```bash
cd /home/seeed/rk_deploy/rk3588_zero_copy
python3 scripts/mqtt_sub.py --host <Broker IP> --port 1883 \
        --topic "device/$GATE_SN/#" --seconds 30 --out /tmp/w1_mqtt.jsonl
head -5 /tmp/w1_mqtt.jsonl
```

> ★ `mqtt_sub.py` 的参数是 `--host/--port/--topic/--out/--seconds/--client-id/--quiet`，
>   **没有 `--sn`** —— 设备号要写进 `--topic` 的通配里。
> ★ `--topic "device/$GATE_SN/#"` 里的 `$GATE_SN` 来自 `config/gate.env`（记得先 `. config/gate.env`）。

### 5.3 ★ 验收判据

| 判据 | 要求 |
|---|---|
| `connack=1` 且 `connack_rc=0` | 必须 |
| `published=1`（开 `--publish-test` 时） | 必须 |
| 抓包里看到 `device/<sn>/...` 主题 | 能订阅到 |
| 后台能看到 `preflight_test` 消息 | 能收到 |

> ★ `GATE_MQTT_HOST` **留空 = 不发 MQTT**（只本地计数），而且看门狗的 MQTT 探针也不会生效。
> ★ 预检的测试消息发在**专用主题** `device/<sn>/message/up/preflight_test`，
>   不会污染生产的 `ivs_result` / `tripwire` 主题。

---

## 六、第 6 步：写正式配置

预检全绿、IPM 标完、MQTT 通了，把 `config/gate.env.field` 的内容落到 `config/gate.env`：

```bash
cd /home/seeed/rk_deploy/rk3588_zero_copy
sudo cp -av config/gate.env config/gate.env.bak.local_mp4   # ★ 再备份一次(保险)
sudo cp    config/gate.env.field config/gate.env
```

确认几行关键值：

```bash
grep -E '^(GATE_CH[1-4]|GATE_CH[1-4]_KIND|GATE_CH[24]_IPM|GATE_MQTT_HOST|GATE_MQTT_PORT|GATE_MQTT_QOS|GATE_IPADDR|GATE_LOG_DIR)=' config/gate.env
```

| 键 | 期望值 |
|---|---|
| `GATE_IPADDR` | `auto`（**不要写死 IP** —— R12：写死的 IP 过期后不报错，现场表现为"设备在线但数据看不到"） |
| `GATE_LOG_DIR` | `/var/log.hdd`（**不要改回 `/var/log`** —— 那是 47MB 的 zram，写满应用会崩，R19） |
| `GATE_MQTT_HOST` | 现场 broker 的 IP（**不能留空**） |
| `GATE_LOG_LEVEL` | `warn` |

> ★ 改 `GATE_LOG_DIR` 之后**必须**重跑 `sudo scripts/install_gate_service.sh` ——
>   它会把 unit 里的日志路径、崩溃取证路径、logrotate 规则**一起**改掉。
>   只改这一行不管用，unit 还会往 `/var/log` 写。
> ★ 本次**不要**改 `GATE_LOG_DIR`（模板里已经是 `/var/log.hdd`），所以这步通常不用重装。
>
> 📌 **顺带说明一个容易误判的现象**：仓库里的模板
> `rk3588_zero_copy/scripts/systemd/rk-gate.service:98-99` 写的仍是
> `append:/var/log/rk-gate.log` / `append:/var/log/rk-gate-error.log`，
> 而**已安装**的 `/etc/systemd/system/rk-gate.service` 已经是 `/var/log.hdd/...`。
> 这是正常的 —— `install_gate_service.sh` 会在安装时按 `GATE_LOG_DIR` 重写 unit，
> 仓库里留的是**安装前的模板**。判断"日志到底在哪"要看已安装的 unit：
>
> ```bash
> grep -n 'StandardOutput\|StandardError' /etc/systemd/system/rk-gate.service
> ```

---

## 七、第 7 步：切生产（4 路 + MQTT 一起上）

```bash
cd /home/seeed/rk_deploy/rk3588_zero_copy
sudo systemctl restart rk-gate
sleep 20
systemctl is-active rk-gate
sudo scripts/gate_health.sh
```

### 7.1 ★ 验收判据

| # | 判据 | 怎么查 |
|---|---|---|
| 1 | 服务 `active`，且 `NRestarts` 没有增长 | `systemctl show rk-gate -p MainPID -p NRestarts` |
| 2 | `gate_health.sh` 结论是 **正常** | `sudo scripts/gate_health.sh` |
| 3 | **4 路帧号都在推进** | `grep '\[ch[0-9]\]\[frame' /var/log.hdd/rk-gate-error.log \| tail -8` |
| 4 | 日志在长 | `ls -l --time-style=+%T /var/log.hdd/rk-gate-error.log` |
| 5 | 后台能收到 `ivs_result` | 用 `mqtt_sub.py` 订阅，或直接看后台 |
| 6 | 预检（systemd 的 ExecStartPre）通过 | `tail -20 /var/log.hdd/rk-gate-preflight.log` |

```bash
# 判据 3 的具体命令: 每一路的最后一帧号
for i in 1 2 3 4; do
  printf 'ch%s: ' "$i"
  grep -o "\[ch$i\]\[frame [0-9]*\]" /var/log.hdd/rk-gate-error.log | tail -1
done
# 隔 10 秒再跑一遍, 帧号必须变
```

**恢复看门狗**：

```bash
sudo rm -f /run/rk-gate/paused
sudo systemctl restart rk-gate-watchdog
```

### 7.2 完整验收

```bash
cd /home/seeed/rk_deploy/rk3588_zero_copy
sudo scripts/acceptance_checklist.sh
```

> ★ 这条命令会 **kill -9 一次主进程**（验证自动拉起）、并**卸载再重装一次服务**（验证零残留）。
>   它是交付验收该做的事，但**别在客户还在用的时候跑**。

---

## 八、切换期间的性能参考（实测值，非估算）

| 项 | 实测值 | 出处 |
|---|---|---|
| 4 路生产配置吞吐 | **105.0 FPS**（4 路合计，限帧 `--max-fps 20`） | `docs/p2a_stability_hardening_report.md` §4.5 / `layer_bench_report.md` |
| 单路检测耗时（640×640，三核） | **23.0 ms/帧**（NPU 锁 1000 MHz） | `layer_bench_report.md` §2 |
| 纯 NPU 算力天花板（三核） | **133 FPS** | `layer_bench_report.md` §6.2 |
| 10bit 软解交付率 | **约 10 FPS/路**（软解 9~10 ms/帧 + CPU 色彩转换 9~10 ms/帧） | `docs/p2a_stability_hardening_report.md` §七-1（P2-A1） |
| 生产稳态内存 | RSS **228.6~231.7 MB** / 活堆 **73.3~74.2 MB** / cgroup **190.4~243.4 MB**（46.7 分钟长跑，278 次采样，零重启零告警） | `config/gate.env` §11 注释 / `docs/p2a_stability_hardening_report.md` §4.5 |

> ★ 现场真实相机的码流特性与仓库素材不同，**上面的数字只能当参考，不能当承诺**。
>   切完以第 7 步的实测为准：4 路合计吞吐应落在 **≥ 79.5 FPS**（限帧口径）。
>   达不到就是现场码流有问题（10bit / 分辨率过高 / 码率过高），回到第 2 步查。

---

## 九、第 8 步：一键回滚（切回本地 MP4）

**任何时候现场出问题，一条命令切回我方可控的本地 MP4 素材：**

```bash
cd /home/seeed/rk_deploy/rk3588_zero_copy
sudo cp -av config/gate.env.bak.local_mp4 config/gate.env && sudo systemctl restart rk-gate
```

### 9.1 回滚后验收判据

```bash
sleep 15
systemctl is-active rk-gate                                    # 期望 active
sudo scripts/gate_health.sh                                    # 期望 [正常]
for i in 1 2 3 4; do printf 'ch%s: ' "$i"; \
  grep -o "\[ch$i\]\[frame [0-9]*\]" /var/log.hdd/rk-gate-error.log | tail -1; done
```

| 判据 | 要求 |
|---|---|
| 服务 `active`，`NRestarts` 无增长 | 必须 |
| 4 路帧号恢复推进 | 必须 |
| 日志里重新出现本地素材的统计行 | 必须 |

### 9.2 回滚后清理

```bash
sudo rm -f /run/rk-gate/paused
sudo systemctl restart rk-gate-watchdog
```

> ★ 回滚**只回滚 `config/gate.env`**。不要去动 `models/`、`archive/rk12ch/`、`third_party/`、
>   也不要重新编译 —— 现场切源不涉及代码。
> ★ 回滚后**立刻**把现场现象记下来（哪一路、什么现象、日志片段），这是下一轮定位的输入。

---

## 十、故障速查

| 现象 | 先看 | 常见原因 |
|---|---|---|
| 服务起不来，journal 报 `EnvironmentFile` 错误 | `journalctl -u rk-gate -n 30` | `gate.env` 里写了 `export` 或有语法错误 |
| 服务起来但**没有一路有帧号** | `grep -i 'ch[0-9]' /var/log.hdd/rk-gate-error.log \| head` | RTSP 地址不对；或相机在鉴权；或**10bit 被当 8bit 解释**（P2-A1 之前是静默零检出） |
| 有帧号但 `objs=0` | 同上 | 模型不认这个场景；或 ROI 参数不对；或画面里真的没有目标 |
| 车速数值离谱 | `GATE_CH2_IPM` / `GATE_CH4_IPM` / `GATE_IPM_WIDTH_M` | IPM 四点量错（近/远反了）或车道宽度不对（R5） |
| 车牌识别成乱码 | 是不是绿牌 | 现网唯一识别模型是 `ppocrv4_rec_fp16.rknn`（217 张绿牌实拍整牌准确率 **86.64%**）；旧 LPRNet 已 `DEPRECATED`。蓝牌正常就说明识别链路是通的 |
| 看门狗误报卡死并重启了服务 | `tail -50 /var/log.hdd/rk-gate-alerts.log` | 看门狗阈值：`STALL_SEC=60`（整机心跳 1 分钟）/ `CH_STALL_SEC=180`（单路帧号 3 分钟）/ `SILENT_STALL_SEC=60`（日志在写但帧号全不涨 1 分钟）/ `FAIL_ALERT_THRESHOLD=3`（连续失败 3 次才升级告警）。检查周期 20 秒 |
| 日志盘满 / 应用崩 | `df -h /var/log.hdd` + `grep -c Suppressed /var/log.hdd/rk-gate-error.log` | 临时开了 `GATE_LOG_LEVEL=info`（info 级实测 36.5 MB/小时 ≈ 17 GB/天）；生产必须 `warn`（实测 398 KB/小时 ≈ 9.5 MB/天） |

---

## 十一、附：本次交付物清单

| 文件 | 作用 |
|---|---|
| `rk3588_zero_copy/config/gate.env.field.example` | 现场配置模板（**不含 `export`**），逐行注释 |
| `rk3588_zero_copy/scripts/field_preflight.sh` | 一键预检（只读）：连通性 / 编码 / 分辨率 / 位深 / 帧率 / MQTT / IPM 抓图 |
| `rk3588_zero_copy/scripts/field_mqtt_probe.py` | 极简 MQTT 3.1.1 探针（纯标准库，不装 mosquitto 也能测） |
| `rk3588_zero_copy/tools/field_probe.c` + `tools/build_field_probe.sh` | 兜底流信息探针（用工程自带的 libav*，**不需要 root**；板上没装 ffmpeg 时自动启用） |

---

## 十二、修订记录

| 日期 | 版本 | 变更 |
|---|---|---|
| 2026-11-24 | v1.0 | 初版。适用 `v2.3-p2a-hardened`（`main@588c563`）。 |
