#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""longrun_from_log.py —— 从生产活日志算【帧计数器差分】总吞吐 + detwait/ocrwait 统计

★ 口径（任务书 §4.1）:
#   吞吐 = 【帧计数器差分】, 禁用 deliv
#   差分窗口 = 进程 ExecMainStartTimestamp -> ExecMainExitTimestamp（>=300 s）
#   detwait / ocrwait 取全部统计行的分位数
"""
import re
import subprocess
import sys
from collections import defaultdict

LOG = sys.argv[1]
START = sys.argv[2]      # 'YYYY-MM-DD HH:MM:SS'
END = sys.argv[3]

LINE = re.compile(
    r"^\[(ch\d)\]\[frame (\d+)\]\s+fps=([0-9.]+)\s+deliv=([0-9.]+).*?"
    r"npu=([0-9.]+)ms\s+detwait=([0-9.]+)ms\s+ocrwait=([0-9.]+)ms")

first = {}
last = {}
detwait = defaultdict(list)
ocrwait = defaultdict(list)
npu_ms = defaultdict(list)
fps = defaultdict(list)
deliv = defaultdict(list)
n = 0

for raw in open(LOG, encoding="utf-8", errors="replace"):
    m = LINE.match(raw)
    if not m:
        continue
    n += 1
    ch = m.group(1)
    fr = int(m.group(2))
    first.setdefault(ch, fr)
    last[ch] = fr
    fps[ch].append(float(m.group(3)))
    deliv[ch].append(float(m.group(4)))
    npu_ms[ch].append(float(m.group(5)))
    detwait[ch].append(float(m.group(6)))
    ocrwait[ch].append(float(m.group(7)))


def pct(v, p):
    s = sorted(v)
    if not s:
        return float("nan")
    k = (len(s) - 1) * p / 100.0
    lo = int(k); hi = min(int(k) + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def fmt_ts(ts):
    return subprocess.run(["date", "-d", "@%d" % int(ts), "+%F %T"],
                          capture_output=True, text=True).stdout.strip()


start_ep = int(subprocess.run(["date", "-d", "%s" % START, "+%s"],
                              capture_output=True, text=True).stdout.strip())
end_ep = int(subprocess.run(["date", "-d", "%s" % END, "+%s"],
                            capture_output=True, text=True).stdout.strip())
dt = end_ep - start_ep

print("=" * 74)
print(" 生产长跑 —— 帧计数器差分总吞吐（口径: 任务书 §4.1, 禁用 deliv）")
print("=" * 74)
print(f" 窗口   : {START}  ->  {END}   = {dt} s = {dt/3600:.2f} h"
      f"   (门槛: 差分间隔 >= 300 s)")
print(f" 统计行 : {n} 行")
print()
tot = 0
for ch in ("ch1", "ch2", "ch3", "ch4"):
    if ch not in last:
        continue
    d = last[ch] - first.get(ch, 0)
    tot += d
    print(f"   {ch}: 帧号 {first.get(ch,0):>7} -> {last[ch]:<7}  Δ={d:>7}  "
          f"= {d/dt*60:>7.2f} 帧/min   ({d/dt:.2f} FPS)")
print()
print(f"   >>> 总吞吐 = {tot} 帧 / {dt} s = {tot/dt:.2f} FPS   "
      f"门槛 >= 79.5  => {'PASS' if tot/dt >= 79.5 else 'FAIL'}")
print()
print("   对照(仅供理解 deliv 为什么不能用): "
      f"4 路 deliv 平均 {sum(sum(deliv[c])/len(deliv[c]) for c in deliv)/len(deliv):.1f} "
      f"= 限帧后的交付数, 不是处理吞吐")
print()
print("=" * 74)
print(" detwait / ocrwait / npu 分位数（全部统计行）")
print("=" * 74)
for ch in ("ch1", "ch2", "ch3", "ch4"):
    if ch not in detwait:
        continue
    dw = detwait[ch]
    print(f"   {ch} detwait: 中位 {pct(dw,50):.2f} ms  p90 {pct(dw,90):.2f}  "
          f"max {max(dw):.2f}   (目标 ~20-24 ms)")
print()
for ch in ("ch1", "ch2", "ch3", "ch4"):
    if ch not in npu_ms:
        continue
    v = npu_ms[ch]
    print(f"   {ch} npu    : 中位 {pct(v,50):.2f} ms  p90 {pct(v,90):.2f}  max {max(v):.2f}")
print()
for ch in ("ch1", "ch2", "ch3", "ch4"):
    if ch not in fps:
        continue
    v = fps[ch]
    print(f"   {ch} fps    : 中位 {pct(v,50):.2f}   src 上限见日志")
