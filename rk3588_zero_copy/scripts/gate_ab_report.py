#!/usr/bin/env python3
# gate_ab_report.py —— 4 路大门前后对比: 4 进程 vs 单进程+全局线程池
#
# 用法:
#   scripts/gate_ab_report.py [--base logs/gate_4ch_base] [--new logs/gate_4ch_single]
#
# 两边的 load.csv 都是【同一个采样器】写的, 列含义:
#   基线(4 进程): t,npu0,npu1,npu2,vpu,rga,cpu_pct,rss1,rss2,rss3,rss4,fps1,fps2,fps3,fps4
#   新版(1 进程): t,npu0,npu1,npu2,vpu,rga,cpu_pct,rss_mb,fps1,fps2,fps3,fps4
#
# 输出: 每核 NPU 占用 / CPU / RSS / 各路 FPS 的前后对照表 + 结论。
import argparse
import csv
import os
import re
import sys


def load_csv(path, n_fps_col=4):
    """返回 (rows, 采样点说明)。行是 [float...]。"""
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path) as f:
        for r in csv.reader(f):
            if len(r) < 7 or not r[0].strip().isdigit():
                continue
            try:
                rows.append([float(x) for x in r])
            except ValueError:
                pass
    return rows


def stable(rows, frac=0.15):
    """去掉首尾各 frac —— 启动爬坡和收尾不算稳态。"""
    if not rows:
        return rows
    k = max(1, int(len(rows) * frac))
    return rows[k:len(rows) - k] or rows


def avg(rows, i):
    return sum(x[i] for x in rows) / max(len(rows), 1)


def mx(rows, i):
    return max((x[i] for x in rows), default=0.0)


def pct(rows, i, p):
    if not rows:
        return 0.0
    v = sorted(x[i] for x in rows)
    k = min(len(v) - 1, max(0, int(round(p * (len(v) - 1)))))
    return v[k]


def core_stat_from_log(path):
    """从程序日志里抓【应用内自测】的每核占用(如果有)。"""
    if not os.path.exists(path):
        return []
    out = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.search(r"(识别|检测)\s+mask=0x([0-9a-f]+)\s+任务\s+(\d+)\s+占用\s+([\d.]+)%", line)
            if m:
                out.append((m.group(1), int(m.group(2), 16), int(m.group(3)), float(m.group(4))))
    return out


def plates_from_log(path, tag):
    if not os.path.exists(path):
        return 0
    with open(path, encoding="utf-8", errors="replace") as f:
        txt = f.read()
    if tag == "base":
        return len([p for p in re.findall(r'-> "([^"]*)"', txt) if p])
    return len(re.findall(r'\[车牌\]', txt))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default="/home/seeed/rk_deploy/logs/gate_4ch_base")
    ap.add_argument("--new", default="/home/seeed/rk_deploy/logs/gate_4ch_single")
    args = ap.parse_args()

    base_rows = load_csv(os.path.join(args.base, "load.csv"))
    new_rows = load_csv(os.path.join(args.new, "load.csv"))
    if not base_rows or not new_rows:
        print(f"采样数据缺失: base={len(base_rows)} 行 new={len(new_rows)} 行")
        return 1

    b = stable(base_rows)
    n = stable(new_rows)

    print("=" * 78)
    print(" 4 路大门: 4 进程  vs  单进程 + 全局 NPU 线程池")
    print("=" * 78)
    print(f" 稳态采样点: 基线 {len(b)} 个 / 新版 {len(n)} 个\n")

    def line(label, bv, nv, unit="%", better="up"):
        if better == "up":
            arrow = "↑" if nv > bv else ("↓" if nv < bv else "=")
        else:
            arrow = "↓" if nv < bv else ("↑" if nv > bv else "=")
        print(f"  {label:<26}{bv:>9.1f}{unit}  ->{nv:>9.1f}{unit}   {arrow}")

    print("── NPU 三核负载 (debugfs, 驱动侧) ──")
    for i, name in ((1, "Core0"), (2, "Core1"), (3, "Core2")):
        line(f"NPU {name} 平均", avg(b, i), avg(n, i))
    print()
    print("── 其余硬件 / CPU ──")
    line("VPU 解码", avg(b, 4), avg(n, 4))
    line("RGA", avg(b, 5), avg(n, 5))
    line("CPU 总占用", avg(b, 6), avg(n, 6))
    # RSS: 基线是 4 路之和, 新版是单进程
    b_rss = sum(avg(b, i) for i in (7, 8, 9, 10))
    n_rss = avg(n, 7)
    line("RSS 合计 (MB)", b_rss, n_rss, unit=" ", better="down")
    print()

    print("── 各路 FPS ──")
    fps_names = ("ch1 近景/车牌", "ch2 高位/测速", "ch3 近景/车牌", "ch4 高位/测速")
    for k, name in enumerate(fps_names):
        bi = 11 + k if len(b[0]) > 12 else 8 + k
        ni = 8 + k
        line(f"{name}", avg(b, bi), avg(n, ni), unit=" ", better="up")
    print()
    print("  各路 FPS 分位数(新版):")
    for k, name in enumerate(fps_names):
        ni = 8 + k
        print(f"    {name:<18} P10={pct(n, ni, 0.10):>6.2f}  P50={pct(n, ni, 0.50):>6.2f}  "
              f"P90={pct(n, ni, 0.90):>6.2f}  min={min((x[ni] for x in n), default=0):>6.2f}")

    # ---- 应用内自测的每核占用 ----
    cs = core_stat_from_log(os.path.join(args.new, "gate.log"))
    if cs:
        print("\n── 应用内自测每核占用(该核 rknn_run 耗时 / 墙钟)──")
        for kind, mask, tasks, pctv in cs:
            print(f"    {kind}  mask=0x{mask:x}  任务 {tasks:>7}  占用 {pctv:>6.2f}%")
        print("    (对照命令, 需 root: sudo cat /sys/kernel/debug/rknpu/load)")

    # ---- 车牌识别次数 ----
    bp = plates_from_log(os.path.join(args.base, "ch1.log"), "base")
    np_ = plates_from_log(os.path.join(args.new, "gate.log"), "new")
    if bp or np_:
        print(f"\n── 车牌识别次数(ch1) ──  基线 {bp}  ->  新版 {np_}")

    print("\n" + "=" * 78)
    return 0


if __name__ == "__main__":
    sys.exit(main())
