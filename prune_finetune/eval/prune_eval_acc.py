#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""prune_eval_acc.py —— 剪枝模型的检测精度评估 (mAP@0.5 / 召回率 / 精确率)

======================================================================
【口径说明 —— 务必先看这一段】
======================================================================
仓库里【没有人工标注的检测框】(accuracy/data/manifests/frames.csv 只有
帧号 -> 车牌数 的映射; 只有车牌文本有人工真值, 检测框没有)。
所以这里的 mAP@0.5 是仓库既有的【pseudo-mAP】口径:
    ★ 把【未剪枝的同族模型】的检测框当"真值", 算剪枝模型的 mAP@0.5。
      —— 与 accuracy/aggregate.py 的 pseudo_map50() 完全同一套实现, 口径一致。
    ★ 它衡量的是"剪枝后相对未剪枝掉了多少", 不是绝对 mAP。
      两者都错时它看不出来, 所以【绝对精度仍需业务侧用人工标注回归】。
    ★ 召回率 = 参考框被匹配上的比例; 精确率 = 预测框能对上参考框的比例。

======================================================================
【YOLOv8 原始输出的解码】
======================================================================
本模型(yolov8n_optimized.onnx)的输出是【未解码】的:
    box: [1, 64, H, W]   4 个边 × 16 个 DFL 分布 bin
    cls: [1, 80, H, W]   80 个类别的原始得分
解码: DFL(softmax over 16 bins × bin 坐标) -> 4 个边长(像素) ->
      以 stride=8/16/32 的网格为锚点还原成 xyxy -> 按类别做 NMS。

用法:
    ./prune_eval_acc.py --ref models_ref/D1_relu_512.rknn \
                        --tests models/relu512_p20.rknn models/relu512_p30.rknn \
                        --frames 400
"""

import argparse
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "model_opt"))
sys.path.insert(0, os.path.join(REPO, "accuracy"))

from export_quantize import rknn_infer, verify_output_shapes   # noqa: E402
from aggregate import pseudo_map50, detection_compare, iou       # noqa: E402

import cv2

STRIDES = [8, 16, 32]
BOX_IDX = [0, 3, 6]      # 3 个尺度的 box 张量
CLS_IDX = [1, 4, 7]      # 3 个尺度的 cls 张量


def dfl_decode(box, stride):
    """[1,64,H,W] -> [1,4,H,W] 的 4 个边长(像素)。"""
    b = box.reshape(1, 4, 16, box.shape[2], box.shape[3])
    e = np.exp(b - b.max(axis=2, keepdims=True))
    p = e / e.sum(axis=2, keepdims=True)
    bins = np.arange(16, dtype=np.float32).reshape(1, 1, 16, 1, 1)
    return (p * bins).sum(axis=2) * stride


def decode(model_out, size, min_score=0.05):
    """-> [(cls, score, x1,y1,x2,y2), ...]"""
    dets = []
    for si, (bi, ci) in enumerate(zip(BOX_IDX, CLS_IDX)):
        box = model_out[bi]
        cls = model_out[ci]
        h, w = box.shape[2], box.shape[3]
        st = STRIDES[si]
        d = dfl_decode(box, st)
        gx = (np.arange(w, dtype=np.float32) + 0.5) * st
        gy = (np.arange(h, dtype=np.float32) + 0.5) * st
        X, Y = np.meshgrid(gx, gy)
        x1 = X - d[0, 0]
        y1 = Y - d[0, 1]
        x2 = X + d[0, 2]
        y2 = Y + d[0, 3]
        c = cls[0]
        flat = c.reshape(c.shape[0], -1)
        best_cls = flat.argmax(axis=0)
        best_score = flat.max(axis=0)
        idx = np.nonzero(best_score >= min_score)[0]   # 先粗筛
        ys, xs = idx // w, idx % w
        for k in range(len(idx)):
            i = idx[k]
            dets.append((int(best_cls[i]), float(best_score[i]),
                         float(x1[ys[k], xs[k]]), float(y1[ys[k], xs[k]]),
                         float(x2[ys[k], xs[k]]), float(y2[ys[k], xs[k]])))
    return dets


def nms(dets, conf=0.15, iou_thr=0.45):
    """按类别做 NMS, 返回 [(cls,score,x1,y1,x2,y2)]。"""
    out = []
    by_cls = {}
    for d in dets:
        if d[1] < conf:
            continue
        by_cls.setdefault(d[0], []).append(d)
    for c, lst in by_cls.items():
        lst.sort(key=lambda x: -x[1])
        keep = []
        for d in lst:
            good = True
            for k in keep:
                if iou(d, k) > iou_thr:
                    good = False
                    break
            if good:
                keep.append(d)
        out.extend(keep)
    return out


def run_model(model_path, frames, conf=0.15):
    """-> {frame_path: [(cls,score,x1,y1,x2,y2)]}"""
    meta = verify_output_shapes(model_path, 0)
    size = meta["input_hw"][0]
    dims = [d for _, d in meta["outputs"]]       # 每个输出的形状
    imgs = []
    for p in frames:
        im = cv2.imread(p)
        if im is None:
            continue
        imgs.append(cv2.resize(im, (size, size), interpolation=cv2.INTER_LINEAR)[:, :, ::-1].copy())
    outs = rknn_infer(model_path, imgs)
    # ctypes 取回来的是一维数组, 按模型声明的形状 reshape
    outs = [[np.asarray(o).reshape(d) for o, d in zip(frame_outs, dims)]
            for frame_outs in outs]
    res = {}
    i = 0
    for p in frames:
        if i >= len(outs):
            break
        res[p] = nms(decode(outs[i], size, min_score=min(conf, 0.05)), conf=conf)
        i += 1
    return res, size, i


def compare(ref_map, test_map, iou_thr=0.5):
    st = detection_compare(ref_map, test_map, iou_thr)
    n_ref = st["ref_boxes"]
    n_test = st["test_boxes"]
    matched = st["matched"]
    ap, ncls = pseudo_map50(ref_map, test_map, iou_thr)
    rec = matched / n_ref if n_ref else 0.0
    prec = matched / n_test if n_test else 0.0
    rs = [b[1] for v in ref_map.values() for b in v]
    ts = [b[1] for v in test_map.values() for b in v]
    return {
        "ref_boxes": n_ref, "test_boxes": n_test, "matched": matched,
        "recall": rec, "precision": prec,
        "pseudo_map50": ap, "n_classes": ncls,
        "ref_mean_score": float(np.mean(rs)) if rs else 0.0,
        "test_mean_score": float(np.mean(ts)) if ts else 0.0,
        "cls_agree": st["cls_agree"],
        "iou_sum": st["iou_sum"],
        "missed_gone": st["iou_bucket_gone"],
        "missed_shifted": st["iou_bucket_shifted"],
        "missed_partial": st["iou_bucket_partial"],
        "missed_different": st["iou_bucket_different"],
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", required=True, help="参考(未剪枝)模型 .rknn")
    ap.add_argument("--tests", nargs="+", required=True, help="被测模型 .rknn 列表")
    ap.add_argument("--frames-dir", default=os.path.join(REPO, "accuracy", "data", "frames"))
    ap.add_argument("--frames", type=int, default=400)
    ap.add_argument("--conf", type=float, default=0.15)
    ap.add_argument("--iou", type=float, default=0.5)
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    frames = sorted(os.path.join(args.frames_dir, f)
                    for f in os.listdir(args.frames_dir) if f.endswith(".jpg"))[:args.frames]
    print("验证集: %s (%d 帧)" % (args.frames_dir, len(frames)))
    print("参考模型: %s" % os.path.basename(args.ref))
    print("口径: pseudo-mAP@0.5 —— 未剪枝同族模型的框当真值(仓库既有口径)\n")

    ref_map, ref_size, n = run_model(args.ref, frames, args.conf)
    print("参考模型: %d 帧, %d 个检测框, 输入 %dx%d"
          % (n, sum(len(v) for v in ref_map.values()), ref_size, ref_size))

    results = {"ref": os.path.basename(args.ref), "n_frames": n,
               "conf": args.conf, "iou_thr": args.iou, "tests": {}}
    base = compare(ref_map, ref_map, args.iou)
    print("\n%-18s %-10s %-10s %-10s %-10s %-10s" %
          ("模型", "mAP@0.5", "召回率", "精确率", "框数", "均分"))
    print("%-18s %-10.4f %-10.4f %-10.4f %-10d %-10.4f" %
          ("(参考自身)", base["pseudo_map50"], base["recall"], base["precision"],
           base["ref_boxes"], base["ref_mean_score"]))

    for t in args.tests:
        tm, size, nn = run_model(t, frames, args.conf)
        c = compare(ref_map, tm, args.iou)
        name = os.path.basename(t).replace(".rknn", "")
        results["tests"][name] = dict(c, size=size, n_frames=nn)
        print("%-18s %-10.4f %-10.4f %-10.4f %-10d %-10.4f" %
              (name, c["pseudo_map50"], c["recall"], c["precision"],
               c["test_boxes"], c["test_mean_score"]))

    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f, indent=2, ensure_ascii=False)
        print("\nJSON: %s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
