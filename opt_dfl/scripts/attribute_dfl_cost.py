#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""attribute_dfl_cost.py —— 从 audit JSON 里精确归因「DFL 解码」的算子耗时

Arm A 里我插入的解码节点都带 s0/ s1/ s2/ 前缀, 外加 4 个收尾节点
(ConcatBox / ConcatCls / TransposeBoxCHW / TransposeClsCHW),
所以可以直接按名字把它们挑出来求和 —— 这就是「DFL 解码留在 NPU 里」的净成本。

用法: attribute_dfl_cost.py <audit_armA.json> [<audit_armB.json>]
"""
import json
import re
import sys

MY_RE = re.compile(r"(s[0-2]/|Concat(Box|Cls)$|Transpose(Box|Cls)CHW$)")
# Arm B 里没有我的节点; 对照组用这些"同位置"的算子做参照


def load(path):
    d = json.load(open(path))
    rt = d["runtime"]
    return rt


def summarize(rt, pick):
    tot_us = 0.0
    tot_npu = 0.0
    tot_cpu = 0.0
    rows = []
    for op in rt["ops"]:
        name = op.get("fullname", "")
        if not pick(name):
            continue
        t = op.get("time_us", 0) or op.get("us", 0)
        tgt = op.get("target", "")
        rows.append((name, op.get("op_type", ""), tgt, t))
        tot_us += t
        if tgt == "NPU":
            tot_npu += t
        else:
            tot_cpu += t
    return tot_us, tot_npu, tot_cpu, rows


def main():
    a = load(sys.argv[1])
    ta, na, ca, rows = summarize(a, lambda n: bool(MY_RE.search(n)))
    print("=" * 72)
    print("Arm A —— 「图内 DFL 解码」相关算子的耗时 (按节点名挑出来)")
    print("=" * 72)
    for name, ot, tgt, t in sorted(rows, key=lambda r: -r[3]):
        print(f"  {tgt:4s} {ot:14s} {t:6d} us   {name[:58]}")
    print(f"  {'合计':4s} {'':14s} {ta:6.0f} us  (NPU {na:.0f} / CPU {ca:.0f})")
    print(f"  占 Arm A 单次推理 {a['driver_run_us']} us 的 {ta / a['driver_run_us'] * 100:.1f}%")

    if len(sys.argv) > 2:
        b = load(sys.argv[2])
        tb, nb, cb, _ = summarize(b, lambda n: False)
        print()
        print("=" * 72)
        print("两臂对比")
        print("=" * 72)
        print(f"  Arm A (DFL 在 NPU) 单次推理 : {a['driver_run_us']:6d} us  "
              f"({1000000 / a['driver_run_us']:.2f} FPS)")
        print(f"  Arm B (DFL 在 CPU) 单次推理 : {b['driver_run_us']:6d} us  "
              f"({1000000 / b['driver_run_us']:.2f} FPS)")
        print(f"  Δ(单次推理)                  : {a['driver_run_us'] - b['driver_run_us']:6d} us "
              f"= {(a['driver_run_us'] - b['driver_run_us']) / 1000:.2f} ms")
        print(f"  Arm A 算子数 {len(a['ops'])} / 内部内存 {a['internal_mb']:.2f} MB")
        print(f"  Arm B 算子数 {len(b['ops'])} / 内部内存 {b['internal_mb']:.2f} MB")


if __name__ == "__main__":
    main()
