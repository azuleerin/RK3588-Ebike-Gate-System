#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""eval_plate_recognition.py —— 真实车牌整牌识别率评估(生产同款候选择优)

★ 判定逻辑与 src/gate_main.cc 完全一致:
  · 两行牌: 上行 3 个切分点候选 + 下行 1 个
  · 选上行: 含汉字 > 字符多 > 分数高 (include/rk/plate_roi.h better_top_row)
  · 整牌 = 上行 + 下行
★ 真值: DJI_20261102155401_0126_D.MP4 的两行绿牌 苏N / A339058
  · 整牌命中: 整牌文本里出现串号 A339058
  · 省份命中: 上行文本里出现 "苏"
  · 全串命中: 整牌文本去掉省份字后 == NA339058
"""
import argparse
import csv
import os
import sys
from collections import defaultdict

GT_SERIAL = "A339058"
GT_ALNUM = "NA339058"
PROV = "苏"
# GT 车牌框内的文字行相对位置(实测自 f00000 特写: 上行"苏 N"在 0.149~0.422, 下行串号在 0.446~0.907)
PROV_ROW = (0.149, 0.422)
SERIAL_ROW = (0.446, 0.907)


def has_cjk(t):
    return any(ord(ch) >= 0xE0 for ch in t)


def better(a, a_score, b, b_score):
    """include/rk/plate_roi.h better_top_row: 含汉字 > 字符多 > 分数高"""
    ca, cb = has_cjk(a), has_cjk(b)
    if ca != cb:
        return ca
    if len(a) != len(b):
        return len(a) > len(b)
    return a_score > b_score


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True, help="plate_crops/<tag> 目录")
    ap.add_argument("--label", default=None)
    args = ap.parse_args()
    tag = args.label or os.path.basename(args.dir.rstrip("/"))

    # GT 框(plates.csv, 有一列错位, 按尾部位置解析)
    gt = {}
    with open("/home/seeed/rk_deploy/accuracy/data/manifests/plates.csv") as fh:
        for row in csv.reader(fh):
            if not row or row[0] == "crop_path":
                continue
            x, y, w, h = (int(v) for v in row[-6:-2])
            gt[int(row[3])] = (x, y, w, h)

    crops = {}
    with open(os.path.join(args.dir, "crops.csv")) as fh:
        for r in csv.DictReader(fh):
            crops[r["crop_path"]] = r
    recs = {}
    with open(os.path.join(args.dir, "rec.csv")) as fh:
        for r in csv.DictReader(fh):
            recs[r["crop_path"]] = r

    by_frame = defaultdict(lambda: {"top": [], "bot": [], "all": []})
    for path, r in crops.items():
        rec = recs.get(path)
        if rec is None:
            continue
        text = (rec.get("ppocr_text") or "").strip()
        try:
            score = float(rec.get("ppocr_score") or 0)
        except ValueError:
            score = 0.0
        kind = r["kind"]
        grp = "top" if kind.startswith("top") else ("bot" if kind == "bot" else "all")
        by_frame[int(r["frame_idx"])][grp].append(
            (text, score, path, int(r["roi_w"]), int(r["roi_h"]), int(r["crop_w"]),
             int(r["crop_h"]), int(r["roi_x"]), int(r["roi_y"]), kind))

    n_frames = 0
    # O9 几何判据: ① ROI 顶边不切进省份字行 ② 存在一个上行候选把省份字行整行框住
    o9_top_cut = 0
    o9_row_ok = 0
    serial_row_ok = 0
    hit_serial = 0
    hit_prov = 0
    hit_alnum = 0
    empty = 0
    top_empty = 0
    crop_ws = []
    texts = {}
    for fi, g in sorted(by_frame.items()):
        n_frames += 1
        best_top = None
        roi_rect = None
        for text, score, path, rw, rh, cw, ch, rx, ry, kd in g["top"] + g["all"]:
            if roi_rect is None:
                roi_rect = (rx, ry, rw, rh, kd)
            if best_top is None or better(text, score, best_top[0], best_top[1]):
                best_top = (text, score)
        bot = ""
        if g["bot"]:
            # 下行只有一个候选, 取分数最高的
            bot = max(g["bot"], key=lambda x: x[1])[0]
        gbox = gt.get(fi)
        if gbox is not None:
            gx, gy, gw, gh = gbox
            p_top = gy + PROV_ROW[0] * gh
            p_bot = gy + PROV_ROW[1] * gh
            s_top = gy + SERIAL_ROW[0] * gh
            s_bot = gy + SERIAL_ROW[1] * gh
            if roi_rect and roi_rect[1] > p_top:
                o9_top_cut += 1
            # 上行候选: 切分条 = ROI[ry : ry+rh*sp], sp ∈ {0.40,0.45,0.50}(top0/1/2)
            #   整行框住省份字行 <=> ry <= 行顶 且 ry+rh*sp >= 行底
            # crops.csv 里 roi_y/roi_h 记的是【切分条】的矩形, 要还原整块 ROI:
            #   top0/1/2: 条 = ROI[ry : ry+ROI_h*sp], sp = 0.40/0.45/0.50
            #   bot     : 条 = ROI[ry+ROI_h*0.45 : ry+ROI_h]
            #   all     : 条 = 整块 ROI
            ry0 = rh0 = None
            if roi_rect is not None:
                kind0 = roi_rect[4]
                ry_s, rh_s = roi_rect[1], roi_rect[3]
                if kind0.startswith("top"):
                    sp = {"top0": 0.40, "top1": 0.45, "top2": 0.50}[kind0]
                    ry0, rh0 = ry_s, rh_s / sp
                elif kind0 == "bot":
                    rh0 = rh_s / 0.55
                    ry0 = ry_s - rh0 * 0.45
                else:
                    ry0, rh0 = ry_s, rh_s
            if ry0 is not None:
                sps = [0.40, 0.45, 0.50]
                if any(ry0 <= p_top and ry0 + rh0 * sp >= p_bot for sp in sps):
                    o9_row_ok += 1
                # 下行候选 = ROI[ry+rh*0.45 : ry+rh]
                if (ry0 + rh0 * 0.45) <= s_top and (ry0 + rh0) >= s_bot:
                    serial_row_ok += 1
        if best_top is None:
            continue
        top_text = best_top[0]
        whole = top_text + bot
        if not top_text:
            top_empty += 1
        if not whole.strip():
            empty += 1
        if GT_SERIAL in whole:
            hit_serial += 1
        if PROV in top_text:
            hit_prov += 1
        if GT_ALNUM in whole or (GT_SERIAL in whole and PROV in top_text):
            hit_alnum += 1
        for c in g["top"] + g["all"] + g["bot"]:
            crop_ws.append(c[5])
        texts.setdefault(whole.strip(), 0)
        texts[whole.strip()] += 1

    n = max(1, n_frames)
    print("=== %s ===" % tag)
    print("  真牌帧(有裁剪)      : %d" % n_frames)
    print("  整牌命中(含串号 %s) : %d  (%.2f%%)" % (GT_SERIAL, hit_serial, 100.0 * hit_serial / n))
    print("  省份字命中(%s)      : %d  (%.2f%%)" % (PROV, hit_prov, 100.0 * hit_prov / n))
    print("  全串命中(%s)      : %d  (%.2f%%)" % (GT_ALNUM, hit_alnum, 100.0 * hit_alnum / n))
    print("  上行空输出          : %d  (%.2f%%)" % (top_empty, 100.0 * top_empty / n))
    print("  整牌空输出          : %d  (%.2f%%)" % (empty, 100.0 * empty / n))
    if crop_ws:
        import statistics
        print("  喂给 PP-OCR 的裁剪宽: 中位 %d (min %d, max %d)"
              % (statistics.median(crop_ws), min(crop_ws), max(crop_ws)))
    print("  O9 几何: ROI 顶边切进省份字行的帧 = %d (%.2f%%)  <- 应为 0"
          % (o9_top_cut, 100.0 * o9_top_cut / n))
    print("  O9 几何: 存在把省份字行整行框住的上行候选 = %d (%.2f%%)"
          % (o9_row_ok, 100.0 * o9_row_ok / n))
    print("  串号行整行被下行候选框住 = %d (%.2f%%)"
          % (serial_row_ok, 100.0 * serial_row_ok / n))
    top5 = sorted(texts.items(), key=lambda kv: -kv[1])[:6]
    print("  识别文本分布(前 6): %s" % top5)
    return 0


if __name__ == "__main__":
    sys.exit(main())
