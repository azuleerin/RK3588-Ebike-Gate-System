# 遗留风险清单

> 扫描时间: 2026-11-13 04:40
> 扫描范围: `src/` `include/` `scripts/` `CMakeLists.txt` (排除 `build/`、`third_party/`)
> 判定标准: **代码里没有 TODO/FIXME 标记, 但"能跑但不对/跑不了/没测过"的地方就是风险**。

---

## 一、TODO / FIXME 扫描结果

```bash
grep -rnE "TODO|FIXME|XXX|HACK" --include=*.cc --include=*.cpp --include=*.h \
          --include=*.sh --include=*.py . | grep -v build/
```
**结果: 业务代码 0 处。** 仅 3 处命中, 均无害:

| 位置 | 内容 | 判定 |
|---|---|---|
| `third_party/include/rknn_api.h:427` | Rockchip 官方头文件的 `RKNN_FLAG_XXX_XXX` | ✅ 第三方, 无需改 |
| `scripts/collect_crash.sh:18` | `mktemp -d /tmp/crashcollect.XXXXXX` | ✅ 正常用法 |
| `scripts/longrun.sh:99` | `mktemp /tmp/rk-minimal.service.XXXXXX` | ✅ 正常用法 |

> **但"没有 TODO"不等于"没有风险"** —— 本次几个最贵的坑(AVFrame 结构体泄漏、FP16 喂法错误、
> RGA 慢路)代码里**一个 TODO 都没有**, 全靠实测才发现。
> 所以下面的清单是**按排查过程中实际踩到的事实**整理的, 不是按注释整理的。

---

## 二、真实风险清单

### 🔴 高危(会导致客户现场功能异常)

| # | 风险 | 现象 | 证据 | 影响 | 处置建议 | 责任 |
|---|---|---|---|---|---|---|
| R1 | **生产 LPRNet 读不出绿牌(新能源)** | 真实车牌 `苏N A339058` → 识别成 `1` / `E1` | `scripts/rk_lpr_eval` 实测 1664 帧, int8/float 两种喂法**都错** | 新能源车全部识别失败 | 算法组补绿牌样本重训; 短期可先用蓝牌验证 | 算法组 |
| R2 | **板上编出来的 FP16 .rknn 输出恒定噪声** | 每帧 8874 个 class-66 框, 换图结果一样 | 同一个 ONNX 在模拟器里正常(一致率 97.5%), 板上三种喂法全废 | 任何"拿 FP16 做对照"的验证都会得出错误结论 | 重新编 FP16 时去掉 `mean/std`, 或让算法组确认 RKNN 版本对 FLOAT16 输入的支持 | 算法组 |
| R3 | **实时相机没对准路口机位** | 相机对着办公桌; 车牌内容是 `深圳W`/`深圳7`(模型把行人的鞋误检成 ebike) | 现场勘察: 网段内仅 `192.168.31.200` 一台相机, 端口 80 是设备后台 | 现场演示拿不到真实车牌 | 现场前把相机对准路口, 并用真实机位重新标定 IPM | 现场/集成 |
| R4 | **`--lprnet-feed float` 选项实际不可用** | 给 INT8 模型喂 float32 → 输出恒定 `E1` | `LprNet::infer()` 的 float 分支: 模型逻辑输入是 INT8, 喂 FLOAT32 会被运行时错误解释 | 有人拿它做精度对照会得出"float 比 int8 还差"的错误结论 | 在下个版本里明确报错拒绝(或删掉该选项) | 板端 |
| R5 | **IPM 标定值与实际机位强绑定** | `--ipm-points "300,622;1500,643;1450,877;550,819"` 是某个特定机位的标定 | `README`/`run_demo.sh` 里明确写了"相机流要先重新标定" | 换了机位, 测速距离全错(但不会崩) | 交付时必须附"换机位要重新标定"的说明 | 集成 |

### 🟡 中危(影响稳定性/可维护性)

| # | 风险 | 现象 | 证据 | 影响 | 处置建议 |
|---|---|---|---|---|---|
| R6 | **跟踪器 vector 的高水位扩容台阶** | `std::vector<Track>`, `sizeof(Track)≈4.1KB`(4 组环形缓冲) → 扩容时一次跳 +65KB / +129KB | 实测活堆曲线有 65KB/129KB 台阶, 台阶后完全平 | 长跑曲线不是纯直线, 容易被误判成泄漏 | ①报告按"尾段斜率"判定(已做); ②长期可把 Track 里的环形缓冲改成按需分配, 或 shrink_to_fit |
| R7 | **MPP 解码器偶发卡死** | 某次 45 秒内一帧未出(`mpp_dec: task no send to hal que`) | 开工前实测复现过一次, 重启即恢复 | 无人值守时会停在那里 | 夜间长跑已有"帧号 3 分钟不动 → 主动重启"的看护; 建议**长期加 MPP 重置逻辑** |
| R8 | **软解路径 RGA 走 CPU 慢路** | 软解时 RGA 拿到的是 CPU 指针 → 11.42ms(硬解是 2.31ms) | 实测 4.9× 差距 | 降级路径整体慢 1.43× | 把软解的 NV12 也放进 dma-buf(技术上可行, 本次未做) |
| R9 | **`--npu-pipe 2` 是新路径, 生产未验证** | 双实例双缓冲并行, 只在 300 帧 + 15/15 一致性上验证过 | `docs/NPU_SCHED_REPORT.md` | 若在生产开启, 线程同步问题可能暴露 | **默认关闭**; 开启前先跑 10 小时长跑 |
| R10 | **量化校准集只有 24 张且全是桌面场景** | `_bench/calib/` 24 张, 来自办公桌场景 | INT8 模型就是用这 24 张编出来的 | 路口光照/角度下的 INT8 绝对精度未知 | 换成路口实拍帧重编一次(数据已备好: `dataset_mp4/` 1664 帧) |
| R11 | **并行会话有未提交的改动在树上** | `include/license_plate_rec.h`、`src/license_plate_rec.cc` 未提交 | `git status` 一直能看到 | 编译出来的二进制**包含**这份未提交代码, 与 HEAD 不一致 | 交付前必须让算法组提交, 然后重新编译并复跑验收 |
| R12 | ~~**`ipaddr` 默认硬编码 `192.168.31.77`**~~ | `main.cc:372`, `result_sink.h:50` | 代码扫描 | 换板子后 MQTT 报文里的设备 IP 仍是旧值 | ✅ **2026-11-24 已闭环**: `gate.env` 改 `GATE_IPADDR=auto`; `run_gate_service.sh` 启动时自动取主网卡 IPv4(`ip route get` 的 src, 退到 scope global 首个非回环地址); `preflight_gate.sh` 新增 10b 项校验——上报地址必须在本机网卡上, 不匹配直接 FAIL 拒绝启动。实测: 板子早已换到 `192.168.8.77`, 而 gate.env 还写着 `192.168.31.77`, 线上进程一直带着错的 `--ipaddr` 在跑 |
| R19 | **日志盘是 47MB 的 zram 内存盘** | `/var/log` 挂的是 `/dev/zram1` (ext4, 47MB, 已用 18MB); `/var/log/syslog` 10MB + `kern.log` 7.2MB 还在自己长 | `df -hT /var/log` 实测; 本板 journald 特意软链到 `/var/log.hdd/journal` | 业务日志写满 → `write()` 返回 ENOSPC → **应用可能直接崩** | ①`GATE_LOG_LEVEL=warn` 关掉逐帧 guard 日志(实测 36.5MB/h → 降两个数量级); ②**生产必须把 `GATE_LOG_DIR` 指向 `/var/log.hdd`**(真实磁盘) |
| R20 | **逐帧 guard 日志的速率** | 帧守卫对**每一帧**都打构造/析构 INFO, 4 路并发实测 **36.5 MB/小时 ≈ 17 GB/天** | 2026-11-18 板端实测(旧二进制, `RK_LOG_LEVEL` 开关未编入) | 见 R19 —— 单独就能撑爆日志盘 | 开关已进 `log.cc`(`RK_LOG_LEVEL=warn`), 需**重新编译**才生效; 见 PRODUCTION_GUARD.md 第 7 节 |
| R21 | **journald 是 volatile, 重启即丢** | `Storage=volatile` + `SystemMaxUse=20M` | `/etc/systemd/journald.conf` 实测 | 崩溃现场的 journal 证据在一次重启后消失 | 所有业务日志走 `StandardOutput/StandardError=append:` 落文件 + logrotate; 崩溃取证走 `OnFailure=rk-collect-crash.service`(ramoops + dmesg + cgroup memory.peak) |
| R22 | **阻塞读不可中断, 优雅关闭会拖到 SIGKILL** | `gate_main.cc:683` 的 `av_read_frame` 在 RTSP 断流时不可中断 → SIGTERM 后通道线程卡住 → `main:1603` 的 `join()` 永久阻塞 → systemd 等满 `TimeoutStopSec=30` 后 SIGKILL → **RKNN 上下文未在 worker 线程内销毁** | 代码走查(`gate_main.cc:683/849/1594-1608`) | 现场维护/看门狗重启时 NPU 上下文泄漏, 且退出码变成 137(看起来像 OOM) | ✅ **2026-11-24 已闭环**(P2-A2): 加了 `AVFormatContext::interrupt_callback`(g_quit 一置位就中断 io, 且在 `avformat_open_input` 之前就挂上, 连打开过程也可打断) + `sigaction`(不设 `SA_RESTART`, 让 read() 返回 EINTR 而不是被内核自动重启)。A/B 实测(同一阻塞 RTSP 源, 唯一差别是这一处): 旧逻辑 **45.00 s / 退出码 137(被 SIGKILL 强杀)**, 新逻辑 **0.38 s / 退出码 0** |

### 🟢 低危(已知但可接受)

| # | 风险 | 说明 | 影响 | 处置 |
|---|---|---|---|---|
| R13 | 生产模型没有 FP16 对照源 | 板上只有 .rknn, 没有 ONNX/PT | 量化损失只能在同源 yolov8n 上测 | 找算法组要源模型 |
| R14 | 没有带人工真值的标注集 | 全盘无 YOLO/COCO 格式标注 | 算不了真 mAP, 只能用 teacher-student 一致性 | 建一个小规模精标集(100~200 张就够) |
| R15 | 演示依赖 PC 上的 ActiveMQ | `192.168.31.30:1883` | PC 关机则推流中断 | 已有 5 秒自动重连, 不崩; 可在板子上跑一个本地 broker 兜底 |
| R16 | `scripts/` 里有 26 处硬编码 IP/路径 | 换环境要改 | 维护成本 | 抽成 `.env` 或配置文件 |
| R17 | 调试用的环境变量开关仍留在产品代码里 | `RK_FORCE_CPU_RGA` / `RK_RGA_SRC` / `RK_RGA_STAGE` | 误设会改变行为 | 交付版注释掉或加"仅调试"警告 |
| R18 | 夜间长跑看护脚本曾有"人工暂停误判为卡死"的 bug | 已修(帧号变小=重启; 进程不在不插手; 暂停标记文件) | 再犯会导致无限重启 | 已加 3 重保护; 建议把"重启次数"也做成看护自身的自检项 |
| R23 | 生产守护层之前完全缺失 | 崩溃后不自动拉起; 不会开机自启; 双实例无互斥; 环境坏了照样起 | 无 systemd 托管, 全靠人工 | **2026-11-18 已补齐** —— `docs/PRODUCTION_GUARD.md`, 板端实测通过(kill -9 自动拉起 / 预检阻断 / 限流后看门狗自愈 / 日志追加) |

---

## 三、当前已修复并验证的问题(不用再担心)

| 问题 | 修复 | 验证方式 |
|---|---|---|
| 整机硬挂死(P0-1/P0-2) | RAII 帧守卫 + ROI 最小尺寸约束 | 4.4 小时 / 39 万帧零崩溃 |
| AVFrame 结构体泄漏(416B/帧) | `av_frame_unref` → `av_frame_free` | 活堆斜率 +0.612 → +0.036 KB/帧 |
| LPR 车牌缓存只增不减 | 按 TTL 清理 | 缓存大小随 TTL 有界 |
| `av_frame_ref` 往空指针写(拷贝构造) | 先 alloc 再 ref | 代码路径修正 |
| RKNN 上下文跨线程死锁 | 每个实例的上下文在自己的工作线程建 | 并行模式 300 帧无死锁 |
| 喂 uint8 还是 float 判错 | 按逻辑输入 `type` 判(含 FLOAT16) | 三种喂法对拍 |
| 看护脚本无限重启 | 3 重保护 | 连跑 4 分钟计数稳定 |

---

## 四、建议的收尾动作(按优先级)

1. **R1 绿牌重训** —— 直接影响客户现场的核心功能。
2. **R11 提交并行会话的改动 + 重新编译 + 复跑验收** —— 当前二进制与 HEAD 不一致。
3. **R10 换路口校准集重编 INT8** —— 半小时的事, 提升路口场景的绝对精度。
4. **R4 让 `--lprnet-feed float` 明确报错** —— 防止误导后续对比。
5. **R3 现场前相机对准路口 + 重新标定 IPM**。
6. **R9 `--npu-pipe 2` 上生产前先跑 10 小时长跑**。
7. R12 设备 IP 自动获取; R16/R17 脚本配置化与调试开关清理。

---

## 五、P2 级技术债(本期立项不做, 记录在案)

> 判据: 有明确的改进方案和收益, 但**当前有别的兜底手段**, 且改动会碰到核心链路 ——
> 这种"能改但不急"的, 归 P2, 记下来而不是拖着不提。

### P2-1 · 优雅关闭: 让阻塞读可中断

| 项 | 内容 |
|---|---|
| **现象** | RTSP 断流时 `gate_main.cc:683` 的 `av_read_frame` 不可中断 → 收到 SIGTERM 后通道线程卡在读 → `main:1603` 的 `join()` 永久阻塞 → systemd 等满 `TimeoutStopSec=30` 后 SIGKILL → **RKNN 上下文没能在 worker 线程内销毁**(坑位 03 的同类风险) |
| **当前兜底** | `Restart=on-failure` + 看门狗双重兜底: 进程被强杀后几秒内被拉起, 系统迅速恢复健康。**"牺牲优雅关闭换取稳定性"是符合当前交付节奏的取舍** |
| **根治方案** | ① `AVFormatContext::interrupt_callback` 挂一个检查 `g_quit` 的回调, 让 ffmpeg 的阻塞 I/O 自行中断; ② `signal()` 换成 `sigaction()`(不带 SA_RESTART)。**约 40 行, 不改任何业务逻辑** |
| **风险** | 动的是解码主链路, 需要重新跑 300 帧冒烟 + 一次长跑 |
| **触发条件** | 当现场出现"维护/看门狗重启时 NPU 上下文泄漏"或"退出码 137 被误判成 OOM"的实际故障时, 立即启动本项 |
| **关联** | R22 |
