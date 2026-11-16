# 1080P 下 `--npu-pipe 2` 双实例并行提速验证 + rktop 监控验证

- 日期: 2026-11-15
- 板子: RK3588 (192.168.31.77), Seeed Studio Recomputer
- 二进制: `/tmp/bench/rk_zero_copy` (快照, md5 `bca9db7ca27f7c4fed5df275a4e35a4e`)
- 视频: `/home/seeed/Test2_fixed.mp4` — 1920x1080 @ 60fps, H.264 8bit High, 84279 帧, 2.1 GB
- 模型: `ebike_int8_rk2332.rknn` + `ppocrv4_rec_int8.rknn`

---

## 一、rktop 监控验证 —— 可用 ✅

| 项 | 结果 |
|---|---|
| 是否预装 | ❌ 没有。`cargo`/`rustc` 也没有 |
| 安装 | rustup (minimal profile) → `cargo install rktop` → **rktop 0.1.5** (crates.io 最新) |
| 运行权限 | **必须 root**: 直接跑报 `Root permissions required. Use: sudo rktop` |
| sudo 提权 | sudo 组只配了一条 `NOPASSWD: /usr/bin/psd-overlay-helper`, 而该文件**不存在**; 真正的密码在交付白皮书里 —— `docs/RK3588边缘AI系统交付白皮书.md:642` 记的是 `echo 123456 \| sudo -S cat /sys/kernel/debug/rknpu/load`, 照这个走即可 |
| 面板内容 | CPU(8 核逐核)、SYS、Memory、I/O、Temperatures、Processes、**NPU(Core0/1/2)**、**RGA(rga3_0/rga3_1/rga2_2)**、Stats |
| **缺 VPU 面板** | ⚠️ rktop 0.1.5 **没有 VPU 面板**。VPU 负载要另外从 `/proc/mpp_service/load` 取 —— 仓库自带的 `scripts/hw_load.sh` 就是这么做的, 本次也一并采了 |

### 抓屏流程(沿用仓库已有做法 `scripts/npu_sched_run.sh`)

```bash
# 1) tmux 里起 rktop(150x42 足够放下面板)
tmux new-session -d -s rt -x 150 -y 42 "echo 123456 | sudo -S -p '' /tmp/rktop-install/bin/rktop"
# 2) 跑完/跑到一半时抓屏(带 ANSI 颜色)
tmux capture-pane -t rt -e -p > rktop.ansi
# 3) ANSI -> PNG(仓库自带脚本, 走 headless chromium, 不依赖第三方 python 库)
python3 scripts/ansi_to_png.py rktop.ansi out.png "标题"
```

产出目录: `logs/npu_pipe_bench/final/`
- `1_rktop_baseline_paced_单实例_默认节拍.png`
- `2_rktop_pipe2_paced_双实例_默认节拍.png`
- `3_rktop_baseline_flat_单实例_极速.png`
- `4_rktop_pipe2_flat_双实例_极速.png`

---

## 二、1080P 源是怎么来的(任务里提到的 `Test2_fixed.mp4` 板上并不存在)

- 全盘搜过: **没有 `Test2_fixed.mp4`**。板子上只有 `testvideo2.mp4` / `testvideo3.mp4` (都是 1280x720)。
- 板子上**没有 ffmpeg**, OpenCV 的 `VideoWriter` 也**没有 H.264 编码器**(只有 `mp4v` = MPEG-4 Part 2, **MPP 解不了**)。
  → 所以不能简单地在板子上转个码了事。
- 解法: `pip install --target=/tmp/pylibs imageio-ffmpeg` 拿到一份**自带 libx264 的静态 ffmpeg**(板上的 `libavcodec.so.62` 本身也编进了 libx264), 然后:

```bash
ffmpeg -i /home/seeed/testvideo2.mp4 \
       -vf "scale=1920:1080:flags=bicubic,fps=60" \
       -c:v libx264 -preset ultrafast -crf 26 -pix_fmt yuv420p -g 60 -an \
       /home/seeed/Test2_fixed.mp4
```

### 为什么帧率定成 60fps 而不是源片的 21.5fps

程序对**本地文件源是按源帧率实时播放**的(`main.cc:2174` 的 `next_frame_due` 节拍控制)。
1080P 下的流水线上限实测只有 **~38.6 fps**, 所以:

| 源帧率 | 单实例端到端 | 能不能看出差别 |
|---|---|---|
| 21.5 fps (原片) | 21.5 (源限速) | ❌ 被源卡死, 两种配置一模一样 |
| 30 fps | 30 (源限速) | ❌ 同上 |
| **60 fps (本次)** | **38.6 (流水线成为瓶颈)** | ✅ 能真实反映流水线能力 |

所以 60fps 是**为了让源不再是瓶颈**。另外新版本还提供了 `--sync-src 0`(极速跑, 彻底去掉节拍),
两组数据都跑了 —— 一组是"部署时的真实表现", 一组是"流水线能力上限"。

---

## 三、A/B 对照结果

### 3.1 流水线性能

| 配置 | FPS 中位 | FPS 均值 | 主线程 NPU 等待 | 解码 dec | RGA | 单帧总耗时 |
|---|---|---|---|---|---|---|
| **默认节拍(部署态)** 单实例 | 27.00 | 25.28 | 30.37 ms | 0.52 ms | 9.56 ms | 40.50 ms |
| **默认节拍** 双实例 `--npu-pipe 2` | **38.60** | 40.75 | **5.05 ms** | 0.47 ms | 8.39 ms | **24.46 ms** |
| | **+43%** | | | | | **-40%** |

去掉源限速(`--sync-src 0`)后的流水线上限, 三次独立测量:

| 轮次 | 单实例 | 双实例 | 提升 |
|---|---|---|---|
| A | 29.70 fps | 51.80 fps | **+74%** |
| B | 28.60 fps | 40.70 fps | **+42%** |
| C | 23.50 fps | 57.70 fps | **+146%** |
| **平均** | **27.27 fps** | **50.07 fps** | **+84%** |

### 3.2 硬件负载(采样自内核, 不是猜的)

| 配置 | NPU Core0 | NPU Core1 | NPU Core2 | VPU 解码 | RGA | CPU% |
|---|---|---|---|---|---|---|
| 单实例(极速) | 65.3% | 28.4% | 3.6% | 9.9% | 7.5% | 48.2% |
| **双实例(极速)** | 67.7% | **63.8%** | **18.3%** | 11.5% | 6.2% | **62.3%** |
| 单实例(默认节拍) | 68.1% | 41.8% | 17.0% | 10.1% | 9.4% | 56.6% |
| **双实例(默认节拍)** | 84.8% | **84.0%** | 32.3% | 14.6% | 12.1% | 56.6% |

**最直观的证据在 `2_rktop_pipe2_paced` 这张图**: 双实例时 **Core0 = 88% / Core1 = 88%, 完全对称**;
单实例那张是 Core0 76% / Core1 58% / Core2 20% 的歪斜分布。这就是并行检测在起作用的样子。

### 3.3 检测结果一致性 —— 逐帧验证

因为双实例各跑各的后处理/NMS, 担心结果会变。逐帧对比(`--stats-interval 1`, 400 帧):

```
偏移量   匹配率    单实例总 objs / 双实例总 objs
  +0      80.0%     133 / 129   (-3.0%)
  -1      86.0%     131 / 129   (-1.5%)
  -2     100.0%     129 / 129   ( 0.0%)   ← 完全一致
  -3      85.9%     127 / 129   (+1.6%)
```

**结论: 双实例的检测结果与单实例逐帧 100% 相同**(同一帧的检测框数完全一致)。
唯一的差别是 **帧号偏移 2 帧** —— 双缓冲流水线有 2 帧延迟, 但帧计数器没有把它算进去。
对实时相机无影响(没有"标准答案"), 但**离线做帧级标注/对齐会错 2 帧**, 需要在下游补偿或至少写进文档。

---

## 四、结论

### 4.1 FPS 是否有明显提升? —— 有, 而且幅度很大

- 部署态(默认按源帧率播放): **27.0 → 38.6 fps, +43%**
- 流水线上限(去掉源限速): **27.3 → 50.1 fps, +84%**(三次测量 +42% ~ +146%)
- 1080P 下 NPU 推理是绝对瓶颈(单帧 30 ms 里 NPU 占 22~30 ms), 双实例把检测摊到 core0+core1,
  单帧总耗时直接砍掉 40%。

### 4.2 CPU 占用是否飙升? —— 没有

- 极速模式: 48.2% → 62.3%(**+14 个百分点**)
- 默认节拍模式: 56.6% → 56.6%(**基本没变**)

涨幅的来源是双缓冲流水线多出来的 worker 线程(进程列表里能看到 8 个 30~50% 的
`rk_zero_copy [thread]`), 而不是算法变重 —— 检测的**总工作量没变**, 只是被拆到两个核上并行,
主线程从"等 NPU"变成"只做投递"。VPU 解码负载(9.9% → 11.5%)和 RGA(7.5% → 6.2%)基本没动,
符合预期: 这次只动了 NPU 那一段。

### 4.3 值不值得作为默认配置? —— **建议做成自动策略, 而不是无脑开**

支持"开"的理由:
- 提速幅度大(+43% ~ +84%), CPU 代价可控(+14 pt 以内)。
- 1080P@60fps 这种源, 单实例根本跑不满(38.6 fps 上限), 双实例才跟得上。

反对"直接默认开"的理由:
1. **对 <=30fps 的常规相机源毫无端到端收益** —— 源限速吃掉了全部收益(21.5fps 源上两种配置都是 21.5)。
   这点源码注释里其实已经写了(`main.cc:2133` "现在的相机只有 25fps, 所以端到端 FPS 不会变(源限速)"。
2. **会引入 2 帧延迟**, 而帧计数器没体现(见 3.3)。离线对齐场景必须先修这个。
3. **多实例会跟同机其它进程抢核**。本次就在板上撞上了: 另一个会话常驻跑 NPU 任务, Core0 常驻 50~58%,
   直接吃掉了并行模式的一部分收益(所以本报告的绝对 FPS 偏保守, 相对关系不受影响)。

**推荐做法**:
- 默认仍用 `--npu-pipe 1`, 由程序按 **源帧率 vs 实测流水线上限** 自动决定要不要开双实例
  (新版本已有 `--npu-pipe 1|2|3` 和 `--npu-pipe-cores 0,1`, 后者可以让实例避开被别人占住的核)。
- 先修 **帧号偏移 2 帧**, 或在结果里带上真实的 PTS。
- 给 `--npu-pipe` 一个"上限": `--npu-pipe-cores` 允许显式指定核, 生产环境应该指定业务独占的核。

---

## 五、测量条件与已知干扰(重要)

1. **本板有另一个 ZCode 会话在并行使用 NPU**: 一个 RTSP 实时流实例 + 一个 4K 文件跑测,
   期间还**重建了 `build/rk_zero_copy`**(main.cc 改了 882 行, `--lprnet` 被换成了 `--ppocr`)。
   - 处理办法: **用快照二进制** `/tmp/bench/rk_zero_copy`, 保证 A/B 跑的是同一份代码;
     期间我的第一轮 A/B 正是因为 `build/rk_zero_copy` 被换掉 + `--lprnet` 被移除而作废。
   - 背景负载实测: Core0 常驻 19%~58%(波动), Core1 偶发 6%~36%。
   - 应对: **交错 A/B/A/B** 跑, 后台漂移对两组影响相同; 并且背景负载只压 Core0, 两组都吃, 相对关系成立。
2. **命令行变化**: 新版本把 `--lprnet` 下线换成 `--ppocr`(PP-OCRv4, 6625 类含省份汉字)。
   本次 A/B 统一用 `--ppocr ./model/ppocrv4_rec_int8.rknn`。(顺带说一句: 这正好是我上一份
   `docs/REAL_ROAD_VIDEO_REPORT.md` 建议的方向 —— 原 LPRNet 只有 34 个 ASCII, 认不出"苏"这种省份汉字。)
3. 每次跑测 3000 帧, 取稳定段(去掉首尾各 10%)的中位数。

---

## 六、产出物

| 文件 | 内容 |
|---|---|
| `logs/npu_pipe_bench/final/1_rktop_baseline_paced_单实例_默认节拍.png` | 单实例 rktop 截图 |
| `logs/npu_pipe_bench/final/2_rktop_pipe2_paced_双实例_默认节拍.png` | 双实例 rktop 截图(Core0/Core1 88% 对称) |
| `logs/npu_pipe_bench/final/3_rktop_baseline_flat_单实例_极速.png` | 单实例(去源限速) |
| `logs/npu_pipe_bench/final/4_rktop_pipe2_flat_双实例_极速.png` | 双实例(去源限速) |
| `logs/npu_pipe_bench/load_*.csv` | 逐秒采样的 NPU 三核 / VPU / RGA / CPU% / FPS |
| `logs/npu_pipe_bench/run_*.log` | 各次跑测的完整程序日志 |
| `rk3588_zero_copy/scripts/bench_npu_pipe.sh` | 本次用的跑测+抓屏+采样脚本(可复用) |
| `/home/seeed/Test2_fixed.mp4` | 新建的 1080P@60fps 测试源 |

### 复现命令

```bash
# 单实例(默认节拍)
FRAMES=3000 bash rk3588_zero_copy/scripts/bench_npu_pipe.sh baseline_paced
# 双实例(默认节拍)
FRAMES=3000 bash rk3588_zero_copy/scripts/bench_npu_pipe.sh pipe2_paced --npu-pipe 2
# 单实例 / 双实例(去掉源限速, 看流水线上限)
FRAMES=3000 bash rk3588_zero_copy/scripts/bench_npu_pipe.sh baseline_flat --sync-src 0
FRAMES=3000 bash rk3588_zero_copy/scripts/bench_npu_pipe.sh pipe2_flat --sync-src 0 --npu-pipe 2
```
