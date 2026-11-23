# 演示流程傻瓜式操作手册

> **目标**: 从板子上电, 到网页上看到真实车牌 JSON, 全程照抄命令即可。
> **预计用时**: 编译 3 分钟 + 跑通 2 分钟 + 验收 2 分钟。
> 板子: `192.168.31.77` (账号 `seeed` / 密码 `<SUDO_PASS_REDACTED>`)
> PC 侧: MQTT broker `192.168.31.30:1883`

---

## 第 0 步: 准备清单(只做一次)

| 需要什么 | 在哪 | 怎么确认 |
|---|---|---|
| 板子通电 + 网线 | — | `ping 192.168.31.77` |
| 相机在推流 | `192.168.31.200:8557` | 见 Step 1.3 |
| PC 上的 MQTT broker | `192.168.31.30:1883` | 见 Step 1.4 |
| 模型文件 | 板子 `/home/seeed/rk_deploy/models/` | 见 Step 1.5 |
| 代码 | 板子 `/home/seeed/rk_deploy/rk3588_zero_copy/` | 见 Step 1.5 |

---

## 第 1 步: 上电自检(在 PC 的终端里)

### 1.1 连上板子

```bash
ssh seeed@192.168.31.77
```
密码 `<SUDO_PASS_REDACTED>`。

### 1.2 板子状态正常吗

```bash
uname -a; cat /proc/device-tree/model; free -h | head -2
```
**期望看到**:
```
Linux ... 6.1.172-vendor-seeed-rk3588
Seeed Studio Recompute Host ... 
Mem:  total 7.7Gi  used ...
```

### 1.3 相机在推流吗

```bash
ffprobe -v error -show_entries stream=width,height,avg_frame_rate -of csv rtsp://192.168.31.200:8557/h264
```
板子上没有 ffprobe 的话, 直接跳到 Step 1.5, 跑起来看日志更快。
**期望**: `1920,1080,25/1`

### 1.4 MQTT broker 通吗

```bash
timeout 5 bash -c 'echo > /dev/tcp/192.168.31.30/1883' && echo "broker OK"
```
**期望**: `broker OK`
(不通也不影响演示 —— 程序会每 5 秒自动重连, 不会崩, 见 3.5)

### 1.5 模型和代码都在吗

```bash
cd /home/seeed/rk_deploy
ls -la models/*.rknn rk3588_zero_copy/model/*.rknn
cd rk3588_zero_copy && git log --oneline -1
```
**期望**: 能看到 `ebike_int8_rk2332.rknn` / `lprnet_finetuned_int8.rknn`, 以及一条 commit 记录。

---

## 第 2 步: 编译

```bash
cd /home/seeed/rk_deploy/rk3588_zero_copy
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```
**期望**: 最后一行是
```
[100%] Built target rk_zero_copy
```

> 三个硬件开关(默认全开, 一般不用改):
> `-DUSE_MPP=1 -DUSE_RGA=1 -DUSE_NPU=1`
> 想做纯 CPU 对比: `-DUSE_MPP=0 -DUSE_RGA=0 -DUSE_NPU=0`

---

## 第 3 步: 跑通(一条命令)

```bash
cd /home/seeed/rk_deploy/rk3588_zero_copy
export LD_LIBRARY_PATH="$PWD/third_party/lib:$PWD/lib:/usr/lib:$LD_LIBRARY_PATH"

./build/rk_zero_copy \
  rtsp://192.168.31.200:8557/h264 \
  model/ebike_int8_rk2332.rknn \
  --labels model/labels_ebike.txt \
  --no-show \
  --decoder mpp \
  --track-cls 1 --nms 0.45 \
  --lprnet /home/seeed/rk_deploy/models/lprnet_finetuned_int8.rknn \
  --plate-cls 1 \
  --roi-ratio 0.72 --roi-y 0.70 --roi-height 0.16 \
  --ipm-points "300,622;1500,643;1450,877;550,819" --ipm-width-m 3.5 \
  --mqtt-host 192.168.31.30 --mqtt-port 1883 --mqtt-qos 1 \
  --sn RK3588-12CH-01 \
  --mqtt-topic 'device/RK3588-12CH-01/message/up/ivs_result'
```

### 3.1 启动时**必须**看到的几行(按顺序)

```
[LPRNet] 输入 name=input type=2 fmt=1 dims=[1,48,160,3,] n_elems=23040
[LPRNet] 已启用: 牌类=1 缓冲=dma-buf(零拷贝) 最低分=0.30 外扩=10% 输入=int8 量化码
[MQTT] 连接成功 192.168.31.30:1883, topic=device/RK3588-12CH-01/message/up/ivs_result (QoS=1)
开始处理...
[NV12] 数据源: dma-buf fd (RGA import)          ← ★ 必须是 dma-buf, 不是 CPU 指针
[RGA] rga_init() 正常返回, RGA 硬件通道可用
[RGA] 输入来源: DMA-BUF fd (fd=23, importbuffer_fd 注册为 RGA 源缓冲)   ← ★ 零拷贝生效
[自检] 形态C: nc=2 num_anchors=8400 cls_max=0.7189 (须 ∈(0,1]) box_max=649.8  ← 正常
[测速] IPM 就绪: f=1288.8px 标定区 3.50m x 7.56m  鸟瞰 350x756 @100px/m  矩形误差=0.000
[MQTT] 报文发送成功 (frame 0, topic=device/RK3588-12CH-01/message/up/ivs_result, 531 字节)
```

**如果 `[RGA] 输入来源` 显示"CPU 映射地址"** → 零拷贝没生效, 性能会掉一半, 停下来查(见第 5 章)。

### 3.2 跑起来之后每秒打印的帧行

```
[frame 60] fps=25.0 src=25.0 gap=40.0ms wait=8.42ms dec=0.49ms rga=2.31ms npu=28.33ms(run22.57/out0.17) post=0.46ms draw=0.00ms total=40.02ms objs=5 tracks=8 rss=93072KB heap=11967KB trim=0 guard[av 构0/析0/存0] guard[mpp 构60/析59/存1] fd[登记0/未登记0]
```

**关键字段怎么看**:

| 字段 | 期望 | 不对说明 |
|---|---|---|
| `fps=` | **25.0** | 低于 25 说明算力不够, 看哪个阶段大 |
| `src=25.0` | 相机的标称帧率 | — |
| `gap=40.0ms` | 相机实际推送间隔 | 不是 40 就不是相机的问题 |
| `wait=` | 8~12 ms | 接近 40 说明在等源, 说明算力富余 |
| `dec=` | **< 1 ms** | 大了说明硬解没走通 |
| `rga=` | **< 3 ms** | 大了(>10ms)说明 RGA 走了 CPU 兜底 |
| `guard[mpp 构N/析N/存1]` | 构≈析 | 不相等说明有帧泄漏 |
| `heap=` | **基本不变** | 每分钟涨 >1MB 就是泄漏 |

### 3.3 结束看统计

Ctrl-C 退出, 会打印:
```
===== 性能统计 (最近 60 帧) =====
  流水线 FPS   : 24.99   (帧间隔 40.02 ms)
  相机推送 FPS : 24.98   (解码间隔 40.02 ms)
  解码等待     : 8.42 ms
  解码         : 0.49 ms
  RGA 转换     : 2.31 ms
  NPU 推理     : 28.33 ms  (其中 rknn_run 22.57 / 取回输出 0.17)
  后处理       : 0.46 ms
  单帧总耗时   : 40.02 ms
  我们的活儿   : 31.59 ms (占帧间隔 79%)
  判定          : 瓶颈=【NPU 算力】: 推理占比最高, 考虑量化/批处理/换小模型
```

---

## 第 4 步: 验收(三选一, 任选其一带板子去现场)

### 4.1 网页看板(推荐, 最有说服力)

**在板子上**起看板:
```bash
cd /home/seeed/rk_deploy/rk3588_zero_copy
python3 scripts/mqtt_web.py --host 192.168.31.30 --port 1883 --topic 'device/#' --http-port 8090
```
**在 PC 浏览器**打开: `http://192.168.31.77:8090/`

**期望看到**:
- 标题栏: 绿点 + "已连接 192.168.31.30:1883" + "收到报文 N"(N 在涨)
- 左上"最新车牌": 显示车牌(没有时显示"无车牌")
- 右侧"报文 JSON(ivs_result)": 完整的 JSON, 含 `plate_no` / `license`(Base64) / `frame_w` / `frame_h`
- 左下"最近报文": 逐条列出, 带时间戳

> 看板是零依赖自写的(自带 MQTT 3.1.1 客户端), 不需要装任何东西。

### 4.2 命令行抓包(最原始, 最可信)

**PC 上**:
```bash
cd /home/seeed/rk_deploy/rk3588_zero_copy
python3 scripts/mqtt_sub.py --host 192.168.31.30 --port 1883 \
    --topic 'device/#' --out /tmp/cap.jsonl --seconds 60
```
**期望**: 每秒打印一条
```
[mqtt] #1 device/RK3588-12CH-01/message/up/ivs_result
        {"id":"RK3588-12CH-01","sn":"RK3588-12CH-01","name":"ivs_result",...,"plate_no":"深圳W",...}
```
板子上跑 Step 3 的命令, 这里就会刷屏。

### 4.3 日志 + 硬件负载(给懂技术的人看)

**板子上**:
```bash
# 1) 看负载(需 root)
sudo cat /sys/kernel/debug/rknpu/load
# 2) 看 VPU
sudo cat /proc/mpp_service/load | head -3
# 3) 看温度
for z in /sys/class/thermal/thermal_zone*; do echo "$(cat $z/type): $(cat $z/temp)"; done | grep -E "npu|soc|bigcore|little"
```
**期望**:
```
NPU load:  Core0: 48%, Core1:  0%, Core2:  0%
fdc38100.rkvdec-core      load:   1.9% utilization:   1.6%
npu-thermal: 37923
```

---

## 第 5 步: 常见故障速查(照着做, 不要慌)

| 症状 | 原因 | 处置 |
|---|---|---|
| `[RGA] 输入来源: CPU 映射地址` | 有一端不是 dma-buf, RGA 退回 CPU(慢 5 倍) | 查 MPP 解码帧的 fd 有没有传下去; 确认 `rga_convert.cc` 的 `RgaImage.fd` 是真 fd |
| `rga: irq handler err! INTR[0x2099]` | ROI 退化成极扁矩形(如 780x8), RGA 缩放器扛不住 | 调 `--roi-ratio/--roi-y/--roi-height`; 代码里已有 `kMinRoiH=16/kMinRoiW=32` 兜底 |
| `rga: Only get buffer N byte from dma_buf_ptr` | `wstride` 填了字节数 | 必须填**像素数**(README 坑②) |
| `[警告] 没有找到 h264_rkmpp 解码器, 退回软件解码` | 系统 FFmpeg 没带 rkmpp | 不影响, 本工程的硬解走的是 MPP 库, 看 `[解码] 后端 = Rockchip MPP 硬解` 那行 |
| `mpp_create` 段错误 | MPP 头文件与板子库版本不一致 | 删掉工程目录里的旧 MPP 头, 只能从 `/usr/include/rockchip` 找(CMake 已强制) |
| `[MQTT] 连接 192.168.31.30:1883 失败` | PC 的 broker 没开 | **不影响演示**, 程序每 5 秒自动重连; 或去 PC 上起 broker |
| `[MQTT] 将每 5 秒自动重连, 不影响视频流水线` | 同上 | 这是正常的提示, 不是错误 |
| `[自检] 形态C: cls_max=0.0019 ← 异常` | 喂了 /255 两次(双重归一化) | 检查喂给 NPU 的是 uint8 原始像素, 还是 float |
| 每帧 `objs=0` | 模型没检出东西 | 确认置信度(`--conf`, 默认 0.25); 确认 `src.fd` 是 dma-buf |
| 车牌识别出来是乱码(如 `E1`) | 模型只见过蓝牌, 绿牌(新能源)读不出 | **已知问题**, 需算法组补绿牌样本重训 |
| 活堆(`heap=`)每分钟涨 1MB+ | 泄漏 | 用 `python3 scripts/heap_slope.py <日志>` 判定; 已知泄漏已修(AVFrame 结构体) |
| 程序跑着跑着不动了 | MPP 偶发卡死 | 重启即可; 夜间长跑有看护进程会自动拉起 |

---

## 第 6 步: 夜间无人值守长跑(可选, 演示后挂着跑)

```bash
# 启动(10 小时后自动停机并出报告)
sudo systemctl start rk-night-soak.service rk-night-soak-monitor.service

# 看状态
systemctl is-active rk-night-soak rk-night-soak-monitor
sudo systemctl list-timers | grep night-soak

# 看报告(每 10 分钟自动刷新)
cat /home/seeed/rk_deploy/rk3588_zero_copy/docs/NIGHT_SOAK_REPORT.md

# 看告警(有崩溃/活堆超涨/卡死才会出现)
cat /home/seeed/rk_deploy/logs/night_soak_alerts.log

# 看采样数据
tail -3 /home/seeed/rk_deploy/logs/night_soak_metrics.csv

# 停止
sudo systemctl stop rk-night-soak.service rk-night-soak-monitor.service
```

**报告回答四个问题**: 跑了多久/多少帧 · 重启几次 · 活堆是不是水平线 · 最高温度+有没有降频。

---

## 第 7 步: 现场演示时的"一句话讲解"模板

> "这条链路是 **MPP 硬解 → RGA 零拷贝预处理 → NPU 推理 → CPU 后处理 → MQTT 上报**。
> 整帧 40 毫秒, 其中解码只占 0.5 毫秒(VPU), 预处理 2.3 毫秒(RGA),
> 推理 28 毫秒(NPU)。**全链一帧图像只存在一份物理内存, 零 CPU 拷贝。**
> 现在跑满 25 FPS 是因为相机就是 25 FPS —— 我们用了这个帧预算的 79%,
> 换 30 FPS 的相机还能跟; NPU 那 48% 的占用说明还有一半余量。"
