# 02 · 稳定性与缺陷修复证据

> 三个曾经让现场"看着正常、其实已经坏掉"的缺陷，每个都配了**板端实证**，不是文字描述。

---

## 2.1 fd 泄漏（dma-buf）—— 通道静默死掉

### 症状（修复前，`fd_leak_before.log`）

```
mpp[998235]: mpp_dma_heap: ioctl alloc failed for Too many open files
mpp[998235]: mpp_dma_heap: os_allocator_dma_heap_alloc dma_heap_alloc failed ret -1
mpp[998235]: mpp_buffer: mpp_buffer_create failed to create buffer with size 1843200
[ch4][frame 3100] ... rss=0KB heap=82420KB
```

**最难发现的地方**：进程还活着、健康检查是绿的，但那一路视频**已经不再解码**。
日志里 `rss=0KB` 就是铁证 —— 连 `/proc/self/status` 都读不出来了（fd 已耗尽）。
以前只能靠生产看门狗定时重启兜底。

### 根因与修法（`fd_fix_diff.txt`）

根因在 `mpp_decoder.cc` 的背压重试循环：

```c
if (take_one_frame(&tmp, &tmp_raw, &got_real) && got_real) {
    held_frame_ = tmp_raw;      // ← 直接覆盖
    held_out_   = tmp;
}
```

`held_frame_` 是"上一次攒到的帧"。**直接覆盖它，上一帧就再也没人 `mpp_frame_deinit`** ——
它的 dma-buf fd 永远留在本进程里。触发链：MPP 输入队列满 → 进入重试循环
（500 次 × 2ms = 1 秒）→ 1 秒内队列一直满 → 函数带着 `held_frame_` 直接 `return false`
→ 下一轮 `decode()` 又取到新帧 → 覆盖 → **旧帧泄漏**。**重试循环里每取一帧就漏一帧。**

修法（**覆盖前先把旧帧释放掉**）：

```c
if (held_frame_) {
    MppFrame old = static_cast<MppFrame>(held_frame_);
    mpp_frame_deinit(&old);      // ★ 这一行就是修复的全部
    held_frame_ = nullptr;
}
held_frame_ = tmp_raw;
```

### 修复后（`fd_stable_10min.csv`）

10 分钟持续压测，每 10 秒采一次 fd / RSS / 活堆 / 四路帧号：
**fd 数全程恒定**，四路帧号持续推进，无一条 EMFILE。

### 为什么这个 bug 值得单独讲

它不是"崩溃"，是**静默降级**：进程活着、健康检查绿、视频少了一路。
交付后如果只看"服务还活着吗"，这个问题会一直藏着。

---

## 2.2 看门狗自愈（`self_healing.log`）

**执行动作**：`sudo systemctl kill -9 rk-gate.service`（等价于进程被 SIGKILL 打死）

**观察**：journal 里从 `Main process exited, code=killed, status=9/KILL` 到
`Started rk-gate.service` 的完整链路。

- systemd `Restart=on-failure` + `RestartSec=10` → **秒级拉起**
- 看门狗 `gate_watchdog.sh` 作为第二道保险（带指数退避，60~300s）
- 崩溃现场取证：`rk-collect-crash.service`（ramoops + dmesg + cgroup 内存峰值）

> 这条链路是"生产不能因为一次崩溃就停摆"的底线保障。

---

## 2.3 日志限流（`rate_limit_suppressed.log`）

**要解决的问题**：`/var/log` 是 **47MB 的 zram 内存盘**，写满后应用 `write()` 返回 ENOSPC → 进程崩溃。
而现场为了排查问题临时开 `info`/`debug` 级别时，帧守卫对**每一帧**都打日志。

**限流器自述行**（每分钟一条，WARN 级别，一定能记下来）：

```
[2026-11-19 13:00:34.804][tid=1383941][WARN][log][rate-limit] Suppressed 8152 messages (日志限流: 每 60 秒最多 1000 行, 超出部分已丢弃)
[2026-11-19 13:01:34.820][tid=1383942][WARN][log][rate-limit] Suppressed 8849 messages (日志限流: 每 60 秒最多 1000 行, 超出部分已丢弃)
[2026-11-19 13:02:34.824][tid=1383943][WARN][log][rate-limit] Suppressed 8048 messages (日志限流: 每 60 秒最多 1000 行, 超出部分已丢弃)
[2026-11-19 13:15:17.123][tid=1389943][WARN][log][rate-limit] Suppressed 12446 messages (日志限流: 每 60 秒最多 1000 行, 超出部分已丢弃)
```

**效果**（同一块板、同样开 debug、同样跑 3 分钟）：

| | 写入行数 | 速率 | 吃掉 zram |
|---|---|---|---|
| 不限流 | 28043 | 155.8 行/秒 | 4 MB |
| **限流后** | **2762** | **15.2 行/秒** | **0.3 MB** |
| | **↓90%** | | |

> ⚠️ **限流是底线防御，不是根本**。被丢掉的往往正是最关键的最后几条。
> **排查问题必须先把日志切到真实磁盘**（`GATE_LOG_DIR=/var/log.hdd`），排查完立刻切回 `warn`。
> 详见 `docs/现场运维手册.md` 第三节。

> 📌 附带修复：这套限流上线前，**logrotate 其实从来没成功轮转过**（配置里的 `su seeed root`
> 让轮转降权到无权在 `/var/log` 建文件的用户，错误只进 logrotate 自己的 stderr）。
> 已修正为 `su root root`，并用 `postrotate` 把轮转产物交还给 `seeed`。
