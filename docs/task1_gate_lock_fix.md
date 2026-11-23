# 任务① 报告 · 修复 rk-gate 因锁文件权限起不来

> 2026-11-21 22:52 ~ 23:15　　执行人: ZCode Agent　　范围: 只改部署层(unit + 脚本), 未动任何推理/业务代码

---

## 一、根因确认

**不是"权限没设对", 是锁文件被 tmpfs 冲掉了。**

```bash
# scripts/run_gate_service.sh:43-55 (改前)
LOCK="${GATE_LOCK_FILE:-/var/run/rk-gate.lock}"
mkdir -p "$(dirname "$LOCK")" 2>/dev/null || true
if ! ( : >> "$LOCK" ) 2>/dev/null; then     # ← seeed 建不出 /run 根下的文件
    echo "[gate] 打不开锁文件 $LOCK (权限不足?)" >&2
    exit 75                                 # ← 连续 367 次死在这
fi
exec 9>"$LOCK"
if ! flock -n 9; then exit 75; fi           # ← 真正的互斥机制, 一直有效
```

完整因果链(每条都有实测证据):

| # | 事实 | 证据 |
|---|---|---|
| 1 | `/run` 是 **tmpfs**, 重启即清空 | `findmnt /run` → `tmpfs size=812052k mode=755` |
| 2 | 锁文件只在**安装时**建过 | `install_gate_service.sh:228` `touch "$LOCK" && chmod 0666 ...` |
| 3 | 装机时间 11-19 18:34, **重启时间 11-20 14:55** | `stat /etc/systemd/system/rk-gate.service` / `uptime -s` |
| 4 | `/run` 是 `root:root 755`, seeed 无权新建 | `ls -ld /run` / `sudo -u seeed touch /run/x` → Permission denied |
| 5 | 探测失败 → exit 75 → systemd 标 exit-code → 看门狗反复拉起 | `journalctl -u rk-gate` / `/run/rk-gate/fail_count = 367` |

**真正的互斥机制一直是好的** —— 是 `flock -n 9`(fd 9 随 `exec` 继承给 rk_gate, 进程死锁自动释放), 不是"文件存不存在"。修复没有动这个机制。

---

## 二、改了什么(5 个文件, 全部部署层)

### 2.1 `scripts/systemd/rk-gate.service` —— 核心改动

```diff
 [Service]
 Type=simple
 User=seeed
 Group=seeed
 WorkingDirectory=/home/seeed/rk_deploy/rk3588_zero_copy
+
+# ★ RuntimeDirectory: systemd 在启动前自动建 /run/rk-gate/ (owner=seeed:seeed,
+#   模式 0755); 目录已存在时 systemd 也会把 owner/权限改成上面这个。
+# ★★ 为什么必须有它(2026-11-21 实锤):
+#     /run 是 tmpfs, 每次重启都被清空。install_gate_service.sh 只在【安装时】
+#     建过 /var/run/rk-gate.lock (0666), 2026-11-20 那次重启把它冲掉了;
+#     而 /run 是 root:root 755, seeed 无权新建文件 ->
+#     run_gate_service.sh 的锁文件探测失败 -> exit 75 -> 服务起不来。
+# ★ 不能靠 chmod /run (破坏系统安全面), 也不能把锁放 /tmp
+#     (tmpfs 清理后锁落在已删除的 inode 上, flock 失效 = 双实例)。
+# ★ RuntimeDirectoryPreserve=yes: 服务停止时【不删】这个目录 ——
+#     因为看门狗的维护标记 paused / stop_alerted / 退避状态也住在这里
+#     (scripts/gate_watchdog.sh:54, scripts/gate_health.sh:33)。
+RuntimeDirectory=rk-gate
+RuntimeDirectoryMode=0755
+RuntimeDirectoryPreserve=yes
```

`RuntimeDirectoryPreserve=yes` 是有意加的: `/run/rk-gate/` 里还住着看门狗的维护窗口标记
`paused` / `stop_alerted` / 退避状态(默认行为会在服务停止时删目录, 把运维的"维护窗口"冲掉)。

### 2.2 `scripts/run_gate_service.sh` —— 锁路径 + 提示

```diff
-LOCK="${GATE_LOCK_FILE:-/var/run/rk-gate.lock}"
-mkdir -p "$(dirname "$LOCK")" 2>/dev/null || true
+LOCK="${GATE_LOCK_FILE:-/run/rk-gate/rk-gate.lock}"
+LOCK_DIR="$(dirname "$LOCK")"
+mkdir -p "$LOCK_DIR" 2>/dev/null || true
@@ 失败提示 @@
-    echo "[gate]   手工建: sudo touch $LOCK && sudo chmod 666 $LOCK" >&2
+    echo "[gate]   目录 $LOCK_DIR 必须对 $USER 可写。" >&2
+    echo "[gate]   systemd 启动时应该由 RuntimeDirectory=rk-gate 建好;" >&2
+    echo "[gate]   若 unit 没装上, 手工补:" >&2
+    echo "[gate]     sudo mkdir -p $LOCK_DIR && sudo chown $USER:$USER $LOCK_DIR" >&2
```

**`flock -n 9` 与 exit 75 的互斥逻辑一字未改。**

### 2.3 `config/gate.env` —— 锁路径配置化

```diff
+# ★ 互斥锁文件路径。改这个 = 改"双实例互斥"用的锁。
+#   标准值 /run/rk-gate/rk-gate.lock (子目录), 不要改回 /run 根上: ...
+GATE_LOCK_FILE=/run/rk-gate/rk-gate.lock
```

### 2.4 `scripts/install_gate_service.sh` —— 防止将来重装又踩坑

- `LOCK=/var/run/rk-gate.lock` → `LOCK=$RUN_DIR/rk-gate.lock`
- 装完**自动把 gate.env 里的锁路径切回标准值**(用 `cat >` 而不是 `sed -i`, 后者会换 inode 让属主变 root)

### 2.5 `docs/PRODUCTION_GUARD.md` —— 文档同步

---

## 三、验证结果(5 项逐条)

| # | 项 | 结果 | 证据 |
|---|---|---|---|
| 1 | `systemctl start` 能起来 | ✅ | `Active: active (running) since 22:56:50`, MainPID=1053829 |
| 2 | 锁文件正常创建 | ✅ | `/run/rk-gate/rk-gate.lock` (seeed:seeed); `fd 9 -> /run/rk-gate/rk-gate.lock` 由主进程持有 |
| 3 | 4 路出帧 | ✅ | ch1 frame 7000 / ch2 7500 / ch3 7500 / ch4 7500, 持续递增 |
| 4 | 双实例互斥仍生效 | ✅ | 手动再起一个 → `已有 rk_gate 实例在跑(锁 /run/rk-gate/rk-gate.lock 被占) —— 拒绝启动双实例`, **exit 75** |
| 5 | `kill -9` 后自愈 | ✅ | kill 1056384 → 6 秒后 systemd 拉起新实例 1053829, 4 路继续出帧 |

**额外确认(systemd 语义真的生效了)**:

```
$ ls -lad /run/rk-gate
drwxr-xr-x 2 seeed seeed 400 Nov 21 23:04 /run/rk-gate
   ↑ 装机时它是 root:root(看门狗建的), 是 systemd 在服务启动时把它 chown 成 seeed 的
$ ls /run/rk-gate/
backoff  ch1_frame  ch1_slow  ch1_ts  ...  fail_count  last_fail_ts  last_pid
last_start_ts  rk-gate.lock
   ↑ 看门狗的状态文件全在(RuntimeDirectoryPreserve=yes 生效, 没被删)
```

---

## 四、验收标准逐条核对

- [x] 改前有备份(unit + 脚本 + gate.env + installer) —— `/home/seeed/rk-gate-fix-backup-20261121_225210/`, 带 sha256
- [x] 用 RuntimeDirectory 标准做法, 没有粗暴改 `/run` 权限 —— 只加了 3 行 unit 指令, `/run` 权限未动
- [x] 双实例互斥逻辑保留且仍生效 —— `flock -n 9` 未改, 实测 exit 75
- [x] `systemctl start` 后 active, 4 路出帧 —— 见上表 1/2/3
- [x] 自愈(`kill -9` 拉起)没被破坏 —— 见上表 5
- [x] 报告含 Diff + 验证证据 + 回滚方式 —— 本文件
- [x] 不改任何推理/业务代码 —— 只动 unit / 启动脚本 / 配置 / installer / 文档

---

## 五、回滚方式

```bash
BK=/home/seeed/rk-gate-fix-backup-20261121_225210
cd /home/seeed/rk_deploy/rk3588_zero_copy
cp -p $BK/run_gate_service.sh.bak          scripts/run_gate_service.sh
cp -p $BK/rk-gate.service.repo.bak         scripts/systemd/rk-gate.service
cp -p $BK/gate.env.bak                     config/gate.env
cp -p $BK/install_gate_service.sh.bak     scripts/install_gate_service.sh
sudo cp -p $BK/rk-gate.service.installed.bak /etc/systemd/system/rk-gate.service
sudo systemctl daemon-reload && sudo systemctl restart rk-gate
```

⚠ 回滚到旧版 = 恢复旧行为(重启后又会因锁文件被 tmpfs 冲掉而起不来), 只作应急。

**不想回滚的话, 更省事的收尾**: 装 unit 时 `install_gate_service.sh` 会自动把
`gate.env` 的锁路径切回 `/run/rk-gate/rk-gate.lock`, 不用手工改。

---

## 六、附带发现(不是本次任务范围, 但要记一笔)

1. **服务停了多久**: 看门狗 `fail_count = 367` —— 从 11-20 14:55 重启后一直没起来,
   期间只有看门狗在 300s 退避里反复拉起。**这类"重启后静默失效"不会被告警发现**,
   因为看门狗只保证"尝试拉起", 不保证"成功"。建议加一条"服务连续 failed > N 次"的告警。
2. **本板 CPU 没有 1GHz 档**(可用频率 408/600/816/1008/1200/1416/1608/1800 MHz),
   任务书里"min=max=1GHz"的锁频口径在 CPU 上做不到; NPU 的 1GHz 是它的最高频, 可以锁。
   这条影响的是任务②的 A/B 口径, 详见 `task2_fork9_ab.md`。
