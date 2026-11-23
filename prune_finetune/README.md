# prune_finetune —— YOLOv8 结构化通道剪枝 + 量化 + 板端评估

> 本目录独立于主工程，**不改动 `rk3588_zero_copy/` 与 `models/`**。
> 产出模型放在 `prune_finetune/models/`，与生产模型完全隔离。

---

## 0. 一句话现状

**性能链路已完整落地**：6 个剪枝模型（SiLU576 / ReLU512 两族 × 20%/30%/40%）
已导出 RKNN INT8，板端纯推理耗时实测完成。

**精度链路被环境卡住**：本板**没有**人工标注的检测数据集、**没有** `yolov8n.pt`、
**没有** ultralytics、**没有** GPU、**没有**外网 —— 这四样是"稀疏化训练 + 微调"的
硬前提。因此：

| 任务要求 | 状态 | 说明 |
|---|---|---|
| 结构化通道剪枝（BN-L1 语义） | ✅ 已实现并跑通 | `prune_onnx.py`，判据 = `‖W[o]‖₁ · E\|act[o]\|`（见 §2） |
| 稀疏化训练（L1 正则） | ✅ 代码就绪 | `sparse_train.py`，需 GPU 环境 |
| 剪枝后微调恢复精度 | ✅ 代码就绪 | `finetune.py`，需 GPU 环境 |
| 3 档剪枝比例的量化模型 | ✅ 已产出 | `models/` 下 6 个 .rknn（per-tensor INT8） |
| 板端性能测试 | ✅ 已完成 | `run_prune_bench.sh`，单核 / 零拷贝 / 30s 稳态 / 3 轮 |
| 精度评估（mAP/召回/精确率） | ⚠️ **只有"剪枝前 vs 剪枝后"的相对退化** | 没有人工真值，用仓库既有的 pseudo-mAP 口径 |

> **本目录里的剪枝模型是【未微调】版本，按任务约束「禁止直接剪枝后上线」，
> 它们只用于量化"微调需要补回多少"，不可直接投产。**

---

## 1. 目录结构

```
prune_finetune/
├── prune_onnx.py              # ★ ONNX 结构化通道剪枝(本板可跑, 已落地)
├── build_pruned_models.py     # ★ 剪枝 -> ONNX -> RKNN INT8 -> 形状核对
├── run_prune_bench.sh         # ★ 板端 A/B(维护窗口 + 轮转 + 污染检测 + 自动恢复)
├── sparse_train.py            # BN gamma L1 稀疏化训练(需 GPU + ultralytics)
├── finetune.py                # 剪枝后微调恢复精度(需 GPU + ultralytics)
├── export_onnx.py             # (见下) 微调权重 -> ONNX -> RKNN 的衔接
├── configs/prune_{20,30,40}.yaml   # 3 档剪枝的完整配置
├── data/dataset.yaml          # YOLO 数据集配置模板(需要业务侧填路径)
├── eval/
│   ├── validate_pruned_onnx.py   # 结构自洽 + 输出形状 + 数值漂移(逐输出 cos)
│   ├── diff_layers.py            # 逐层对比原/剪后, 定位第一处数值分叉
│   └── prune_eval_acc.py         # mAP@0.5 / 召回率 / 精确率
├── models/                    # ★ 6 个剪枝后的 RKNN
├── models_bench/              # 基准测试用的模型集(含 2 个未剪枝参照)
├── onnx/                      # 剪枝后的中间 ONNX
└── out/                       # 全部实测数据
```

---

## 2. 剪枝判据：为什么用"折叠后卷积核 × 激活"

任务要求的经典路线是 **BN gamma 的 L1 稀疏化**：给 BN 的 gamma 加 L1 正则 → 训练 →
按 `|gamma|` 排序剪通道。

本项目的现实是：**手上的 ONNX 里已经没有 BatchNorm 节点**（BN 已被折叠进 Conv 的
权重/偏置），所以拿不到 gamma。但折叠关系是

```
W_folded = γ · W / sqrt(σ² + ε)
```

即**折叠后第 c 个输出通道的权重尺度 ∝ |γ_c| · ‖W_c‖**。因此本脚本用

```
importance(c) = ‖W_folded[c]‖₁ · E|act[c]|
```

—— 纯权重 L1 只看"这组权重有多大"，没看"这个通道下游到底起多大作用"；
乘上校准集上的平均绝对激活后，才是**这个通道对输出的实际贡献**。
它与 `|gamma|` 排序高度一致（只差一个逐层常数），且不需要重训就能拿到激活侧信息。

`prune_onnx.py` 的 `--no-act-aware` 可以退回纯权重判据。

---

## 3. 结构约束（保证剪完还是规则结构、RKNN 兼容）

| 规则 | 内容 | 为什么 |
|---|---|---|
| R1 | **受保护 Conv 不剪输出通道**：检测头 box 分支(64ch)、cls 分支(80ch) | 这两个决定图输出的形状，剪了输出对不上 |
| R2 | **C2f 块宽度一致**：cv1 输出 = n × 块宽；bottleneck 各层 = 块宽 | Split 的各份必须等宽，否则 Split 算子非法 |
| R3 | **Concat 分支可不同宽**，但下游消费者的输入数 = 各分支实际宽度之和（由传播决定） | Concat 只做拼接 |
| R4 | 其它 Conv 输出 = `round(out_c × (1-r))`，至少留 `min_keep` | — |
| R5 | **bottleneck 末端沿用 Add 另一侧（Split chunk）的幸存下标** | 逐元素 Add 的两个操作数要下标一一对应，否则语义错位 |
| — | `Split` 节点的 `split=[16,16]` 属性同步改写 | 不改 ONNX 直接报 `ShapeInferenceError` |
| — | `Mul`(SiLU 激活) 的两个输入同步裁剪 | SiLU = `Mul(X, Sigmoid(X))`，两路必须同宽 |

**自检**：`eval/validate_pruned_onnx.py` 会逐条检查 Add 等宽 / Split 等宽 /
Conv 输入通道数与生产者输出是否匹配，并核对图输出形状是否与原模型完全一致。
6 个模型全部通过。

---

## 4. 复现步骤

```bash
cd /home/seeed/rk_deploy/prune_finetune

# ① 剪枝 + 量化 + 导出 (6 个模型)
/home/seeed/rk_deploy/_bench/venv/bin/python -I build_pruned_models.py
#    产物: models/{relu512,silu576}_p{20,30,40}.rknn + models/pruned_models.json

# ② 结构与数值校验
/home/seeed/rk_deploy/_bench/venv/bin/python -I eval/validate_pruned_onnx.py \
    --orig ../model_opt/onnx/yolov8n_silu_576.onnx --pruned onnx/yolov8n_silu_p20.onnx --size 576

# ③ 板端性能 A/B (维护窗口 + 轮转 3 轮 + 逐核污染检测 + 自动恢复服务)
sudo -v && ./run_prune_bench.sh

# ④ 精度评估 (pseudo-mAP@0.5 / 召回率 / 精确率)
/home/seeed/rk_deploy/_bench/venv/bin/python -I eval/prune_eval_acc.py \
    --ref ../model_opt/models/C1_silu_576.rknn \
    --tests models/silu576_p20.rknn models/silu576_p30.rknn models/silu576_p40.rknn \
    --frames 300 --conf 0.15
```

---

## 5. 要跑真正的「稀疏化训练 + 微调」，需要补齐什么

```bash
# 在一台有 GPU 的机器上
pip install ultralytics torch torchvision

# 1) 准备带标注的数据集(YOLO 格式), 填 data/dataset.yaml
#    ★ 本仓库没有: accuracy/data/frames 只有无标注抽帧
#      (manifests/frames.csv 只有"帧号 -> 车牌数", 检测框没有人工真值)

# 2) 稀疏化训练 (BN gamma L1)
python sparse_train.py --model yolov8n.pt --data data/dataset.yaml \
    --out runs/sparse --epochs 30 --l1 1e-4 --sparsity 0.5 --prune-ratio 0.3

# 3) 剪枝后微调 (恢复精度)
python finetune.py --model runs/sparse/pruned_p30.pt --data data/dataset.yaml \
    --out runs/ft_p30 --epochs 15 --lr0 1e-4

# 4) 微调权重 -> ONNX -> RKNN (用 model_opt 的统一量化口径)
python ../model_opt/export_quantize.py ...
```

**注意**：微调后**必须重新标定 conf 阈值** —— 剪枝会改变分数分布
（见评估报告 §4.3 的实测：未剪枝 max_cls=0.71，剪 20% 后掉到 0.0020）。

---

## 6. 踩过的坑

1. **`rknn.load_onnx(input_size_list=...)` 在 toolkit 2.3.2 上被静默忽略** ——
   改分辨率必须直接改 ONNX 输入形状，并**清空图输出的 shape dim**（否则形状自相矛盾）。
2. **ONNX 里改过模型后，必须存成文件再让 onnxruntime 加载** —— 直接用原路径建
   session 会用回原始图，中间张量拿不到（本项目在这个坑上栽了两次）。
3. **给 ONNX 临时加图输出时不能用副本以外的模型** —— 加了旧形状声明的中间输出，
   会把 ORT 的形状推理带偏，症状是 `Add: Incompatible dimensions`。
4. **`/sys/class/devfreq/fdab0000.npu/load` 在本板恒为 100%** —— 判断 NPU 空闲要用
   `/sys/kernel/debug/rknpu/load` 的逐核百分比。
5. **`Split` 节点的 `split` 属性不跟着剪枝改** —— 症状是 ONNX 加载报
   `Mismatch between the sum of 'split' and the split dimension of the input`。
6. **SiLU 激活在 ONNX 里是 `Mul(X, Sigmoid(X))`** —— 遍历图找"通道的生产者"时，
   必须能穿过 `Mul`，否则整条 C2f 的宽度规则不会生效（表现是剪枝后 Add 两侧不等宽）。
7. **`/run/rk-gate/paused` 是【共享状态】** —— 本板同时有多个会话在跑，
   任何一方进了维护窗口却没清标记，看门狗就会一直闭嘴，
   `rk-gate` 停了也没人拉起（本次实测踩到过一次，表现为服务 inactive 且无看门狗告警）。
   恢复：`sudo rm -f /run/rk-gate/paused && sudo systemctl start rk-gate`。
   压测前后都应确认这个标记不存在。
8. **给 RKNN 工具链构建时切到临时目录** —— 它会把中间优化产物
   （`check0_base_optimize.onnx` 等，每份 12 MB）写到**当前工作目录**，会污染交付目录。
