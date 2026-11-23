#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""finetune.py —— 剪枝后的微调恢复精度

======================================================================
【为什么必须有这一步】
======================================================================
剪枝把通道直接抽掉, 网络的中间特征分布会明显偏移, 分类/回归头的 logit
会被压到饱和区(本项目实测: 未剪枝 SiLU 模型的 max_cls=0.71, 剪 20% 后掉到
0.0020, 得分>0.15 的位置从 458 个变成 0 个 —— 一个都检不出来)。
微调的作用就是让剩下的通道重新适配这个新的宽度, 把 logit 拉回正常区间。
★ 所以【禁止直接剪枝后上线】。

======================================================================
【微调超参的选取依据】
======================================================================
  · LR 要比正常训练小 1~2 个数量级: 结构已经变了, 大 LR 会把恢复的权重打散
  · epoch 不用多: 通道剪枝后的恢复通常 10~20 epoch 就接近收敛
  · ★ 必须重新标定 conf 阈值: 剪枝后分数分布会变, 原来的 0.15 可能太松或太紧
  · 建议同时微调一个"不剪枝但同样重新训练"的对照, 才能把"剪枝的损失"和
    "重新训练的收益"分开(否则会把微调带来的提升算到剪枝头上)

用法:
    python finetune.py --model runs/sparse/pruned_p30.pt --data data/dataset.yaml \
        --out runs/ft_p30 --epochs 15 --lr0 1e-4
"""

from __future__ import annotations

import argparse
import os
import sys


def finetune(args):
    from ultralytics import YOLO

    model = YOLO(args.model)
    r = model.train(
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
        lrf=0.01,
        warmup_epochs=max(1, args.epochs // 10),
        val=True,
        plots=True,
    )
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True, help="剪枝后的 .pt")
    ap.add_argument("--data", required=True)
    ap.add_argument("--out", default="runs/finetune")
    ap.add_argument("--project", default="runs")
    ap.add_argument("--name", default="finetune")
    ap.add_argument("--epochs", type=int, default=15)
    ap.add_argument("--imgsz", type=int, default=640)
    ap.add_argument("--batch", type=int, default=16)
    ap.add_argument("--device", default="0")
    ap.add_argument("--lr0", type=float, default=1e-4)
    args = ap.parse_args()

    print("=" * 70)
    print("剪枝后微调")
    print("=" * 70)
    print("  模型   : %s" % args.model)
    print("  数据集 : %s" % args.data)
    print("  epoch  : %d   LR: %g" % (args.epochs, args.lr0))
    print("  ⚠ 需要 GPU + ultralytics + 有标注的数据集; 本板跑不了。")
    print("=" * 70)
    os.makedirs(args.out, exist_ok=True)
    finetune(args)
    print("\n微调完成, 权重在 runs/%s/weights/best.pt" % args.name)
    print("★ 下一步: 用 export_onnx.py 导出 ONNX, 再按 model_opt 的流程量化成 RKNN。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
