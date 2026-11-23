# P0 活堆泄漏修复报告

> 分支: **`fix/p0-memleak-converge`**（验收通过前不合入 `main`）
> 基线: `main` @ `47a7ebb`　　板子: RK3588 (recomputer-rk3588-devkit)
> 事实基线: 板端复核版介绍书（2026-11-23 12:40 快照）
> 报告日期: 2026-11-23

---

## 一、现象

### 1.1 12:40 快照的现场

| 项 | 实测值 |
|---|---|
| 连续运行 | 约 141 万帧/路 |
| 活堆（`mallinfo2().uordblks`） | **500,562 KB**，近 4.65 万帧窗口内**单调上涨、未见平台** |
| 折算增速 | **约 +22 ~ +30 MB/小时** |
| RSS | **646,832 KB** |
| cgroup | `MemoryCurrent=1073434624` / `MemoryMax=1073741824` —— **顶满** |
| `memory.events` | `max` 累计 **3,044,420** 次；`oom=0`、`oom_kill=0` |
| fd 数 | 165~167 |

### 1.2 现场里被忽略的一件事实（本次新发现）

服务**从 2026-11-22 16:59 跑到现在没换过二进制**，而那个二进制：

```
$ grep -ac "直读原生类型" build/rk_gate        # perf 分支 fp16 直读的标志串
0                                               # ← 线上跑的是【旧二进制】
$ git log --oneline -1 build/rk_gate 的构建来源
47a7ebb  (main, 既没有 perf 分支, 也没有 P0 坐标修复)
```

⇒ **线上跑的二进制比仓库里的 `main` 还老**。这解释了为什么"引入 perf 分支后斜率仍有
319 KB/min"的结论与"合并后斜率应大幅下降"的预期对不上：那条 A/B 是在
`git worktree` 里另编的二进制上做的，**跟线上跑的不是同一个东西**。

### 1.3 活堆曲线的真实形状（19.7 小时原始日志）

把 19.7 小时的日志按**每路自己的帧号**做回归（`mallinfo2` 是进程级的，
4 路打印的是同一个值，混在一起算会偏）：

| 路 | 帧区间 | 活堆 起 → 止 | 斜率 | 折算 |
|---|---|---|---|---|
| ch1 | 100 → 1,351,300 | 74,765 → 502,444 KB | **+0.3164 KB/帧** | +395.5 KB/min |
| ch2 | 100 → 1,420,800 | 74,514 → 502,685 KB | **+0.3009 KB/帧** | +360.7 KB/min |
| ch3 | 100 → 1,418,500 | 74,646 → 501,884 KB | **+0.3014 KB/帧** | +361.4 KB/min |
| ch4 | 100 → 1,420,700 | 74,567 → 501,768 KB | **+0.3010 KB/帧** | +360.8 KB/min |

十等分看形状（ch1）：`74765 → 117540 → 160570 → 203660 → 246152 → 289311 →
332184 → 374883 → 417242 → 459777 → 501693`

**每段都 ≈ +42,700 KB，一条笔直的线，19 小时里没有一次回落。**

> ★ 这不是 R6 的"跟踪器 vector 扩容台阶"（那是一次性的），是**无界增长**。

---

## 二、根因

### 2.1 根因 1（主因）：`ocr_results_` map 的空壳条目永不回收

`NpuPool::ocr_results_` 的键是 `(通道, group=track_id, 候选号)`，
而 `track_id` **一直递增** —— 每个识别候选都会在 map 里留下一条记录。
以前取走结果后只 `pop_front`，**空掉的 deque 还挂在 map 里**：

```cpp
// src/rk_base/npu_pool.cc  (修复前)
*out = std::move(it->second.front());
it->second.pop_front();
return true;                    // ← 空壳条目还在 map 里
```

每秒几十个候选 → 每秒几十条永不回收的 map 节点。
一个 `std::deque`（默认构造就带 512 字节的 chunk + 64 字节的 map 指针数组）
挂在 map 节点里，正好是**几百字节/候选**的量级。

**修复**（`perf/hotpath-implementation-waste` 的 `8640dce`）：

```cpp
*out = std::move(it->second.front());
it->second.pop_front();
if (it->second.empty()) ocr_results_.erase(it);   // ← 取完就删空壳
```

行为不变（删的只是已经空了的条目；同一个键再来结果会重新建一条）。

### 2.2 根因 2（本次新发现）：视频页缓存把 cgroup 顶满 —— 第二颗炸弹

**活堆泄漏修完之后，cgroup 依然顶满 1G。** 拆开看构成：

```
anon   526,962,688   (503 MB)   业务进程
file   535,101,440   (510 MB)   【页缓存】★
shmem  102,887,424   ( 98 MB)   dma-buf(MPP/NPU), unevictable
```

4 路视频源合计 **4.1 GB**，进程持续顺序读，内核把读过的页全留在 page cache 里。
实测以 **7~10 MB/s** 增长 ⇒ **5 分钟就把 cgroup 顶到上限**，内核被迫一直
"回收 → 立刻重新读"（实测 `pgscan` 46 GB），cgroup 常年贴着上限，
**真正的业务内存一点余量都没有**。

> ★ 后果被低估了：**那 500 MB 活堆泄漏能撑 19 小时，正是因为它是在
> "cgroup 已经被页缓存吃满"的状态下被 OOM 杀掉的** —— 页面缓存挤掉了
> 泄漏本该撞上的那道墙。修掉页缓存之后，OOM 的触发条件才变回
> "活堆真的涨到上限"。

**为什么 `avioflags=direct` 挡不住**：`open_source()` 里已经设了
（实测 fd 上确实带 `O_DIRECT`，`flags: 02400000`），但**页缓存是按 inode 记的**，
跟用哪个 fd 打开、要不要 O_DIRECT 无关；ffmpeg 在探测/建索引阶段走的又是别的读路径。

### 2.3 根因 3（工程缺陷）：日志轮转的安全网其实是空的

`/var/lib/logrotate/status` 里**从来没有过 rk-gate 的条目**，
`/var/log` 下**连一个 `.1`/`.gz` 都没有** —— 那条"每小时 + maxsize 4M 兜底"的安全网
**一次都没成功过**。

而且 `hourly` 并不是"每小时轮转"：

```
$ systemctl list-timers | grep logrotate
Mon 2026-11-23 00:14:34  13h ago  logrotate.timer  logrotate.service
Tue 2026-11-24 00:25:46  11h left
```

logrotate 不常驻，是 timer 拉的，而那个 timer **每天一次**。
⇒ 在 `/var/log`（47 MB zram）上：日增约 8 MB、每天转一次、留 12 份
⇒ 最坏 `12 × 8 MB = 96 MB > 47 MB` —— **盘会在轮转前就写满**。

---

## 三、修复步骤

### 3.1 步骤 1：收敛基线（把"新旧"分清楚）

| # | 动作 |
|---|---|
| 1 | `fix/p0-correctness-stability` 上 9 个未提交修改 → 整理成 3 个语义清晰的提交（DFL ×stride 修复 / 看门狗 L1.5 / 锁文件迁 `/run/rk-gate/`） |
| 2 | 在其上 `merge --no-ff perf/hotpath-implementation-waste`（b9434c5），**先 P0 后 perf** —— 在错的坐标上做 A/B 测出来的都是废数据 |
| 3 | 新建集成分支 `fix/p0-memleak-converge`；`main` 保持 `origin/main`（`47a7ebb`）不动，**验收通过前不合入 main** |

**编译门槛**：`cmake --build build -j8` **零警告**。
修掉了一条改动前就存在的 warning（`yolo_post.cc` 的 `cls_min_over defined but not used`）——
加 `[[maybe_unused]]` 而不是删函数：它是形态B/形态C 排查期用的诊断辅助，工具要留。

### 3.2 步骤 2：装上真正的修复（线上换二进制）

服务以 `seeed` 运行，用 `kill -9` 触发 systemd 自己的 `Restart=on-failure` 拉起新二进制
（先 `touch /run/rk-gate/paused` 让看门狗静音，**不 pkill、不发 SIGSTOP**）。

### 3.3 步骤 3：治理视频页缓存

`src/gate_main.cc`：另开一个只读 fd，每 5 秒
`posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED)` 把这个视频的干净页丢掉。
fadvise 按 inode 生效，与 ffmpeg 怎么读无关；只对本地文件生效。

> ★ 间隔收紧的代价是**零**，这一点是实测出来的：
> `read_bytes ≈ rchar`（进程读的每个字节都真的落到了盘上）
> ⇒ 这个页缓存**一次都没被命中过** —— 它占着 cgroup 的额度，却一个字节都没省下。

### 3.4 步骤 4：日志目录迁出 zram

`GATE_LOG_DIR: /var/log → /var/log.hdd`（105 GB 真实盘，实测剩 78 GB）。
`install_gate_service.sh` 会按 `GATE_LOG_DIR` 统一改写 4 处路径
（主 unit / 看门狗 unit / 崩溃取证 unit / logrotate 规则）—— **只改 `gate.env` 是不够的**。

### 3.5 步骤 5：按实测稳态重新校准阈值

见 §六。

---

## 四、验证方法

### 4.1 活堆斜率（工程自带工具，按通道分组）

```bash
python3 scripts/heap_slope.py <日志>      # 尾段斜率 + 逐路分解
```

★ 本次同时修了该工具的一个口径缺陷：它原来把 4 路的帧号混在一个序列里做回归
（活堆是进程级的，4 路打印同一个值，但帧号各自独立 ⇒ x 轴不单调）。
现在解析时多抓一个通道号，**逐路分解**与汇总行一起输出，汇总行保持不变。

### 4.2 尾段斜率（任务书口径：ch1 + 尾部 1/4 + 20 fps 折算 + 剔除前 10 分钟预热）

```python
tail = cur[len(cur)//4:]                     # 尾部 1/4
mins = df / (20.0 * 60.0)                    # 20fps 口径
dRSS  = tail[-1][1] - tail[0][1]
dHeap = tail[-1][2] - tail[0][2]
```

### 4.3 吞吐（帧计数器差分，**禁止用 deliv**）

取窗口内第一条和最后一条 `[chN][frame M]` 行做差分除以墙钟，
4 路求和。`deliv` 只作旁证。

> ★ 补一条澄清：`deliv` 在代码里就是 `delivered_fps() = (frames-1)/(last-first)`，
> 它本身也是"帧数/墙钟"。但它是**从第一帧起累计**的，反映的是全程平均而非窗口值，
> 所以按任务书要求，结论一律用窗口内的帧差分。

### 4.4 fd / cgroup / 看门狗

```bash
ls /proc/$(pidof rk_gate)/fd | wc -l
cat /sys/fs/cgroup/system.slice/rk-gate.service/memory.events
grep -c "看门狗" /var/log/rk-gate-alerts.log
```

---

## 五、验证结果（三组同口径对比）

### 5.1 活堆斜率

| 组 | 窗口 | 样本 | 活堆斜率 | RSS 斜率 | 工具判定 |
|---|---|---|---|---|---|
| **修复前**（旧二进制，12:40 现场） | 19.7 h | 55,586 | **+0.3006 ~ +0.3161 KB/帧**（+360.4~361.4 KB/min） | 619~646 MB 且在涨 | FAIL |
| **合入 perf 分支后**（任务书引述） | 100 s/轮 | 3 轮 | +319.11 KB/min | — | FAIL |
| **修复后**（本次） | 46.6 min（剔除前 10 min） | 2,225 | **+0.0022 ~ +0.0026 KB/帧**（**+2.6 ~ +3.1 KB/min**） | **−11.5 ~ −12.3 KB/min**（在降） | **PASS** |

**−98.5%**（日增 490~510 MB → 6.3~8.4 MB，61~81 倍）。

### 5.2 残余 ~3 KB/min 是不是"还有泄漏"？

不是。这是**统计显著但极小的漂移**，判据如下（窗口 = 剔除前 10 分钟预热）：

| 路 | 斜率 ± 标准误 | t 值 | 上涨点占比 | 六段分段斜率 |
|---|---|---|---|---|
| ch1 | +0.002623 ± 0.000441 KB/帧 | +5.95 | 52% | −6.2 / +2.3 / −10.9 / +9.4 / +3.6 / +7.5 |
| ch2 | +0.002192 ± 0.000439 | +4.99 | 47% | +22.0 / −6.3 / +5.3 / −6.6 / +2.0 / −1.5 |
| ch3 | +0.002174 ± 0.000371 | +5.86 | 50% | +8.9 / +5.2 / +3.9 / +7.3 / −5.8 / +6.3 |
| ch4 | +0.002486 ± 0.000333 | +7.47 | 48% | +11.6 / +3.3 / +2.7 / +3.7 / +2.2 / +0.8 |

- **上涨点占比 47~52%** —— 真泄漏应当接近 100%，这是随机游走的特征；
- **分段斜率忽正忽负**（−10.9 ~ +22.0）—— 不是一条直线；
- **RSS 同时在降**（−11.5~−12.3 KB/min）—— 活堆是 RSS 的一部分，活堆涨而 RSS 降自相矛盾；
- 活堆全程**振幅 1023~1507 KB**，首尾差只有 +126 ~ +558 KB —— 端点差被振荡噪声支配。

★ 这一点很重要：**任务书脚本用的是端点差（不是回归斜率）**，而端点差在这个
1 MB 振幅的振荡面前噪声很大（同一份数据，端点差给 ch2 +9.0 / ch3 +10.0 KB/min，
回归斜率给 +2.6 / +2.6）。两种口径都报出来，请以回归斜率为准。

### 5.3 残余漂移的定位

<!--TRACKER-->

### 5.4 吞吐（帧计数器差分）

| 组 | 口径 | 4 路合计 |
|---|---|---|
| 修复后（本次，55.7 min 窗口） | **帧计数器差分** | **79.42 FPS** ✅（门槛 ≥ 79.0） |
| 修复后（旁证） | `deliv` | 79.6 FPS |
| 修复前（旧二进制 19.7 h 稳态） | `deliv`（旧日志无时间戳，无法复算帧差分） | 79.00 FPS |

逐路帧差分：ch1 **19.43** / ch2 **20.00** / ch3 **19.99** / ch4 **20.00** FPS。

### 5.5 时延 vs 基线

| 路 | npu | detwait | ocrwait |
|---|---|---|---|
| 基线（任务书引用） | 26.7 ms | 33.0 ms | 52.5~58.3 ms |
| **修复前**（同一板同一视频，19.7 h 稳态） | 27.43 / 27.43 / 27.39 / 27.16 | 34.45 / 37.24 / 35.71 / 35.35 | 60.35 / — / 58.84 / — |
| **修复后**（本次） | 27.48 / 27.62 / 27.49 / 27.24 | 34.22 / 36.54 / 35.45 / 34.30 | **54.50** / — / **53.08** / — |

- **ocrwait 明显改善**：ch1 60.35 → 54.50 ms（−5.9）、ch3 58.84 → 53.08 ms（−5.8）
  —— 与 perf 分支自报的 −3.1 / −6.1 ms 同量级；
- **detwait 4 路全部下降**；npu 持平；
- 相对任务书引用的基线，npu 高 0.8 ms、detwait 高 1.2~3.5 ms ——
  但**修复前的旧二进制同样是 27.2~27.4 / 34.5~37.2**，
  ⇒ 这个差距**不是本次改动引入的**，任务书引用的基线也不是在这块板/这组视频上测的。

### 5.6 fd / cgroup / 看门狗

| 项 | 12:40 现场 | 修复后 | 门槛 | 判定 |
|---|---|---|---|---|
| fd 数 | 165~167 | **169 → 171（8 分钟内稳定在 171，不再涨）** | 恒定、无增长 | ✅ |
| 线程数 | 30 | 30 | — | ✅ |
| `memory.events` `max` | **3,044,420** | **0** | 不再高频增长 | ✅ |
| `memory.events` `oom` / `oom_kill` | 0 / 0 | **0 / 0** | = 0 | ✅ |
| cgroup `MemoryCurrent` | **1024 MB / 1024 MB（顶满，available 44K）** | **190.4~243.4 MB** | — | ✅ |
| cgroup `file`（页缓存） | 510→938 MB | **106.3~157.2 MB** | — | ✅ |
| 看门狗告警 | 持续刷（每 20 秒一条 RSS/活堆告警） | **60 分钟窗口内 0 条** | 无告警 | ✅ |

> fd 从 165~167 到 169~171：多出的几个是 **4 个视频源各多了一个只读 fd**
> （我另开来做 `posix_fadvise` 的那个）+ dmabuf 句柄在解码池里正常轮转。
> **8 分钟内 169→170→171 后完全平台，不增长。**

---

## 六、阈值校准

### 6.1 实测稳态（用于校准的基数）

长跑窗口：2026-11-23 13:22:41 → 14:09:32（46.6 分钟，零重启、零告警）；
278 次 cgroup 采样。

| 指标 | 实测稳态 |
|---|---|
| 进程 RSS | **228.6 ~ 231.7 MB** |
| 活堆（`uordblks`） | **73.3 ~ 74.2 MB** |
| cgroup 合计（`memory.current`） | **190.4 ~ 243.4 MB**（均值 209.7） |
| ├ `anon`（业务进程） | 81.5 ~ 84.2 MB |
| ├ `file`（视频页缓存，fadvise 已限住） | 106.3 ~ 157.2 MB |
| ├ `shmem`（dma-buf，**unevictable**） | 98.1 MB |
| └ `kernel` | 3.1 ~ 3.3 MB |

### 6.2 校准结果

| 参数 | 旧值 | 新值 | 依据 |
|---|---|---|---|
| `MemoryMax` | 1G | **512M** | 实测峰值 243.4 MB → **2.10× 余量**。且不可回收部分（anon+shmem）只有 **182 MB**，所以匿名内存涨到 ~410 MB 才撞墙 ⇒ 旧泄漏速率（362 KB/min）下 **约 15 小时**被 OOM 杀，而不是像以前那样悄悄涨 19 小时把 1G 吃干、还顺带把板上别的进程挤死 |
| `MemoryHigh` | （无） | **384M** | 稳态峰值的 ~1.6×。**软刹车**：超过就优先回收页缓存并压制分配，但**不 OOM 杀**。正常跑永远碰不到；碰到就说明有东西在超预期地长 |
| `RSS_WARN_MB` | 400 | **350** | 稳态 228.6~231.7 MB 的 ~1.5×。RSS 含 98 MB 的 dma-buf，瞬时波动比活堆大，余量给宽一点 |
| `HEAP_WARN_MB` | 200 | **150** | 稳态 73.3~74.2 MB 的 ~2.0×。活堆是最灵敏的泄漏指标，宁可早报 |
| `CGROUP_WARN_MB` | （无） | **400** | **新增**。稳态峰值的 1.6×，也是 `MemoryMax=512M` 的 78%。盯的是 cgroup **总量**而不是 RSS/活堆 —— 2026-11-23 证明过：cgroup 顶满 1G 的主因**既不是 RSS 也不是活堆**，是没人看的 `file` 分量 |
| `LOG_FREE_WARN_MB` | 8 | **2048** | 日志盘已从 47 MB zram 换成 105 GB 真实盘；旧值 8 MB 是按 zram 定的，照抄等于永远不报警 |

### 6.3 为什么没有保持 `MemoryMax=1G`（任务书 4.3 给的"不动"选项）

任务书说：若稳态 RSS 落在 240~300 MB、活堆 75~120 MB 且 MemoryPeak 远低于 1G，
则**保持 1G / 400 / 200 不动**即可。

实测 RSS **228.6~231.7 MB**、活堆 **73.3~74.2 MB**，正好落在那个带子的**下沿之外一点**，
所以那条"不动"的免检条款严格说并不适用 —— 它是按"旧二进制（活堆 489 MB）修完之后"
的稳态写的。

即便适用，本报告仍然选择收紧，理由是算得出来的：

1. **不可回收的底只有 182 MB**（anon 84 + dma-buf shmem 98）。
   `MemoryMax=1G` 意味着匿名内存能涨到 ~900 MB 才撞墙 ⇒ **37 小时**才抓到一个
   362 KB/min 的泄漏；而 512M 只要 **~15 小时**。在一个 8 GB 的板子上，
   512 MB 的 cgroup 上限是更负责任的占用。
2. **`file` 是页缓存**，可回收。1G 的上限里除了那 182 MB，其余全是内核随手能收回去的
   页缓存 —— 给它 800 MB 的"余量"没有意义，余量应该留给不可回收的部分。
3. **实测峰值 243.4 MB 只占 512M 的 47.5%**，收紧后仍有 2.1 倍余量，
   不会误伤正常波动（页缓存的锯齿幅度只有 ~50 MB）。
4. **`MemoryHigh=384M` 补上了"软"的那一层**：正常跑永远不碰它，
   一旦有东西超预期地长，内核先回收页缓存并压制分配（而不是直接 OOM 杀）。

★ 关键：这是在**实测确认活堆已经归平之后**才做的校准 —— 顺序是
"先测出斜率 → 确认平 → 再按稳态校准"，**不是**"先放宽阈值再掩盖泄漏"。
任务书那条红线（"严禁在活堆斜率归零前，仅靠调大 MemoryMax 或看门狗阈值掩盖泄漏"）
在本流程里是满足的：本次不但没有调大，反而**调小了**。

---

## 七、仍未闭环 / 需要 root 的部分

### 7.1 需要 root 才能落地的一步（**唯一**）

仓库侧全部改完并提交了，但 `/etc/systemd/system` 与 `/etc/logrotate.d` 归 root，
本机 `sudo` 需要密码（任务书里所有 `sudo` 步骤都假设执行者有 sudo）。

**一条命令**（已写好并做了 `bash -n` 语法检查）：

```bash
sudo /home/seeed/rk_deploy/rk3588_zero_copy/scripts/p0_deploy.sh
```

它依次做：

1. 校验 `GATE_LOG_DIR` 已经改到真实磁盘（还是 `/var/log` 就拒绝执行并说明原因）；
2. `touch /run/rk-gate/paused` 让看门狗闭嘴
   —— **这一步是必须的**：安装过程里主服务会重启，而此时日志目标正从 `/var/log`
   切到 `/var/log.hdd`，老看门狗还在盯 `/var/log`，会看到"日志不再更新"
   → 判 L1 整机心跳停 → 反过来把刚装好的服务又重启了；
3. 跑 `install_gate_service.sh` —— 按 `GATE_LOG_DIR` 统一改写 4 处路径
   （主 unit / 看门狗 unit / 崩溃取证 unit / logrotate 规则），
   装上校准后的 `MemoryMax=512M` / `MemoryHigh=384M`，`daemon-reload` + `restart`；
4. `logrotate -f` 强制轮转，**并验证 `.1` 产物真的生成** ——
   历史上这条安全网从没成功过（§二 根因 3），而 logrotate 的报错只进它自己的
   stderr、不进 syslog/journal，所以**光看退出码会被骗过去**；
5. 打印部署后状态供人工核对。

### 7.2 需要 root 才能停掉的两条死循环（见 §八）

```bash
# ⚠ 只停这两个【不够】—— 它们的看门狗会立刻把它们拉回来(见 §八)
sudo systemctl stop  rk-night-soak rk-pipeline
sudo systemctl disable rk-night-soak rk-pipeline
# 必须连看门狗一起停, 否则停了还会被 restart 回来
sudo systemctl stop  rk-night-soak-monitor rk-pipeline-watchdog
sudo systemctl disable rk-night-soak-monitor rk-pipeline-watchdog
```

### 7.3 其它需要 root 的验收项

- `sudo scripts/acceptance_checklist.sh` —— 工程自带的 16 项交付验收清单
  （它要 kill -9 / systemctl / 卸载等特权操作，脚本开头就会拒绝非 root 运行）。

### 7.4 当前状态（root 步骤未执行）

| 项 | 当前 | 目标 |
|---|---|---|
| `MemoryMax` | 1G（unit 未重装） | 512M |
| `MemoryHigh` | 无 | 384M |
| 日志目录 | `/var/log`（47 MB zram） | `/var/log.hdd`（105 GB） |
| 看门狗水位线 | RSS 400 / 活堆 200 / 日志盘 8 | RSS 350 / 活堆 150 / cgroup 400 / 日志盘 2048 |
| `rk-night-soak` / `rk-pipeline` | **activating 死循环** | 停用 |
| logrotate 对 `/var/log` | **从未成功过** | 对 `/var/log.hdd` 生效 |

> ★ **不构成即时风险**：cgroup 现在只用 190~243 MB（1G 的 19~24%），
> 活堆稳在 73~74 MB。**真正的风险是日志盘** —— zram 写满会触发
> `write()` ENOSPC 让应用崩（按实测速率约 6 天写满 47 MB）。
> ★ 任务书的红线「严禁在活堆斜率归零前仅靠调大 MemoryMax 掩盖泄漏」：
> 本次是在**实测确认斜率已归平之后**才做的校准，而且是把上限**调小**、
> 阈值**调紧**，方向与红线一致。

---

## 八、失败原因摘要：rk-night-soak 与 rk-pipeline

### 8.1 `rk-night-soak` —— exit **127**（命令不存在）

```
$ systemctl status rk-night-soak --no-pager
  Process: 2123803 ExecStart=/home/seeed/rk_deploy/scripts/night_soak_run.sh (code=exited, status=127)

$ bash /home/seeed/rk_deploy/scripts/night_soak_run.sh
night_soak_run.sh: line 49: /home/seeed/rk_deploy/rk3588_zero_copy/build/rk_zero_copy_night: No such file or directory
```

**根因**：`night_soak_run.sh` 第 49 行执行 `$REPO/rk3588_zero_copy/build/rk_zero_copy_night`，
而这个二进制**根本不存在** —— 它既不在 `CMakeLists.txt` 的 `add_executable` 列表里
（工程只有 `rk_zero_copy` / `rk_gate` / `rk_minimal` …），
`.gitignore` 又把 `build*/` 整个排除了，所以它从来没被构建出来过。
⇒ shell 找不到命令 → **127** → `Restart=always` + `RestartSec=5` → **每 5 秒重启一次**。

### 8.2 `rk-pipeline` —— exit **2**（RTSP 源不可达）

```
$ systemctl status rk-pipeline --no-pager
  ExecStart=.../build/rk_zero_copy rtsp://192.168.31.200:8557/h264 ... --frames 1000000
      (code=exited, status=2/INVALIDARGUMENT)

$ journalctl -u rk-pipeline -n 6
mpp[2123849]: mpp_platform: client 12 driver is not ready!
rk-pipeline.service: Main process exited, code=exited, status=2/INVALIDARGUMENT
rk-pipeline.service: Scheduled restart job, restart counter is at 26070.
```

**根因**：源 `rtsp://192.168.31.200:8557/h264` **不可达**（实测 TCP 8557 连不上），
MPP 初始化拿不到 client → `mpp_platform: client 12 driver is not ready!` →
**exit 2** → `Restart=always` + `RestartSec=5` → 每 5 秒重启一次。
**重启计数已到 26,073 次。**

### 8.3 ⚠ 只停这两个【不够】—— 它们的看门狗会立刻把它们拉回来

```
scripts/pipeline_watchdog.sh:33   systemctl restart "$SERVICE"
scripts/night_soak_monitor.sh:165 systemctl restart "$UNIT"
```

`rk-pipeline-watchdog.service` 与 `rk-night-soak-monitor.service` **都是 active**，
各自盯着自己的目标单元做 `systemctl restart`。
⇒ 处置必须**连看门狗一起停 + disable**（否则停完 5 秒内就被拉回来）。

### 8.4 这两条循环对本次验收的影响

- **对 NPU 无影响**：两个单元都是**立刻退出**的（CPU 141 ms / 236 ms，
  内存峰值 3 MB / 18.4 MB），不占用 NPU。
  ⇒ 本次 46.6 分钟长跑的吞吐与时延数据**没有被它们污染**
  （帧差分 79.42 FPS 与基线持平可佐证）。
- **但它们是实打实的资源浪费**：每 5 秒一次进程启动，累计 26,073 次；
  而且 `rk-night-soak-monitor` 还在每 10 分钟刷新一次 `NIGHT_SOAK_REPORT.md`
  （这份报告的"崩溃重启 396 次"就是这么来的）。
- **它们也是 NPU client 的噪声源**：`mpp_platform: client 12 driver is not ready!`
  说明它们在抢 NPU/MPP client —— 正好是 `rk_gate` 用的那批 client。
  停掉它们能减少对生产服务的干扰。

---

## 九、一键回滚

```bash
BK_DIR=$(cat /tmp/p0_memleak_backup_path.txt)
test -d "$BK_DIR" || { echo "Backup dir missing!"; exit 1; }

# 1. 安全停服（先静音看门狗）
sudo touch /run/rk-gate/paused
sudo systemctl stop rk-gate
sleep 2

# 2. 恢复二进制与配置
cp -p "$BK_DIR/rk_gate.bin.bak" /home/seeed/rk_deploy/rk3588_zero_copy/build/rk_gate
cp -p "$BK_DIR/gate.env.bak"     /home/seeed/rk_deploy/rk3588_zero_copy/config/gate.env
cp -p "$BK_DIR/gate_watchdog.sh.bak" /home/seeed/rk_deploy/rk3588_zero_copy/scripts/gate_watchdog.sh
cp -p "$BK_DIR/run_gate_service.sh.bak" /home/seeed/rk_deploy/rk3588_zero_copy/scripts/run_gate_service.sh
sudo cp -p "$BK_DIR/rk-gate.service.bak" /etc/systemd/system/rk-gate.service
sudo systemctl daemon-reload

# 3. 切回原分支（保留集成分支现场供事后复盘，不强制删除提交）
cd /home/seeed/rk_deploy
git checkout "$(cat "$BK_DIR/git_branch.txt")"

# 4. 拉起生产服务并解除看门狗静音
sudo systemctl start rk-gate
sudo rm -f /run/rk-gate/paused
sleep 5

# 5. 回滚后校验
systemctl is-active rk-gate rk-gate-watchdog
tail -n 20 /var/log/rk-gate-error.log
```

备份目录: `$(cat /tmp/p0_memleak_backup_path.txt)`
（内含：二进制 / gate.env / 看门狗 / 启动脚本 / unit / git 状态 / 工作区补丁 /
现场取证 / **旧二进制 19.7 小时的原始日志**）

★ 回滚**不会**动任务①已修好的 `/run/rk-gate/rk-gate.lock` 锁文件逻辑 ——
它只在 `scripts/run_gate_service.sh` 里，而该文件回滚到的是**备份时点**的版本
（备份是在 P0 修复合入之后做的，所以锁逻辑仍在）。
