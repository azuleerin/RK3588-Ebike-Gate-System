# P1 · ebike fork9(真实权重) 上线报告

> 日期: 2026-11-23　　板子: RK3588 (recomputer-rk3588-devkit)
> 仓库: `/home/seeed/rk_deploy`　　分支: `fix/p0-memleak-converge` @ `d9c0c4c`
> 结论一句话: **模型侧全部达标(NPU −24.2%, 4 路解限帧吞吐 +40.7%), 但上线被"车牌识别硬门槛"拦下,
> 已按预案回滚, 生产仍在跑原模型; 新模型已归档待用, 下一步先重标 `GATE_ROI_*`。**

---

## 一、现象与目标

生产检测模型 `models/ebike_int8_rk2332.rknn` 把 **DFL 解码整条子图放在 NPU 里**
(逐算子表实测: DFL 链 25 个算子, 合计 **10875 us**, 占整模型 37.0%)。
目标: 同一份权重, 把 DFL 搬到 CPU(NPU 只吐原始 DFL logits 的 9 输出形态),
换单帧 NPU 耗时下降, 且框坐标不低于现状。

本次用的是**真实权重** `best_split.onnx`(算法组 `exp02_person_ebike_gpu2`), 不是模板占位权重。

### 1.1 现场冻结(任务书 §二)

```
备份: /home/seeed/rk-gate-backup-p1fork9-20261123_185030
  gate.env.bak / acc_batch.cc.bak / baseline.md5 / git_branch.txt / git_head.txt
baseline.md5:
  63c33d1bce244f66283a46d4d80a8a40  models/ebike_int8_rk2332.rknn
  dddc85a30c7106441c2759dc31d3d176  rk3588_zero_copy/build/rk_gate
git: fix/p0-memleak-converge @ d9c0c4c
```

---

## 二、转换与自检

| 项 | 实测 |
|---|---|
| 输入 ONNX | `opt_ebike_fork9/onnx/ebike_best_split.onnx`, 12,266,908 B, md5 `e1a432a61924faa3a7e25ac71668d658` |
| `onnx.checker` | OK; ir_version 8, producer pytorch 2.5.1, **opset 17** |
| 输入/输出契约 | 输入 `images` [1,3,640,640]; 原生输出 [1,4,8400] + [1,2,8400] ⇒ **nc=2 (person/ebike)** |
| fork9 ONNX | 9 输出 `[1,64,H,W]/[1,2,H,W]/[1,1,H,W]` × 80/40/20 ⇒ `verify_fork9_structure.py` **PASS** |
| 形态C 孪生 ONNX | [1,4,8400] + [1,2,8400]; `verify_formC_equiv.py` **PASS, max\|Δ\| = 0.000079 px**(亚像素) |
| 产物 | `models/ebike_real_fork9_int8.rknn` (md5 `dd943d03…`, 4,176,326 B) / `models/ebike_real_formC_int8.rknn` (md5 `243dec63…`) |
| 编译器 | 生产/fork9/formC 三者均 **2.3.2**; 工具链 rknn-toolkit2 **2.3.2**(与板端 librknnrt 2.3.2 一致) |
| 标定集 | `calib/ebike_calib.txt` 200 张路口实拍, 与 1724 帧回归集**零交集** |

opset 17 ⇒ 不存在 opset≤12 的 Softmax legacy 2D 强制转换(零宽框静默缺陷那一类)。

---

## 三、A/B 与精度实测

### 3.1 逐算子 NPU 审计(NPU 锁 1 GHz, 板子静默, 逐核 0/0/0)

| 模型 | 算子数 | 逐算子合计 | NPU | CPU | CPU 计算回落 |
|---|---|---|---|---|---|
| 生产 `ebike_int8_rk2332` | 122 | 29369 us | 29347 us | 22 us | 无 |
| **新 `ebike_real_fork9_int8`** | **108** | **22258 us** | 22196 us | 62 us | **无** ✅ |
| 孪生 `ebike_real_formC_int8`(对照臂) | 154 | 37571 us | 36412 us | 1159 us | 6 个 Sub 落 CPU(3.03%) |

- **fork9 vs 生产: −7111 us(−24.21%)**; `driver_run` 31198 → 23824 us(−23.63%)
- fork9 vs 同权重孪生(唯一变量=DFL 位置): **−15313 us(−40.76%)**

### 3.2 4 路交错 A/B(base,new,base,new; 帧计数器差分口径)

**口径① 生产限帧 `--max-fps 20`**(每轮 100 s, 剔除前 15 s / 后 5 s)

| 指标 | base | new | 变化 |
|---|---|---|---|
| 总吞吐 | 79.973 / 80.020 FPS | 80.007 / 80.039 FPS | **均 ≥79.5 ✅**(限帧天花板 80) |
| npu | 25.62 ms | **18.29 ms** | **−28.6%** |
| detwait | 31.33 ms | **20.58 ms** | **−34.3%** |
| ocrwait | 55.49 ms | 54.70 ms | 持平 ✅ |
| 逐核 NPU | 71.0 / 72.8 / 71.5 % | **42.8 / 44.9 / 43.8 %** | **−40%** |

**口径② 解除限帧 `--max-fps 0`**(每轮 180 s, 剔除前 20 s / 后 5 s)

| 指标 | base | new | 变化 |
|---|---|---|---|
| 总吞吐 | 108.66 / 106.62 FPS(均值 **107.64**) | 151.38 / 151.56 FPS(均值 **151.47**) | **+43.83 FPS(+40.72%)** ✅ |
| npu | 24.06 ms | 18.04 ms | −25% |
| detwait | 31.12 ms | 22.00 ms | −30% |
| ocrwait | 52.84 ms | 53.26 ms | 持平 ✅ |
| 逐核 NPU | 85.8 / 87.0 / 86.5 % | 74.4 / 75.9 / 74.8 % | −13% |

base 的 107.64 FPS 落在历史基线区间 **105.02–109.78 FPS** 内 ⇒ 口径与历史可比 ✅。
每轮起跑前逐核 `rknpu/load` 均为 **0 0 0**; 有一轮因外部会话干扰起跑时占用 >8%, **已按纪律作废重跑**。

### 3.3 1724 帧精度回归

| 组 | 匹配率(IoU≥0.5) | 类别一致率 | 平均 IoU | 漏/多 | 分数差 |
|---|---|---|---|---|---|
| A: fork9 vs 同权重形态C(唯一变量=DFL 位置) | **91.19%** (300/329) | 100.00% | 0.8020 | 29 / 29 | −0.0001 |
| B: fork9 vs 生产模型 | 34.43% (115/334) | 100.00% | 0.7242 | 219 / 214 | +0.0007 |

**组A 的 91.19%(门槛 98.0%)**: 根因实测 = NPU 内 INT8 DFL 相对 CPU FP32 DFL 的 **±1~5 px** 量化误差
(同帧两臂坐标差 mean 2.22 px / IoU 0.9879), 在小目标上击穿 IoU 0.5。
用同一套工具回测模板权重对是 99.78% ⇒ 工具链无偏, 这份真实权重特有。**fork9 是更准的那一臂。**
按【人工-2】复议: 改用替代判据(类别一致率 ≥99.5% / 平均 IoU ≥0.80 / 零宽零高框 = 0)⇒ **通过**。

**组B 的 34.43%**: 根因 = 生产模型的 box 张量与这份 ONNX 的 dist2bbox 输出**逐 anchor 对不上**
(同 anchor IoU 0.0835, 任何读法/任何重排都对不上), 详见 §3.4。

### 3.4 附带查明: 生产模型的框"中心对、尺寸错"(重要发现)

| 通道 | 语义 | 与真实权重 fork9 的相关 | 高置信 anchor 上的比值中位数 |
|---|---|---|---|
| ch0 | 框中心 x | +0.9865 | 1.008 |
| ch1 | 框中心 y | +0.9500 | 1.004 |
| ch2 | 框宽 | +0.8861 | **0.468** |
| ch3 | 框高 | **+0.3310** | **0.135** |

1724 帧解码框统计: 生产模型 **706 框**, 宽高比中位 **1.789**, **23.7% 零高度**;
真实权重 fork9 **383 框**, 宽高比中位 **0.482**, 零高度 0。

已排除的解释: 运行时 `want_float=1` 反量化**逐位无损**(max\|Δ\|=0); 两模型 box 量化 scale
**2.54839 vs 2.54894**(差 0.02% ⇒ 同权重同标定); xy 互换/半宽/双宽等 4 种约定全试均对不上。
⇒ 生产模型不是由这份 `best_split.onnx` 编译出来的(它带的是另一版 DFL 收尾子图),
**它的框尺寸是错的**(中心对、宽高错)。

---

## 四、切换验证(已执行 → **硬门槛不通过 → 回滚**)

### 4.1 上线前准备(全部完成)

| 项 | 结果 |
|---|---|
| O14 nc 推导修复合入 `accuracy/acc_batch.cc` | ✅ 3 行(fork9 用 `dims[n_dims-3]`; 形态C 走 `yolov8_postprocess` 内部 `dims[n_dims-2]`) |
| 重编译 `acc_batch` | ✅ 零警告 |
| nc=80 模型输出不变验证 | ✅ `yolov8n_int8_calib200.rknn` 1724 帧输出 **MD5 `f0a6796727b173e6a0dbe45f5098c9fc` 修复前后逐字节一致**(`cmp` 通过) |
| 附带修掉的 bug | ✅ 原版跑 nc=2 模型(`models/best.rknn`)**Segmentation fault**(nc 写死 80 → 越界读); 修复后正常跑完 1724 帧 |
| 模型归档(选项 B) | ✅ `models/ebike_real_fork9_int8_rk2332.rknn` (md5 `dd943d03…`); 旧 `ebike_int8_rk2332.rknn` **保留未删** |
| `config/gate.env` | 切换期间只改 `GATE_MODEL=` 一行(无 export、无空格), diff 仅 1 行 |
| 12 项预检 | ✅ **FAIL=0 / WARN=1(MQTT 未配) / 退出码 0**(不是 78) |
| 300 帧冒烟(4 路) | ✅ 两轮各 250 帧/路, **零崩溃、零自检失败、零告警**, 4 路全出帧 |

### 4.2 ✘ 车牌识别硬门槛: **不通过 ⇒ 回滚**

【人工-2】决议: "切, 但把车牌识别对比设为硬门槛, 不达标立刻回滚"。

同一生产二进制、同一路视频(ch3 近景)、70 s:

| 模型 | 走到车牌 ROI 的轨迹数 | ROI 裁剪文件 | 裁剪宽度中位 | **[车牌] 识别行数** |
|---|---|---|---|---|
| 生产模型 | **130** | 414 | **169 px** (66~320) | **7** |
| 新 fork9 | **37** | 128 | **66 px** (19~96) | **0** |

根因(公式级, `src/main.cc:1119 plate_roi_from_box`):

```cpp
float w = bw * ratio;   // ROI 宽 = 检测框宽 × GATE_ROI_RATIO(0.164)
```

生产模型的框是 810×17 / 902×167 这种宽扁畸形框(§3.4), ROI 宽 = 810×0.164 ≈ 133 px,
**恰好是 PP-OCR 能用的分辨率**; 换成几何正确的 fork9(框宽 ~171 px)后
ROI 宽 ≈ 28 px —— **PP-OCR 没有分辨率了**。

⇒ **`GATE_ROI_*` 这套参数是照着旧模型的畸形框宽标定的**; 换模型必须同时重标 ROI,
而任务书的单变量红线禁止在本次动 `GATE_ROI_*`。

(补充: 4 个测试片段里没有清晰车牌, 旧模型那 7 条"识别结果"('#','品','百','中')是 PP-OCR 在模糊路面条带上的噪声,
不是真车牌; 目视核对两边的 ROI 裁剪图也都是路面。所以本片段无法用"识别率"直接判生死,
但**裁剪宽度 169→66 px(最大 320→96)是确定性的、公式驱动的** —— 现场真实车牌需要 ≥160 px 宽。)

### 4.3 回滚执行与生产现状核验

```
① gate.env 已回滚: diff 与切换前【逐字节一致】
   GATE_MODEL=/home/seeed/rk_deploy/models/ebike_int8_rk2332.rknn
② rk-gate / rk-gate-watchdog: active(active), 线上 --model = ebike_int8_rk2332.rknn
③ /run/rk-gate/paused 已删; /run/systemd/system/rk-*.d 临时 drop-in 已撤; daemon-reload 已做
④ NPU 频段已还原: governor=rknpu_ondemand, min=300000000, max=1000000000(与备份一致)
⑤ 4 路出帧: ch1=1900 / ch2=2000 / ch3=2000 / ch4=2000, npu=27.4 ms, 活堆=73.4 MB(P0 稳态)
⑥ 车牌识别: ch1 "[车牌] ... 裁剪=320x48 -> "100"" / ch3 正常输出
⑦ /var/log/rk-gate-alerts.log: 20:00 之后【零新增告警】
```

**生产状态 = 切换前的原样**; 新模型文件留在 `models/` 里但**未被任何配置引用**(无害, 待 ROI 重标后启用)。

---

## 五、交付物清单

| 类别 | 路径 |
|---|---|
| 上线报告(本文) | `docs/p1_ebike_fork9_rollout_report.md` |
| 详细实测报告 | `opt_ebike_fork9/docs/04_real_weight_eval.md` |
| 真实权重 ONNX | `opt_ebike_fork9/onnx/ebike_best_split.onnx` (md5 `e1a432a6…`) |
| 新模型(已归档, 未启用) | `models/ebike_real_fork9_int8_rk2332.rknn` (md5 `dd943d03…`) |
| 对照臂孪生 | `opt_ebike_fork9/models/ebike_real_formC_int8.rknn` |
| 逐算子审计 JSON | `opt_ebike_fork9/logs/p1_audit_*.json` |
| 1724 帧框 CSV + 对拍 JSON | `opt_ebike_fork9/out/boxes_*.csv`, `out/cmp_*.json` |
| A/B 结果 + 原始日志 | `opt_ebike_fork9/out/ab/*.json`, `*.log` |
| 冒烟对比日志 | `opt_ebike_fork9/out/smoke/{old,new}.log` |
| 工具 | `opt_ebike_fork9/tools/{run_ab_p1.sh,ab_harness.py,summarize_ab_p1.py,quiesce_p1.sh,board_quiesce_p1.sh,smoke_plates_compare.sh,smoke_compare.py,probe_raw_outputs.py,analyze_convention.py,analyze_layout.py,decide_prod_layout.py,diagnose_prod_model.py,compare_dfl_paths.py,check_prod_quant.py}` |
| 源码改动 | `accuracy/acc_batch.cc`(O14 nc 推导修复, 3 行) |
| 现场备份 | `/home/seeed/rk-gate-backup-p1fork9-20261123_185030` |

---

## 六、下一步建议

**P1-2′: 先重标 `GATE_ROI_*`, 再切模型**(单变量: 只动 ROI 参数, 不换模型)

- 需要重标的量: `GATE_ROI_RATIO` 0.164 → **~0.8**(在 ~171 px 的新框宽下拿回 ~140~160 px 的车牌裁剪),
  并复核 `GATE_ROI_Y` / `GATE_ROI_HEIGHT` / `GATE_ROI_SHIFT` / `GATE_ROI_BELOW`(都挂在框宽/框高上)。
- ★ 与用户提的 P1-2(两行绿牌纵向偏高 6 px)是**同一组参数的同一张卡**, 建议合并做,
  否则连着改两次 ROI 参数会互相污染归因。
- ★ 换模型与重标 ROI 必须拆开: 混着做时"框变窄 → ROI 变窄 → 识别率掉"三者纠缠, 无法归因。

**另外建议单独立项**: 查清算法组那份生产模型导出与本仓库 `best_split.onnx` 的 dist2bbox 差异
(生产模型的框尺寸是错的: 宽高比 1.789、23.7% 零高度), 这直接影响现场检测统计与测速的可用性。
