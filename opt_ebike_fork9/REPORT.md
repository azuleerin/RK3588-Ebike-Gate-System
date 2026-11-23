# 阶段① 报告 · ebike fork9(DFL 解码移出 NPU)—— 候选模型 + 板端 A/B

> 2026-11-22 20:00 ~ 21:10　　范围: 独立沙盒 `opt_ebike_fork9/`
> **未 merge、未改任何生产/共享代码、未动板端配置**
>
> 本机 = RK3588 板子本身(seeed studio recomputer rk3588 devkit),
> NPU 跑在 1GHz(本板最高频), runtime = librknnrt 2.3.2。

---

## 一、结论先行

| 问题 | 答案 |
|---|---|
| **ebike 的 ONNX 源在本机拿得到吗** | **拿不到**。任务书写的 `02_Models_and_Data/.../best_split.onnx` 在**算法组的训练仓**(`E:\shenzhenproject`), 不在本仓库/本板/GitHub。**七路排查全部阴性**(见 §二) |
| **那还交付了什么** | **整条流水线全部打通并板端验证**: 9 输出 fork9 导出 → INT8 RKNN(2.3.2) → 1724 帧精度回归 → NPU 耗时 A/B → CPU 侧代价实测。**接上真 ONNX 后一条命令出候选** |
| **NPU 能省多少(ebike 架构)** | **−13.0 ms/帧(−36.5%)**: 单次推理 35617 us → **22605 us**; NPU 算子耗时 −13767 us(−42.4%) |
| **精度会掉吗(nc=2 口径)** | **不掉**。同权重 A/B(唯一变量 = DFL 在 NPU 还是 CPU): **匹配率 99.78%**、**类别一致率 100.00%**、**平均分数差 +0.0000** |
| **CPU 侧代价** | 正常阈值下 **≈ +0.05 ms/帧**; 最坏情况(8400 个 anchor 全解码) **+1.5 ms/帧**。**不会吃掉 NPU 省下的时间** |
| **途中修掉的两个坑** | ① 回归工具 `acc_batch.cc` **把 nc 写死成 80** → nc=2 的 fork9 模型会越界读 → 垃圾框; ② **opset ≤ 12 的 Softmax 有 legacy 2D 强制转换** → 形态C 的 DFL 全部退化成 0 → **框全是零宽, 且全程不报错** |
| **能不能上生产** | **技术上是, 但还差一个文件**。等算法组给 `best_split.onnx`, 跑 `scripts/build_ebike_fork9.sh` 即可出候选 |

---

## 二、前提核查: ebike 的 ONNX 源确实拿不到(七路排查)

任务书写的:
> ✅ ONNX 源在仓库:`02_Models_and_Data/training_data/detect_exp/exp02_person_ebike_gpu2/weights/` 下有 best.pt + best.onnx + best_split.onnx

| # | 途径 | 结果 |
|---|---|---|
| 1 | 全盘 `find` 所有 `.onnx` / `.pt` | 只有 yolov8n / yolo26 / ppocrv4; **没有 ebike 的** |
| 2 | `find -type d -name "02_Models_and_Data" -o -name "01_Deliverable" -o -name "detect_exp"` | **0 命中** |
| 3 | 本仓库 git 全历史 | **0 命中** |
| 4 | GitHub 上游仓(唯一那个)的完整树(515 项) | 只有 `models/ebike_int8*.rknn`, **没有 ONNX** |
| 5 | 该账号下全部仓 / release | 只有 1 个仓; release 只有 `RK3588.AI.pdf` |
| 6 | GitHub 全站搜仓名(`02_Models_and_Data` / `exp02_person_ebike` / `Ebike+detect_exp` / `01_Deliverable+rknn`) | 后三个 **0 命中** |
| 7 | 内网可达性(扫 `192.168.8.0/24`, SSH) | 板子已换网段(`192.168.31.77` → `192.168.8.77`), **原 Windows 工作机不可达** |

补充: 全盘归档(`.zip/.tar*/.7z`)里也没有; **`.rknn` 容器里也没有 ONNX**
(RKNN v2 是 FlatBuffers 容器, 存的是编译后的算子表 + int8 权重,
扫 ModelProto 头 / zlib 头都 0 命中)。细节见 `docs/00_source_not_available.md`。

**上一阶段(`docs/task2_fork9_ab.md` §八.2)已经记过同一件事**, 本次把它查到底了。

---

## 三、★ 关键: 用生产模型自己把结构钉死, 造出同构的 fork9 候选

没有 ONNX, 就没法直接导出。但**生产模型的结构可以从 `.rknn` 里读出来**,
而且读出来的每一项都有硬证据:

1. **权重清单完全对得上** —— 生产 `.rknn` 内嵌的权重名 = 仓库里 fork9 模板的
   126 个 initializer 名字 **+ 2 个 DFL 专属的**(`model.22.dfl.conv.*`),
   **一个不多一个不少** ⇒ 同一张网络图。
2. **逐算子表给出检测头的真实形状** —— 用 RKNN 运行时
   `RKNN_QUERY_PERF_DETAIL` 直接读出每个卷积的权重形状:

```
model.22/cv2.0/cv2.0.2/Conv   (1,64,80,80),(64,64,1,1),(64) -> (1,64,80,80)   ← box 分支出 64ch 原始 logits
model.22/cv3.0/cv3.0.2/Conv   (1,64,80,80),(2,64,1,1),(2)   -> (1,2,80,80)    ← cls 分支出 2ch
```

⇒ **生产 ebike 头 = c2=64(box) / c3=64(cls) / nc=2**,
与仓库模板(COCO: c3=80 / nc=80)**只差 cls 分支**。

于是 `scripts/build_ebike_arch_fork9.py` 只重写 cv3 的 18 个张量,
得到一个**结构与生产 ebike 完全一致**的 9 输出 fork9 ONNX
(权重取自模板 —— 形状对、数值不是 ebike 的, 见 §六的边界说明)。

> 证据全文: `docs/01_ebike_topology_evidence.md`

---

## 四、板端结果

### 4.1 NPU 耗时 A/B(★ 这是本阶段最硬的一个数)

`audit/model_audit.py` 的逐算子表(NPU 锁 1GHz, 同一块板):

| 模型 | 算子数 | NPU 算子合计 | **单次推理** |
|---|---|---|---|
| 生产 `ebike_int8_rk2332`(形态C, DFL 在 NPU, 真权重) | 122 | 32483 us | **35617 us** |
| **`ebike_nc2_fork9_int8`**(fork9, DFL 在 CPU) | 108 | **18716 us** | **22605 us** |
| `ebike_nc2_formC_int8`(孪生形态C, 只用于精度对照) | 154 | 35020 us | 41307 us |

> **Δ(生产 → fork9): NPU 算子 −13767 us(−42.4%), 单次推理 −13012 us(−36.5%)**

**为什么这个 Δ 比上一阶段的 −7.27 ms 还大?**
上一阶段比的是「yolov8n 的 fork9」vs「yolov8n 手术版形态C」。
本阶段比的是**生产 ebike 模型本身** vs 同构 fork9, 而生产形态C 的图内尾巴更重:

```
Concat(66ch) → Reshape → Concat_3 (1,66,105,80) → Reshape (1,66,1,8400)   ← 1684 us
Split → DFL: Reshape/Transpose/exSoftmax13/Conv                              ← ~4.3 ms
dist2bbox: Transpose/Slice/Sub/Add×2/Sub/Mul/Concat_4/Mul_2/Reshape         ← ~1.6 ms
```

⇒ **生产模型每帧有 13 ms 花在"把 DFL 解码 + 拼锚点"上**, 占它总 NPU 时间的 **36.5%**。
这部分可以**整体搬到 CPU**, 一行图都不用改骨干。

**这个 Δ 是不是"公平"的?** —— 是。三点保证:
* 两臂**结构逐项相同**, 唯一差别就是"图内 DFL + dist2bbox" vs "9 个原始输出";
* INT8 卷积耗时由**形状**决定、与权重取值无关(数据无关 GEMM),
  所以"占位权重"不影响 NPU 耗时这个结论;
* 量化与否也不影响(两臂都是 w8a8)。

### 4.2 精度回归(同权重 A/B, 唯一变量 = DFL 在哪)

1724 帧门禁相机域, `conf=0.25 / nms=0.45`, 用**修好的**解码器:

| 指标 | 形态C(DFL 在 NPU) vs fork9(DFL 在 CPU) |
|---|---|
| **匹配率 (IoU ≥ 0.5)** | **99.78%** (1362 / 1365) |
| **类别一致率** | **100.00%** (1362) |
| 平均 IoU | 0.8665 |
| 参考漏掉 / 被测多出 | 3 / 2 |
| **平均分数差(被测 − 参考)** | **+0.0000** |

⇒ **把 DFL 从 NPU 搬到 CPU, 在 nc=2 的 ebike 架构上, 精度不掉、类别零差错、分数零漂移。**

(平均 IoU 0.8665 反映的是两臂**量化路径不同**这件事:
形态C 臂把 DFL 的中间张量也量化了, fork9 臂的 64 个原始 logits 直接出图。
上一阶段 nc=80 的同类对比是 92.17% vs 89.68% / IoU 0.9709 vs 0.8375,
方向一致。)

### 4.3 fork9 的 9 个输出各自量化(cls 不会被清零)

```
OUT 0  318                 [1, 64, 80, 80]  INT8  scale=0.112681195   zp=-65   ← box (原始 DFL logits)
OUT 1  onnx::ReduceSum_326 [1,  2, 80, 80]  INT8  scale=0.00232297857 zp=-128  ← cls (概率)
OUT 2  331                 [1,  1, 80, 80]  INT8  scale=0.00232306076 zp=-128  ← sum
OUT 3  338                 [1, 64, 40, 40]  INT8  scale=0.094649002   zp=-57
OUT 4  onnx::ReduceSum_346 [1,  2, 40, 40]  INT8  scale=0.00196078443 zp=-128
OUT 5  350                 [1,  1, 40, 40]  INT8  scale=0.000871299533 zp=-128
OUT 6  357                 [1, 64, 20, 20]  INT8  scale=0.0778426751  zp=-53
OUT 7  onnx::ReduceSum_365 [1,  2, 20, 20]  INT8  scale=0.00301738968 zp=-128
OUT 8  369                 [1,  1, 20, 20]  INT8  scale=0.00301739946 zp=-128
```

⇒ box / cls / sum **各自一张量化表**。cls 的 scale≈2e-3(≈1/500),
和 box 的 scale≈0.1 差 50 倍 ⇒ **cls 通道不可能被"清零"**。
这正是任务书说的"9 输出的天然优势", 板上实测确认。

### 4.4 CPU 侧代价(★ 上一阶段留的开放问题)

DFL 不是消失, 是**搬到 CPU**。用**生产解码器**
(`rk3588_zero_copy/src/yolo_post.cc` 的 `yolov8_postprocess`, 含已修好的 ×stride)
实测(`src/fork9_cpu_bench.cc`):

| 场景 | 生产形态C | fork9 | Δ |
|---|---|---|---|
| 正常阈值 conf=0.25 | 0.549 ms/帧(5 个框) | 0.056 ms/帧 | **−0.49 ms** |
| 最坏情况 conf=0(8400 个 anchor 全解码 + NMS 爆炸) | 43.227 ms | 44.720 ms | **+1.49 ms** |

**结论: CPU 侧代价上限 ≈ +1.5 ms/帧, 正常情况是 0 附近(甚至更省, 因为少了一堆
Concat/Reshape)。** 相对 NPU 省下的 13 ms, **完全不是一个量级的对手**。

> 为什么正常情况这么便宜: fork9 的解码里有 `sum` 预过滤
> (`sum <= conf_thr → 直接跳过`), 只有少数 anchor 会真的进 DFL。
> 上一阶段报告 §八.4 担心的"DFL 搬上 CPU 要盯 CPU"—— **盯过了, 不成问题**。

---

## 五、途中修掉的两个坑(都是**静默失败**, 不测就发现不了)

### 坑① 回归工具把 nc 写死成 80

`accuracy/acc_batch.cc:321` 的 9 输出分支写死 `nc=80`。
对 nc=2 的 ebike fork9 模型, 它会按 `c=0..79` 去读只有 2 个通道的 cls 张量 ⇒ **越界读**。
上一阶段没踩到, 因为那版的 fork9 是 COCO 的 nc=80。

**修法**: 沙盒副本 `opt_ebike_fork9/src/acc_batch_nc.cc`, 从 cls 张量维度自动推导 nc
(与生产 `yolo_post.cc:591-593` 同一个口径)。
**验证: 在 nc=80 的 fork9 模型上, 新旧解码器输出 md5 完全一致**
(`3272622a71fb57aca214ab195403cf8f`), 形态C 路径也与仓库 11-13 那份逐字节一致。
⇒ 这个副本是**行为等价的**, 唯一变化就是 nc 这个数。细节见 `docs/02_decoder_nc_fix.md`。

### 坑② opset ≤ 12 的 Softmax「legacy 2D 强制转换」(★ 新发现)

本阶段的模板 ONNX 是 **opset 12**。ONNX 规定 opset 1~12 的 Softmax 要先把输入
压成 2D `[d0..d(axis-1), d(axis)..d(n-1)]` 再做 softmax。
对 `[1,4,16,H,W]` + `axis=2` 来说, 实际是在 **16×80×80 = 102400** 个元素上做 softmax
⇒ 每个元素趋近 0 ⇒ DFL 期望值 ≈ 0 ⇒ **每一个框都是零宽**。

**而 `onnx.checker` 通过、RKNN 编译器正常编译、程序跑起来不报任何错。**

上一个阶段的 `make_dfl_in_npu_onnx_v3.py` 跑在 opset 13 上(13 起取消了这个强制转换),
所以没踩到 —— **同一个脚本, 换个 opset 就静默坏掉**。

**修法**: opset ≤ 12 时先把 16 个 bin 转到最后一维再 softmax 再转回来;
ReduceSum / Squeeze 的 axes 形态也按 opset 适配(≤12 属性 / 13~17 输入 / ≥18 属性)。

**防复发**: 新增 `scripts/verify_formC_equiv.py` —— 把 fork9 的原始 DFL logits 在 CPU 上
手工解码, 和形态C 图内解出来的框逐像素对比, **不通过就不许转 RKNN**。
修好后 max|Δ| = **0.000127 px**。细节见 `docs/03_softmax_opset12_bug.md`。

---

## 六、边界: 这一版候选模型**不能**用来回答什么

| 能回答 | 不能回答 |
|---|---|
| NPU 耗时能省多少(结构问题) | 与生产 ebike 的**精度**对比(权重不同) |
| fork9 解码路径是否正确 | 真实 ebike 权重的检出率 |
| nc=2 形态的解码/量化行为 | 生产上线用的模型文件 |
| 流水线是否可交付 | —— |

**原因**: 手上没有算法组的 `best_split.onnx`, 候选模型的**权重**取自仓库的
yolov8n 模板(COCO 的 person/bicycle 前 2 类), 只保证了**结构**与生产 ebike 一致。

实测的差距(量化一下"权重不同"值多少):

```
生产 ebike(真权重) vs fork9(占位权重), 1724 帧:
  匹配率 0.30%   类别一致 0.00%   参考漏掉 333 / 被测多出 1363
```

⇒ **99.7% 的差别来自权重, 0.2% 来自形态。** 这也正说明:
**拿到真 ONNX 之后, 只需要重跑一遍转换 + 回归, 形态那一半的结论不用重做。**

---

## 七、交付清单

```
opt_ebike_fork9/
├── REPORT.md                      ← 本文件
├── docs/
│   ├── 00_source_not_available.md   ebike ONNX 拿不到(七路排查证据 + 交付清单)
│   ├── 01_ebike_topology_evidence.md 生产 ebike 结构证据(权重清单 + 逐算子表)
│   ├── 02_decoder_nc_fix.md          回归工具 nc 写死的坑 + 等价性验证
│   └── 03_softmax_opset12_bug.md      opset≤12 Softmax legacy 2D 强制转换(静默失败)
├── scripts/
│   ├── export_fork9_from_detect.py  ★ 任务书第一步: 原生 Detect ONNX → 9 输出 fork9 ONNX
│   ├── make_dfl_in_npu_onnx_v3.py      孪生形态C(DFL 留图内), 含 opset 适配 + softmax 修复
│   ├── build_ebike_arch_fork9.py    造"与生产同构"的 nc=2 fork9 ONNX
│   ├── convert_rknn_int8.py        ★ 任务书第二步: → INT8 RKNN (toolkit 2.3.2)
│   ├── verify_fork9_structure.py      9 输出结构自检
│   ├── verify_formC_equiv.py         形态C 数值自检(逐像素)
│   ├── build_ebike_calib.py           造 ebike 量化标定集
│   ├── build_ebike_fork9.sh         ★ 一条命令: ONNX → fork9 ONNX → RKNN → 孪生 → 自检
│   ├── run_regression.sh              板端回归 + NPU A/B
│   ├── compare_boxes.py              精度对比(复用 accuracy/aggregate.py 判定口径)
│   └── summarize_npu_ab.py           NPU 耗时 A/B 汇总
├── src/
│   ├── acc_batch_nc.cc + acc_batch_nc    nc 自适应的回归解码器(沙盒副本)
│   └── fork9_cpu_bench.cc + ...          CPU 侧解码代价实测
├── calib/ebike_calib.txt            200 张(替代算法组那份, 见 §八)
├── onnx/  models/  out/  logs/      全部产物与实测数据
```

**红线自查**:

| 红线 | 状态 |
|---|---|
| 不 merge | ✅ 只在 `opt_ebike_fork9/` 新增目录, 未动任何已有分支 |
| 不改生产推理逻辑 | ✅ `rk3588_zero_copy/src/**` 一个字节没动(mtime 未变) |
| 不改共享回归工具 | ✅ `accuracy/acc_batch.cc` 一个字节没动, 改的是沙盒副本 |
| 精度回归用修正后的解码器 | ✅ 用 `src/acc_batch_nc.cc`(含 ×stride 与跨步读修正, 且已验证与原版等价) |
| 独立目录 | ✅ 全部在 `opt_ebike_fork9/` |
| 板端测完恢复现场 | ✅ `rk-gate.service` 仍在跑(生产模型 + `--max-fps 20`), NPU 仍 1GHz, `GATE_MAX_FPS=20` 未动 |
| 生产配置不变 | ✅ `git status` 与开工时一致 |

---

## 八、需要算法组补的东西 + 接手后怎么跑

### 交接链路已做过端到端自测

用 `scripts/build_ebike_fork9.sh` 跑了一遍完整链路(输入是一份真的原生 Detect ONNX),
产物与直接造的参考模型**输出逐字节一致**:

```bash
bash scripts/build_ebike_fork9.sh onnx/ebike_nc2_formC.onnx e2e_test
#   -> models/e2e_test_fork9_int8.rknn  (4,177,184 B)
#   -> models/e2e_test_formC_int8.rknn  (4,660,995 B, 与参考模型同尺寸)
#   -> 结构自检 PASS / 形态C 数值自检 PASS (max|Δ|=0.000127 px)

# e2e 导出的 fork9  vs  直接造的参考 fork9, 同 120 帧:
IDENTICAL ✔
```

⇒ **"导出 → 转换 → 回归 → A/B" 这条链路是通的**, 缺的只是源文件本身。

### 需要的文件(按优先级)

```
detect_exp/exp02_person_ebike_gpu2/weights/best_split.onnx   ← 必需
detect_exp/exp02_person_ebike_gpu2/weights/best.pt            ← 备选
calib/ebike_calib.txt                                         ← 建议(否则用本仓造的那份)
```

> `calib/ebike_calib.txt` 本仓没有, 已用 `scripts/build_ebike_calib.py` 造了一份
> 200 张(取自 `dataset/` + `dataset_mp4/`, **与 1724 帧回归测试集不相交**),
> 但**算法组那份用的是训练分布**, 更合适。

### 接手后的命令

```bash
cd /home/seeed/rk_deploy/opt_ebike_fork9
cp <best_split.onnx> onnx/ebike_best_split.onnx

# 一条命令: fork9 ONNX → 结构自检 → INT8 RKNN → 孪生形态C → 数值自检 → 孪生 RKNN
bash scripts/build_ebike_fork9.sh onnx/ebike_best_split.onnx ebike_real

# 板端: 1724 帧精度回归 + NPU 耗时 A/B
bash scripts/run_regression.sh ebike_real_fork9_int8 ebike_real_formC_int8
```

**预期**: 流水线与本次验证过的完全一致; 只需把 §六里"占位权重"那一格换成真数。

---

## 九、选型建议(不 merge, 待决策)

| 决策条件 | 实测 | 判定 |
|---|---|---|
| ΔNPU ≤ 1ms → 不动 | **ΔNPU = −13.0 ms/帧** | 不满足 ⇒ 该动 |
| ΔNPU ≥ 3ms 且框精度是硬指标 | ≥3ms ✅; 精度**不掉**(99.78% 匹配 / 100% 类别一致 / 分数零漂移) ✅ | **满足** |
| CPU 侧代价可接受 | ≤ +1.5 ms/帧(正常 ≈0) | ✅ |

**建议: 走 fork9, 但顺序不能变**

1. **先拿到 `best_split.onnx`** —— 这是唯一的硬阻塞;
2. 用本仓流水线出候选, 重跑 1724 帧回归, 确认真权重下仍然是"匹配率 ≥ 99%";
3. **生产 FPS 当下不会涨** —— 生产是被 20fps/路 限帧卡住的
   (上一阶段实测 79.96/80 FPS), fork9 的价值是**余量**, 不是当下的帧率;
4. 上线前把 `src/acc_batch_nc.cc` 里那 3 行 nc 推导**合并回** `accuracy/acc_batch.cc`
   (回归工具, 不是生产代码), 否则以后谁拿 nc≠80 的 fork9 做回归都会踩坑;
5. fork9 的 CPU 侧解码走的是**形态B 分支**(`post_process_fork_9out`),
   它的 ×stride 已在 2026-11-22 修好并对拍通过(偏差 0.0019 px)—— **可以上线**。
