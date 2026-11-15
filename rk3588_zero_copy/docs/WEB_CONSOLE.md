# 网页控制台 —— 实时看负载, 点开关动态切硬件

> 一句话: **浏览器打开一个地址, 就能看到 MPP / RGA / NPU 的实时负载和流水线 FPS,
> 点三个开关就能当场把硬解/加速/推理关掉, 看性能曲线怎么变。**
>
> 面向: 现场演示的人 / 调性能的人 / 想知道"到底卡在哪"的人。

---

## 1. 它长什么样

```
┌────────────────────────────────────────────────────────────────┐
│ RK3588 流水线控制台  ● MQTT 已连接  设备 rk3588-01  ...        │
├───────────────────────────┬────────────────────────────────────┤
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

## 2. 数据通路

```
  ┌──────────── PC ────────────┐        MQTT         ┌──── 板子 ────┐
  │  web_console.py            │                      │              │
  │   ├─ 网页(HTML+canvas)      │                      │  rk_zero_copy │
  │   ├─ HTTP :8080             │                      │              │
  │   │    GET  /               │                      │  ┌────────┐  │
  │   │    GET  /api/state      │◀─ up/status ────────│  │控制线程│  │
  │   │    POST /api/control    │─ down/control ─────▶│  └────────┘  │
  │   └─ MQTT 客户端            │                      │  ┌────────┐  │
  │                             │◀─ up/load ──────────│  │状态线程│  │
  └─────────────────────────────┘                      │  └────────┘  │
                                                       └──────────────┘
```

三个话题(默认 `sn = rk3588-01`):

| 方向 | 主题 | 内容 |
|---|---|---|
| 网页 → 板子 | `device/rk3588-01/message/down/control` | `{"cmd":"set_engine","mpp":true,"rga":false,"npu":true}` |
| 板子 → 网页 | `device/rk3588-01/message/up/status` | `{fps,dec_ms,rga_ms,npu_ms,post_ms,total_ms,wait_ms,engine,decoder,rga_mode,load{...}}` |
| 板子 → 网页 | `device/rk3588-01/message/up/load` | `{npu,npu_core,rga,vpu,cpu,dmc,gpu}`(root 采样器发的, 见 §5) |

---

## 3. 部署步骤

### 3.1 板子: 重新编译(带了控制通道)

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

### 3.2 板子: 启动(加上 `--mqtt-host` 就同时开了控制+上报)

```bash
cd /home/seeed/rk_deploy/rk3588_zero_copy
export LD_LIBRARY_PATH="$PWD/third_party/lib:$PWD/lib:/usr/lib:$LD_LIBRARY_PATH"

./build/rk_zero_copy \
    rtsp://192.168.31.200:8557/h264 \
    ./model/ebike_int8_rk2332.rknn \
    --labels ./model/labels_ebike.txt \
    --track-cls 1 --nms 0.45 \
    --no-show \
    --mqtt-host 192.168.31.214 \
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

### 3.3 PC: 启动网页控制台

```bash
cd /home/seeed/rk_deploy/rk3588_zero_copy
python3 scripts/web_console.py --host 192.168.31.214 --sn rk3588-01 --http-port 8080
```

> **不需要 pip install**。只用了 Python 标准库(`http.server` + `socket`),
> 现场 PC 没装 flask/paho 也能直接跑。

浏览器打开: **http://<PC的IP>:8080/**

---

## 4. 怎么用(验收步骤)

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

## 5. 硬件负载的来源(为什么有的显示 -1)

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

## 6. 排错

| 现象 | 原因 / 怎么办 |
|---|---|
| 网页上小圆点是红的 | MQTT 没连上。检查 broker 地址/端口; 控制台终端会打印 `[web] MQTT 连接断开: ...` |
| 开关点了没反应 | ① 板子上的 `--mqtt-host` 没开, 或者主题对不上(板子的 `--sn` 和控制台的 `--sn` 要一致); ② 板端日志里没有 `[控制] 收到指令` |
| 开关点完板端日志没反应, 但网页显示"已下发" | 指令发出去了但板子没收到 —— 多半是两边 `--sn` 不一致, 主题对不上 |
| MPP 开关是灰的/点了没反应 | MPP 初始化就没成功(板端日志有 `[警告] MPP 初始化失败`)。控制台会忽略 `mpp=true` |
| 状态报文里 `npu_ms` 是 0 但 FPS 没变 | 正常: 源是 25fps, 去掉推理省下的时间变成【等流】(`wait_ms` 会变大) |
| 关 MPP 后 RGA 也变慢了 | 正常: 软解出来的帧是 CPU 内存, 不是 dma-buf, RGA 没法 import, 只能走 CPU 兜底 |

---

## 7. 兼容性说明

- **MQTT 3.1.1**, 不依赖 paho-mqtt。板子和 PC 两端都是零依赖实现。
- 三条独立的 MQTT 连接(结果上行 / 控制订阅 / 状态上报), 互不干扰 ——
  一条连接上收发混用会让 qos=1 的 PUBACK 等待被对端 PUBLISH 插队。
- 板端控制线程**只写三个原子开关**, 真正的切换动作由主线程在下一帧执行 ——
  控制线程不碰解码器/推理上下文, 没有跨线程的数据竞争。
- 网页端只用标准库 + 原生 canvas 画图, **不引任何 CDN**, 没外网也能正常显示。
