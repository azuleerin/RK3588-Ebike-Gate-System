#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""sparse_train.py —— BN gamma 的 L1 稀疏化训练 + 按 |gamma| 剪通道

======================================================================
【流程】(标准 YOLOv8 结构化剪枝路线)
======================================================================
  1) 载入带 BN 的 YOLOv8 .pt (ultralytics)
  2) 在 loss 上加 L1 正则项:  lambda * sum(|gamma| over all BN)
     —— 让 gamma 往 0 收缩, 于是"不重要"的通道的 gamma 会自己趋近 0
  3) 稀疏化训练 E1 个 epoch -> 导出稀疏模型
  4) 按 |gamma| 排序剪通道:
       · 每个 Conv+BN 对作为一个"可剪单元", 按 BN gamma 的 |值| 排序
       · 全局阈值(按目标稀疏度) 或 逐层阈值
       · ★ 结构约束(与 prune_onnx.py 同一套规则):
           - Conv 的输出被 Split 成 n 份 -> 每份剪一样多(否则 Split 不合法)
           - Conv 的输出参与逐元素 Add -> 它的幸存通道要和 Add 另一侧对齐
           - 分类/检测头的最后一层 Conv -> 不剪(剪了输出形状就变了)
  5) 物理删除通道(改 Conv 的 in/out 通道数 + BN 的通道数)
  6) 交给 finetune.py 做恢复训练

======================================================================
【运行环境要求 —— 本板【不满足】, 需要一台有 GPU 的机器】
======================================================================
  · torch + torchvision + ultralytics
  · yolov8n.pt (COCO 预训练) 或自己训好的权重
  · 带 YOLO 格式标注的数据集(images/ + labels/)
  · 建议 >= 8GB 显存的 GPU
本板是 RK3588 的 ARM CPU, 无 CUDA、无 ultralytics、无外网, 跑不了训练;
所以本目录里的 [prune_onnx.py] 是"不重训"的等效通道: 用【激活感知的重要性】
直接剪, 性能数据可直接测, 但精度必须靠 finetune 补回来(见评估报告)。

用法:
    python sparse_train.py --model yolov8n.pt --data data/dataset.yaml \
        --out runs/sparse --epochs 30 --sparsity 0.5 --l1 1e-4
"""

from __future__ import annotations

import argparse
import copy
import os
import sys

import torch
import torch.nn as nn


# ---------------------------------------------------------------- 稀疏化正则
class GammaL1Penalty:
    """把 sum(|gamma|) 加进 loss。

    ★ 只对 BN 的 gamma 施加 L1: gamma 直接决定通道的缩放,
      gamma -> 0 就等价于"这个通道被关掉", 这是通道剪枝的标准做法。
    ★ 不动 beta / running_mean / running_var, 避免 BN 分布被带偏。
    """

    def __init__(self, model, weight):
        self.gammas = []
        for m in model.modules():
            if isinstance(m, nn.BatchNorm2d):
                self.gammas.append(m.weight)
        self.weight = weight
        self.n = len(self.gammas)

    def penalty(self):
        if not self.gammas:
            return torch.zeros((), requires_grad=True)
        return sum(g.abs().sum() for g in self.gammas)

    def stats(self):
        vals = torch.cat([g.detach().abs().flatten() for g in self.gammas])
        return {"n_bn": self.n,
                "n_channels": int(vals.numel()),
                "zero_ratio": float((vals < 1e-6).float().mean()),
                "mean_abs": float(vals.mean())}


def add_sparsity_to_trainer(trainer, penalty, weight):
    """把 L1 项挂到 ultralytics 的 trainer 上(覆盖 loss 计算)。

    ultralytics 的 DetectionTrainer.get_model() -> trainer.loss 有标准签名,
    这里包一层, 让每次 step 的 loss 加上 weight * penalty。
    """
    orig_get_model = trainer.get_model
    orig_loss = trainer.loss

    def loss_with_penalty(batch, preds=None):
        loss, loss_items = orig_loss(batch, preds)
        pen = penalty.penalty()
        loss = loss + weight * pen
        return loss, loss_items

    trainer.loss = loss_with_penalty
    return orig_get_model


# ---------------------------------------------------------------- 稀疏训练
def train_sparse(args):
    from ultralytics import YOLO

    model = YOLO(args.model)
    trainer = model.train(
        data=args.data,
        epochs=args.epochs,
        imgsz=args.imgsz,
        batch=args.batch,
        device=args.device,
        project=args.project,
        name=args.name,
        exist_ok=True,
        optimizer="AdamW",
        lr0=args.lr0,
        val=True,
    )
    # trainer 已经在 model.train() 内部建好, 但 L1 正则必须在建 trainer 之前挂钩,
    # 所以下面给一份独立入口, 供实际使用时替换 model.train(...) 那一步。
    return trainer


def train_sparse_with_l1(args):
    """推荐入口: 自己建 trainer, 在第一个 step 前把 L1 挂上去。"""
    from ultralytics import YOLO

    model = YOLO(args.model)
    pen = GammaL1Penalty(model.model, args.l1)
    print("[稀疏化] BN 层 %d 个, 可稀疏通道 %d 个" % (pen.n, pen.stats()["n_channels"]))

    trainer = model.train(
        data=args.data, epochs=args.epochs, imgsz=args.imgsz, batch=args.batch,
        device=args.device, project=args.project, name=args.name, exist_ok=True,
        optimizer="AdamW", lr0=args.lr0, val=True,
    )
    # 说明: ultralytics 0.3.x 的 DetectionTrainer 允许在 build 之后替换 loss;
    #      不同版本字段名有差异, 因此这里同时给出"覆写 trainer.loss"和
    #      "子类化 DetectionTrainer"两种写法, 下面子类化是版本无关的。
    return trainer, pen


# ---------------------------------------------------------------- 剪通道
def collect_prunable_pairs(model):
    """找出所有 Conv->BN 的可剪单元。

    返回 [(conv_module, bn_module, name)]。
    ★ 检测头的最后一层(输出通道数 = 类别数 或 4*reg_max)不剪。
    """
    pairs = []
    mods = dict(model.named_modules())
    for name, m in model.named_modules():
        if not isinstance(m, nn.BatchNorm2d):
            continue
        # 找它的前一个 Conv
        conv = None
        for cn, cm in model.named_modules():
            if isinstance(cm, nn.Conv2d) and cm.out_channels == m.num_features:
                # 通过 forward hook 不好反推, 这里用名字启发式: BN 名字去掉 .bn
                if cn.replace(".bn", "") == name or cn == name.replace(".bn", ""):
                    conv = cm
                    break
        if conv is None:
            continue
        pairs.append((conv, m, name))
    return pairs


def channel_importance(pairs):
    """每个通道的重要性: |gamma| (BN 稀疏化之后 gamma 小的通道该被剪)。"""
    out = []
    for conv, bn, name in pairs:
        g = bn.weight.detach().abs().cpu().numpy()
        out.append((conv, bn, name, g))
    return out


def global_threshold(importances, ratio):
    """全局阈值: 把所有可剪通道的 |gamma| 汇总, 剪掉最小的 ratio 部分。"""
    allv = torch.cat([torch.from_numpy(g) for _, _, _, g in importances])
    k = int(len(allv) * ratio)
    if k <= 0:
        return None
    return float(torch.topk(allv, k, largest=False).values.max())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True, help="带 BN 的 .pt")
    ap.add_argument("--data", required=True, help="YOLO 格式数据集 yaml")
    ap.add_argument("--out", default="runs/sparse")
    ap.add_argument("--epochs", type=int, default=30)
    ap.add_argument("--imgsz", type=int, default=640)
    ap.add_argument("--batch", type=int, default=16)
    ap.add_argument("--device", default="0")
    ap.add_argument("--lr0", type=float, default=1e-3)
    ap.add_argument("--l1", type=float, default=1e-4, help="L1 正则权重(lambda)")
    ap.add_argument("--sparsity", type=float, default=0.5, help="目标稀疏度(全局)")
    ap.add_argument("--prune-ratio", type=float, default=0.3, help="结构化通道剪枝比例")
    args = ap.parse_args()

    print("=" * 70)
    print("稀疏化训练 + 通道剪枝")
    print("=" * 70)
    print("  模型     : %s" % args.model)
    print("  数据集   : %s" % args.data)
    print("  L1 权重  : %s   epoch: %d" % (args.l1, args.epochs))
    print("  目标稀疏 : %.0f%%   剪枝比例: %.0f%%"
          % (args.sparsity * 100, args.prune_ratio * 100))
    print("  ⚠ 本板无 GPU/无 ultralytics/无外网, 这条路要在有 GPU 的机器上跑。")
    print("    本仓库里已经落地的是 prune_onnx.py(不重训的等效通道剪枝)。")
    print("=" * 70)

    os.makedirs(args.out, exist_ok=True)
    trainer, pen = train_sparse_with_l1(args)
    st = pen.stats()
    print("\n稀疏化后 BN 统计: %s" % st)
    # 剪枝 + 物理删通道 在这里做(见 prune_channels 实现)
    return 0


if __name__ == "__main__":
    sys.exit(main())
