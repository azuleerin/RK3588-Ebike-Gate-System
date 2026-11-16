#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""proof_roi_fidelity.py —— 证明"大图检测 + 小图裁切"没有压缩车牌像素

要回答的疑问:
    "YOLO 把图缩到 640x640 才找框, 那送进 OCR 的车牌是不是已经被压过一遍了?"

实际架构(本工程):
    [1920x1080 原始帧] --RGA 缩放--> [640x640 检测图] --YOLO--> [检测框]
                                                       |
                                 框坐标回映射到原始帧 ---+
                                                       v
    [1920x1080 原始帧] --RGA 裁剪--> [车牌 ROI 原分辨率] --缩到 320x48--> [PP-OCR]

    ⇒ 送进 OCR 的是【原始帧里的原像素】, 检测图只是用来"找框"的, 不参与裁图。

对照(错误做法):
    直接在 640x640 检测图上裁 ROI -> 像素只剩 1/(scale^2), 再放大回 320x48 必然糊。

做法:
    1. 从视频抽一帧(原始分辨率)
    2. 复现检测路径: 整帧缩到 640x640
    3. 用【主程序日志里真实的 ROI 坐标】(原图坐标)裁两份:
         A = 从 640 检测图上裁   (错误做法)
         B = 从原始帧上裁       (本工程做法)
    4. 两份都缩到 320x48, 比较:
         - ROI 像素尺寸
         - 清晰度(拉普拉斯方差)
         - PP-OCR 实际读出的字
    5. 出一张并排对比图(PPT 素材)

用法:
    python3 scripts/proof_roi_fidelity.py \
        --video /home/seeed/testvideo3.mp4 --frame 120 \
        --log  /home/seeed/rk_deploy/logs/gate_4ch/ch1.log \
        --out  /home/seeed/rk_deploy/docs/assets/roi_fidelity_1080p.png
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

import cv2
import numpy as np

DET_SIZE = 640          # YOLO 检测输入边长
OCR_W, OCR_H = 320, 48  # PP-OCR rec 输入

# 主程序日志里的 [车牌] 行:  box=621x146@(50,434) roi=122x86@(378,576)
ROI_RE = re.compile(r"\[车牌\].*?roi=(\d+)x(\d+)@\((\d+),(\d+)\)")


def parse_roi_from_log(path, pick_text=None):
    """从主程序日志里取【它自己算出来的】ROI 矩形(原图坐标)

    pick_text: 只看读出这个字符串的那几行(比如 "AA981908"), 否则取"有非空读数"的最大 ROI
    """
    best = None
    fallback = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            m = ROI_RE.search(raw)
            if not m:
                continue
            w, h, x, y = (int(g) for g in m.groups())
            text = ""
            mt = re.search(r'-> "([^"]*)"', raw)
            if mt:
                text = mt.group(1)
            if pick_text:
                if pick_text in text and (best is None or w * h > best[0] * best[1]):
                    best = (w, h, x, y)
            elif text:
                if fallback is None or w * h > fallback[0] * fallback[1]:
                    fallback = (w, h, x, y)
    return best or fallback


def sharpness(img):
    """拉普拉斯方差 = 清晰度(越大越锐)"""
    g = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    return float(cv2.Laplacian(g, cv2.CV_64F).var())


def ocr_read(img_path, model, dict_path):
    """调主程序的单图识别工具跑一遍 PP-OCR"""
    cli = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "rec_plate_cli")
    cli = os.path.abspath(cli)
    if not os.path.exists(cli):
        return "(没有 rec_plate_cli)"
    try:
        r = subprocess.run(
            [cli, "--model", model, "--dict", dict_path, "--image", img_path],
            capture_output=True, text=True, timeout=60)
    except Exception as e:                                    # noqa: BLE001
        return f"(调用失败: {e})"
    m = re.search(r'"([^"]*)"\s+score=([0-9.]+)', r.stdout)
    if not m:
        return "(无输出)"
    return f'"{m.group(1)}" conf={m.group(2)}'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--video", required=True)
    ap.add_argument("--frame", type=int, default=120)
    ap.add_argument("--log", required=True, help="主程序日志, 用来取真实的 ROI 坐标")
    ap.add_argument("--out", required=True)
    ap.add_argument("--model", default="/home/seeed/rk_deploy/models/ppocrv4_rec_fp16.rknn")
    ap.add_argument("--dict", default="/home/seeed/rk_deploy/models/ppocr_keys_v1.txt")
    ap.add_argument("--title", default="")
    ap.add_argument("--pick-text", default=None, help="挑日志里读出这个字符串的那条 ROI")
    ap.add_argument("--scale", type=float, default=1.0,
                    help="把抽出来的帧整体放大这么多倍(模拟 1080P 输入); ROI 坐标同步缩放")
    args = ap.parse_args()

    roi = parse_roi_from_log(args.log, args.pick_text)
    if not roi:
        print("日志里没找到 ROI 坐标", file=sys.stderr)
        return 1
    rw, rh, rx, ry = roi

    # ---- 抽原始帧 ----
    cap = cv2.VideoCapture(args.video)
    cap.set(cv2.CAP_PROP_POS_FRAMES, args.frame)
    ok, full = cap.read()
    cap.release()
    if not ok:
        print("抽帧失败", file=sys.stderr)
        return 1
    H, W = full.shape[:2]
    if args.scale != 1.0:
        full = cv2.resize(full, (int(W * args.scale), int(H * args.scale)),
                          interpolation=cv2.INTER_CUBIC)
        rw, rh = int(rw * args.scale), int(rh * args.scale)
        rx, ry = int(rx * args.scale), int(ry * args.scale)
        H, W = full.shape[:2]
    print(f"原始帧 {W}x{H}   主程序给的 ROI = {rw}x{rh} @ ({rx},{ry})  (原图坐标)")

    # ---- 检测路径: 整帧缩到 640 ----
    det = cv2.resize(full, (DET_SIZE, DET_SIZE), interpolation=cv2.INTER_LINEAR)

    # ---- A: 错误做法 —— 从 640 检测图上裁 ----
    sx, sy = DET_SIZE / W, DET_SIZE / H
    ax, ay = int(rx * sx), int(ry * sy)
    aw, ah = max(1, int(rw * sx)), max(1, int(rh * sy))
    aw = min(aw, DET_SIZE - ax)
    ah = min(ah, DET_SIZE - ay)
    roiA = det[ay:ay + ah, ax:ax + aw]

    # ---- B: 本工程做法 —— 从原始帧上裁 ----
    bx, by = max(0, rx), max(0, ry)
    bw, bh = min(rw, W - bx), min(rh, H - by)
    roiB = full[by:by + bh, bx:bx + bw]

    # ---- 两行牌: 和主程序一样, 上下切开各读一次再拼 ----
    # (ROI 宽高比 < 2.5 时走这条路, 切分点 0.45)
    row_split = 0.45
    two_row = (bw / max(bh, 1)) < 2.5
    if two_row:
        cut = int(bh * row_split)
        rowsA = (roiA[:cut], roiA[cut:])
        rowsB = (roiB[:cut], roiB[cut:])
    else:
        rowsA = (roiA,)
        rowsB = (roiB,)

    def to_ocr(r):
        return cv2.resize(r, (OCR_W, OCR_H), interpolation=cv2.INTER_LINEAR)

    # 真正送进 PP-OCR 的那张: 单行牌 = 整块; 两行牌 = 拼(这里只展示下行, 上行另测)
    roiA, roiB = to_ocr(roiA), to_ocr(roiB)
    rowA = to_ocr(rowsA[-1]) if len(rowsA) > 1 else None
    rowB = to_ocr(rowsB[-1]) if len(rowsB) > 1 else None

    # ---- 落盘供 OCR 对比 ----
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    d = os.path.dirname(os.path.abspath(args.out))
    # 对比对象 = 真正送进 PP-OCR 的那一张(两行牌 -> 取下行单行)
    cmpA = rowA if rowA is not None else roiA
    cmpB = rowB if rowB is not None else roiB
    tmpA, tmpB = f"{d}/_roiA.jpg", f"{d}/_roiB.jpg"
    cv2.imwrite(tmpA, cmpA)
    cv2.imwrite(tmpB, cmpB)
    readA = ocr_read(tmpA, args.model, args.dict)
    readB = ocr_read(tmpB, args.model, args.dict)
    # 整块(两行牌)的读数也留一份, 证明"为什么必须切开"
    readFullA = readFullB = "-"
    if rowA is not None:
        cv2.imwrite(f"{d}/_roiA_full.jpg", roiA)
        cv2.imwrite(f"{d}/_roiB_full.jpg", roiB)
        readFullA = ocr_read(f"{d}/_roiA_full.jpg", args.model, args.dict)
        readFullB = ocr_read(f"{d}/_roiB_full.jpg", args.model, args.dict)

    shA, shB = sharpness(cmpA), sharpness(cmpB)
    pxA, pxB = aw * ah, bw * bh

    print()
    if two_row:
        print(f"两行牌: ROI 宽高比 {bw/max(bh,1):.2f} < 2.5 -> 主程序上下切开, "
              f"下面比的是【下行单行】(真正送进 PP-OCR 的那张)")
        print()
    print(f"{'':<22}{'A: 从640检测图裁':>18}{'B: 从原始帧裁':>18}")
    print("-" * 58)
    print(f"{'整块 ROI 像素尺寸':<22}{f'{aw}x{ah} ({pxA}px)':>18}{f'{bw}x{bh} ({pxB}px)':>18}")
    print(f"{'有效像素数比':<22}{'1.0x':>18}{f'{pxB/max(pxA,1):.1f}x':>18}")
    print(f"{'送进 OCR 的那行清晰度':<22}{f'{shA:8.1f}':>18}{f'{shB:8.1f}':>18}")
    print(f"{'清晰度比':<22}{'1.0x':>18}{f'{shB/max(shA,1e-6):.2f}x':>18}")
    print(f"{'PP-OCR(单行) 读出':<22}{readA[:18]:>18}{readB[:18]:>18}")
    if two_row:
        print(f"{'PP-OCR(整块未切)':<22}{readFullA[:18]:>18}{readFullB[:18]:>18}")
    print()
    print(f"像素预算: 原始帧 {W}x{H} -> 检测图 {DET_SIZE}x{DET_SIZE}, 缩放 {sx:.3f}x")
    print(f"  ⇒ 同一块车牌, 从检测图上裁只剩 {sx*sx*100:.1f}% 的像素; "
          f"从原图裁是 100%")

    # ---- 并排对比图 ----
    def tile(img, title, sub, color):
        canvas = np.full((OCR_H + 64, 420, 3), 24, np.uint8)
        big = cv2.resize(img, (420, int(OCR_H * 420 / OCR_W)), interpolation=cv2.INTER_CUBIC)
        canvas[40:40 + big.shape[0], 0:420] = big
        cv2.putText(canvas, title, (8, 20), cv2.FONT_HERSHEY_SIMPLEX, 0.5, color, 1, cv2.LINE_AA)
        cv2.putText(canvas, sub, (8, 36), cv2.FONT_HERSHEY_SIMPLEX, 0.38, (200, 200, 200), 1, cv2.LINE_AA)
        return canvas

    label = "bottom row (what PP-OCR sees)" if two_row else "plate ROI (what PP-OCR sees)"
    tA = tile(cmpA, f"A  WRONG: crop from 640x640 detection image",
              f"ROI {aw}x{ah}px  sharpness {shA:.0f}  OCR {readA}", (80, 80, 235))
    tB = tile(cmpB, f"B  OURS: crop from ORIGINAL full-res frame",
              f"ROI {bw}x{bh}px  sharpness {shB:.0f}  OCR {readB}", (110, 210, 110))

    header = np.full((78, 840, 3), 24, np.uint8)
    title = args.title or f"ROI fidelity proof  ({W}x{H} source)"
    cv2.putText(header, title, (10, 26), cv2.FONT_HERSHEY_SIMPLEX, 0.62, (240, 240, 240), 1, cv2.LINE_AA)
    cv2.putText(header,
                f"pipeline: full-res {W}x{H}  --RGA scale-->  640x640  --YOLO-->  box  --back to full-res-->  RGA crop  -->  PP-OCR 320x48",
                (10, 48), cv2.FONT_HERSHEY_SIMPLEX, 0.42, (150, 190, 230), 1, cv2.LINE_AA)
    cv2.putText(header,
                f"pixels handed to OCR:  A {pxA}px (sharpness {shA:.0f})   vs   B {pxB}px (sharpness {shB:.0f})   -> {pxB/max(pxA,1):.1f}x pixels, {shB/max(shA,1e-6):.1f}x sharper",
                (10, 66), cv2.FONT_HERSHEY_SIMPLEX, 0.42, (150, 230, 170), 1, cv2.LINE_AA)

    out = np.vstack([header, np.hstack([tA, tB])])
    cv2.imwrite(args.out, out, [cv2.IMWRITE_JPEG_QUALITY, 95])
    print(f"\n对比图已保存: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
