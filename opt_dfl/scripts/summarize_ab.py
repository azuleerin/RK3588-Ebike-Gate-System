#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""summarize_ab.py —— 把 A/B 的 json 汇总成表 (取中位数)

用法: summarize_ab.py <ab_dir> [max_fps]
"""
import glob
import json
import os
import re
import statistics
import sys


def main():
    d = sys.argv[1]
    fps_tag = sys.argv[2] if len(sys.argv) > 2 else None

    groups = {}
    for p in sorted(glob.glob(os.path.join(d, "*.json"))):
        base = os.path.basename(p)[:-5]
        m = re.match(r"^(\w+?)_r(\d+)_f(\d+)$", base)
        if not m:
            continue
        arm, rnd, fps = m.group(1), int(m.group(2)), m.group(3)
        if fps_tag and fps != fps_tag:
            continue
        groups.setdefault(arm, []).append(json.load(open(p)))

    order = [a for a in ("prod", "armA", "armB") if a in groups]
    names = {"prod": "生产 ebike(形态C,nc=2)",
             "armA": "yolov8n DFL在NPU(形态C,nc=80)",
             "armB": "yolov8n fork9 DFL在CPU(形态B)"}

    print("=" * 104)
    print(f"A/B 汇总  (max-fps={fps_tag or '全部'}   中位数)")
    print("=" * 104)
    print(f"{'臂':<34} {'轮数':>4} {'总FPS':>9} {'中位FPS':>9} "
          f"{'npu ms':>9} {'detwait':>9} {'ocrwait':>9} {'NPU核%':>22}")

    for arm in order:
        rs = groups[arm]
        tot = [r["total_fps"] for r in rs]
        npu, det, ocr, cores = [], [], [], []
        for r in rs:
            ch = r.get("channels", {})
            if ch:
                npu.append(statistics.mean(v["npu_ms"] for v in ch.values()))
                det.append(statistics.mean(v["detwait_ms"] for v in ch.values()))
                o = [v["ocrwait_ms"] for v in ch.values() if v["ocrwait_ms"] > 0]
                ocr.append(statistics.mean(o) if o else 0.0)
            if "npu_core_pct" in r:
                cores.append(statistics.mean(r["npu_core_pct"].values()))
        cstr = ""
        if cores:
            cstr = "/".join(f"{c:.0f}" for c in sorted(
                statistics.median([r["npu_core_pct"][k] for r in rs if "npu_core_pct" in r]
                                  for k in r["npu_core_pct"])))
        print(f"{names.get(arm, arm):<34} {len(rs):>4} {statistics.mean(tot):>9.2f} "
              f"{statistics.median(tot):>9.2f} "
              f"{statistics.median(npu) if npu else 0:>9.2f} "
              f"{statistics.median(det) if det else 0:>9.2f} "
              f"{statistics.median(ocr) if ocr else 0:>9.2f} {cstr:>22}")

    # Δ
    if "armA" in groups and "armB" in groups:
        a = statistics.median([r["total_fps"] for r in groups["armA"]])
        b = statistics.median([r["total_fps"] for r in groups["armB"]])
        an = statistics.median([statistics.mean(v["npu_ms"] for v in r["channels"].values())
                                for r in groups["armA"]])
        bn = statistics.median([statistics.mean(v["npu_ms"] for v in r["channels"].values())
                                for r in groups["armB"]])
        print()
        print(f"Δ(armA → armB)  总FPS {a:.2f} → {b:.2f}  ({(b - a) / a * 100:+.1f}%)")
        print(f"                单帧NPU {an:.2f}ms → {bn:.2f}ms  (Δ = {an - bn:+.2f} ms)")
    if "prod" in groups and "armB" in groups:
        p = statistics.median([r["total_fps"] for r in groups["prod"]])
        b = statistics.median([r["total_fps"] for r in groups["armB"]])
        pn = statistics.median([statistics.mean(v["npu_ms"] for v in r["channels"].values())
                                for r in groups["prod"]])
        bn = statistics.median([statistics.mean(v["npu_ms"] for v in r["channels"].values())
                                for r in groups["armB"]])
        print(f"Δ(prod → armB)  总FPS {p:.2f} → {b:.2f}  ({(b - p) / p * 100:+.1f}%)")
        print(f"                单帧NPU {pn:.2f}ms → {bn:.2f}ms  (Δ = {pn - bn:+.2f} ms)")
        print("                注: prod 是 2 类 ebike 模型, armB 是 80 类 yolov8n —— "
              "cls 头和 OCR 触发量不同, 总FPS 不可直接比; 单帧 NPU 时间才是同口径.")


if __name__ == "__main__":
    main()
