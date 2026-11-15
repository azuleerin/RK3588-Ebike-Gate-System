#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""npu_ab_report.py —— 汇总 NPU A/B/C 实测数据

从每组的主程序日志 + MQTT 负载抓包里抽出:
  帧数 / FPS / NPU 耗时 / rknn_run / 检出目标 / 车牌识别次数 / 缓存命中率
  rknnp 三个核的真实占用(如果板上有 root 负载采样器在发)

用法:
    python3 scripts/npu_ab_report.py <输出目录>
"""
from __future__ import annotations

import json
import os
import re
import statistics
import sys
from collections import Counter

RE_FRAME = re.compile(
    r"^\[frame (\d+)\].*fps=([\d.]+).*dec=([\d.]+)ms rga=([\d.]+)ms "
    r"npu=([\d.]+)ms\(run([\d.]+)/out([\d.]+)\) post=([\d.]+)ms .*total=([\d.]+)ms "
    r"objs=(\d+)")
RE_PLATE = re.compile(r"^\[车牌\] .*-> (.*)$")


RE_STATS = re.compile(r"交付 FPS\s*:\s*([\d.]+)")
RE_SRC = re.compile(r"相机推送 FPS\s*:\s*([\d.]+)")
RE_TS = re.compile(r"\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\.")


def parse_log(path):
    frames, plates = [], []
    deliver_fps, src_fps = None, None
    ts_first = ts_last = None
    if not os.path.exists(path):
        return frames, plates
    with open(path, errors="replace") as f:
        for line in f:
            m = RE_FRAME.match(line)
            if m:
                frames.append({
                    "frame": int(m.group(1)), "fps": float(m.group(2)),
                    "dec": float(m.group(3)), "rga": float(m.group(4)),
                    "npu": float(m.group(5)), "run": float(m.group(6)),
                    "total": float(m.group(9)), "objs": int(m.group(10)),
                })
                continue
            m = RE_PLATE.match(line.rstrip("\n"))
            if m:
                plates.append(m.group(1).strip())
            m = RE_STATS.search(line)
            if m:
                deliver_fps = float(m.group(1))
            m = RE_SRC.search(line)
            if m:
                src_fps = float(m.group(1))
            m = RE_TS.search(line)
            if m:
                if ts_first is None:
                    ts_first = m.group(1)
                ts_last = m.group(1)
    wall_fps = None
    if ts_first and ts_last:
        from datetime import datetime
        try:
            t0 = datetime.strptime(ts_first, "%Y-%m-%d %H:%M:%S")
            t1 = datetime.strptime(ts_last, "%Y-%m-%d %H:%M:%S")
            dur = (t1 - t0).total_seconds()
            if dur > 0 and frames:
                wall_fps = frames[-1]["frame"] / dur
        except Exception:
            pass
    return frames, plates, deliver_fps, src_fps, wall_fps


def parse_load(path):
    out = []
    if not os.path.exists(path):
        return out
    with open(path, errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            p = d.get("payload")
            if isinstance(p, str):
                try:
                    p = json.loads(p)
                except json.JSONDecodeError:
                    continue
            if isinstance(p, dict) and "npu" in p:
                out.append(p)
    return out


def core_hist(load):
    """负载报文里的 npu_core = 三个核里的【最大值】(采样器就是这么算的)

    注意: 它是标量不是数组 —— 早先按"字符串/数组"解析, 结果整列被判成"无数据"。
    """
    hist = []
    for m in load:
        c = m.get("npu_core")
        if c is None:
            continue
        if isinstance(c, str):
            nums = [float(x) for x in re.findall(r"(\d+(?:\.\d+)?)", c)]
            if nums:
                hist.append(nums)
        elif isinstance(c, (list, tuple)):
            hist.append([float(x) for x in c])
        else:
            hist.append([float(c)])          # 标量 = 三核最大值
    return hist


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "logs/npu_ab"
    groups = [
        ("A", "基线 TTL2s / 单实例"),
        ("B", "组合 TTL5s / 双实例 / 识别绑Core2"),
        ("C", "单项 TTL5s / 单实例"),
        ("D", "组合 + INT8 识别模型"),
    ]
    rows = []
    for name, desc in groups:
        log = os.path.join(out_dir, f"{name}.log")
        load = parse_load(os.path.join(out_dir, f"{name}_load.jsonl"))
        frames, plates, deliver_fps, src_fps, wall_fps = parse_log(log)
        if not frames:
            print(f"{name}: 没有日志")
            continue
        last = frames[-1]
        nframes = last["frame"]
        fps = statistics.mean([f["fps"] for f in frames])
        npu = statistics.mean([f["npu"] for f in frames])
        run = statistics.mean([f["run"] for f in frames])
        total = statistics.mean([f["total"] for f in frames])
        objs = statistics.mean([f["objs"] for f in frames])
        cached = sum(1 for p in plates if "(缓存)" in p)
        real = len(plates) - cached
        ch = core_hist(load)
        percore, percore_peak = [], 0.0
        if ch:
            n = max(len(c) for c in ch)
            for i in range(n):
                vals = [c[i] for c in ch if len(c) > i and c[i] >= 0]
                if vals:
                    percore.append(statistics.mean(vals))
                    percore_peak = max(percore_peak, max(vals))
        rows.append({
            "name": name, "desc": desc, "frames": nframes, "fps": fps,
            "npu": npu, "run": run, "total": total, "objs": objs,
            "plate_total": len(plates), "plate_real": real,
            "hit": (cached * 100.0 / len(plates)) if plates else 0.0,
            "percore": percore, "percore_peak": percore_peak, "nsrc": len(ch),
            "deliver": deliver_fps, "src": src_fps, "wall": wall_fps,
        })

    if not rows:
        print("没有可汇总的数据")
        return 1

    print()
    print("| 组 | 配置 | 帧数 | 交付FPS(墙钟) | 源推送FPS | NPU ms/帧 | rknn_run | 单帧总耗时 | 检出 | 真实识别次数 | 缓存命中 | 最忙NPU核 |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in rows:
        pc = (f"{r['percore'][0]:.1f}% (峰 {r['percore_peak']:.0f}%)" if r["percore"]
              else "(无)")
        wall = f" (墙钟 {r['wall']:.2f})" if r.get("wall") else ""
        print(f"| **{r['name']}** | {r['desc']} | {r['frames']} | "
              f"**{r['deliver'] if r['deliver'] else r['fps']:.2f}**{wall} | "
              f"{r['src'] if r['src'] else '-'} | "
              f"**{r['npu']:.2f}** | {r['run']:.2f} | {r['total']:.2f} | {r['objs']:.1f} | "
              f"{r['plate_real']} | {r['hit']:.0f}% | {pc} |")

    base = next((r for r in rows if r["name"] == "A"), None)
    if base:
        print()
        for r in rows:
            if r["name"] == "A":
                continue
            dl = (r['deliver'] or r['fps']) - (base['deliver'] or base['fps'])
            print(f"**{r['name']} vs A**: 交付FPS {dl:+.2f} | "
                  f"NPU {r['npu'] - base['npu']:+.2f} ms | "
                  f"单帧总耗时 {r['total'] - base['total']:+.2f} ms | "
                  f"真实识别次数 {r['plate_real'] - base['plate_real']:+d}"
                  + (f" | 最忙核 {(r['percore'][0] - base['percore'][0]):+.1f}pp"
                     if r['percore'] and base['percore'] else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
