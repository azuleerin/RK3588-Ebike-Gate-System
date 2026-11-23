# P0 修复报告 · 正确性/稳定性三项闭环

> 2026-11-22 09:40 ~ 17:05　　分支: **`fix/p0-correctness-stability`**(未 merge)
> 改动面: 4 个文件, 195 增 / 20 删 —— **推理算法零改动**, 只动坐标换算 / 看门狗逻辑 / 启动自检 / 参考工具

---

## 〇、先纠正一条"事实"(重要, 影响任务②的结论)

任务书里写的"stride 的真相不是『形态B 漏乘 stride』"——**这一条不成立**, 而且两个缺陷是**互相独立**的:

| 缺陷 | 在哪 | 性质 |
|---|---|---|
| **A. DFL 内存布局读错** | `accuracy/acc_batch.cc` 的 `decode_dfl_9out`(参考工具) | 读成了【连续 16 个】, 实际是【跨步 grid_len】 |
| **B. 缺 ×stride** | `rk3588_zero_copy/src/yolo_post.cc` 的 `post_process_fork_9out`(生产代码) | DFL 解出来的距离是【格点单位】, 却直接和【像素】的 anchor 中心相减 |

**B 是真 bug, 必须留着修复**, 证据三条(互相独立):

1. **DFL 的量纲**: softmax 是在 16 个 bin 上做的, 期望值天然落在 `[0,15]`。实测三个尺度解出来的
   `d = [0.56,1.31,1.67,3.96] / [1.18,3.55,0.91,5.49] / [0.91,1.47,0.70,3.10]`,
   ×8/16/32 之后才是 `[4,10,13,32] / [19,57,15,88] / [29,47,22,99]` px —— 合理的门禁场景像素距离。
2. **softmax 不变量**: 把 logits 整体乘一个常数**不会改变 softmax 分布** ⇒ "导出时把 stride 烤进 logits"
   在数学上**不可能** ⇒ stride 只能在解码**之后**乘。(`acc_batch.cc` 原来那句注释是错的, 已订正)
3. **工程自己的头文件**: `include/yolo_post.h` 写着一行 —— "ch0..3 = cx, cy, w, h 单位=模型输入图的像素
   (**已乘 stride**)" —— 标准 ultralytics 导出是在图内乘的; fork 形态把 DFL 摘出图, 就必须由 CPU 补上。

**而任务书的验收标准"171/171 框、偏差 < 0.01px"本身也要求 B 的修复在位** —— 不修 B 的话,
形态B 对形态C 是 919 框 vs 171 框、**0 配对**, 根本达不到 171/171。

**A 是真 bug, 也已修**(见子项 1)。两个都修了之后, 三条解码路径**逐位一致**。

---

## 子项 1 · 修形态B 后处理的坐标换算 + 澄清 DFL 布局

### 1.1 改了什么

**`rk3588_zero_copy/src/yolo_post.cc`** —— 两处:

**(a) `compute_dfl` 附近补 DFL 布局注释(防止后人再误读)**:

```diff
+// ★★ DFL 的内存布局 —— 改这个函数之前【必须】先读这段 (2026-11-22 补)
+//
+//   瑞芯微 fork 导出的 box 输出张量是 [1, 4*reg_max, H, W] = [1, 64, H, W],
+//   **NCHW** 布局 (RKNN 运行时按声明的 NCHW 原样返回, 实测同序相关系数 0.9975,
+//   换成 NHWC 排列只有 0.005 —— 见 docs/task2_fork9_ab.md)。
+//
+//   64 个通道的含义是 **c = box * 16 + bin**:
+//       c =  0..15  -> box0(l) 的 16 个 bin
+//       c = 16..31  -> box1(t) 的 16 个 bin
+//       c = 32..47  -> box2(r) 的 16 个 bin
+//       c = 48..63  -> box3(b) 的 16 个 bin
+//   这与 ultralytics 的 DFL 模块完全一致:
+//       class DFL:  x.view(b, 4, self.c1, a).transpose(2, 1).softmax(1)
+//       —— 64 通道被 view 成 (4, 16), 即 c = box*16 + bin。
+//
+//   ⇒ 同一个 anchor 的 16 个 bin 在内存里【相隔 grid_len】(NCHW 的通道维),
+//     不是连续的 16 个! 所以第 (b*16+i) 个 bin 的地址是:
+//         addr(bin) = base + (b*16 + bin) * grid_len        ← 本函数的写法
+//     而【不是】 base + b*16*grid_len + bin(连续读法)。
+//
+//   ⚠ 踩过的坑: 参考工具 accuracy/acc_batch.cc 里的 decode_dfl_9out 用的是
+//     【连续读法】, 于是它读到的其实是"同一个 channel 的连续 16 个 anchor 的
+//     logits", 算出来的框是错的 —— 它和生产 compute_dfl 对不上, 一度让人
+//     以为生产代码有 bug。2026-11-22 已把 acc_batch 那处也改成跨步读法。
+//
+//   ★ 顺带说清楚【单位】: softmax 在 16 个 bin 上做, 期望值天然落在 [0, 15],
+//     所以解出来的距离【一定是格点单位】。而且把 logits 整体乘一个常数
+//     不会改变 softmax 分布 —— 所以"导出时把 stride 烤进 logits"在数学上
+//     不可能, stride 只能在解码【之后】乘(见 post_process_fork_9out)。
```

**(b) `post_process_fork_9out` 里给三个尺度补上 ×stride**(这就是任务书子项 1 要求的):

```diff
                 float d[4];
                 compute_dfl(bt, off, grid_len, d);
 
-                float cx = (static_cast<float>(j) + 0.5f) * static_cast<float>(stride);
-                float cy = (static_cast<float>(i) + 0.5f) * static_cast<float>(stride);
-                YoloBox o;
-                o.cls_id = best_cls;
-                o.score  = best_scr;
-                o.x1 = cx - d[0];
-                o.y1 = cy - d[1];
-                o.x2 = cx + d[2];
-                o.y2 = cy + d[3];
+                // ★★ d[] 是【格点单位】(compute_dfl 的注释写明了: "l, t, r, b (格点单位)")。
+                //   anchor 中心 cx/cy 已经是【像素】, 所以距离必须先 ×stride 换成像素,
+                //   否则就是"像素 - 格点", 单位混用。
+                //   2026-11-22 实测: 不乘的话框坐标平均偏 28~47 px(最大 382 px),
+                //   框宽会小到只剩 1/4 ~ 1/8 —— 跟 acc_batch.cc:58-65 记的是同一个坑。
+                const float st = static_cast<float>(stride);
+                const float dl = d[0] * st;
+                const float dt = d[1] * st;
+                const float dr = d[2] * st;
+                const float db = d[3] * st;
+
+                float cx = (static_cast<float>(j) + 0.5f) * st;
+                float cy = (static_cast<float>(i) + 0.5f) * st;
+                YoloBox o;
+                o.cls_id = best_cls;
+                o.score  = best_scr;
+                o.x1 = cx - dl;
+                o.y1 = cy - dt;
+                o.x2 = cx + dr;
+                o.y2 = cy + db;
                 objs.push_back(o);
```

**`accuracy/acc_batch.cc`** —— 修参考工具的布局读法(只改参考工具, 不动生产):

```diff
-        const float* logits = box + (side * 16) * grid_len + off;
-        float mx = logits[0];
-        for (int i = 1; i < 16; ++i) if (logits[i] > mx) mx = logits[i];
+        // ★ 跨步读(NCHW, c = box*16 + bin): 第 bin 个 bin 的地址
+        //   = off + (side*16 + bin) * grid_len
+        auto logit = [&](int bin) -> float {
+            return box[(size_t)(side * 16 + bin) * grid_len + off];
+        };
+        float mx = logit(0);
+        for (int i = 1; i < 16; ++i) if (logit(i) > mx) mx = logit(i);
         float sum = 0.f;
         float e[16];
-        for (int i = 0; i < 16; ++i) { e[i] = expf(logits[i] - mx); sum += e[i]; }
+        for (int i = 0; i < 16; ++i) { e[i] = expf(logit(i) - mx); sum += e[i]; }
```

同时订正了 acc_batch.cc 里那条**错的**注释("fork 导出的模型把 stride 烤进图里了, 所以工程里那一路不乘")。

### 1.2 布局是怎么钉死的(不是靠推理, 是靠实测)

**实验**: 把 RKNN 原始输出 dump 出来, 跟 onnxruntime 的同名输出逐元素比:

```
RKNN [0] 前6个: [9.286472 6.2273993 4.1515994 2.1850524 1.7480419 -0.32775784]
ONNX [0] 前6个: [9.158598 6.2253923 4.0987144 2.201413  1.6587404 -0.75837994]

同序(NCHW直读) 相关系数 = 0.997507   max|Δ|=3.5764
NHWC 排列后   相关系数 = 0.005338   max|Δ|=16.6487
cls 同序相关系数 = 0.945384
```

⇒ **运行时按声明的 NCHW 原样返回**(不是 NHWC), 所以 64 个通道按 `c = box*16+bin` 跨步读。
工具: `opt_p0/tests/acc_batch_ab --dump-raw`。

### 1.3 对拍证据(任务书要求的 "171/171 框、偏差 < 0.01px")

**方法**: 同一份 fork9 原始输出 → 三条路径各解一遍, 逐框逐坐标比:

| 路径 | 谁在解 |
|---|---|
| 模式 1 | **生产代码**的形态B(瑞芯微 fork 9 输出, CPU 侧 DFL, 修复后) |
| 模式 2 | 参考解码 → 形态C 张量 → **生产代码**的形态C 分支 |
| 模式 0 | `acc_batch.cc` **自带的独立参考解码器**(修复后) |

**40 帧(任务书那个数字)**:

```
同源对拍: 生产形态B(CPU DFL)  vs  生产形态C(同源喂入)
  框数            : 生产形态B=171   生产形态C=171   一致 ✅
  配对框数        : 171  (IoU>=0.5)
  类别不一致      : 0   ✅
  置信度最大差    : 0.000000
  坐标最大偏差    : 0.001920 px   ✅ (<1px)
  坐标平均偏差    : 0.000070 px

同源对拍: 生产形态B(CPU DFL)  vs  acc_batch参考解码器
  框数            : 171 vs 171  一致 ✅     配对 171/171    类别不一致 0
  置信度最大差    : 0.000000
  坐标最大偏差    : 0.000000 px   ✅ (<1px)   ← 逐位一致
```

**全量 1724 帧(回归)**:

```
  框数            : 7386 vs 7386   一致 ✅
  配对框数        : 7386/7386      类别不一致 0      置信度最大差 0.000000
  坐标最大偏差    : 0.001920 px   ✅ (<1px)
  坐标平均偏差    : 0.000060 px
```

**修复前的对照(同一套工具, 只换 yolo_post.cc)**:

| 版本 | 框数 vs 形态C | 配对框数 | 判定 |
|---|---|---|---|
| **修复前** | **919 vs 171** | **0** | ❌ 全错 |
| **修复后** | **171 vs 171** | 171/171, 偏差 0.0019 px | ✅ |

**跨实现交叉验证(独立第三份证据)**: 图内 DFL 的 ONNX 重建版(Arm A v3)用**修复后的** acc_batch 解码后,
与 FP16 参考的匹配率 = **100.00%**、IoU 0.9885 —— 说明我重建的图内 DFL 和标准解码完全一致。
(之前测出来的 "3.13%" 完全是参考解码器读错布局造成的假象。)

### 1.4 顺带重算了检测精度(旧数字是坏解码器算的)

1724 帧, 参考 = `yolov8n_fp16`:

| 模型 | 匹配率 | 类别一致 | 平均IoU | 置信度变化 |
|---|---|---|---|---|
| Arm A v3 FP16(图内DFL) | **100.00%** | 1724 | 0.9885 | +0.0000 |
| Arm A v3 INT8(图内DFL) | 89.68% | 1546 | 0.8375 | −0.0095 |
| **Arm B INT8(fork9, CPU DFL)** | **92.17%** | 1589 | 0.9709 | −0.0089 |
| *(旧报告里的数字, 坏解码器)* | *84.86%* | *1463* | *0.8814* | — |

**⇒ fork9 在 INT8 下反而比图内 DFL 准 2.5pp**(92.17% vs 89.68%)。原因: fork 形态把原始 logits
留在 int8 输出里、在 CPU 上用 float 解码, 不像图内那样让解码中间量也被量化。
**这直接改写了任务②的结论: fork9 不只是省 7.3ms/帧, 精度还更高。**

---

## 子项 2 · 看门狗加「业务帧号是否持续增长」双校验

### 2.1 现状: L2 已经有了, 真正的洞在别处

`scripts/gate_watchdog.sh` 本来就有 **L2 单路心跳**(第 206-290 行): 解析 `[chN][frame M]`,
要求每路的帧号都推进。**任务书描述的"只查日志 mtime"不准确**。

真正缺的是两件事:

1. **整机级、更快的判据**: L2 的"全部路都停了"要等 `CH_STALL_SEC=180s` 才动手。
2. **L2 的盲区**: 如果日志尾部**一条帧号都解析不到**(被别的日志刷出去 / 日志被截断 /
   统计行格式变了), L2 那个 `for` 循环**一个 channel 都不查** —— 等于"看门狗以为正常"。

### 2.2 改了什么(`scripts/gate_watchdog.sh`)

```diff
 CH_SLOW_ROUNDS="${CH_SLOW_ROUNDS:-3}"    # 其余路在推进时, 连续几轮停滞才动手
 L2_COOLDOWN_SEC="${L2_COOLDOWN_SEC:-60}"  # 重启后的 L2 判定冷却窗口(秒)
+SILENT_STALL_SEC="${SILENT_STALL_SEC:-60}" # L1.5 静默卡死阈值: 日志在写但帧号全不涨(秒)
```

在 L2 冷却窗口之后、L2 逐路循环之前, 新增 **L1.5 静默卡死**判据:

```bash
    _log_fresh=0
    [ "$age" -lt "$STALL_SEC" ] && _log_fresh=1
    if [ "$_log_fresh" -eq 1 ] && [ "$_alive" -eq 0 ]; then
        _sil=$(state_get silent_since 0)
        if [ "$_sil" -eq 0 ]; then
            _sil=$now; state_set silent_since "$now"
            note "静默卡死观察开始: 日志在更新(mtime ${age}s 前), 但帧号没涨"
        fi
        _sil_age=$((now - _sil))
        if [ "$_sil_age" -gt "$SILENT_STALL_SEC" ]; then
            _nch=${#FRAME[@]}
            alert "L1.5 静默卡死: 日志仍在更新(mtime ${age}s 前), 但 ${_nch} 路帧号已 ${_sil_age}s 没有增长 (阈值 ${SILENT_STALL_SEC}s) —— 判定业务线程卡死, 重启服务"
            systemctl restart "$UNIT" 2>&1 | head -3 >&2
            state_set last_start_ts "$now"
            state_set silent_since 0
            continue
        fi
    else
        state_set silent_since 0
    fi
```

**阈值可配**: `SILENT_STALL_SEC`(环境变量覆盖, 默认 60s)。防误报的三道闸:
重启后有 `L2_COOLDOWN_SEC` 冷却窗口; 日志 mtime 不新鲜时不判静默(归 L1); 源是有限 mp4 时通道会
rewind 接着放, 帧号不会停。

### 2.3 行为验证(`opt_p0/tests/watchdog_silent_stall_test.sh`)

看门狗脚本是**复制**到沙盒跑的, `systemctl` 用一个**假的**挡在 PATH 前面(只记录调用, 不真的重启任何东西),
全程不碰生产。

| 场景 | 构造 | 期望 | 实测 |
|---|---|---|---|
| **A** | 帧号每���秒 +20, 日志一直在写 | 不误报 | **0 次 restart**, 且从未进入静默观察状态 ✅ |
| **B** | 日志一直在写(写非帧号行), 但帧号冻在 100 | 识别 + 自愈 | **2 次 restart**, 告警:"日志仍在更新(mtime 1s 前), 但 1 路帧号已 11s 没有增长 (阈值 10s)" ✅ |
| **C** | 日志一直在写, 但**一条帧号都解析不到** | 兜住 L2 盲区 | **2 次 restart**, 告警:"但 **0 路**帧号已 11s 没有增长" ✅ |

---

## 子项 3 · 锁文件目录应用层自检

### 3.1 改了什么(`scripts/run_gate_service.sh`)

在建锁文件**之前**加一段自检(目录不存在 / 不可写 → 明确报错 + 写告警 + 非零退出):

```diff
 LOCK="${GATE_LOCK_FILE:-/run/rk-gate/rk-gate.lock}"
 LOCK_DIR="$(dirname "$LOCK")"
-mkdir -p "$LOCK_DIR" 2>/dev/null || true
+
+# ---------------------------------------------------------------------------
+# ★ P0 自检: 锁目录必须【存在】且【可写】, 否则明确报错退出, 不静默失败
+#   ...(注释略, 完整版见代码)
+#   ★ 顺序: systemd 在 ExecStartPre/ExecStart【之前】建 RuntimeDirectory,
+#     所以正常启动时这段自检【一定通过】; 它只是兜底"unit 没生效"的情况。
+#   ★ 退出码用 78 (EX_CONFIG): 这是【配置/部署】问题, 不是瞬时问题,
+#     重试没有意义。unit 里有 RestartPreventExitStatus=78, systemd 不会
+#     陷入重启风暴, 交给看门狗的指数退避处理。
+# ---------------------------------------------------------------------------
+LOCK_CHECK_LOG="${GATE_LOCK_CHECK_LOG:-/var/log/rk-gate-lock-check.log}"
+lock_alert() {
+    # ★ stderr 就是告警通道(unit 的 StandardError=append:/var/log/rk-gate-error.log)
+    echo "[gate] $1" >&2
+    # 再单独留一份到告警文件(尽力而为: /var/log 可能对服务用户不可写)
+    printf '[%s] [gate 锁目录自检] %s\n' "$(date '+%F %T')" "$1" \
+        >> "$LOCK_CHECK_LOG" 2>/dev/null || true
+}
+
+if [ ! -d "$LOCK_DIR" ]; then
+    lock_alert "[自检失败] 锁目录不存在: $LOCK_DIR"
+    lock_alert "  unit 的 RuntimeDirectory=rk-gate 应该在服务启动前把目录建好(owner=服务用户)。"
+    lock_alert "  若 unit 没装上, 手工补: sudo mkdir -p $LOCK_DIR && sudo chown $USER:$USER $LOCK_DIR"
+    exit 78
+fi
+if [ ! -w "$LOCK_DIR" ]; then
+    lock_alert "[自检失败] 锁目录不可写: $LOCK_DIR (当前用户 $USER)"
+    lock_alert "  修: sudo chown $USER:$USER $LOCK_DIR"
+    exit 78
+fi
```

### 3.2 行为验证(`opt_p0/tests/lock_dir_selfcheck_test.sh`)

| 场景 | 期望 | 实测 |
|---|---|---|
| **A** 目录存在且可写 | 自检通过, 锁文件建成 | 退出码 78(因 systemd 托管标记而退, **不启动业务**), 锁文件已建立, 无自检告警 ✅ |
| **B** 目录不存在 | 明确报错 + 非零退出 | 退出码 78, 命中"[自检失败] 锁目录不存在" ✅ |
| **C** 目录存在但不可写(root:root 0555) | 明确报错 + 非零退出 | 退出码 78, 命中"[自检失败] 锁目录不可写" ✅ |

---

## 总验收标准逐条核对

- [x] **形态B stride 修复, 同图两路径坐标偏差 < 1px** —— 171/171 框, 偏差 **0.0019 px**(40 帧)/
      **0.0019 px**(1724 帧); 与独立参考解码器对比 **0.000000 px**
- [x] **看门狗能识别"日志在写但帧号不涨"的静默卡死** —— 场景 B/C 均触发自愈; 场景 A 不误报
- [x] **锁目录自检不静默失败** —— 场景 B/C 明确报错 + exit 78; 场景 A 正常通过
- [x] **全部改动在独立分支, 未 merge** —— 分支 `fix/p0-correctness-stability`, 未 merge 进 main
- [x] **每个改动有对拍/验证证据 + 回滚命令** —— 见各子项 + 第五节
- [x] **生产配置(限帧 20)未变, 4 路出帧正常** —— `GATE_MAX_FPS=20` 未动; 重启后
      ch1~ch4 帧号 400/400/400/400 同步推进; NPU 每核 73/73/75%(基线水平)
- [x] **只动坐标换算 / 看门狗逻辑 / 启动自检** —— 推理算法零改动; `acc_batch.cc` 是参考工具

---

## 回滚命令

```bash
cd /home/seeed/rk_deploy
BK=/home/seeed/rk-gate-fix-backup-20261121_225210

# 1) 源码回滚(三个 P0 改动)
git checkout -- rk3588_zero_copy/src/yolo_post.cc
git checkout -- rk3588_zero_copy/scripts/gate_watchdog.sh
git checkout -- rk3588_zero_copy/scripts/run_gate_service.sh
git checkout -- accuracy/acc_batch.cc

# 2) 二进制回滚
cp -p $BK/rk_gate.binary.prep0.bak rk3588_zero_copy/build/rk_gate

# 3) 重启
sudo systemctl restart rk-gate
```

> 注意: `run_gate_service.sh` 里同时含**任务①的锁文件修复**(第 40-52 行, 已在 main 之外但已装到
> `/etc/systemd/system`)。如果只想回滚 P0 的自检段而保留任务①的修复, 请手工把
> `run_gate_service.sh` 的"★ P0 自检"那一段删掉, 不要整个文件回滚。

**参考工具二进制**也要回滚的话: `accuracy/acc_batch` 是编译产物, 用 git 里的 `acc_batch.cc`
重新编译即可(仓库里没有二进制版本管理)。

---

## 产物清单

```
docs/p0_fixes_report.md                              ← 本文件
opt_p0/tests/acc_batch_ab.cc                         ← 对拍工具(由 make_ab_tool.py 从 acc_batch.cc 生成)
opt_p0/tests/make_ab_tool.py                         ← 生成脚本
opt_p0/tests/pixel_compare.py                        ← 逐框逐坐标对比
opt_p0/tests/compare_boxes.py                        ← 跨模型 IoU 对比
opt_p0/tests/watchdog_silent_stall_test.sh           ← 看门狗三场景验证
opt_p0/tests/lock_dir_selfcheck_test.sh              ← 锁目录自检三场景验证
opt_p0/out/                                           ← 对拍数据(boxes_*.csv / raw_fork9.bin)
```

**没有 merge。生产推理算法一行未改。**
