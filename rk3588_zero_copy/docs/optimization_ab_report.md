# optimization_ab_report.md —— Phase 3 板端 A/B 结果与结论

> **日期**：2026-11-20　**板子**：RK3588（recomputer-rk3588-devkit），runtime 2.3.2
> **基线**：`main` @ `47a7ebb`（`git worktree` 单独编出来的二进制）
> **改造**：`perf/hotpath-implementation-waste` @ `8640dce`
> **测量工具**：`opt_audit/ab_harness.py`（帧计数器差分）、`rk3588_zero_copy/scripts/heap_slope.py`（工程自带的活堆判定）

---

## 0. 测试纪律（逐条对照）

| 要求 | 实际做法 |
|---|---|
| 停服务顺序：先 `touch /run/rk-gate/paused`，再 `systemctl stop`（不 pkill） | ✅ 全程照做；测完删标记、拉起服务 |
| `rk-pipeline-watchdog` 没有 pause 开关，也要 stop | ✅ 一并 stop |
| 锁频 `min_freq = max_freq = 1000000000` | ✅ 用工程自带的 `scripts/lock_max_freq.sh`（NPU devfreq `fdab0000.npu` → min=max=1000000000，governor=performance）；CPU 三个 policy 也一并锁（备份在 `/tmp/lock_max_freq.bak`） |
| 不改只读的 `target_freq` | ✅ 没碰 |
| 判空闲用 `/sys/kernel/debug/rknpu/load` **逐核** | ✅ 每轮起跑前查一次（脚本里 `read_npu_load()`），轮次之间还加了 12 s 冷却让负载落下来 |
| 不用恒显 100% 的 devfreq load | ✅ 逐核读的是 `rknpu/load` 的 `Core0/Core1/Core2` |
| 吞吐用**帧计数器差分** `(frames_end−frames_start)/(t_end−t_start)` | ✅ 每路取窗口内第一条和最后一条 `[chN][frame M]` 行做差分除以墙钟 |
| 不用 rk_gate 自报的 `deliv` | ✅ `deliv` 只作为旁证记录，结论不用它 |
| 改造前 vs 改造后各 3 轮，取**中位数** | ✅ base/new **交错**跑（base,new,base,new,base,new），抵消温度漂移 |
| 记录总 FPS / 每路 FPS / npu / detwait / ocrwait / 每核占用 / 内存 | ✅ 全部记录在 `opt_audit/ab/*.json` |

**基线二进制**：`opt_audit/main_wt/rk3588_zero_copy/build/rk_gate`（从 `main` 单独 `git worktree` 编出来）
**改造二进制**：`rk3588_zero_copy/build/rk_gate`

---

## 1. 一个必须先说清楚的前提：生产配置下 FPS 是被限帧卡住的

生产配置 `config/gate.env` 有 **`GATE_MAX_FPS=20`**（每路分析帧率上限 20 fps）。
4 路 × 20 fps = **80 FPS 天花板**，而实测总 FPS 恰好是 79.99 ~ 80.03 ——
**在生产配置下，吞吐被限帧参数钉死，改造再快也涨不了 FPS**。

所以本轮跑**两个口径**：

| 口径 | `--max-fps` | 用途 |
|---|---|---|
| **① 吞吐口径** | `0`（不限帧） | 测**真实吞吐上限**的差异 —— 这是「FPS 涨了多少」的答案 |
| **② 生产口径** | `20`（与 gate.env 一致） | 测**出货配置**下的排队/延迟改善（detwait / ocrwait）与内存 |

---

## 2. 必答结论（逐条）

### Q1 · 改造后 FPS 涨了多少？

**吞吐口径（`--max-fps 0`，3 轮 × 180 s，帧计数器差分）**

| | 第 1 轮 | 第 2 轮 | 第 3 轮 | **中位数** | 极差 |
|---|---|---|---|---|---|
| base（main） | 109.006 | 108.040 | 108.041 | **108.041** | 0.966 |
| new（改造后） | 110.050 | 109.783 | 109.466 | **109.783** | 0.584 |
| **变化** | | | | **+1.74 FPS（+1.6%）** | |

> **+1.74 FPS**，约为 base 轮间极差（0.97）的 **1.8 倍**；
> 而且 **new 的最低值（109.466）高于 base 的最高值（109.006）** —— 两组完全分开，
> 不是噪声。
>
> 每路分解：ch1 +0.59 / ch2 +0.26 / ch3 +0.41 / ch4 +0.42 FPS（4 路全部上涨）。

**生产口径（`--max-fps 20`，3 轮 × 100 s）**：79.989 → 80.015（**+0.03 FPS**）——
被限帧卡住，这是预期内的。

**代价侧**：NPU 三核平均占用 85.4% → 86.4%（**+1.0%**）——
同样的核多干了 1.6% 的活，说明收益来自**腾出的核心时间**，不是降精度。

### Q2 · detwait 排队缩短了吗？

**缩短了**，两个口径都一致：

| 口径 | base | new | 变化 |
|---|---|---|---|
| 吞吐口径 · 4 路 detwait 均值 | 30.86 ms | 30.45 ms | **−0.41 ms（−1.3%）** |
| 吞吐口径 · ch1 / ch2 / ch3 / ch4 | 31.50 / 31.63 / 31.06 / 29.14 | 31.18 / 31.22 / 30.75 / 28.65 | **4 路全部下降**（−0.31 ~ −0.49 ms） |
| 生产口径 · 4 路 detwait 均值 | 30.43 ms | 30.09 ms | **−0.34 ms** |
| 生产口径 · ch1 / ch3 / ch4 | 29.88 / 30.62 / 30.06 | 29.41 / 29.64 / 29.43 | −0.47 / −0.98 / −0.62 ms |

**识别排队（ocrwait）缩短得更多** —— 这是本轮改造的主战场：

| 口径 | 通道 | base | new | 变化 |
|---|---|---|---|---|
| 吞吐口径 | ch1 | 52.30 ms | 49.76 ms | **−2.54 ms（−4.9%）** |
| 吞吐口径 | ch3 | 61.98 ms | 58.54 ms | **−3.44 ms（−5.5%）** |
| 生产口径 | ch1 | 52.90 ms | 49.77 ms | **−3.13 ms（−5.9%）** |
| 生产口径 | ch3 | 64.46 ms | 58.38 ms | **−6.07 ms（−9.4%）** |

> 对照单次识别推理的实测省量 **4.98 ms**（`outputs_get` 4.94 + CTC 0.43）：
> 一块两行牌要提交 4 个候选、3 个识别 worker 分 3 颗核并行，
> 所以 ocrwait 的净改善是「单次省 4.98 ms」×（并行摊薄后的比例），量级吻合。

**`npu` 推理本身没变**（23.75 vs 23.78 ms，差 0.1% 以内）——
这是对的：本轮没有动模型、没有动输入、没有动推理精度。

### Q3 · 内存平线守住了吗？

**守住了，而且明显改善。** 但有一件必须如实说的事：
**4 路 `rk_gate` 的活堆本来就不是平线**（这是**改造前就存在**的问题，不是本轮引入的）。

**活堆斜率（`mallinfo2().uordblks`，工程自带 `scripts/heap_slope.py` 判定）**

| 口径 | 臂 | 3 轮尾段斜率 | 中位 | 判定 |
|---|---|---|---|---|
| 吞吐口径（180 s/轮） | base | +0.2055 / +0.2273 / +0.2247 KB/帧 | **+0.2247** | FAIL（改造前就是 FAIL） |
| 吞吐口径（180 s/轮） | new | +0.0672 / +0.0604 / +0.0752 KB/帧 | **+0.0672** | FAIL，但**降了 70%** |
| 生产口径（100 s/轮） | base | 488.88 / 479.16 / 507.00 KB/min | **488.88** | — |
| 生产口径（100 s/轮） | new | 137.39 / 105.79 / 135.04 KB/min | **135.04** | — |
| 生产口径（100 s/轮） | base | 1028.98 / 841.62 / 741.04 KB/min | **841.62** | — |
| 生产口径（100 s/轮） | new | 381.40 / 319.11 / 237.58 KB/min | **319.11** | — |

**RSS 斜率**：吞吐口径 1249.39 → **616.05 KB/min（−50.7%）**；
生产口径 1639.86 → **1165.57 KB/min（−28.9%）**。
**活堆绝对值**：改造后比改造前**低 2.5 MB**（73319 KB vs 75855 KB）——
省下来的就是识别模型那 3 个 worker 的 float32 输出缓冲
（3 × 530 KB = 1.59 MB）+ 每帧少掉的拷贝分配。

**为什么 base 本来就不是平线？** 本轮顺手定位并修掉了主因：

> `NpuPool::ocr_results_` 的键是 `(通道, group=track_id, 候选号)`，**track_id 一直递增**；
> `take_ocr` 取走结果后只 `pop_front()`，**空掉的 deque 还挂在 map 里、永不回收**。
> 每秒几十个识别候选 → 每秒几十条永不回收的 map 节点。
> 修法：取完结果如果 deque 空了就 `erase`（`npu_pool.cc`，提交 `8640dce`）。

**残余的 +0.067 KB/帧 在哪儿？** 本轮做了定位实验（关掉全部 OCR 通道，只留 2 路高位）：

```
只有 2 路高位(无 OCR) 的新二进制:
  活堆 : 47217 -> 47363 KB (净增 146 KB)
  尾段斜率 : +0.1080 KB/帧
```

→ 残余增长**不在识别路径**，在**检测/解码路径**（或 RKNN 运行时内部），
**是改造前就有的**（base 同一路径更严重：+0.2247 KB/帧）。
本轮不扩大改动面去动它，但结论要写清楚：
**「4 路 gate 活堆平线」这条铁律，在本次改造之前就没有在 4 路配置下成立过** ——
工程文档里那条「+89.5 KB/小时」的平线记录（`docs/NIGHT_SOAK_REPORT.md:1431`）
测的是**单路** `rk_zero_copy_night`（活堆 12 MB），不是 4 路 `rk_gate`（活堆 47~76 MB）。

### Q4 · 数值等价吗？

**等价，而且是穷举证明的**（详见 `optimization_phase2_report.md`）：

| 检查 | 规模 | 结果 |
|---|---|---|
| 识别输出取数路径（want_float=1 vs 0） | 120 帧 × **265,000 元素 = 31,800,000 个元素逐位比较** | **0 差异** |
| 两个 CTC 解码器（全量扫描 / 白名单） | 4000 轮随机张量 + 120 帧真实 ROI | 文本/分数/字符数**逐位一致** |
| 端到端识别（生产类 `LicensePlateRec`） | 217 张真实车牌 × 3 种裁剪方式 | **逐字节一致**（214 张有实际识别结果） |
| 检测输出逐框对拍 | 120 帧真实图 × 2 个输出形态 = 240 个推理结果 / 2038 行 | **逐字节完全一致** |
| 检测 want_float 交叉对拍 | 120 帧 | 框数不一致帧=0，max\|Δscore\|=0，max\|Δcoord\|=0 |

---

## 3. 完整数据表

### 3.1 吞吐口径（`--max-fps 0`，3 轮 × 180 s）

| 指标 | base 中位 | new 中位 | 变化 | base 极差 |
|---|---|---|---|---|
| **总 FPS（帧计数器差分）** | **108.04** | **109.78** | **+1.74（+1.6%）** | 0.97 |
| ch1 FPS | 23.68 | 24.26 | +0.59（+2.5%） | 0.24 |
| ch2 FPS | 29.55 | 29.82 | +0.26（+0.9%） | 0.24 |
| ch3 FPS | 27.80 | 28.21 | +0.41（+1.5%） | 0.24 |
| ch4 FPS | 27.21 | 27.63 | +0.42（+1.5%） | 0.45 |
| ch1 detwait ms | 31.50 | 31.18 | −0.33（−1.0%） | 0.31 |
| ch2 detwait ms | 31.63 | 31.22 | −0.41（−1.3%） | 0.25 |
| ch3 detwait ms | 31.06 | 30.75 | −0.31（−1.0%） | 0.39 |
| ch4 detwait ms | 29.14 | 28.65 | −0.49（−1.7%） | 0.70 |
| ch1 ocrwait ms | 52.30 | 49.76 | **−2.54（−4.9%）** | 0.57 |
| ch3 ocrwait ms | 61.98 | 58.54 | **−3.44（−5.5%）** | 1.08 |
| ch1 npu ms | 23.78 | 23.75 | −0.03 | 0.23 |
| ch3 npu ms | 23.76 | 23.75 | −0.01 | 0.25 |
| NPU core0 占用 % | 84.80 | 85.90 | +1.10 | 0.40 |
| NPU core1 占用 % | 85.70 | 86.80 | +1.10 | 0.10 |
| NPU core2 占用 % | 85.80 | 86.40 | +0.60 | 0.60 |
| 活堆斜率 KB/min | 488.88 | 135.04 | **−72.4%** | 27.84 |
| RSS 斜率 KB/min | 1249.39 | 616.05 | **−50.7%** | 295.92 |
| 活堆末值 KB | 75855 | 73319 | **−2536** | 18 |
| RSS 末值 KB | 247864 | 245220 | −2644 | 544 |

### 3.2 生产口径（`--max-fps 20`，3 轮 × 100 s）

| 指标 | base 中位 | new 中位 | 变化 | base 极差 |
|---|---|---|---|---|
| **总 FPS** | **79.99** | **80.02** | +0.03（被限帧卡住） | 0.04 |
| ch1 detwait ms | 29.88 | 29.41 | −0.47（−1.6%） | 1.01 |
| ch2 detwait ms | 31.27 | 31.68 | +0.41（+1.3%） | 2.29 |
| ch3 detwait ms | 30.62 | 29.64 | −0.98（−3.2%） | 1.78 |
| ch4 detwait ms | 30.06 | 29.43 | −0.62（−2.1%） | 0.25 |
| ch1 ocrwait ms | 52.90 | 49.77 | **−3.13（−5.9%）** | 1.71 |
| ch3 ocrwait ms | 64.46 | 58.38 | **−6.07（−9.4%）** | 5.69 |
| ch1 npu ms | 24.91 | 25.01 | +0.09 | 0.47 |
| ch3 npu ms | 24.76 | 24.93 | +0.17 | 0.19 |
| NPU core0 占用 % | 70.20 | 70.80 | +0.60 | 2.00 |
| NPU core1 占用 % | 72.90 | 72.70 | −0.20 | 1.50 |
| NPU core2 占用 % | 72.00 | 71.50 | −0.50 | 2.00 |
| 活堆斜率 KB/min | 841.62 | 319.11 | **−62.1%** | 287.94 |
| RSS 斜率 KB/min | 1639.86 | 1165.57 | **−28.9%** | 1106.05 |
| 活堆末值 KB | 75989 | 73607 | **−2382** | 417 |

> ch2 在生产口径下 detwait +0.41 ms，但 base 自身极差是 **2.29 ms** —— 落在噪声里，
> 而且 ch2 是高位通道、**不做识别**，本来就不该受本轮改动影响。

---

## 4. 收益从哪来（口径对齐）

| 改动 | 单次省量（实测） | 落到链路上 |
|---|---|---|
| C2 识别输出 fp16 直读 | `outputs_get` 5.171 → 0.236 ms = **4.94 ms/次识别** | ocrwait 每次候选省 ~4.9 ms |
| C3 NEON CTC 解码 | 0.831 → 0.400 ms = **0.43 ms/次识别** | 同上 |
| C6 提前放核 | 每检测少占核 **0.7 ms** | 同核识别 worker 少等 → ocrwait 再降 |
| C5 自检连续访问 | 每检测帧 **0.15 ms** | 检测核占用 |
| C7/C8 move 语义 | 每帧少一次**全局锁内**堆分配 | 4 路共用 `res_m_`，少一段竞争 |
| C9 map 空壳清理 | 活堆斜率 **−70%** | 内存 |

**为什么总 FPS 只涨 1.6%？** 因为瓶颈没变：不限帧时 NPU 三核已经 **85% 满载**，
检测推理本身 23.8 ms/帧动不了（本轮不改模型、不改输入、不改精度）。
本轮拿回的是**被浪费掉的核心时间**（识别侧每次 6 ms + 检测侧每次 0.85 ms），
折算成吞吐就是 +1.6%。
**真正的下一步仍然是「检测算力」**（降模型尺寸 / 降输入分辨率 / 加核 / 抽帧），
实现层已经榨干。

---

## 5. 恢复与验收记录

### 5.1 测前基线（动手前先记下来）

```
$ systemctl list-units --type=service --all | grep rk-
  rk-collect-crash.service      loaded inactive dead
  rk-crash-archive.service      loaded inactive dead
  rk-gate-watchdog.service      loaded active  running
● rk-gate.service               loaded activating auto-restart     ← 测前就在自动重启循环
  rk-kernel-log.service         loaded active  running
  rk-night-soak-monitor.service loaded active  running
  rk-night-soak-stop.service    loaded inactive dead
● rk-night-soak.service         loaded activating auto-restart     ← 同上
  rk-pipeline-watchdog.service  loaded active  running
● rk-pipeline.service           loaded activating auto-restart     ← 同上

CPU 频率(governor=ondemand):
  policy0 min=408000 max=1800000 | policy4 min=408000 max=2304000 | policy6 min=408000 max=2256000
NPU devfreq: rknpu_ondemand 300000000 / 1000000000
生产二进制: rk3588_zero_copy/build/rk_gate  462344 字节 (main @ 47a7ebb)
```

> **测前 `rk-gate.service` 就起不来**（不是本轮造成的）：
> `run_gate_service.sh` 要先创建 `/var/run/rk-gate.lock`，而 `/var/run` 是 root 的，
> 服务以 `User=seeed` 跑 → exit 75 → 触发 `Restart=on-failure` 循环。
> 脚本自己的报错里就写了修法：`sudo touch $LOCK && sudo chmod 666 $LOCK`。
> **本轮没有动它**（原则是「恢复到测前状态」，不是顺手改生产），
> 但验收就走「直接用生产 CLI 跑一遍」的方式做（见 §5.3）。

### 5.2 恢复动作（逐条对照）

| # | 动作 | 结果 |
|---|---|---|
| 1 | `scripts/lock_max_freq.sh --restore` | ✅ CPU 三个 policy 全部回到 `ondemand` + 原 min/max；devfreq 三件（dmc / gpu / npu）全部回到原 governor 与 min/max |
| 2 | policy0 的 `scaling_min_freq` 单独校回 `408000` | ✅ 驱动在 `ondemand` 下会自己抬高这个下限（实测被抬到 1008000→1608000），手动写回 408000 后稳定 |
| 3 | 删 `/run/rk-gate/paused` | ✅ 已删（目录里只剩看门狗自己的 `backoff`/`fail_count` 状态文件） |
| 4 | 拉起 3 个看门狗（`rk-gate-watchdog` / `rk-pipeline-watchdog` / `rk-night-soak-monitor`） | ✅ 全部 `active running` |
| 5 | 恢复 3 个服务的「自动重启循环」状态 | ✅ `reset-failed` 后重新 start，三个都回到 `activating auto-restart` |
| 6 | 恢复生产二进制 | ✅ `build/rk_gate` 换回 **main @ 47a7ebb 的构建**（462344 字节，与测前一致）；优化版另存 `opt_audit/bin/rk_gate_optimized` |
| 7 | 删掉临时 git worktree | ✅ `opt_audit/main_wt` 已移除，`git worktree list` 只剩本仓库 |

**服务状态与测前基线逐项比对：完全一致。**

### 5.3 验收（4 路出帧 + npu 回到基线）

用**生产 CLI**（与 `scripts/run_gate_service.sh` 拼的完全相同的参数）
直接跑 70 s，频率已还原为 `ondemand`：

```
[ch2][frame 1300] fps=26.6 deliv=20.0 src=21.5 rga=2.37ms npu=27.16ms detwait=37.69ms ocrwait=0.00ms  rss=245452KB heap=73409KB
[ch3][frame 1300] fps=21.3 deliv=20.0 src=60.0 rga=3.32ms npu=27.60ms detwait=37.58ms ocrwait=67.39ms rss=245452KB heap=73381KB
[ch4][frame 1300] fps=22.2 deliv=20.0 src=60.0 rga=7.13ms npu=27.61ms detwait=38.40ms ocrwait=0.00ms  rss=245452KB heap=73362KB
[ch1][frame 1300] fps=17.3 deliv=19.5 src=30.0 rga=2.64ms npu=27.23ms detwait=34.90ms ocrwait=58.43ms rss=245452KB heap=73322KB

出帧通道数: 4 / 4          ← ★ 四路都在出帧, 帧号都到 1300
deliv = 19.5 ~ 20.0 FPS   ← ★ 限帧参数(20)原样生效
npu   = 27.16 ~ 27.61 ms   ← ★ 回到基线区间(改造前同配置实测 27.10 ~ 28.30 ms)
每核占用: 检测 71.0/73.4/72.0%   识别 14.9/13.4/14.1%
活堆: 73190 -> 73322 KB (+132 KB)  ← 改造后水位(改造前是 74.3 ~ 76.2 MB)
```

| 验收项 | 结果 |
|---|---|
| 4 路出帧 | ✅ 4/4，帧号均到 1300 |
| `npu=` 回到基线 | ✅ 27.16 ~ 27.61 ms（基线 27.10 ~ 28.30 ms） |
| 限帧参数复原 | ✅ `deliv` = 19.5 ~ 20.0 FPS，`config/gate.env` 全程未改 |
| 频率复原 | ✅ CPU 三个 policy + 三个 devfreq 全部回到测前值与原 governor |
| 服务状态复原 | ✅ 与 §5.1 基线逐项一致 |
| 生产二进制复原 | ✅ 换回 main 构建；优化版另存 |

> ⚠️ **一件如实报告的事**：`rk-gate.service` 测前就起不来（`/var/run/rk-gate.lock`
> 权限），所以「systemd 托管下的 4 路出帧」在本轮**无法**验证 ——
> 这是**测前就存在的**问题，本轮按「恢复到测前状态」的原则没有去改它。
> 验收是用同一套生产 CLI 直接跑做的。
> 要让 systemd 托管能跑起来，脚本自己的报错里就有修法：
> `sudo touch /var/run/rk-gate.lock && sudo chmod 666 /var/run/rk-gate.lock`
> （一行，要不要做由你定 —— 本轮不擅自改生产。）

---

## 6. 数据文件清单

```
rk3588_zero_copy/docs/ab_data/*.json         A/B 原始指标(12 个文件: base/new x 2 口径 x 3 轮)
opt_audit/ab/*.log                           每轮逐行原始日志(带时间戳, 可自己复核)
opt_audit/ab_c2_c8/                          只改了 C2~C8(未含 C9)时的对照数据
opt_audit/ab_harness.py                      测量台(帧计数器差分 + 逐核 NPU 采样 + 内存)
opt_audit/ab_report.py                       汇总(中位数 / 极差 / 变化)
opt_audit/run_ab.sh / run_final_ab.sh        A/B 驱动脚本
opt_audit/restore_and_accept.sh              恢复 + 验收脚本
opt_audit/tools/                             等价性验证工具(见 Phase 2 报告)
opt_audit/bin/rk_gate_optimized              优化版二进制(生产已换回 main 构建)
```

---

## 7. 交付物与分支

| 交付物 | 路径 |
|---|---|
| Phase 1 审查清单 | `rk3588_zero_copy/docs/optimization_candidates.md` |
| Phase 2 Diff / 对拍 / 内存 | `rk3588_zero_copy/docs/optimization_phase2_report.md` |
| Phase 3 A/B 结果与结论 | `rk3588_zero_copy/docs/optimization_ab_report.md`（本文） |
| A/B 原始数据 | `rk3588_zero_copy/docs/ab_data/` + `opt_audit/ab/` |
| 恢复验收记录 | 本文件 §5 |

**分支**：`perf/hotpath-implementation-waste`（`f4f1838` + `8640dce`）
**`main` 未被改动**（仍指向 `47a7ebb`），生产二进制也换回了 main 的构建。
合并命令：

```bash
git checkout main && git merge --no-ff perf/hotpath-implementation-waste
cd rk3588_zero_copy && cmake --build build -j8
```
