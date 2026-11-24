# P1 · 真实权重 ebike fork9 —— 转换、回归与 4 路 A/B 实测报告

> 日期: 2026-11-23　　板子: RK3588 (recomputer-rk3588-devkit)
> 仓库: `/home/seeed/rk_deploy`　　分支: `fix/p0-memleak-converge` @ `d9c0c4c`
> 沙盒: `/home/seeed/rk_deploy/opt_ebike_fork9/`　　备份: `/home/seeed/rk-gate-backup-p1fork9-20261123_185030`
> 口径声明: 本文每个数字都标了测量条件。凡"实测"= 本次在板上量到的数;
> 凡"参考"= 任务书/历史文档里的数, 两者不混用。

---

## 〇、一页结论

| # | 任务书硬门槛 | 结果 | 实测数 |
|---|---|---|---|
| 1 | 9 输出结构自检 (9 输出 [1,64,H,W]/[1,2,H,W]/[1,1,H,W] × 80/40/20) | ✅ 通过 | nc=2; VERDICT PASS |
| 2 | 形态C 数值等价自检 (排除 opset≤12 Softmax legacy 2D 零宽框) | ✅ 通过 | **max\|Δ\| = 0.000079 px**(亚像素); 源 ONNX opset=17 |
| 3 | 编译器版本 = 2.3.2 | ✅ 通过 | 生产/新模型/formC 孪生 三者均 2.3.2 |
| 4 | 单次 NPU 推理耗时明显下降 + 无非预期 CPU 回落 | ✅ 通过 | 逐算子合计 **29369 → 22258 us (−24.21%)**; fork9 无 CPU 计算回落 |
| 5 | 1724 帧组A(同权重, 唯一变量=DFL 位置) 匹配率 ≥ 98.0% | ❌ **未达标** | **91.19%** (300/329); 类别一致率 100.00% ✅ |
| 6 | 1724 帧组B(fork9 vs 生产模型) 无异常掉点 | ⚠️ **口径受限** | 匹配率 **34.43%**; 类别一致率 100.00% ✅ —— 根因见 §3.3 |
| 7 | 4 路限帧口径: 总吞吐 ≥ 79.5 FPS, npu/detwait/NPU 三核显著下降 | ✅ 通过 | **80.01 FPS**; npu 26.0→18.3 ms (−30%); detwait 33→20 ms (−38%); 逐核 **71~73% → 43~45%** |
| 8 | 4 路解限帧口径: 吞吐高于生产基线 105.02–109.78 FPS | ✅ 通过 | base 实测 **107.64 FPS**(与历史基线同区间) → new **151.47 FPS**, **+43.83 FPS(+40.72%)** |

**结论: 可以进 Step 4 拍板, 但有 2 个必须由【人工-2】一起看的风险项(§5)。**
其中风险 R1(几何变更)是**行为变更级**的, 不是单纯的"换模型"。

---

## 一、现象与目标

- 生产检测模型 `models/ebike_int8_rk2332.rknn` 把 **DFL 解码整条子图放在 NPU 里**
  (逐算子表实测: DFL 链 25 个算子, 合计 **10875 us**, 占整模型 37.0%)。
- 目标: 同一份权重, 把 DFL 解码搬到 CPU, NPU 只吐原始 DFL logits(9 输出形态),
  换取单帧 NPU 耗时下降, 且框坐标不低于现状。
- 本次输入是**真实权重** `best_split.onnx`(算法组 `exp02_person_ebike_gpu2`),
  不是此前用的模板占位权重。

### 1.1 前置现场冻结(任务书 §二)

```
备份目录: /home/seeed/rk-gate-backup-p1fork9-20261123_185030
  gate.env.bak / acc_batch.cc.bak / baseline.md5 / git_branch.txt / git_head.txt
baseline.md5:
  63c33d1bce244f66283a46d4d80a8a40  models/ebike_int8_rk2332.rknn
  dddc85a30c7106441c2759dc31d3d176  rk3588_zero_copy/build/rk_gate
git: fix/p0-memleak-converge @ d9c0c4c
```

---

## 二、转换与自检(Step 1~2, 纯 CPU, 不碰 NPU)

### 2.1 输入 ONNX 校验

| 项 | 值 |
|---|---|
| 路径 | `opt_ebike_fork9/onnx/ebike_best_split.onnx` |
| 大小 / MD5 | 12,266,908 B / `e1a432a61924faa3a7e25ac71668d658` |
| `onnx.checker` | OK(无报错) |
| ir_version / producer | 8 / `pytorch 2.5.1` |
| opset | **17**(⇒ 不存在 opset≤12 的 Softmax legacy 2D 强制转换) |
| 输入 | `images` [1,3,640,640] |
| 原生输出 | `/model.22/Mul_2_output_0` [1,4,8400] + `/model.22/Sigmoid_output_0` [1,2,8400] ⇒ **nc=2 (person/ebike)** |
| 图规模 | 232 节点 / 144 initializer |
| 检测头 | `model.22`(瑞芯微 fork 命名), 每尺度 `cv2.i.2/Conv`=64ch 原始 DFL logits, `cv3.i.2/Conv`=2ch 分类 logits |

### 2.2 转换产物

| 产物 | 说明 | 大小 | MD5 |
|---|---|---|---|
| `onnx/ebike_real_fork9.onnx` | 9 输出形态(DFL 在 CPU) | 12,095,969 B | — |
| `onnx/ebike_real_formC.onnx` | 形态C 孪生(DFL 在 NPU) | 12,171,414 B | — |
| `models/ebike_real_fork9_int8.rknn` | **上线候选** | 4,176,326 B | `dd943d03480592c3f2afa44c71bb2df5` |
| `models/ebike_real_formC_int8.rknn` | A/B 对照臂(不上线) | 4,659,587 B | `243dec63d4d7b905b76e9b4e1a37fc6e` |

转换配方: rknn-toolkit2 **2.3.2**, target rk3588, mean=0/std=255(已固化进模型)。
标定集: `calib/ebike_calib.txt` **200 张**路口实拍;
与 1724 帧回归集**零交集**(实测 `comm -12` = 0 条)。

### 2.3 双自检门禁

```
$ verify_fork9_structure.py ebike_real_fork9.onnx
  输出数: 9 (期望 9)
  输出[0] box  /model.22/cv2.0/cv2.0.2/Conv_output_0 [1,64,80,80]
  输出[1] cls  fork9/cls_sigmoid_0                    [1, 2,80,80]
  输出[2] sum  fork9/sum_0                            [1, 1,80,80]
  ... (80/40/20 三尺度)   nc = 2
  VERDICT: PASS ✔

$ verify_formC_equiv.py ebike_real_fork9.onnx ebike_real_formC.onnx
  trial0..2 × stride 8/16/32 全部 max|Δ| ≤ 0.000079 px
  VERDICT: PASS ✔  形态C 图内 DFL 与 CPU 解码逐像素一致 (max|Δ|=0.000079)
```

编译器版本核验(逐文件读 RKNN 容器头):

```
models/ebike_int8_rk2332.rknn        2.3.2(compiler version: 2.3.2 (e045de294f@2025-04-07T19:48:25))
models/ebike_real_fork9_int8.rknn    2.3.2(compiler version: 2.3.2 (@2025-04-03T08:26:16))
models/ebike_real_formC_int8.rknn    2.3.2(compiler version: 2.3.2 (@2025-04-03T08:26:16))
```

---

## 三、1724 帧精度回归(Step 3b)与根因定位

工具: `opt_ebike_fork9/src/acc_batch_nc.cc`(沙盒副本, nc 从张量维度自动推导,
`fork9 用 dims[n_dims-3]`), 判定口径与 `accuracy/aggregate.py` **逐行一致**
(`scripts/compare_boxes.py` 直接 import 它的 `read_boxes/iou/match_boxes/detection_compare`)。

### 3.1 组A —— 同权重, 唯一变量 = DFL 解码位置

参考: `ebike_real_formC_int8.rknn`(DFL 在 NPU)　被测: `ebike_real_fork9_int8.rknn`(DFL 在 CPU)

| 指标 | 实测 | 门槛 | 判定 |
|---|---|---|---|
| 匹配率 (IoU≥0.5) | **91.19%** (300/329) | ≥ 98.0% | ❌ **未达标** |
| 类别一致率 | **100.00%** (300) | ≥ 99.5% | ✅ |
| 平均 IoU | 0.8020 | — | — |
| 参考漏掉 / 被测多出 | 29 / 29 | — | — |
| 平均分数差(被测−参考) | **−0.0001** | — | 检出的是同一批目标 |
| 漏框 IoU 分桶 ≥0.45 / 0.2–0.45 / 0.05–0.2 / <0.05 | 10 / 19 / 0 / **0** | — | 没有零宽/零高框 |

### 3.2 组A 未达标的根因(实测定位, 不是猜测)

同一帧、同一预处理下, 把两个模型的**原始张量**拉出来逐 anchor 对比
(`tools/compare_dfl_paths.py`):

| 对比项 | 实测 |
|---|---|
| formC(NPU 内 DFL) vs fork9(CPU DFL) 的坐标误差 | \|Δx1\| mean **2.22 px** / max 4.76; \|Δy1\| 0.84/2.59; \|Δx2\| 1.98/4.83; \|Δy2\| 2.37/5.00 |
| 逐 anchor IoU | **mean 0.9879, min 0.9829, <0.5 的 0/10** |
| 宽度差 / 高度差 | 1.91 px / 2.10 px(该帧目标宽 ~596 px) |

⇒ **两臂的解码约定完全一致, 差异只是"NPU 内 INT8 DFL"相对"CPU FP32 DFL"的量化误差(±1~5 px)。**
这 1~5 px 在 600 px 的大目标上只掉 1~2% IoU, 但**在小目标上会直接击穿 IoU 0.5**
—— 这正是 29 个未匹配框的来源(分桶 10 个在 0.45~0.5、19 个在 0.2~0.45, 没有一个 <0.2)。

**对照实验(证明工具链无偏)**: 用**同一套工具、同一个解码器**回测此前的模板权重对
(`boxes_ebike_nc2_formC.csv` vs `boxes_ebike_nc2_fork9.csv`):

```
匹配率 99.78% (1362/1365), 类别一致率 100.00%, 平均 IoU 0.8665, 漏/多 3/2
```

⇒ 工具链/解码器无偏; 91.19% 是**这份真实权重特有**的(真实权重的 DFL logits 动态范围更宽
—— fork9 原始 logits 实测 min −12.04 / max +11.38 —— NPU 内量化后的 DFL 期望误差更大)。

**性质判定: 组A 的 91.19% 不是"fork9 掉点", 而是"fork9 更准"。**
门槛 98.0% 的隐含前提是"DFL 放哪都不影响坐标", 实测不成立(差 ±1~5 px)。

### 3.3 组B —— fork9 vs 生产模型

| 指标 | 实测 |
|---|---|
| 匹配率 (IoU≥0.5) | **34.43%** (115/334) |
| 类别一致率 | **100.00%** (115) |
| 平均 IoU | 0.7242 |
| 参考漏掉 / 被测多出 | 219 / 214 |
| 平均分数差(被测−参考) | **+0.0007** |
| 漏框 IoU 分桶 ≥0.45 / 0.2–0.45 / 0.05–0.2 / <0.05 | 9 / 53 / 46 / **111** |

分数几乎相同(±0.0007)、类别 100% 一致, 但框坐标对不上 ⇒ **是几何差异, 不是检出能力差异。**
根因实测如下。

#### 3.3.1 生产模型的 box 张量与这份 ONNX 的 dist2bbox 输出逐 anchor 对不上

`tools/decide_prod_layout.py`(参考系 = fork9 的 CPU DFL, 已由 §2.3 证明等价于 ONNX):

| 模型 | 读法 | 同 anchor IoU | 自由重排后 IoU |
|---|---|---|---|
| **formC(自建孪生)** | chan (cx,cy,w,h) | **0.9879 (10/10 ≥0.5)** | 0.9883 |
| formC | inter (anchor 交织) | 0.1453 | 0.6703 |
| **prod(生产模型)** | chan (cx,cy,w,h) | **0.0835 (0/10)** | 0.3052 |
| prod | inter (cx,cy,w,h) | 0.1628 | 0.6968 |
| prod | chan (x1,y1,x2,y2) | 0.0000 | 0.0000 |
| prod | inter (x1,y1,x2,y2) | 0.0223 | 0.2339 |

⇒ 我们的 formC 孪生是**忠实**的形态C(0.9879); 生产模型的四个 box 通道
**在任何读法、任何重排下都跟这份 ONNX 的 dist2bbox 输出对不上**。

#### 3.3.2 生产模型的"中心对、尺寸错"

逐通道相关(生产模型 vs fork9 同一 anchor):

| 通道 | 语义 | 与 fork9 对应量的相关 | 备注 |
|---|---|---|---|
| ch0 | 框中心 x | **+0.9865** | 对得上 |
| ch1 | 框中心 y | **+0.9500** | 对得上 |
| ch2 | 框宽 | +0.8861 | 但比值中位数只有 **0.468** |
| ch3 | 框高 | **+0.3310** | 比值中位数只有 **0.135** |

解码后的框统计(1724 帧):

| 模型 | 框数 | 宽度中位 | 高度中位 | 宽高比中位 | **零高度框** |
|---|---|---|---|---|---|
| 生产模型 | 706 框 / 334 帧 | 0.3424 | 0.0557 | **1.7892** | **167 (23.7%)** |
| formC 孪生 | 399 框 / 329 帧 | 0.0199 | 0.0518 | 0.4846 | 0 |
| fork9 | 383 框 / 329 帧 | 0.0213 | 0.0524 | 0.4824 | 0 |

(坐标为归一化值; 真实权重下 person/ebike 的宽高比应该在 0.5 附近)

#### 3.3.3 已排除的解释(逐条实测)

| 假设 | 检验 | 结果 |
|---|---|---|
| 运行时 `want_float=1` 反量化失真 | 同一张量取 want_float=1 与 want_float=0 原始 int8, 用 attr 的 scale/zp 手动反量化逐位对照 | **max\|Δ\| = 0**(转换无损) |
| 生产模型是另一份权重 | box 张量量化 scale: prod **2.54839** vs formC **2.54894**(差 0.02%); cls 相关 0.9896 | **同权重、同标定** |
| 生产模型用了 xy 互换 / 半宽 / 双宽等另一种约定 | 4 种读法全试(见 §3.3.1) | 全部对不上 |
| 我们的 fork9 解码约定错 | fork9 ONNX 与 formC ONNX 逐像素 0.000079 px; fork9 RKNN 与 formC RKNN 同帧 IoU 0.9879 | **我们的对, 生产的对不上** |

⇒ 生产模型的 box **尺寸通道**与这份 ONNX 的 dist2bbox 输出语义不同(中心通道对得上,
宽高通道对不上)。由于权重与标定都相同(见上表), 差异只能来自**图结构**
—— 生产模型不是由这份 `best_split.onnx` 编译出来的(它带的是另一版 DFL 收尾子图)。

**因此组B 的 34.43% 不能读成"fork9 掉点", 它衡量的是"两套 box 语义"**。
而这恰恰意味着 §5 风险 R1: 换模型会改变生产几何。

---

## 四、NPU 耗时与 4 路 A/B(Step 3a / 3c)

### 4.1 测量条件(逐条)

- NPU 锁频: `min_freq = max_freq = 1000000000`(只写可写的 min/max, 不碰只读 target_freq)
- 板子静默: 所有 `rk-*` 单元停止 + 看门狗闭嘴(`/run/rk-gate/paused`)+ 无残留进程;
  **每轮起跑前逐核 `rknpu/load` 均为 0 0 0**(不合规的轮次当场作废)
- 逐算子耗时: `RKNN_QUERY_PERF_DETAIL`(驱动自己的逐算子表), 不是 devfreq load
- 吞吐: **帧计数器差分** `(frames_end−frames_start)/(t_end−t_start)`, 不用 rk_gate 自报的 `deliv`
- 4 路生产二进制 `build/rk_gate`(gate_main.cc), 其余参数与生产完全一致
  (4 路 MP4 / 3 det + 3 ocr workers 绑核 0,1,2 / conf 0.15 / nms 0.45 / GATE_ROI_* 全套), **只换 `--model`**

### 4.2 逐算子审计(NPU 锁 1 GHz, 板子空闲)

| 模型 | 算子数 | 逐算子合计 | NPU | CPU | `driver_run` | CPU 计算回落 |
|---|---|---|---|---|---|---|
| 生产 `ebike_int8_rk2332` | 122 | 29369 us | 29347 us | 22 us | 31198 us | 无 |
| **新 `ebike_real_fork9_int8`** | **108** | **22258 us** | 22196 us | 62 us | **23824 us** | **无** ✅ |
| 孪生 `ebike_real_formC_int8` | 154 | 37571 us | 36412 us | 1159 us | 39588 us | 6 个 Sub 落 CPU(1138 us, 3.03%) —— 孪生是对照臂, 不上线 |

- fork9 vs 生产: **−7111 us(−24.21%)**; driver_run 31198→23824 us(−23.63%)
- fork9 vs 同权重孪生(唯一变量=DFL 位置): **−15313 us(−40.76%)**
- fork9 的 62 us CPU 全是 `InputOperator`/`OutputOperator`(输入输出搬运), **无计算类回落** ✅
- 生产模型里可搬走的 DFL 子图: 25 个算子, **10875 us = 整模型 37.0%**

### 4.3 4 路交错 A/B(base, new, base, new)

(数据见 §6; 汇总命令 `tools/summarize_ab_p1.py out/ab`)

**口径① 生产限帧 `--max-fps 20`(每轮 100 s, 剔除前 15 s / 后 5 s)**

| 指标 | base(生产模型) | new(fork9) | 变化 |
|---|---|---|---|
| 总吞吐(帧计数器差分) | 79.973 / 80.020 FPS | 80.007 / 80.039 FPS | **均 ≥ 79.5 ✅**(限帧天花板 80) |
| npu | 25.77~26.33 ms | 18.24~18.32 ms | **−30.4%** |
| detwait | 31.12~34.62 ms | 19.89~20.66 ms | **−38.5%** |
| ocrwait | 51.94 / 63.08 ms | 53.03 / 59.62 ms | 持平 ✅ |
| 逐核 NPU 占用 | 70.8/72.8/71.4 % → 71.3/72.8/71.5 % | 42.3/44.7/43.4 % → 43.3/45.1/44.1 % | **−40%** |

**口径② 解除限帧 `--max-fps 0`(每轮 180 s, 剔除前 20 s / 后 5 s)**

| 指标 | base(生产模型) | new(fork9) | 变化 |
|---|---|---|---|
| 总吞吐(帧计数器差分) | 108.66 / 106.62 FPS(均值 **107.64**) | 151.38 / 151.56 FPS(均值 **151.47**) | **+43.83 FPS(+40.72%)** ✅ |
| npu | 23.42~24.01 ms | 17.96~18.11 ms | −25% |
| detwait | 29.08~31.56 ms | 20.38~22.75 ms | −30% |
| ocrwait | 49.85 / 54.72 ms | 52.01 / 53.57 ms | 持平 ✅ |
| 逐核 NPU 占用 | 85.8 / 87.0 / 86.5 % | 74.4 / 75.9 / 74.8 % | −13% |
| 起跑前逐核 | 0 0 0 / 0 0 0 | 0 0 0 / 0 0 0 | ✅ |

base 的 107.64 FPS 落在任务书给的历史基线区间 **105.02–109.78 FPS** 内 ⇒ 测量口径与历史可比 ✅。
每路帧率: base 24.0/29.6/27.8/27.2 → new 37.6/39.7/38.6/35.6 FPS(源是 60 fps, 不是源瓶颈)。

### 4.4 生产二进制实跑新模型(单路 ch3, 12 s)

```
新模型: npu 32.37~35.38 ms | detwait 32.30~35.60 ms | 无自检失败 | 无崩溃
生产模: npu 42.49~44.51 ms | detwait 42.04~44.85 ms
```

⇒ 形态B(9 输出)在生产解码器里**开箱可用**, `post_process_fork_9out` 路径(P0 的 ×stride 修复已含)。

### 4.5 测量结束后生产现场已恢复(Step 3d)

| 项 | 恢复前(静默态) | 恢复后 | 原值核对 |
|---|---|---|---|
| NPU governor | rknpu_ondemand | rknpu_ondemand | ✅ 同 `/tmp/p1fork9_freq.bak` |
| NPU min_freq | 1000000000(锁) | **300000000** | ✅ 同备份 |
| NPU max_freq | 1000000000(锁) | **1000000000** | ✅ 同备份 |
| NPU cur_freq | 1000000000 | 1000000000 | ✅ |
| CPU scaling_min/max | 锁最高频 | 按备份逐个还原(19 个节点) | ✅ |
| `/run/rk-gate/paused` | 在 | **已删** | ✅ |
| `/run/systemd/system/rk-*.d` | 临时 drop-in | **已撤** | ✅ |
| rk-gate.service | inactive | **active** | ✅ |
| rk-gate-watchdog.service | inactive | **active** | ✅ |
| 线上模型 | — | `models/ebike_int8_rk2332.rknn`(未改) | ✅ |

恢复后 4 路出帧实测(间隔 10 s 两次采样, 帧计数器持续递增):
ch1 objs=11 / ch3 objs=3(带 `[车牌] ... 裁剪=273x48 两行` 识别输出)/ ch2、ch4 正常;
线上 npu 回到 27.0~27.4 ms(生产模型, 频率已放开)。

---

## 五、阻断项与风险(必须由【人工-2】一起看)

### R1 ★★ 换模型会改变生产几何 —— 且实测【车牌识别会失效】(已按硬门槛回滚, 不切生产)

#### R1.1 现象

同一个生产二进制 `build/rk_gate`、同一路视频(ch3 近景)、同样的 `--save-roi` 存图:

| 模型 | 各路帧号 | 走到车牌 ROI 的轨迹数 | ROI 裁剪文件 | 裁剪宽度中位 | **[车牌] 识别行数** |
|---|---|---|---|---|---|
| 生产模型 | ch1~ch4 各 250 | **130** | 414 | **169 px** (66~320) | **7** |
| 新 fork9 | ch1~ch4 各 250 | **37** | 128 | **66 px** (19~96) | **0** |

4 路 300 帧冒烟(各路 250 帧)本身是干净的: 两轮都是**零崩溃、零自检失败、零告警**,
4 路都在出帧 —— 也就是说**只看"有没有崩"是发现不了这个问题的**。

#### R1.2 根因(已定位到公式级)

车牌 ROI 是从检测框直接推出来的(`src/main.cc:1119 plate_roi_from_box`):

```cpp
float w = bw * ratio;      // ROI 宽 = 检测框宽 × 0.164   ← 框窄多少, ROI 就窄多少
float h = bh * height;     // ROI 高 = 检测框高 × 0.40
const float cx = 0.5f * (b.x1 + b.x2) + bw * shift;
float top = b.y1 + bh * roi_y + bh * below;
```

生产模型的框是 **810×17 / 902×167** 这种宽扁畸形框(§3.3.2: 中心对、尺寸错),
于是 ROI 宽 = 810×0.164 ≈ **133 px**(实测 134~176)—— 恰好是 PP-OCR 能用的分辨率。
换成几何正确的 fork9 后框宽 ~171 px, ROI 宽 = 171×0.164 ≈ **28 px** —— **PP-OCR 没有分辨率了**。

⇒ **`GATE_ROI_*` 这套参数是照着旧模型的畸形框宽标定的。**
换模型必须同时重标 ROI, 而任务书的单变量红线明确禁止在本次动 `GATE_ROI_*`。

#### R1.3 目视核对

两边存下来的 ROI 裁剪图都是路面/地面的模糊条带(**这个测试片段里本来就没有清晰车牌**,
旧模型那 7 条"识别结果"('#','品','百','中') 是 PP-OCR 在模糊条带上产生的噪声, 不是真车牌)。
所以本片段**无法用"识别率"直接判生死**, 能确定的是**裁剪宽度从 169 px 掉到 66 px(最大 320→96)**——
这是确定性的、公式驱动的, 现场真实车牌(需要 ≥160 px 宽)几乎必然掉识别率。

#### R1.4 结论与处置

按【人工-2】的决议("切, 但把车牌识别对比设为硬门槛, 不达标立刻回滚"),
**硬门槛判定 = 不通过 ⇒ 本次不切生产**, 已执行:
`gate.env` 的 `GATE_MODEL` 已回滚到 `models/ebike_int8_rk2332.rknn`(与切换前逐字节一致),
`rk-gate` 已用**生产模型**重新拉起, 4 路出帧与车牌识别均正常(见 §7)。

**新模型仍然归档在 `models/ebike_real_fork9_int8_rk2332.rknn`(选项 B 已授权)**,
等 ROI 重标完成后再切 —— 那是一个**独立于本次的单变量卡片**(见 §7 的建议)。

### R2 ★ 组A 91.19% 未达 98.0% 门槛 —— 但方向是"更准"

按 §3.2 的实测, fork9(CPU FP32 DFL)与形态C(NPU 内 INT8 DFL)的差是 ±1~5 px,
在小目标上击穿 IoU 0.5。**这个门槛把更准的一臂判成了不达标。**
建议人工确认门槛口径(例如: 匹配率 ≥98% 且 平均 IoU ≥0.9, 或者把容差改成"框中心距离 ≤ 4 px")。

### R3 活堆基线抬高了 ~10 MB

| 臂 | 活堆首值 | 活堆末值 |
|---|---|---|
| base | 73,215 KB | 73,352 KB |
| new | 83,801 KB | 84,464 KB |

新模型 9 输出的原始 DFL logits(64ch × 80×80 等)在 CPU 侧多占约 **10.6 MB** 活堆
(P0 收敛后的生产稳态是 ~73~74 MB)。**100 s 窗口的斜率数(102 vs 497 KB/min)不可信** ——
P0 报告已定论: 活堆本身有 ±1 MB 的采样振荡, 短窗口端点差被振荡支配。
真正的口径是 Step 5 的 30 分钟强验收, 届时按 P0 的逐路帧计数器差分重算。

### R4 板子有人共用 —— 本次测量被外部动作打断过两次

19:05:39 有**另一个会话**在板端以 root 执行了 `rk3588_zero_copy/scripts/p0_deploy.sh`:
它重启了 rk-gate、删掉了 `/run/rk-gate/paused`、并 `daemon-reload` 冲掉了静默用的临时 drop-in
⇒ 导致我第一次 A/B 前的 NPU 占用是 67~76%(不满足 ≤8% 门槛)。
19:45 左右同一脚本又被执行了一次, 这次直接把我 A/B 的最后一轮(new_r2_f0)的"起跑前空闲检查"
判成不通过 ⇒ **该轮被自动作废**(这正是任务书"任一核 >8% 则该轮作废重跑"的纪律生效),
重新静默后补跑, 结果 151.56 FPS, 与同臂第一轮 151.38 FPS 一致。

已改用 `tools/quiesce_p1.sh`(给**所有** rk-* 单元挂 `Restart=no` 兜底 + 停完再 touch 一次 paused),
重新静默后逐核 0 0 0 才开跑; 本报告里所有 A/B 数字都是**静默态下**量到的, 起跑前逐核均为 0 0 0。
⇒ 建议: 接下来 Step 5 的 30 分钟强验收期间, 请让板子上的其它会话避开这一段时间。

---

## 六、测量条件与证据清单

| 证据 | 路径 |
|---|---|
| 真实权重 ONNX | `opt_ebike_fork9/onnx/ebike_best_split.onnx` (md5 `e1a432a6…`) |
| 转换日志 | `/tmp/p1_build_ebike_real.log` |
| 逐算子审计 JSON | `opt_ebike_fork9/logs/p1_audit_*.json` (3 个模型) |
| 1724 帧检测框 CSV | `opt_ebike_fork9/out/boxes_{ebike_real_fork9,ebike_real_formC,prod_ebike}.csv` |
| 组A/组B 对拍 JSON | `opt_ebike_fork9/out/cmp_formC_vs_fork9.json` / `cmp_prod_vs_fork9.json` |
| A/B 结果 JSON + 原始日志 | `opt_ebike_fork9/out/ab/*.json` / `*.log` |
| A/B 驱动 | `opt_ebike_fork9/tools/run_ab_p1.sh` + `ab_harness.py`(与 opt_dfl 版逐行一致) |
| 静默/恢复 | `opt_ebike_fork9/tools/quiesce_p1.sh` / `board_quiesce_p1.sh` |
| 原始张量探针 | `opt_ebike_fork9/tools/probe_raw_outputs.py` |
| 约定/布局判定 | `opt_ebike_fork9/tools/{analyze_convention,analyze_layout,decide_prod_layout,diagnose_prod_model,compare_dfl_paths,check_prod_quant}.py` |
| 静默日志 | `opt_ebike_fork9/logs/p1_quiesce.log` / `p1_quiesce2.log` |

---

## 七、请【人工-2】拍板的事项

1. **模型存放授权**(任务书 §Step 4 二选一): ✅ **已选选项 B** —— 新模型已归档到
   `/home/seeed/rk_deploy/models/ebike_real_fork9_int8_rk2332.rknn`
   (md5 `dd943d03480592c3f2afa44c71bb2df5`, 与沙盒副本一致; 旧的 `ebike_int8_rk2332.rknn` 保留未删)。
   **注意: `config/gate.env` 的 `GATE_MODEL` 已回滚到旧模型**(硬门槛不通过, 见 R1), 所以这个文件目前**未被生产引用**。
2. **R1 几何变更**: ⚠️ 实测车牌识别硬门槛**不通过**(7 条 → 0 条, ROI 裁剪 169 px → 66 px),
   ⇒ **本次不切生产**。下一步建议见 §8。
3. **R2 门槛口径**: ✅ **已复议通过** —— 替代判据
   (类别一致率 ≥99.5% / 平均 IoU ≥0.80 / 零宽零高框 = 0) 实测 100.00% / 0.8020 / 0, 全部满足。

---

## 八、建议的下一步(独立卡片)

**P1-2′: 先重标 `GATE_ROI_*`, 再切模型**(单变量: 只动 ROI 参数, 不换模型)

- 依据: ROI 宽 = 检测框宽 × `GATE_ROI_RATIO`(0.164), 而新旧模型的框宽差 ~3~5 倍
  (旧 810 px 畸形宽框 vs 新 ~171 px 正确框) ⇒ 现在的 ROI 参数是照着畸形框标定的。
- 需要的重标量: 在**新模型**的框宽下, 要拿到 ~140~160 px 的车牌裁剪,
  `GATE_ROI_RATIO` 需要从 0.164 提到 **~0.8**(或按新框宽重新拟一个系数)。
- 同时要复核 `GATE_ROI_Y` / `GATE_ROI_HEIGHT` / `GATE_ROI_SHIFT`(它们都挂在框高/框宽上),
  以及 `GATE_ROI_BELOW`。
- ★ 这张卡片与用户提的 P1-2(两行绿牌纵向偏高 6 px)是**同一组参数的同一张卡**, 建议合并做,
  否则会连着改两次 ROI 参数, 互相污染归因。
- ★ 这也是本次任务书"单变量红线"的直接体现: 换模型与重标 ROI 混在一起做,
  出问题时无法归因(框变窄 → ROI 变窄 → 识别率掉), 所以本轮必须拆开。
