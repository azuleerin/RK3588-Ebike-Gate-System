#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""smoke_compare.py —— 冒烟日志对比(新旧模型)+ 车牌识别硬门槛判定

输入: 两份 rk_gate 的 stderr 日志
输出: 帧数 / 各路 objs / [车牌] 行数 / 识别文本 / 框尺寸 / 裁剪尺寸 的逐项对比
      + 硬门槛判定(车牌识别率不得下降)
"""
import re
import sys
from collections import Counter

STAT = re.compile(r"\[(?P<ch>ch\d+)\]\[frame (?P<frame>\d+)\]\s+fps=(?P<fps>[\d.]+)")
PLATE = re.compile(
    r"\[(?P<ch>ch\d+)\] \[车牌\] track=(?P<track>\d+) cls=(?P<cls>\d+) "
    r"score=(?P<score>[\d.]+) box=(?P<bw>\d+)x(?P<bh>\d+)@\((?P<bx>-?\d+),(?P<by>-?\d+)\) "
    r"roi=(?P<rw>\d+)x(?P<rh>\d+)@\((?P<rx>-?\d+),(?P<ry>-?\d+)\) "
    r"裁剪=(?P<cw>\d+)x(?P<ch2>\d+)(?P<two> 两行)? -> \"(?P<text>.*)\"")
ERR = re.compile(r"(Segmentation fault|Aborted|terminate called|自检失败|\[警告\]|\[ERROR\])")


def parse(path):
    st = {"frames": {}, "plates": [], "errors": []}
    with open(path, errors="replace") as fh:
        for line in fh:
            m = STAT.search(line)
            if m:
                st["frames"][m.group("ch")] = int(m.group("frame"))
                continue
            m = PLATE.search(line)
            if m:
                st["plates"].append(m.groupdict())
                continue
            m = ERR.search(line)
            if m:
                st["errors"].append((m.group(1), line.strip()[:140]))
    return st


def summarise(tag, s):
    print("=" * 96)
    print("【%s】" % tag)
    print("   各路累计帧号: %s" % ", ".join("%s=%d" % (c, f) for c, f in sorted(st_frames(s).items())))
    print("   [车牌] 行数 = %d" % len(s["plates"]))
    if s["plates"]:
        per_ch = Counter(p["ch"] for p in s["plates"])
        print("   各路车牌行数: %s" % dict(sorted(per_ch.items())))
        texts = Counter(p["text"] for p in s["plates"])
        nonempty = sum(v for k, v in texts.items() if k.strip() not in ("", "-"))
        print("   识别文本分布(前 8): %s" % dict(texts.most_common(8)))
        print("   非空识别行数 = %d / %d" % (nonempty, len(s["plates"])))
        bw = [int(p["bw"]) for p in s["plates"]]
        bh = [int(p["bh"]) for p in s["plates"]]
        cw = [int(p["cw"]) for p in s["plates"]]
        if bw:
            print("   检测框尺寸: 宽 中位 %d (min %d, max %d) | 高 中位 %d (min %d, max %d)"
                  % (sorted(bw)[len(bw) // 2], min(bw), max(bw),
                     sorted(bh)[len(bh) // 2], min(bh), max(bh)))
            print("   车牌裁剪尺寸: 宽 中位 %d (min %d, max %d)"
                  % (sorted(cw)[len(cw) // 2], min(cw), max(cw)))
    if s["errors"]:
        print("   ⚠ 异常/告警 %d 条:" % len(s["errors"]))
        for e in s["errors"][:8]:
            print("      %s" % e[1])
    else:
        print("   异常/告警: 无")
    return s


def st_frames(s):
    return s["frames"]


def main():
    old = parse(sys.argv[1])
    new = parse(sys.argv[2])
    summarise("旧模型(生产模型)", old)
    summarise("新模型(fork9)", new)

    print("=" * 96)
    print("★ 硬门槛判定(按【人工-2】决议: 车牌识别对比是硬门槛)")
    o = old["plates"]
    n = new["plates"]

    def non_empty(ps):
        return sum(1 for p in ps if p["text"].strip() not in ("", "-"))

    o_ne, n_ne = non_empty(o), non_empty(n)
    o_rate = (o_ne / len(o)) if o else 0.0
    n_rate = (n_ne / len(n)) if n else 0.0
    print("   车牌行数: 旧 %d -> 新 %d" % (len(o), len(n)))
    print("   非空识别率: 旧 %.1f%% -> 新 %.1f%%" % (100 * o_rate, 100 * n_rate))
    ok = True
    if new["errors"]:
        print("   ✘ 新模型有异常/告警 %d 条 -> 不通过" % len(new["errors"]))
        ok = False
    if not old["frames"] or not new["frames"]:
        print("   ✘ 有日志没有帧号 -> 不通过")
        ok = False
    else:
        for ch in sorted(new["frames"]):
            if ch not in old["frames"]:
                print("   ✘ 缺通道 %s" % ch)
                ok = False
    # 识别率不得下降(允许 5 个百分点的绝对波动)
    if o_rate > 0 and n_rate < o_rate - 0.05:
        print("   ✘ 识别率下降超过 5 个百分点 -> 不通过(应回滚)")
        ok = False
    # 识别文本集合不得大幅缩水
    ot = set(p["text"] for p in o if p["text"].strip() not in ("", "-"))
    nt = set(p["text"] for p in n if p["text"].strip() not in ("", "-"))
    if ot and len(nt) < max(1, len(ot) // 2):
        print("   ✘ 识别出的车牌种类从 %d 掉到 %d -> 不通过(应回滚)" % (len(ot), len(nt)))
        ok = False
    print("   识别出的车牌文本: 旧 %d 种 -> 新 %d 种" % (len(ot), len(nt)))
    if ot and nt:
        print("   两边都识别到的: %s" % sorted(ot & nt)[:10])
        only_old = sorted(ot - nt)[:10]
        only_new = sorted(nt - ot)[:10]
        if only_old:
            print("   只有旧模型识别到: %s" % only_old)
        if only_new:
            print("   只有新模型识别到: %s" % only_new)
    print()
    print("==> 硬门槛: %s" % ("通过 ✅ 可以切生产" if ok else "不通过 ❌ 应回滚"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
