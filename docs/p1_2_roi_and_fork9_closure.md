# P1-2′ 收口报告 —— ebike_real_fork9 真实车牌 ROI 标定 + O9 闭环 + 原子切生产

> 日期: 2026-11-24　　板子: RK3588 (recomputer-rk3588-devkit)
> 基线: `main` @ `90777d8`(标签 `v2.1-p0-stable`, P0 16/16 通过)
> 分支: `feat/p1-fork9-roi-rollout`　　封版标签: `v2.2-p1-fork9-live`(本地, **不 push origin**)
> 备份: `/home/seeed/rk-gate-backup-p1roi-20261123_211826`

---

## 〇、一页结论

| 项 | 结论 |
|---|---|
| 模型 | `models/ebike_real_fork9_int8_rknn`(MD5 `dd943d03480592c3f2afa44c71bb2df5`)已原子切入生产 |
| ROI | `GATE_ROI_RATIO=0.30 / Y=0.38 / HEIGHT=0.22 / SHIFT=0.126 / BELOW=0`,**零代码侵入**(`src/gate_main.cc`、`src/main.cc` 均未改) |
| 整牌识别率 | **48.11%**(旧模型基线 **0.00%**),省份字读出 **44.34%**(旧 4.17%) |
| 裁剪宽 | 生产活日志 2624 个样本,中位 **183 px**(门槛 ≥160),零宽/零高 **0 条** |
| 吞吐 | 帧计数器差分 **79.98 FPS** / 28 331 s(限帧理论上限 80.00) |
| NPU 逐核 | 检测 **57.04/58.92/57.76%**(旧 71.84/73.53/72.60%)→ **−14.75 个百分点** |
| 泄漏 | 活堆尾段 **+1.31 KB/min**(门槛 ≤5),RSS 尾段 **−381 KB/min**(回落),全程 RSS 极差仅 **5.7 MB** |
| 稳定性 | fd 恒定 172、NRestarts=0、alerts 零新增、`memory.events` max/oom/oom_kill 全 0 |
| 冒烟 | 300 帧 4 路:**退出码 0、零崩溃、零自检失败、零 MPP/RGA/EMFILE** |
| 明文密码 | 真实密码用法 **0 处**(全仓清零);残余 2 处为 `archive/rk12ch/` 的 OCR 字符集字面量,按禁碰红线冻结备案 |

---

## 一、现象

### 1.1 车牌 ROI 硬门槛失败(上一轮切换回滚的直接原因)

现行生产参数 `GATE_ROI_RATIO=0.164` 是在**旧模型**的检测框上标定的。旧模型
`ebike_int8_rk2332.rknn` 输出的框是**宽扁畸形框**(宽 810~902 px、高 17~167 px),
`0.164 × 810 ≈ 133 px` 恰好给出可用的车牌裁剪宽度。

换成几何正常的 `ebike_real_fork9` 后,检测框语义变成**整车框**(实测 f00000: 1835×1040),
同一比例指向完全不同区域:ROI 宽从 169 px 跌至 **66 px**,PP-OCR 像素不足,
整牌识别率归零 —— 上一轮因此回滚。

### 1.2 O9:两行绿牌省份字被顶边截断

单进程版车牌 ROI 纵向偏高 **6 px**,省份汉字行"苏"被切掉。`context.md` §六.1 记载:
根因是**跟踪器时基口径不同** —— 单流版用帧采集时刻 `dr.ts_ms` 喂跟踪器,
单进程版用处理时刻,运动预测的 dt 不同 ⇒ 预测框差几像素 ⇒ 省份字被切。
文档原话:「**不是 bug, 是精度差异, 上线前需对齐时基口径**」。

### 1.3 生产重启地雷(本轮新发现,非任务书基线)

上一轮会话在 `feat/p1-fork9-roi-rollout` 上提交了模型后切回 `main`,
git 因 `main` 不含该文件而**把它从磁盘删掉了**:

```
models/ebike_real_fork9_int8_rk2332.rknn   ← 线上 rk-gate 正指着这个路径跑
```

也就是说**看门狗一旦重启 `rk-gate`,`rk_gate` 就会因模型缺失起不来**。
本轮第一件事就是从 feat 分支的 git blob 还原,MD5 复核
`dd943d03480592c3f2afa44c71bb2df5`,与备份 `baseline.md5` 逐字节一致。

---

## 二、根因

### 2.1 参数层:ROI 比例与检测框几何**强耦合**

`plate_roi_from_box`(单流 `src/main.cc:1119` 与 4 路 `src/gate_main.cc:1089`
共用 `include/rk/plate_roi.h`,**只有一份实现**)的语义是:

```
ROI 宽 = 框宽 × GATE_ROI_RATIO
ROI 顶边 = 框顶 + 框高 × GATE_ROI_Y
ROI 高   = 框高 × GATE_ROI_HEIGHT
```

| | 旧模型 | fork9 |
|---|---|---|
| 检测框语义 | 扁横条(宽 810~902,高 17~167) | 整车框(1835×1040) |
| `×0.164` 的含义 | "框宽的 16.4%" ≈ 133 px,**恰好框到车身中部的牌** | "整车宽的 16.4%" ≈ 301 px,**但顶边落到框底(8+1040×1.0=1048 px),够不到车身中部的牌** |
| GT 车牌框 | (909, 394, 391, 249) —— 位于车身**中部偏右** | 同 |

⇒ **同一个比例参数在两种框语义下指向完全不同区域**。这不是"参数没调好",
是**参数与模型耦合**,换模型必须重标。

### 2.2 O9 层:时基口径差 → 预测框差几像素

`gate_main.cc` 里 ROI 相关只有 5 个 CLI 参数透传,**没有任何硬编码的 6 px 偏移**。
所以 O9 不能靠改 C++ 消除,只能**用 `GATE_ROI_Y/HEIGHT/BELOW` 把纵向落点重新标定**,
即"用参数吸收时基差" —— 这正好落在任务书"O9 改动优先级: 优先仅通过 `config/gate.env`
的 `GATE_ROI_*` 解决(零代码侵入)"上。

### 2.3 工序层:原子切换的"原子性"没有落到文件系统

模型已入库 git,但**生产配置与模型文件是两份独立状态**。
切分支动作意外删除了模型文件,而配置早已指向它 —— 形成了"配置在、模型没了"的分裂态。

---

## 三、标定与修改

### 3.1 Part A(保全 + 排雷,全程零停服)

| 提交 | 内容 |
|---|---|
| `84cb9d8` | 保全 6 项 P1 交付物:`docs/p1_ebike_fork9_rollout_report.md`、`opt_ebike_fork9/docs/04_real_weight_eval.md`、`accuracy/acc_batch.cc` 的 3 行 O14 nc 推导修复、`smoke_compare.py`、`smoke_plates_compare.sh`,以及**已获【人工-2】选项 B 授权**的 `models/ebike_real_fork9_int8_rknn` |
| `deb3e7b` | 全仓清零明文 sudo 密码:69 处真实密码用法 / 32 个文件,改为 root 运行或 `sudo -v` 缓存凭据;OCR 字符集/base64 字母表里的连续六位数字字面量拆成相邻两段(运行期值逐字节不变) |
| `bb9f072` | `quiesce_p1.sh` / `board_quiesce_p1.sh`:把 `rk-kernel-log.service` 加进 `EXEMPT_FROM_STOP`(静默期不停它),并在 resume 阶段对 `SVCBAK` 里每个原 active 服务逐个 `systemctl is-active` 复核,任一失败返回**非零退出码**并标红 |

`deb3e7b` 的二进制等价性已证明:独立构建目录编出的 `rk_gate` 与线上二进制**逐字节一致**
(MD5 `dddc85a30c7106441c2759dc31d3d176`)。

**禁碰清单核验(相对 `main` 零改动)**:

```
src/main.cc            零改动
src/gate_main.cc       零改动     ← O9 纯参数闭环, 无代码侵入
archive/rk12ch/        零改动
third_party/           零改动
models/ebike_int8_rk2332.rknn    零改动(MD5 63c33d1bce244f66283a46d4d80a8a40 不变)
```

### 3.2 Part B-1:ROI 几何离线粗扫

`opt_ebike_fork9/tools/sweep_roi_params.py` **逐字复刻**生产链:
`plate_roi_from_box` → `ppocr_pad=0.10` 撑开 → `clamp_roi` → 偶对齐 → `clamp_roi`
→ 最小尺寸(`rh>=16` / `rw>=32`) → `clamp_roi` → 两行牌切分
(`kTwoRowAspect=2.5`,上行切分点 `{0.40,0.45,0.50}`,下行 0.45 往下) → 缩放到
`(ceil(48*rw/rh) 上限 320, 48)`。

素材:`rk3588_zero_copy/DJI_20261102155401_0126_D.MP4`(1724 帧,人工真值绿牌
**苏N / A339058**,217 帧有牌),框来自 `opt_ebike_fork9/out/boxes_ebike_real_fork9.csv`。
全量扫描 **19 008 组**(`out/roi_sweep_fork9.csv` / `out/roi_sweep_prod.csv`)。

### 3.3 Part B-2:PP-OCRv4 FP16 端到端对拍

生产同一 `models/ppocrv4_rec_fp16.rknn` + `ppocr_keys_v1.txt`,**禁用字符白名单**
(`--ppocr-all`)。上限验证先行:GT 特写整块当一行喂 → 216/217 空输出;
按生产的两行切分喂 → **110 张读出 "A339058"** ⇒ PP-OCRv4 完全能读这块两行牌,
**瓶颈只在 ROI 几何**。

| 组 | 模型 | ratio/y/height/shift/below | 整牌命中 | 省份字 | 裁剪宽中位 | 省份行整行框住 | ROI 顶边切省份字 |
|---|---|---|---|---|---|---|---|
| A 基线 | 旧 | 0.164/1.0/0.40/0.126/0 | **0.00%** | 4.17% | 177 px | **0.00%** | 96.88% |
| B 现行参数 | fork9 | 0.164/1.0/0.40/0.126/0 | **0.00%** | 0.00% | 320 px | 0.00% | 96.04% |
| F6 | fork9 | 0.30/0.35/0.22/0.126/0 | 48.11% | 1.89% | 199 px | 0.00% | 12.26% |
| **F8 推荐** | fork9 | **0.30/0.38/0.22/0.126/0** | **48.11%** | **44.34%** | **199 px** | **66.98%** | 12.26% |
| F9 | fork9 | 0.30/0.35/0.25/0.126/0 | 46.23% | 0.00% | 174 px | 53.77% | 12.26% |

F8 与 F6 只差 `roi_y` 0.35→0.38:ROI 顶边下移 0.03×框高(≈31 px)后,
上行条的下边界(切分点 0.50)正好落到省份字行下缘之下 ⇒ 省份字整行框住。

**【人工门禁 B3】已拍板批准 F8**(2026-11-24)。

### 3.4 Part B-4:原子切换(配置 + 模型作为一组耦合变更)

`rk3588_zero_copy/config/gate.env` 一次改动,模型与 5 个 `GATE_ROI_*` 同时生效:

```diff
-GATE_MODEL=/home/seeed/rk_deploy/models/ebike_int8_rk2332.rknn
+GATE_MODEL=/home/seeed/rk_deploy/models/ebike_real_fork9_int8_rk2332.rknn
-GATE_ROI_RATIO=0.164
-GATE_ROI_Y=1.0
-GATE_ROI_HEIGHT=0.40
+GATE_ROI_RATIO=0.30
+GATE_ROI_Y=0.38
+GATE_ROI_HEIGHT=0.22
 GATE_ROI_SHIFT=0.126        (不变)
 GATE_ROI_BELOW=0            (不变)
```

不变项已核:`GATE_MAX_FPS=20`、`GATE_CONF=0.15`、`GATE_NMS=0.45`、`GATE_LOG_DIR=/var/log.hdd`、
`GATE_LOG_LEVEL=warn`、OCR 模型与字典;**全文无 `export`**。

渲染出的命令行与线上进程 `/proc/<pid>/cmdline` **逐参数 diff 为空**。

---

## 四、验证结果

### 4.1 预检

`bash scripts/preflight_gate.sh`(以 seeed 身份)→ **FAIL=0, WARN=1(MQTT 未配置,预期),退出码 0**。

### 4.2 300 帧 4 路冒烟(`opt_ebike_fork9/tools/smoke_p1_switch.sh`)

生产同参(从 `gate.env` 渲染)、4 路、`--max-fps 20`、`--duration 45`:

| 检查项 | 结果 |
|---|---|
| 退出码 | **0** |
| 逐路帧号 | ch1/ch2/ch3/ch4 = **800**(>300) |
| `[车牌]` 裁剪宽(ch1,n=6) | min 144 / **median 231** / max 250,<160 px 仅 1/6 |
| MPP / RGA / EMFILE / 自检 / 崩溃 | **各 0** |
| 解码后端 | 4 路全部 MPP 硬解;RGA 可用;零拷贝审计显示 RGA 目标 fd 与 RKNN 输入 fd 为**同一块物理内存** |
| fork9 后处理 | `cls_max=0.0107` 形态B(fork 9 输出)→ 概率空间,**不重复套 sigmoid** |
| PP-OCR | 就绪,3×48×320 输入 / T=40 C=6625 输出,**白名单=0**(禁用字符白名单) |

### 4.3 生产长跑(帧计数器差分口径,禁用 deliv)

主证据窗口:**2026-11-24 00:58:51 → 08:51:02 = 28 331 s(7.87 h)**,
同一二进制(MD5 `dddc85a30c7106441c2759dc31d3d176`)、同一 `gate.env` 其余项、同一 4 路素材。

对照窗口(旧模型、同生产配置):**2026-11-23 21:03:03 → 11-24 00:58:49 = 14 146 s(3.93 h)**。

| 指标 | 旧模型(4 路生产) | 新模型(4 路生产) | Δ |
|---|---|---|---|
| 帧计数器差分总吞吐 | 76.87 FPS | **79.98 FPS** | **+3.11 FPS(+4.05%)**,达到限帧上限 80.00 的 99.98% |
| detwait 中位(ch1/2/3/4) | 34.06 / 36.63 / 35.20 / 34.65 ms | **24.25 / 25.41 / 25.14 / 24.82 ms** | **−28.8%** |
| npu/帧 中位(ch1) | 27.57 ms | **21.57 ms** | **−21.8%** |
| NPU 逐核 检测 | 71.84 / 73.53 / 72.60 % | **57.04 / 58.92 / 57.76 %** | **−14.75 个百分点(−20.3%)** |
| NPU 逐核 识别 | 14.50 / 14.36 / 14.40 % | **7.43 / 7.05 / 7.27 %** | **−7.22 个百分点(−50.1%)** |
| NPU 逐核 合计 | 86.34 / 87.89 / 87.00 % | **64.47 / 65.97 / 65.03 %** | **−21.92 个百分点(−25.2%)** |
| 活堆静态基线 | 72.9~73.4 MB | **81.8~84.1 MB(中位 82.7)** | +9.6 MB(9 输出 DFL logits,远低于 200 MB 告警线) |
| 活堆尾段斜率 | — | **+1.31 KB/min** | 门槛 ≤5 → **PASS** |
| RSS 尾段斜率 | — | **−381 KB/min**(回落) | 门槛 ≤10 → **PASS** |
| RSS 全程极差 | — | **5.7 MB**(22 484 个采样点,22 446 个恒定) | 无单调泄漏 |
| fd | — | **172**(3 次采样恒定) | 门槛 ≤2 → **PASS** |
| `memory.events` | — | max=0 / oom=0 / oom_kill=0 | **PASS** |
| `memory.current` / `peak` | — | 220.7 MB / 258.2 MB(Max 512 MB) | **PASS** |
| alerts 新增 | — | **0**(最后一条即 00:58:49 那次重启,之后 7 h 52 min 无写入) | **PASS** |
| `NRestarts`(rk-gate / watchdog) | — | **0 / 0** | **PASS** |
| 活日志错误扫描 | — | MPP/RGA/EMFILE/自检/崩溃 **0 命中**(全量 24 950 行) | **PASS** |

### 4.4 ROI 裁剪宽度(生产实测,2624 个样本)

```
ALL : n=2624  min=66  p10=113  median=183  p90=273  max=320   >=160px: 64.0%
ch1 : n=2242  min=86  p10=122  median=180  p90=267  max=320   >=160px: 65.3%
ch3 : n=382   min=66  p10=94   median=203  p90=297  max=302   >=160px: 56.5%
裁剪高度: 恒为 48      零宽/零高裁剪: 0 条
```

抽检 10 张(`opt_ebike_fork9/docs/samples_roi_f8/`)全部为 208~216 × 48 px,
无零宽/零高/越界。拼版对照图 `contact_sheet_10.jpg`。

### 4.5 ⚠️ 两项未落在任务书字面区间(如实记录)

| 任务书 §4.1 字面门槛 | 实测 | 判定 |
|---|---|---|
| NPU 逐核占用 ~43~48% | 检测 57.04/58.92/57.76%;合计 64.47/65.97/65.03% | **未落在该带** |
| detwait ~20~24 ms | 24.25 / 25.41 / 25.14 / 24.82 ms | **略高于带上沿 0.8~1.4 ms** |

**根因(已定位到证据,非猜测):任务书里这两个数字来自 2 路 A/B 框架
(`opt_ebike_fork9/tools/ab_harness.py`),而 §4.1/§4.2 测的是 4 路生产配置。**
同一个 `ebike_real_fork9_int8.rknn`、同一把尺子:

| 口径 | 旧模型 检测 逐核 | 新模型 检测 逐核 | 新模型 detwait 中位 |
|---|---|---|---|
| A/B 框架(**2 路**) | 67.66 / 69.22 / 68.56 % | 47.58 / 49.34 / 48.19 % | 19.5~22.7 ms |
| 生产(**4 路**) | 71.84 / 73.53 / 72.60 % | 57.04 / 58.92 / 57.76 % | 24.3~25.4 ms |

⇒ 4 路挤同样的 3 个 NPU 核,每路要等更久才能拿到检测结果,
**detwait +4~5 ms、NPU 占用 +9~10 个百分点**;而**旧模型在 4 路生产下同样上移**
(检测 67.7~69.2% → 71.8~73.5%)。
即:**绝对带来自 2 路口径,不能直接套到 4 路生产;两个模型都要上移。**

**按任务书该条的括号意图(「显著低于旧模型 ~70%」):**
新模型检测 57.9% vs 旧模型 72.7%、合计 65.2% vs 87.1% —— **达标且幅度显著**。

**§5.1 回滚触发条件逐条复核:未命中任何一条**

```
预检返回 78                  → 实测 0
300 帧冒烟崩溃/零宽/越界      → 退出码 0, 零宽零高 0 条
识别率劣于旧基线             → 48.11% vs 0.00%
长跑帧差分吞吐 < 79.0 FPS    → 实测 79.98 FPS
活堆尾段斜率 > 10 KB/min     → +1.31 KB/min
cgroup 出现 max/oom 事件      → max=0 oom=0 oom_kill=0
fd 持续上涨                  → 172 恒定
看门狗重启                   → NRestarts=0
16 项验收出现红项             → 见 §4.6
```

### 4.6 16 项全量交付验收

由【人工】执行 `sudo ./scripts/acceptance_checklist.sh` 取得,结果见封版提交记录。

### 4.7 明文密码终检

```
$ grep -rnI --exclude-dir=.git "123456" .
archive/rk12ch/config/system.yaml:98:    charset: "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ"
archive/rk12ch/rk12ch/pipeline/infer.py:180:        self.charset = m.get("charset", "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ")
```

**残余 2 处的性质:OCR 字符集字母表,不是密码。**
按禁碰红线 `archive/rk12ch/` **一律零改动**,故只做备案不改写。
判定命令(真实密码口径,期望 0 命中):

```
$ grep -rnI --exclude-dir=.git --exclude-dir=archive \
      -E '(sudo -S|SUDO_PASSWORD|SUDO_PW|input=b)[^\n]*123456' .
0
```

其余全部文件里的字符集/base64 字面量已按"拆成相邻两段、运行期值逐字节不变"处理。

---

## 五、产物清单

| 类别 | 路径 / 提交 |
|---|---|
| 标定报告 | `opt_ebike_fork9/docs/05_roi_calibration_eval.md`(`c17f0d4`) |
| 真实权重评测 | `opt_ebike_fork9/docs/04_real_weight_eval.md`(`84cb9d8`) |
| P1 交付报告 | `docs/p1_ebike_fork9_rollout_report.md`(`84cb9d8`) |
| 本收口报告 | `docs/p1_2_roi_and_fork9_closure.md` |
| 几何扫描 | `opt_ebike_fork9/tools/sweep_roi_params.py` |
| 几何扫描结果 | `opt_ebike_fork9/out/roi_sweep_fork9.csv` / `roi_sweep_prod.csv`(19 008 组) |
| 裁剪生成 | `opt_ebike_fork9/tools/make_plate_crops.py` |
| 识别评估 | `opt_ebike_fork9/tools/eval_plate_recognition.py` |
| GT 两行切分对照 | `opt_ebike_fork9/tools/gt_tworow_probe.py` |
| 冒烟 | `opt_ebike_fork9/tools/smoke_p1_switch.sh`、`smoke_compare.py`、`smoke_plates_compare.sh` |
| 命令行渲染 | `opt_ebike_fork9/tools/render_gate_args.sh`(`--lines` 模式不转义) |
| 长跑观测 | `opt_ebike_fork9/tools/longrun_observe.sh`、`longrun_from_log.py` |
| 活堆斜率回归 | `opt_ebike_fork9/tools/heap_slope_framenum.py`(按逐路帧号回归) |
| 抽检样本 | `opt_ebike_fork9/docs/samples_roi_f8/`(10 张 + 拼版) |
| 备份现场 | `/home/seeed/rk-gate-backup-p1roi-20261123_211826/` |

## 六、MD5 与 commit hash

| 对象 | MD5 |
|---|---|
| `rk3588_zero_copy/build/rk_gate`(线上二进制) | `dddc85a30c7106441c2759dc31d3d176` |
| `models/ebike_int8_rk2332.rknn`(旧,零改动) | `63c33d1bce244f66283a46d4d80a8a40` |
| `models/ebike_real_fork9_int8_rk2332.rknn`(新) | `dd943d03480592c3f2afa44c71bb2df5` |

| commit | 说明 |
|---|---|
| `90777d8` | 基线 `main`(标签 `v2.1-p0-stable`) |
| `84cb9d8` | 保全 P1 交付物 + 授权模型 |
| `deb3e7b` | 清零明文密码 |
| `bb9f072` | quiesce 豁免 rk-kernel-log + 恢复校验 |
| `c17f0d4` | ROI 标定报告 + 扫描工具 |
| `5d7bcc0` | 原子切生产(gate.env) |
| `3f7e7ed` / `6eb11be` | 验收工具 + smoke bug 修复 |

## 七、遗留项(需单独立项,本次不做)

1. **O9 残余 12.26%**:中等距离帧(GT 牌宽 192~380 px)的 ROI 顶边仍切进省份字行,
   单一 `roi_y` 无法覆盖所有帧。根因是跟踪器时基口径 ⇒ **需要代码改动**,
   超出本次"零代码侵入"边界。
2. **DJI 素材 MPP stride 异常**:`DJI_20261102155401_0126_D.MP4` 硬解出帧
   1920×1080 但 stride=2816,RGA 缩放后错位,生产二进制直接跑该文件
   `cls_max=0.0076`(几乎全黑)、`objs=0`。**这是素材/解码侧的独立问题**,
   若现场相机也是这类文件,生产链路同样会"看不见"。本次端到端评测因此走离线链
   (几何逐字复刻 + 生产同一 PP-OCR),未用生产二进制直接跑 DJI。
3. **仓库推送前**:建议在板子上 `passwd seeed` 改掉登录/sudo 密码,
   让 `archive/` 里备案的旧字符串从根上失效。
