#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""sweep_roi_params.py —— 面向 ebike_real_fork9 离线扫描 GATE_ROI_* 参数(纯 CPU)

★ 为什么必须重标: 生产 ROI 参数是照着【旧模型的畸形宽扁框】标定的。
  fork9 的框是【整车框】(实测 f00000: 1835x1040, 覆盖整辆车), 而现行
  roi_y=1.0 表示"ROI 顶边 = 框底" => ROI 落到车身底部/画面下缘, 根本碰不到
  位于车身中部的车牌(GT 绿框 909,394,391,249)。

★ 复刻的是生产同一条链(逐字照抄 src/main.cc:1119 plate_roi_from_box +
  include/rk/plate_roi.h clamp_roi + src/gate_main.cc 的 pad/偶对齐/最小尺寸):
    plate_roi_from_box -> ppocr_pad(0.10) -> clamp -> 偶对齐 -> clamp
    -> 最小尺寸(rh>=16 / rw>=32, 向上/向两侧撑开) -> clamp
  任何一处不一致, 扫出来的参数上线就会偏。

★ 判据(用 DJI 真车牌帧 + plates.csv 的 GT 框):
    文字行覆盖: ROI 纵向覆盖 GT 的 [0.10,0.90], 横向覆盖 [0.02,0.98]
    整块覆盖  : ROI 完全包含 GT 框
    宽度      : 任务书门槛 >= 160 px(PP-OCR 的可用分辨率)
    两行牌判定: rw/rh < kTwoRowAspect(2.5)

用法: sweep_roi_params.py --boxes <fork9 boxes.csv> [--boxes2 <prod boxes.csv>] --out <csv>
"""
import argparse
import csv
import json
import os
import sys

import numpy as np

FW, FH = 1920, 1080
TWO_ROW_ASPECT = 2.5
MIN_ROI_H, MIN_ROI_W = 16, 32
PPOCR_PAD = 0.10
ACC = "/home/seeed/rk_deploy/accuracy"


# ---------------------------------------------------------------- ROI 复刻
def lround(x):
    # C++ lround: 四舍五入到最近整数(远离零方向)
    return int(np.floor(x + 0.5)) if x >= 0 else int(np.ceil(x - 0.5))


def clamp_roi(x, y, w, h, fw, fh, min_w=8, min_h=6):
    if w < 0:
        w = 0
    if h < 0:
        h = 0
    if x < 0:
        w += x
        x = 0
    if y < 0:
        h += y
        y = 0
    if x > fw:
        x = fw
    if y > fh:
        y = fh
    if x + w > fw:
        w = fw - x
    if y + h > fh:
        h = fh - y
    if w < min_w or h < min_h:
        return None
    return x, y, w, h


def roi_of_box(b, ratio, roi_y, height, shift, below, fw=FW, fh=FH,
               pad=PPOCR_PAD):
    """b = (x1,y1,x2,y2) 像素坐标; 返回 (x,y,w,h) 或 None"""
    x1, y1, x2, y2 = b
    bw = x2 - x1
    bh = y2 - y1
    if bw < 4 or bh < 4:
        return None
    w = bw * ratio
    h = bh * height
    if w < 8:
        w = 8.0
    if h < 6:
        h = 6.0
    cx = 0.5 * (x1 + x2) + bw * shift
    left = cx - w * 0.5
    top = y1 + bh * roi_y + bh * below
    if left < 0:
        w += left
        left = 0.0
    if top < 0:
        h += top
        top = 0.0
    if left + w > fw:
        w = fw - left
    if top + h > fh:
        h = fh - top
    if w < 8 or h < 6:
        return None
    rx, ry = lround(left), lround(top)
    rw, rh = lround(w), lround(h)
    r = clamp_roi(rx, ry, rw, rh, fw, fh, 8, 6)
    if r is None:
        return None
    rx, ry, rw, rh = r
    # ppocr_pad
    if pad > 0:
        dx = int(rw * pad)
        dy = int(rh * pad)
        rx -= dx
        ry -= dy
        rw += dx * 2
        rh += dy * 2
    r = clamp_roi(rx, ry, rw, rh, fw, fh, 8, 6)
    if r is None:
        return None
    rx, ry, rw, rh = r
    # 偶对齐
    rx &= ~1
    ry &= ~1
    rw &= ~1
    rh &= ~1
    r = clamp_roi(rx, ry, rw, rh, fw, fh, 8, 6)
    if r is None:
        return None
    rx, ry, rw, rh = r
    # 最小尺寸: 高度不足向上撑, 宽度不足向两侧撑
    if rh < MIN_ROI_H:
        need = MIN_ROI_H - rh
        ry -= need
        rh = MIN_ROI_H
        if ry < 0:
            rh += ry
            ry = 0
    if rw < MIN_ROI_W:
        need = MIN_ROI_W - rw
        rx -= need // 2
        rw = MIN_ROI_W
        if rx < 0:
            rw += rx
            rx = 0
    r = clamp_roi(rx, ry, rw, rh, fw, fh, 8, 6)
    return r


# ---------------------------------------------------------------- 数据装载
def load_boxes(path):
    """-> {frame_key: [(cls,score,x1,y1,x2,y2)]} 坐标归一化"""
    out = {}
    with open(path) as fh:
        for row in csv.reader(fh):
            if not row or row[0] == "img":
                continue
            n = int(row[1])
            bs = []
            for i in range(n):
                b = row[2 + 6 * i:2 + 6 * i + 6]
                bs.append((int(b[0]), float(b[1]), float(b[2]), float(b[3]),
                           float(b[4]), float(b[5])))
            out[row[0]] = bs
    return out


def load_gt(path):
    """plates.csv (有一列错位, 按尾部位置解析) -> {frame_idx: (x,y,w,h)}"""
    gt = {}
    with open(path) as fh:
        for row in csv.reader(fh):
            if not row or row[0] == "crop_path":
                continue
            x, y, w, h = (int(v) for v in row[-6:-2])
            gt[int(row[3])] = (x, y, w, h)
    return gt


def frame_key_to_idx(boxes):
    """'data/frames/f00042.jpg' -> 42"""
    k = {}
    for key in boxes:
        base = os.path.basename(key)
        if base.endswith(".jpg"):
            k[key] = int(base[1:6])
    return k


# ---------------------------------------------------------------- 扫描
def evaluate(boxes, gt, kmap, ratio, roi_y, height, shift, below):
    widths, heights = [], []
    tworow = 0
    n_valid = 0
    clamped = 0
    gt_widths = []      # 只统计【有 GT 的帧】上的 ROI 宽度(车牌帧才是有效总体)
    gt_effw = []        # 裁剪内【车牌文字行】的有效像素宽度(横向交集)
    gt_frac = []        # 车牌在裁剪里的占比(PP-OCR 缩到 320 宽, 占比低=字太小)
    gt_pxin = []        # ★ 车牌在【PP-OCR 输入(<=320x48)】里的有效像素宽 —— 真正的判据
                        #   crop_plate_roi: w = ceil(48*rw/rh), 上限 320
                        #   => 车牌在输入里的宽度 = eff_w * (w / rw)
    text_cov = 0        # 按【帧】计: 该帧至少有一个检测的 ROI 盖住了文字行
    full_cov = 0        # 按【帧】计: 该帧至少有一个 ROI 完全包住 GT 框
    n_gt_seen = 0       # 有 GT 且该帧有检测的帧数
    for key, bs in boxes.items():
        fi = kmap.get(key)
        g = gt.get(fi) if fi is not None else None
        frame_text = False
        frame_full = False
        frame_any = False
        for c, sc, x1, y1, x2, y2 in bs:
            r = roi_of_box((x1 * FW, y1 * FH, x2 * FW, y2 * FH),
                           ratio, roi_y, height, shift, below)
            if r is None:
                continue
            n_valid += 1
            frame_any = True
            rx, ry, rw, rh = r
            widths.append(rw)
            heights.append(rh)
            if g is not None:
                gt_widths.append(rw)
                gx, gy, gw, gh = g
                eff = max(0, min(rx + rw, gx + 0.98 * gw) - max(rx, gx + 0.02 * gw))
                gt_effw.append(int(eff))
                gt_frac.append(round(eff / max(1, rw), 4))
                out_w = min(320, int(np.ceil(48.0 * rw / max(1, rh))))
                gt_pxin.append(int(round(eff * (out_w / max(1, rw)))))
            if rw / max(rh, 1) < TWO_ROW_ASPECT:
                tworow += 1
            if rx <= 0 or ry <= 0 or rx + rw >= FW or ry + rh >= FH:
                clamped += 1
            if g is not None:
                gx, gy, gw, gh = g
                t_top, t_bot = gy + 0.10 * gh, gy + 0.90 * gh
                lft, rgt = gx + 0.02 * gw, gx + 0.98 * gw
                if ry <= t_top and ry + rh >= t_bot and rx <= lft and rx + rw >= rgt:
                    frame_text = True
                    if rx <= gx and ry <= gy and rx + rw >= gx + gw and ry + rh >= gy + gh:
                        frame_full = True
        if g is not None and frame_any:
            n_gt_seen += 1
            if frame_text:
                text_cov += 1
            if frame_full:
                full_cov += 1
    W = np.array(widths) if widths else np.array([0])
    H = np.array(heights) if heights else np.array([0])
    return {
        "ratio": ratio, "roi_y": roi_y, "roi_height": height,
        "roi_shift": shift, "roi_below": below,
        "n_rois": n_valid,
        "w_min": int(W.min()), "w_med": int(np.median(W)), "w_p90": int(np.percentile(W, 90)),
        "w_max": int(W.max()),
        "h_min": int(H.min()), "h_med": int(np.median(H)), "h_max": int(H.max()),
        "two_row_pct": round(100.0 * tworow / max(1, n_valid), 2),
        "clamped_pct": round(100.0 * clamped / max(1, n_valid), 2),
        "gt_frames": n_gt_seen,
        "gt_w_min": int(np.min(gt_widths)) if gt_widths else 0,
        "gt_w_med": int(np.median(gt_widths)) if gt_widths else 0,
        "gt_w_p90": int(np.percentile(gt_widths, 90)) if gt_widths else 0,
        "gt_w_max": int(np.max(gt_widths)) if gt_widths else 0,
        "eff_w_med": int(np.median(gt_effw)) if gt_effw else 0,
        "eff_w_min": int(np.min(gt_effw)) if gt_effw else 0,
        "plate_frac_med": round(float(np.median(gt_frac)), 4) if gt_frac else 0.0,
        "px_in_med": int(np.median(gt_pxin)) if gt_pxin else 0,
        "px_in_min": int(np.min(gt_pxin)) if gt_pxin else 0,
        "px_in_p25": int(np.percentile(gt_pxin, 25)) if gt_pxin else 0,
        "text_cov_pct": round(100.0 * text_cov / max(1, n_gt_seen), 2),
        "full_cov_pct": round(100.0 * full_cov / max(1, n_gt_seen), 2),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--boxes", required=True)
    ap.add_argument("--boxes2", default=None, help="对照臂(旧模型)的 boxes.csv")
    ap.add_argument("--gt", default=os.path.join(ACC, "data/manifests/plates.csv"))
    ap.add_argument("--out", required=True)
    ap.add_argument("--top", type=int, default=12)
    args = ap.parse_args()

    boxes = load_boxes(args.boxes)
    kmap = frame_key_to_idx(boxes)
    gt = load_gt(args.gt)
    print("boxes 帧数=%d, GT 帧数=%d" % (len(boxes), len(gt)))

    grid = {
        "ratio": [0.164, 0.20, 0.25, 0.30, 0.35, 0.40, 0.50, 0.60, 0.70, 0.80, 1.00],
        "roi_y": [0.25, 0.30, 0.35, 0.40, 0.45, 0.50, 0.55, 0.60, 0.70, 0.80, 0.90, 1.00],
        "roi_height": [0.10, 0.15, 0.20, 0.25, 0.30, 0.35, 0.40, 0.50],
        "roi_shift": [0.0, 0.05, 0.087, 0.126, 0.15, 0.20],
        "roi_below": [0.0, 0.02, 0.05],
    }
    rows = []
    n = 0
    for ratio in grid["ratio"]:
        for ry in grid["roi_y"]:
            for h in grid["roi_height"]:
                for sh in grid["roi_shift"]:
                    for be in grid["roi_below"]:
                        rows.append(evaluate(boxes, gt, kmap, ratio, ry, h, sh, be))
                        n += 1
    print("扫描组合数 = %d" % n)

    # 排序: 先看宽度门槛(>=160 中位), 再看文字行覆盖, 再看整块覆盖
    def key(r):
        # ① 文字行覆盖率(功能底线)
        # ② 车牌在 PP-OCR 输入里的像素宽 >= 60 (对照: GT 特写切分后是 ~64 px, 能读出来)
        # ③ 该像素宽越大越好
        return (-r["text_cov_pct"],
                0 if r["px_in_med"] >= 60 else 1,
                -r["px_in_med"],
                -r["eff_w_med"])

    rows.sort(key=key)
    fields = list(rows[0].keys())
    with open(args.out, "w", newline="") as fh:
        wr = csv.DictWriter(fh, fieldnames=fields)
        wr.writeheader()
        wr.writerows(rows)
    print("\n=== Top %d (宽度中位>=160 优先, 其次文字行覆盖) ===" % args.top)
    hdr = ("ratio", "roi_y", "h", "shift", "below", "px_in", "px_p25", "px_min",
           "eff_w", "GTw_med", "h_med", "2row%", "text_cov%", "full_cov%", "clamp%")
    print("  %-7s %-7s %-6s %-7s %-7s | %-6s %-7s %-7s | %-6s %-7s %-6s %-6s | %-9s %-9s %-7s" % hdr)
    for r in rows[:args.top]:
        print("  %-7s %-7s %-6s %-7s %-7s | %-6d %-7d %-7d | %-6d %-7d %-6d %-6.1f | %-9.2f %-9.2f %-7.2f"
              % (r["ratio"], r["roi_y"], r["roi_height"], r["roi_shift"], r["roi_below"],
                 r["px_in_med"], r["px_in_p25"], r["px_in_min"], r["eff_w_med"], r["gt_w_med"],
                 r["h_med"], r["two_row_pct"],
                 r["text_cov_pct"], r["full_cov_pct"], r["clamped_pct"]))
    # 现行参数基线
    base = [r for r in rows if r["ratio"] == 0.164 and r["roi_y"] == 1.0
            and r["roi_height"] == 0.40 and r["roi_shift"] == 0.126 and r["roi_below"] == 0]
    if base:
        b = base[0]
        print("\n=== 现行生产参数基线 (0.164/1.0/0.40/0.126/0) ===")
        print("  w_med=%d w_min=%d w_max=%d h_med=%d 2row%%=%.1f text_cov%%=%.2f full_cov%%=%.2f clamp%%=%.2f"
              % (b["w_med"], b["w_min"], b["w_max"], b["h_med"], b["two_row_pct"],
                 b["text_cov_pct"], b["full_cov_pct"], b["clamped_pct"]))
    print("\n完整结果 ->", os.path.abspath(args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
