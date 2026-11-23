# P0 收尾报告 · OOM 与日志盘两颗定时炸弹

> 日期: 2026-11-23　　板子: RK3588 (recomputer-rk3588-devkit)
> 仓库: `/home/seeed/rk_deploy`　　主线: `main` @ `94941c9`
> 口径声明: 本文每个数字都标了测量条件; 凡是"文档里的说法"一律标「〈某文档〉陈述」,
> 与"本次实测"区分; 文档之间口径不一致的地方并列列出, 不做裁决。

---

## 〇、一页结论

| # | P0 项 | 状态 | 关键实测 |
|---|---|---|---|
| 1 | 收敛代码基线(停止分支分裂) | ✅ **完成** | 3 个分支 → 单一 `main`; 编译通过; 线上换上新二进制 |
| 2 | 残余活堆增长(319 KB/min) | ✅ **完成** | 活堆斜率 **0.3006~0.3161 → 0.0037~0.0050 KB/帧(−98.5%)**; 日增 **490~510 MB → 6.3~8.4 MB**; 19 小时的直线增长被终结 |
| 2b | **新发现**: 视频页缓存把 cgroup 顶满 | ✅ **完成** | cgroup **1024 MB(顶满) → 190~243 MB**; 页缓存 938 → 106~157 MB |
| 3 | 长跑复测稳态 RSS / 活堆 | ✅ **完成** | 46.6 分钟 / 5.6 万帧每路 / 零重启零告警; 活堆斜率 **0.30 → 0.0045 KB/帧**; cgroup 峰值 **243 MB(23.8%)** |
| 4 | 校准 MemoryMax / 看门狗阈值 | ⚠️ **仓库侧完成, 待 root 装上** | MemoryMax 1G→512M + 新增 MemoryHigh 384M; 阈值 400/200/8 → 350/150/2048 |
| 5 | 日志目录迁出 zram + 验证 logrotate | ⚠️ **仓库侧完成, 待 root 装上** | `GATE_LOG_DIR` → `/var/log.hdd`; **实测发现轮转从未成功过** |

> **还剩一件事**: 一条 root 命令。仓库侧的改动全部已提交, 但 `/etc/systemd/system`
> 与 `/etc/logrotate.d` 归 root, 本机 sudo 需要密码。见 §7。

---

## 1 · P0-1 收敛代码基线

### 1.1 起始状态(为什么必须先做这个)

| 项 | 起始状态 |
|---|---|
| 工作区 | `main` 与 `fix/p0-correctness-stability` 指向**同一个 commit** `47a7ebb`, 挂着 **9 个未提交修改** |
| `perf/hotpath-implementation-waste` | 停在 `b9434c5`, 比 main **多 3 个 commit**, 未合入 |
| **线上跑的二进制** | `build/rk_gate`, 时间戳 **Nov 22 16:59** —— 比 main 还旧 |

**最后一条是本次最关键的发现**, 也是所有内存数字对不上的原因:

```
$ grep -ac "直读原生类型" build/rk_gate        # perf 分支 fp16 直读的标志串
0                                            # ← 线上二进制【没有】perf 分支的改动
$ grep -ac "直读原生类型" build_npu/rk_gate
1                                            # ← 新编出来的才有
```

⇒ **线上跑的是一个既没有 perf 分支、也没有 P0 坐标修复的旧二进制。**
所有"实测值"(RSS 631 MB / 活堆 489 MB / cgroup 994M)都来自这个旧二进制。

### 1.2 做了什么

```
main 47a7ebb
 ├─ eb796cd  fix(yolo)   形态B DFL 距离补乘 stride + 参考工具改跨步读法
 ├─ 499e4c4  feat(watchdog) L1.5 静默卡死判据
 ├─ 09c5473  fix(guard)  锁文件移到 /run/rk-gate/ + 启动前自检
 ├─ fd18d14  chore(soak) 重新生成夜跑报告
 ├─ 83a2b8c  docs(p0)   P0 修复报告 + 项目介绍书 + 三项子任务实证
 ├─ 650efcc  merge(p0)  ← fix/p0-correctness-stability 并入 main
 ├─ a113ff2  merge(perf) ← perf/hotpath-implementation-waste 并入 main
 ├─ c7875b0  fix(mem)   定期丢弃视频页缓存
 ├─ a432f94  feat(p0)   日志目录迁出 zram + MemoryMax/阈值校准
 └─ 94941c9  docs(logrotate) 澄清 "hourly" 的真实含义
```

**合并顺序是刻意的**: 先合 P0(坐标正确性)再合 perf(实现层) ——
在错的坐标上做 A/B, 测出来的都是废数据。

**验证**:
- `cmake --build build` 全量编译通过, `rk_gate` / `rk_zero_copy` / `rk_minimal` / `rk_eval` 全部链接成功, 无 error。
- 线上服务已换成新二进制(见 §2.3 的 PID 变化)。

---

## 2 · P0-2 残余活堆增长

### 2.1 先把"旧二进制 19 小时"的证据坐实

线上服务从 Nov 22 16:59 一直跑到 Nov 23 12:42, 累计 **19.7 小时 / 1,421,000 帧/路**。
把 `mallinfo2().uordblks`(活堆)按**每路自己的帧号**做回归(工程自带的
`heap_slope.py` 把 4 路混在一个序列里算, 那个口径在这份数据上偏大):

| 路 | 帧区间 | 活堆起 → 止 | 斜率 | 折算 |
|---|---|---|---|---|
| ch1 | 100 → 1,351,300 | 74,765 → 502,444 KB | **+0.3164 KB/帧** | 534 MB/24h |
| ch2 | 100 → 1,420,800 | 74,514 → 502,685 KB | **+0.3009 KB/帧** | 508 MB/24h |
| ch3 | 100 → 1,418,500 | 74,646 → 501,884 KB | **+0.3014 KB/帧** | 509 MB/24h |
| ch4 | 100 → 1,420,700 | 74,567 → 501,768 KB | **+0.3010 KB/帧** | 508 MB/24h |

**十等分看形状**(ch1): `74765 → 117540 → 160570 → 203660 → 246152 → 289311 →
332184 → 374883 → 417242 → 459777 → 501693`

每一段都是 ~42,700 KB —— **一条笔直的线, 没有任何台阶**。
⇒ 这不是"跟踪器 vector 扩容台阶"(R6), 是**无界增长**, 而且 19 小时里
**没有一次回落**。

工程自带工具在同一份数据上的口径(4 路混合):

```
$ python3 scripts/heap_slope.py logs/heap_evidence/rk-gate-error.log.1 logs/heap_evidence/rk-gate-error.log
  活堆     : 232062 -> 486052 KB  (净增 253990 KB)
  全程斜率 : +0.3022 KB/帧 | +7.25 KB/s | +612.0 MB/24h
  尾段斜率 : +0.2907 KB/帧  (后 16716 个点)  最大阶跃 +1664 KB
  判定     : FAIL 泄漏 +0.2907 KB/帧 (~588.8 MB/24h)
```

### 2.2 泄漏源是什么

`perf/hotpath-implementation-waste` 的 `8640dce` 已经定位并修了它:
`NpuPool::ocr_results_` 的键是 `(通道, group=track_id, 候选号)`, 而 `track_id`
**一直递增** —— 每个识别候选都在 map 里留下一条记录; 以前取走结果后只 `pop_front`,
空掉的 deque 还挂在 map 里, 于是每秒几十个候选 → 每秒几十条永不回收的 map 节点。

⇒ **旧二进制跑的就是这条路径**(§1.1 已验证它没有 perf 分支的改动),
所以 19 小时涨 428 MB 与这条泄漏的速率完全对得上。

### 2.3 换上新二进制之后

线上服务用 `kill -9` 触发 systemd 自己的 `Restart=on-failure` 拉起新二进制
(服务以 `seeed` 运行, 与当前用户同 uid, 不需要 root 就能换二进制):

```
12:46  kill -9 1498961   ->  MainPID 2007367   cgroup 1073 MB -> 254 MB
13:13  kill -9 2007367   ->  MainPID 2015960   (页缓存修复版)
13:14  kill -9 2015960   ->  MainPID 2031025   (5s 间隔版, 即最终版)
```

**同一套工具、同一份口径**的对比:

| | 旧二进制(19h) | 新二进制(合并后) |
|---|---|---|
| 全程斜率 | +0.3022 KB/帧 (612 MB/24h) | **+0.0350 KB/帧 (64.7 MB/24h)** |
| 尾段斜率 | +0.2907 KB/帧 (588.8 MB/24h) | **+0.0272 KB/帧 (50.2 MB/24h)** |
| 活堆绝对值 | 74.7 MB → **502.4 MB** | 73.1 MB → **73.5 MB**(窗口内) |
| 判定 | FAIL | FAIL(但**降了 91%**) |

**逐路看窗口内的净变化**(更干净的判据, 因为它不受 4 路混合口径的影响):

```
ch1: 73656 -> 73616 KB  (净 -40 KB)
ch2: 73395 -> 73435 KB  (净 +40 KB)
ch3: 73400 -> 73492 KB  (净 +92 KB)
ch4: 73469 -> 73539 KB  (净 +70 KB)
```

⇒ **在噪声里, 没有可测的持续增长。**

### 2.4 顺带的吞吐收益(不是本次的目标, 但要记)

```
                     旧二进制            新二进制
4 路 deliv 合计       79.00 FPS          79.63 FPS
ch1 ocrwait          60.35 ms           55.18 ms   (-5.2 ms)
ch3 ocrwait          58.84 ms           52.31 ms   (-6.5 ms)
```

与 perf 分支自报的 `ocrwait ch1 −3.128 / ch3 −6.072 ms` 同量级。

---

## 3 · P0-2b 【新发现】视频页缓存把 cgroup 顶满 —— 第二颗炸弹

**这一项不在原始 P0 清单里, 是查 §2 的过程中挖出来的。**
不修它, §2 的成果守不住。

### 3.1 现象: 泄漏修完了, cgroup 还是顶满

换上新二进制、活堆降到 73 MB 之后, cgroup **依然是**:

```
$ systemctl show rk-gate -p MemoryCurrent -p MemoryMax
MemoryCurrent=1073668096     # 1024 MB
MemoryMax=1073741824         # 1024 MB  ← 顶满
```

而进程自己的 RSS 只有 619 MB。**差出来的 400 MB 在哪?** 拆开看:

```
$ cat /sys/fs/cgroup/system.slice/rk-gate.service/memory.stat
anon       526962688   (503 MB)  ← 业务进程
file       535101440   (510 MB)  ← 【页缓存】★
shmem      102887424   ( 98 MB)  ← dma-buf(MPP/NPU), unevictable
kernel      11456512   ( 11 MB)
```

`file` 那 500 MB 是**页缓存**。而这 4 路视频源合计 **4.1 GB**
(`Test2_fixed.mp4` 2.2 GB + `电动车素材.mp4` 1.7 GB + 另外两个 ~500 MB),
进程持续顺序读取, 内核把读过的页全留在 page cache 里。

实测增长速率 **~7~10 MB/s** ⇒ **5 分钟就能把 cgroup 顶到上限**,
内核被迫一直"回收 → 立刻重读"(实测 `pgscan` 46 GB), cgroup 常年贴着上限。

### 3.2 后果

**之前那 500 MB 活堆泄漏能撑 19 小时, 正是因为它是在
"cgroup 已经被页缓存吃满"的状态下被 OOM 杀掉的** ——
页面缓存挤掉了泄漏本该撞上的那道墙。修掉页缓存之后,
OOM 的触发条件才变回"活堆真的涨到上限", 也就是 §4 校准的那个值。

### 3.3 为什么 `avioflags=direct` 挡不住

`open_source()` 里**已经**设了 `avioflags=direct`, 而且实测 fd 上确实带了:

```
$ grep ^flags /proc/<pid>/fdinfo/44
flags:  02400000        # = O_RDONLY|O_CLOEXEC|O_DIRECT
```

但页缓存照样涨。原因是**页缓存按 inode 记**, 跟用哪个 fd 打开、
要不要 O_DIRECT 无关; ffmpeg 在探测/建索引阶段走的又是别的读路径。

### 3.4 修法

`src/gate_main.cc`: 另开一个只读 fd, 每 5 秒调一次
`posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED)` 把这个视频的干净页丢掉
(fadvise 按 inode 生效, 与 ffmpeg 怎么读无关)。只对本地文件生效, 网络流不适用。

**★ 间隔收紧的代价是零**, 这一点是实测出来的:

```
$ cat /proc/<pid>/io
rchar:      4630214695 -> 4844741148    (30 秒 +214,526,453 字节 = 7.15 MB/s)
read_bytes: 4655793152 -> 4873245696    (30 秒 +217,452,544 字节 = 7.25 MB/s)
```

`read_bytes ≈ rchar` ⇒ 进程读的**每一个字节都真的落到了盘上**,
说明这个页缓存**一次都没被命中过**。它占着 cgroup 的额度, 却一个字节都没省下。

### 3.5 效果(同一台板、同一套视频、同样 4 路)

| | 修之前 | 修之后(15s 间隔) | 修之后(5s 间隔, 最终) |
|---|---|---|---|
| `file` 页缓存 | 938 MB(顶满) | 118~229 MB | **110~157 MB** |
| cgroup 合计 | **1024 MB (100% of max)** | 205~315 MB | **209~248 MB** |
| `anon` | 84 MB | 84 MB | 87 MB |
| `shmem`(dma-buf) | 98 MB | 98 MB | 98 MB |

---

## 4 · P0-4 MemoryMax 与看门狗阈值校准

### 4.1 实测稳态(4 路 + max_fps=20 + 4 个 1080p 视频源)

| 指标 | 实测稳态 | 来源 |
|---|---|---|
| 进程 RSS | **228.6~231.7 MB** | 日志 `rss=` 字段 + `/proc/<pid>/status` |
| 活堆(uordblks) | **73.3~74.2 MB** | 日志 `heap=` 字段 |
| cgroup `anon` | **81.5~84.2 MB** | `memory.stat` |
| cgroup `file` | **106.3~157.2 MB** | `memory.stat` |
| cgroup `shmem`(dma-buf) | **98.1 MB** | `memory.stat` |
| cgroup `kernel` | **3.1~3.3 MB** | `memory.stat` |
| **cgroup 合计** | **190.4~243.4 MB**(均值 209.7) | `memory.current` |

> 测量窗口: 2026-11-23 13:22:54 → 14:09:32, 46.6 分钟, 278 次 cgroup 采样, 零重启。

### 4.2 校准结果

| 参数 | 旧值 | 新值 | 依据 |
|---|---|---|---|
| `MemoryMax` | 1G | **512M** | 稳态峰值 243.4 MB → 2.10× 余量; 且不可回收部分(anon+shmem)只有 ~182 MB, 所以匿名内存涨到 ~427 MB 才撞墙 |
| `MemoryHigh` | (无) | **384M** | 稳态峰值的 ~1.6×。软刹车: 超过就优先回收页缓存并压制分配, **不 OOM 杀** |
| `RSS_WARN_MB` | 400 | **350** | 稳态 228.6~231.7 MB 的 ~1.5×。RSS 含 98 MB dma-buf, 瞬时波动比活堆大, 余量给宽一点 |
| `HEAP_WARN_MB` | 200 | **150** | 稳态 73.3~74.2 MB 的 ~2.0×。活堆是最灵敏的泄漏指标, 宁可早报 |
| `LOG_FREE_WARN_MB` | 8 | **2048** | 日志盘已从 47 MB zram 换成 105 GB 真实盘; 旧值 8 MB 是按 zram 定的, 照抄等于永远不报警 |
| `CGROUP_WARN_MB` | (无) | **400** | **新增**。稳态峰值 243.4 MB 的 1.6×, 也是 `MemoryMax=512M` 的 78% |

**旧阈值为什么必须改**: 400/200 是**"泄漏还在、页缓存把 cgroup 顶满"**那个状态下定的
(那时稳态 RSS 631 MB / 活堆 489 MB)。照抄到新稳态上, 要么天天误报, 要么失去意义。

**新阈值能抓住多久**: 旧泄漏速率 1.4 MB/min ⇒ 从 74 MB 涨到 150 MB 约 **55 分钟**,
撞到 `MemoryMax=512M` 约 **15 分钟**。留足了排查窗口。

**新阈值会不会误报**: 实测 46.6 分钟窗口内**零告警**
(旧阈值 400/200 那个状态下, 同样的窗口会每 20 秒刷一条)。

### 4.3 为什么 `MemoryMax` 从 1G 调低是"更安全"而不是"更危险"

`MemoryMax` 之前形同虚设: 业务活堆已经吃掉一半, 剩下的全是可回收的页缓存,
内核只能一直回收又重读。调到 512M 之后, 上限真正只用来约束**不可回收的业务内存**。

---

## 5 · P0-3 长跑复测

> **窗口**: 2026-11-23 13:15 起 —— 最终二进制(`5s` 间隔版), 生产配置
> (4 路 / max_fps=20 / 真实视频源 / 看门狗在岗)。
> 采样: 每条统计行(每路每 100 帧一行)抽 (时间, 通道, 帧号, rss, heap);
> cgroup 每 10 秒抽一次 `memory.stat` 全构成。

### 5.1 活堆斜率

**窗口**: 2026-11-23 13:22:54 → 14:09:32(**46.6 分钟**), 5.4 万+ 帧/路,
278 次 cgroup 采样, 采样器 10 秒一次。
**运行条件**: 生产配置(4 路 / `max_fps=20` / 4 个真实 1080p 视频源 / 看门狗在岗),
二进制 md5 `dddc85a3`(与仓库构建产物逐位一致)。
**期间零重启、零看门狗告警**(`NRestarts` 全程为 3, 三次都是本次 P0 主动换二进制造成的,
都在窗口之前)。

### 逐路活堆斜率

```
ch1: n=545  帧 400 -> 54800 (54400 fr / 46.6 min)  活堆 73288 -> 73652 KB (净 +364)
     slope +0.00499 KB/帧  -> +6.0 KB/min  -> +0.35 MB/h -> +8.42 MB/24h
     RSS  slope +0.01341 KB/帧  (236388 -> 236520 KB)
ch2: n=561  帧 400 -> 56400 (56000 fr / 46.7 min)  活堆 73335 -> 74207 KB (净 +872)
     slope +0.00473 KB/帧  -> +5.7 KB/min  -> +0.33 MB/h -> +7.98 MB/24h
     RSS  slope +0.01306 KB/帧  (236388 -> 236520 KB)
ch3: n=560  帧 400 -> 56300 (55900 fr / 46.6 min)  活堆 73351 -> 73824 KB (净 +473)
     slope +0.00374 KB/帧  -> +4.5 KB/min  -> +0.26 MB/h -> +6.32 MB/24h
     RSS  slope +0.01297 KB/帧  (236388 -> 236520 KB)
ch4: n=560  帧 400 -> 56300 (55900 fr / 46.6 min)  活堆 73289 -> 73665 KB (净 +376)
     slope +0.00463 KB/帧  -> +5.6 KB/min  -> +0.33 MB/h -> +7.81 MB/24h
     RSS  slope +0.01286 KB/帧  (236388 -> 236520 KB)
```

4 路的斜率高度一致(0.0037~0.0050 KB/帧), 且 RSS 基本不动(+0.013 KB/帧)。

工程自带工具在同一份数据上:

```
$ python3 scripts/heap_slope.py logs/heap_evidence/newrun.csv
  帧区间   : 400 -> 56400  (2226 个采样点)
  活堆     : 73335 -> 74207 KB   (净增 872 KB)
  全程斜率 : +0.0045 KB/帧 | +0.09 KB/s | +7.6 MB/24h
  尾段斜率 : +0.0020 KB/帧  (后 1113 个点)  最大阶跃 +1189 KB
  判定     : PASS 活堆打平(尾段线性度≈0)
```

> 46.6 分钟里活堆净增 **872 KB**, 而旧二进制在**同样长度**的窗口里会涨
> `0.30 KB/帧 × 56000 帧 = 16.4 MB` —— 差 **19 倍**, 而且旧的是**笔直增长**,
> 新的是**噪声量级的抖动**。

### 5.2 cgroup 内存构成

278 次采样(`memory.stat` 全构成 + `memory.current` + `/proc/<pid>/status`):

| 分量 | min | max | mean | last |
|---|---|---|---|---|
| `anon`(业务进程) | 81.5 MB | 84.2 MB | 83.7 MB | 84.2 MB |
| `file`(视频页缓存) | 106.3 MB | 157.2 MB | 122.6 MB | 122.1 MB |
| `shmem`(dma-buf) | 98.1 MB | 98.1 MB | 98.1 MB | 98.1 MB |
| `kernel` | 3.1 MB | 3.3 MB | 3.2 MB | 3.2 MB |
| **cgroup 合计** | **190.4 MB** | **243.4 MB** | **209.7 MB** | 209.9 MB |
| 进程 RSS | 228.6 MB | 231.7 MB | 230.2 MB | 231.0 MB |

`file` 分量在 106~157 MB 之间**周期性起伏** —— 那就是 fadvise 每 5 秒丢一次页
造成的锯齿(4 路各丢各的, 相位错开)。起伏幅度约 50 MB, 与
"4 路合计读盘 7~10 MB/s x 5 秒 = 35~50 MB"完全对得上。

**峰值 243.4 MB = 当前 MemoryMax(1G)的 23.8%**; 按新配的 `MemoryMax=512M`
折算, 峰值只占 **47.5%**。

### 5.3 与旧二进制的对照

| 指标 | 旧二进制(19.7 小时) | 新二进制(46.6 分钟) | 变化 |
|---|---|---|---|
| 活堆斜率(逐路) | +0.3006 ~ +0.3161 KB/帧 | **+0.0037 ~ +0.0050 KB/帧** | **−98.5%** |
| 折算速率 | +360.4 ~ +361.4 KB/min | **+4.5 ~ +6.0 KB/min** | **−98.5%** |
| 折算日增 | 490 ~ 510 MB/24h | **6.3 ~ 8.4 MB/24h** | **−98.5%**(61~81×) |
| 工程工具判定 | FAIL 泄漏 | **PASS 活堆打平** | — |
| 活堆绝对值 | 74.7 → **502.4** MB | 73.3 → **74.2** MB | 净增 872 KB vs +427.7 MB |
| RSS | 619 ~ 641 MB(还在涨) | **228.6 ~ 231.7 MB**(平) | — |
| cgroup 合计 | **1024 MB / 1024 MB(顶满, available 44K)** | **190.4 ~ 243.4 MB** | 峰值 23.8% |
| cgroup `file`(页缓存) | 510 → **938 MB** | 106.3 ~ 157.2 MB | −83% |
| 4 路 deliv 合计 | 79.00 FPS | **79.63 FPS** | +0.6% |
| ch1 / ch3 ocrwait | 60.35 / 58.84 ms | **55.18 / 52.31 ms** | −5.2 / −6.5 ms |
| 运行期间重启 | — | **0 次**(窗口内) | — |
| 运行期间看门狗告警 | 持续刷(每 20 秒) | **0 条** | — |

> ★ 关于"−98.5%": 这不是把阈值调松了 —— 旧二进制的斜率是**四条路各自独立、
> 高度一致**地测出来的(ch1 +360.4 / ch2 +360.7 / ch3 +361.4 / ch4 +360.8 KB/min),
> 口径没有争议; 新二进制的同口径数值也是四条路各自测出来的。
> 而且**活堆绝对值**这一栏最能说明问题: 旧的是 74.7 → 502.4 MB 笔直上升,
> 新的是 73.3 → 74.2 MB 停在原地。

### 窗口长度够不够?

诚实地说:**46.6 分钟只能证明"没有每帧级别的持续泄漏", 不能证明"没有极缓慢的泄漏"。**
旧二进制的泄漏在**任何**窗口长度下都是笔直的(十等分每一段都 ≈ +42.7 MB),
所以"直线 vs 噪声"这个判据在 46 分钟上是可靠的;
但如果新二进制真有 0.003 KB/帧 这种量级的泄漏, 按此速率 24 小时才涨 7.6 MB,
需要跑满 24 小时以上才能把它和噪声区分开。

⇒ **本次的结论是"与旧泄漏相比降了两个数量级, 且工程工具判定 PASS"**,
而不是"证明了绝对无泄漏"。要把后者坐实, 需要再跑一轮 ≥24 小时的窗口
(`scripts/night_soak_monitor.sh` 可以做这件事)。

---

## 6 · P0-5 日志目录迁出 zram + logrotate 验证

### 6.1 现状问题

1. `/var/log` 是 **47 MB 的 zram**(`/dev/zram1` ext4), 实测只剩 43 MB。
2. **zram 占的是真 RAM** —— 写进去的每一页都算内存。
3. **实测发现: 装好的 logrotate 规则对 /var/log 【一次都没成功轮转过】**:

```
$ grep -A3 "rk-gate" /var/lib/logrotate/status
(无条目)                      ← 状态文件里从来没有过 rk-gate 的记录
$ ls /var/log/rk-gate*.1 /var/log/rk-gate*.gz
(不存在)                      ← 目录里连一个轮转产物都没有
```

对照: `/var/log.hdd/` 下**有** `.1` / `.2.gz` 产物(那是 11-23 00:14 那次
logrotate.timer 跑的时候, 规则还指向 /var/log.hdd 时留下的)
⇒ **规则本身是好的, 坏的是"指向了 /var/log"这件事 + 盘本身太小**。

### 6.2 还有一条: 轮转的真实节奏是【每天一次】

```
$ systemctl list-timers | grep logrotate
Mon 2026-11-23 00:14:34  13h ago  logrotate.timer  logrotate.service
Tue 2026-11-24 00:25:46  11h left
```

logrotate 不常驻, 是 systemd timer 拉的, 而那个 timer **每天一次**。
规则里的 `hourly` 只表示"每小时**有资格**被轮转", 真正什么时候轮转取决于 timer。

⇒ **在 /var/log(47 MB) 上**: 日增约 8 MB, 每天转一次、留 12 份
⇒ 最坏 `12 × 8 MB = 96 MB > 47 MB` —— **盘会在轮转前就写满**。
这给"必须迁走"补上了量化理由。

### 6.3 改了什么

| 位置 | 改动 |
|---|---|
| `config/gate.env` | `GATE_LOG_DIR=/var/log` → **`/var/log.hdd`** (overlayroot 上的真实目录, 105 GB, 实测剩 78 GB) |
| `scripts/run_gate_service.sh` | `LOCK_CHECK_LOG` 从写死 `/var/log/...` 改成跟着 `GATE_LOG_DIR` 走 |
| `scripts/logrotate/rk-gate` | 注释澄清 `hourly` 的真实含义 + 把"为什么必须迁走"量化 |

**只改 `gate.env` 是不够的** —— 下面这些路径都要一起改, 而它们全部由
`install_gate_service.sh` 按 `GATE_LOG_DIR` 统一改写:

- `rk-gate.service` 的 `StandardOutput=` / `StandardError=`
- `rk-gate-watchdog.service` 的 `StandardOutput=` / `StandardError=`
- `rk-collect-crash.service` 的取证路径(`ExecStartPre` 里的 `tail`)
- `/etc/logrotate.d/rk-gate` 规则里的文件列表与 `postrotate`

**看门狗 / 预检 / 体检脚本都是自己 `source` `config/gate.env` 的**, 所以它们会自动跟上
(已逐个核对: `gate_watchdog.sh:55-59`、`preflight_gate.sh:44-46`、
`gate_health.sh:25-29`、`acceptance_checklist.sh:20-23`)。

---

## 7 · 还差的一条 root 命令

仓库侧全部改完并提交了, 但下面这些归 root, 本机 `sudo` 需要密码:

- `/etc/systemd/system/rk-gate.service` 的 `MemoryMax=512M` / `MemoryHigh=384M` 与日志路径
- `/etc/systemd/system/rk-gate-watchdog.service` 的日志路径
- `/etc/systemd/system/rk-collect-crash.service` 的取证路径
- `/etc/logrotate.d/rk-gate` 的路径
- `logrotate -f` 的实测验证

**一条命令搞定**(已写好并做了语法检查):

```bash
sudo /home/seeed/rk_deploy/rk3588_zero_copy/scripts/p0_deploy.sh
```

它依次做:

1. 校验 `GATE_LOG_DIR` 已经改到真实磁盘(还是 `/var/log` 就直接拒绝执行并说明原因);
2. 跑 `install_gate_service.sh` —— 按 `GATE_LOG_DIR` 统一改写上面 4 处路径,
   装上校准后的 `MemoryMax`/`MemoryHigh`, `daemon-reload` + `restart`;
3. `logrotate -f` 强制轮转, **并验证 `.1` 产物真的生成** ——
   历史上这条安全网从没成功过, 而 logrotate 的报错只进它自己的 stderr、
   不进 syslog/journal, 所以光看退出码会被骗过去;
4. 打印部署后状态供人工核对。

**在那之前服务仍按旧配置跑**: cgroup 上限还是 1G、日志还是写 zram、
看门狗阈值还是 400/200/8。

> 注: `MemoryMax` 暂时还是 1G 不构成即时风险 —— §3.5 实测 cgroup 现在只用
> 209~248 MB(1G 的 21~24%), 离上限很远。**真正的风险是 §6 的日志盘**:
> zram 写满会触发 `write()` ENOSPC 让应用崩。

---

## 8 · 本次改动的验证方式

| 改了什么 | 怎么验的 | 结论 |
|---|---|---|
| 分支收敛 | `cmake --build build` 全量编译 + 线上换二进制 | ✅ |
| 活堆泄漏 | 19 小时旧数据逐路回归 + 新二进制同口径对照 + 逐路净变化 | ✅ 0.3022 → 0.0272 KB/帧 |
| 页缓存 | `memory.stat` 前后对照 + `posix_fadvise` 人工验证 + `/proc/<pid>/io` | ✅ 1024 MB → 209~248 MB |
| L1.5 静默卡死 | `opt_p0/tests/watchdog_silent_stall_test.sh` 三个场景全过 | ✅ 场景A 不误报 / B 能自愈 / C 兜住 L2 盲区 |
| 日志目录 | 规则与脚本逐个核对 + logrotate 状态文件/产物检查 | ⚠️ 仓库侧完成, 待 root 装 |
| MemoryMax/阈值 | 按实测稳态重新推导 | ⚠️ 仓库侧完成, 待 root 装 |

**证据留档**: `rk3588_zero_copy/logs/heap_evidence/`
(`rk-gate-error.log.1` + `rk-gate-error.log` = 旧二进制 19 小时原始日志;
`newrun*.csv` / `cgroup*.csv` = 新二进制采样; `SOAK_START.txt` = 窗口起点)

---

## 9 · 与现有文档的口径差异(并列, 不裁决)

| # | 现有文档的说法 | 本次实测 | 差在哪 |
|---|---|---|---|
| 1 | 〈`docs/optimization_ab_report.md`〉"活堆斜率 new = 319.11 KB/min(生产口径)" | 合并后同口径 50.2 MB/24h ≈ **35 KB/min** | 量级差 9 倍。两者都用 `scripts/heap_slope.py`, 但 A/B 用 **100 秒短窗口**, 且混了 4 路帧号; 本次用**逐路自己的帧号 + 19 小时长窗口**, 且新二进制含 P0 修复 |
| 2 | 〈`PROJECT_BRIEFING_20261123.md` §1.3〉"cgroup 994M/1G" | 修完之后 **209~248 MB** | 那 994 MB 里有 500~938 MB 是页缓存, 不是活堆 |
| 3 | 〈`RISK_REGISTER.md` R19〉"日志盘是 47MB zram" | 成立, 且**更严重**: 轮转从未成功 + 实际节奏是每天一次 | R19 只写了"会写满", 没写"轮转这条安全网是空的" |
| 4 | 〈`PROJECT_BRIEFING_20261123.md`〉"RSS 626~641 MB / 活堆 483~496 MB" | 那是**旧二进制**的稳态; 新二进制 230~237 MB / 72~74 MB | 旧二进制连 perf 分支都没有 |

---

## 10 · 遗留与建议(按"还会不会咬人"排序)

1. **【待做·需 root】** 跑 `sudo scripts/p0_deploy.sh` —— 见 §7。
2. **【建议】** `logrotate.timer` 是每天一次, 与规则里 `hourly` 的字面意思不符。
   想真按小时轮转需要改 timer 的 `OnCalendar`(需 root)。
   迁到 78 GB 真实盘后每天一次其实够用, 所以不是 P0。
3. **【建议】** `opt_audit/`、`opt_p0/` 等沙盒(含 500 MB+ 产物)仍未入库,
   而 `license_plate_rec.cc` 的注释直接引用了 `opt_audit/tools/fp16_equiv.cc` ——
   **验证工具不在仓库里, 结论就不可复现**(与〈O15: base 二进制不可复原〉同一类问题)。
   建议把各沙盒的 `tools/*.cc` 与 `tests/*.py` 入库, 产物不入库。
4. **【建议】** `scripts/heap_slope.py` 把 4 路帧号混在一个序列里做回归 ——
   在这次的数据上会明显高估斜率(旧二进制真实 0.30, 工具报 0.30; 但新二进制
   逐路净变化是 ±90 KB, 工具仍报 FAIL)。建议改成**按路分组**再分别回归。
