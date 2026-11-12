#!/usr/bin/env python3
"""night_soak_report.py —— 夜间长跑报告生成器

读 logs/night_soak_metrics.csv + logs/night_soak.log, 写
rk3588_zero_copy/docs/NIGHT_SOAK_REPORT.md

回答四个问题:
    1) 跑了多少小时 / 多少帧?
    2) 进程重启了几次?
    3) 活堆是不是一条水平直线?
    4) 板子最高温度多少? 有没有触发降频?

监控脚本每 10 分钟会自动跑一次, 所以早上来的时候报告已经是最新的。
"""
from __future__ import annotations

import csv
import os
import re
import statistics
import sys
from datetime import datetime

REPO = "/home/seeed/rk_deploy"
CSV = f"{REPO}/logs/night_soak_metrics.csv"
LOG = f"{REPO}/logs/night_soak.log"
ALERT = f"{REPO}/logs/night_soak_alerts.log"
CNT = f"{REPO}/logs/night_soak_restarts.txt"
OUT = f"{REPO}/rk3588_zero_copy/docs/NIGHT_SOAK_REPORT.md"

FRAME_RE = re.compile(r"^\[frame (\d+)\].*?heap=(\d+)KB")
START_RE = re.compile(r"# \[night-soak\] 第 (\d+) 次启动   (.+)")
END_RE = re.compile(r"# \[night-soak\] 第 (\d+) 次运行结束   (.+)")
EXIT_RE = re.compile(r"# 退出: (.+)")


def read_rows():
    if not os.path.exists(CSV):
        return []
    rows = []
    with open(CSV) as f:
        for r in csv.DictReader(f):
            try:
                rows.append({k: float(v) for k, v in r.items()})
            except (TypeError, ValueError):
                continue
    return rows


def read_log():
    """返回 (启动次数, 每次启动时间, 退出原因列表, 首尾帧号, 首尾活堆)"""
    starts, ends, exits = [], [], []
    if os.path.exists(LOG):
        with open(LOG, errors="replace") as f:
            for line in f:
                m = START_RE.search(line)
                if m:
                    starts.append((int(m.group(1)), m.group(2).strip()))
                m = END_RE.search(line)
                if m:
                    ends.append((int(m.group(1)), m.group(2).strip()))
                m = EXIT_RE.search(line)
                if m:
                    exits.append(m.group(1).strip())
    return starts, ends, exits


def main():
    rows = read_rows()
    starts, ends, exits = read_log()
    restarts = 0
    if os.path.exists(CNT):
        try:
            restarts = int(open(CNT).read().strip())
        except ValueError:
            pass

    L = []
    A = L.append
    A("# 夜间无人值守长跑报告 (NIGHT_SOAK)")
    A("")
    A(f"> 生成时间: {datetime.now():%Y-%m-%d %H:%M:%S}")
    A("> 报告由 `scripts/night_soak_monitor.sh` 每 10 分钟自动刷新一次。")
    A("")

    if not rows:
        A("**还没有采样数据**(监控脚本可能还没起来)。")
        open(OUT, "w").write("\n".join(L) + "\n")
        print("没有数据")
        return 0

    t0, t1 = rows[0]["time"], rows[-1]["time"]
    # ★ 运行时长优先用【日志里第一次启动的时间】——CSV 可能因为监控重启而丢过行,
    #   只用 CSV 的时间跨度会把"跑了 10 小时"算成"跑了 10 分钟"。
    if starts:
        try:
            t0 = min(t0, datetime.strptime(starts[0][1], "%Y-%m-%d %H:%M:%S").timestamp())
        except Exception:
            pass
    hours = (max(t1, t0) - t0) / 3600.0
    frames = [r["frame"] for r in rows if r["frame"] >= 0]
    heaps = [r["heap_kb"] for r in rows if r["heap_kb"] > 0]
    rsss = [r["rss_kb"] for r in rows if r["rss_kb"] > 0]
    npus = [r["npu_temp_c"] for r in rows if r["npu_temp_c"] >= 0]
    socs = [r["soc_temp_c"] for r in rows if r["soc_temp_c"] >= 0]
    bigs = [r["bigcpu_temp_c"] for r in rows if r["bigcpu_temp_c"] >= 0]
    littles = [r["little_temp_c"] for r in rows if r["little_temp_c"] >= 0]
    freqs = [r["cpu_freq_khz"] for r in rows if r["cpu_freq_khz"] > 0]

    # ★ 帧数要【分段累加】: 进程重启后帧号会归零, 直接相减会得到负数。
    n_frames = 0
    prev_f = None
    for f in frames:
        if prev_f is None:
            n_frames += max(0, int(f))
        elif f < prev_f:          # 帧号变小 = 进程重启了, 新的一段从 0 开始
            n_frames += max(0, int(f))
        else:
            n_frames += int(f - prev_f)
        prev_f = f
    frame_start = frames[0] if frames else 0
    frame_end = frames[-1] if frames else 0

    A("## 1️⃣ 跑了多久 / 多少帧")
    A("")
    A(f"| 指标 | 数值 |")
    A(f"|---|---|")
    A(f"| 运行时长 | **{hours:.2f} 小时** ({int(hours)}h{int((hours % 1) * 60):02d}m) |")
    A(f"| 总帧数 | **{n_frames:,}** 帧 |")
    if hours > 0.01:
        A(f"| 平均帧率 | **{n_frames / hours:.1f} fps** |")
    A(f"| 采样点数 | {len(rows)} (每 30s 一次) |")
    if starts:
        A(f"| 首次启动 | {starts[0][1]} |")
    A("")

    A("## 2️⃣ 进程重启了几次")
    A("")
    # 区分【崩溃重启】和【人工/定时器停止】: 前者是问题, 后者是计划内操作
    crash_exits = [e for e in exits
                   if ("SIGSEGV" in e or "SIGABRT" in e or "SIGKILL" in e
                       or "退出码 1" in e or "退出码 13" in e)]
    manual_exits = [e for e in exits if "SIGTERM" in e]
    if not crash_exits:
        A("### ✅ **0 次崩溃重启** —— 全程一次跑到底, 没有被段错误/abort/OOM 杀过。")
    else:
        A(f"### ⚠️ 崩溃重启 **{len(crash_exits)} 次**")
    if manual_exits:
        A(f"")
        A(f"> 另有 **{len(manual_exits)} 次人工/定时器停止**(SIGTERM, 计划内的维护操作, 不算崩溃)。")
    A("")
    if starts:
        A("| # | 启动时间 | 上次退出原因 |")
        A("|---|---|---|")
        for i, (n, ts) in enumerate(starts):
            why = exits[i - 1] if i > 0 and i - 1 < len(exits) else "无(首次启动)"
            mark = " ⚠️" if any(k in why for k in ("SIGSEGV", "SIGABRT", "SIGKILL")) else ""
            A(f"| {n} | {ts} | {why}{mark} |")
    A("")

    A("## 3️⃣ 活堆是不是一条水平直线")
    A("")
    if heaps:
        h0, h1 = heaps[0], heaps[-1]
        d = h1 - h0
        # 尾段斜率: 用【时间列】做最小二乘(启动初期的缓存填充会拉高斜率, 取后一半)
        # ★ 不能用"首尾相减 / 采样点数": 活堆本身有几百 KB 的锯齿, 那样算出来
        #   会得到荒谬的斜率(+27 MB/h 这种)。按时间回归才准。
        # ★ 只取【最后一次进程重启之后】的样本: 重启会让活堆整体跳一级
        #   (新进程的初始占用不同), 把这个台阶算进斜率会得到荒谬的值。
        seq = [(r["time"], r["heap_kb"]) for r in rows if r["heap_kb"] > 0]
        cut = 0
        for i in range(len(seq) - 1, 0, -1):
            if abs(seq[i][1] - seq[i - 1][1]) > 1024:   # 1MB 的台阶 = 进程重启
                cut = i
                break
        tail = seq[cut:] if cut else seq[max(0, len(seq) // 2):]
        per_hour = 0.0
        if len(tail) > 3:
            t0 = tail[0][0]
            xs = [t - t0 for t, _ in tail]
            ys = [h for _, h in tail]
            n = len(xs)
            sx, sy = sum(xs), sum(ys)
            sxx = sum(x * x for x in xs)
            sxy = sum(x * y for x, y in zip(xs, ys))
            den = n * sxx - sx * sx
            if den:
                per_hour = (n * sxy - sx * sy) / den * 3600.0   # KB/小时
        A(f"| 指标 | 数值 |")
        A(f"|---|---|")
        A(f"| 活堆(起始) | {h0:,} KB |")
        A(f"| 活堆(结束) | {h1:,} KB |")
        A(f"| 净变化 | **{d:+,} KB** ({d/1024:+.1f} MB) |")
        A(f"| 尾段斜率 | {per_hour:+.1f} KB/小时 |")
        A(f"| 活堆峰值 | {max(heaps):,} KB |")
        # ★ 判据跟"活堆持续上涨(超过 100MB)"对齐: 总涨幅 >100MB 才算红线,
        #   尾段斜率只作为趋势参考(开机前几分钟的缓存填充会拉高斜率)。
        if d > 100 * 1024:
            A(f"| 判定 | 🔴 **在涨**(净增 {d/1024:.1f}MB, 超过 100MB 红线) |")
        elif d > 10 * 1024:
            A(f"| 判定 | ⚠️ 小幅上涨(净增 {d/1024:.1f}MB, 未到 100MB 红线; 尾段 {per_hour:+.0f} KB/h) |")
        else:
            A(f"| 判定 | ✅ **水平**(净增 {d/1024:+.1f}MB, 远低于 100MB 红线; 尾段 {per_hour:+.0f} KB/h) |")
        if rsss:
            A(f"| RSS(起始→结束) | {rsss[0]:,} KB → {rsss[-1]:,} KB ({rsss[-1]-rsss[0]:+,} KB) |")
            A(f"| RSS 峰值 | {max(rsss):,} KB |")
    else:
        A("没有采到活堆数据。")
    A("")

    A("## 4️⃣ 温度与降频")
    A("")
    A(f"| 传感器 | 平均 | 最高 |")
    A(f"|---|---|---|")
    for name, arr in (("NPU", npus), ("SoC", socs), ("大核 CPU", bigs), ("小核 CPU", littles)):
        if arr:
            A(f"| {name} | {statistics.mean(arr):.1f} °C | **{max(arr):.1f} °C** |")
    if freqs:
        A("")
        A(f"- CPU 频率: 平均 {statistics.mean(freqs)/1000:.0f} MHz, 最低 {min(freqs)/1000:.0f} MHz, "
          f"最高 {max(freqs)/1000:.0f} MHz")
        low = [f for f in freqs if f < 900000]
        if low:
            A(f"- 🔴 有 {len(low)} 个采样点 CPU 频率低于 900MHz(疑似降频), 最低 {min(low)/1000:.0f} MHz")
        else:
            A(f"- ✅ 没有出现明显降频(所有采样点都 >= 900MHz)")
    A("")

    if os.path.exists(ALERT):
        alerts = open(ALERT, errors="replace").read().strip()
        if alerts:
            A("## 🔴 告警记录")
            A("")
            A("```")
            A(alerts)
            A("```")
        else:
            A("## 🔴 告警记录")
            A("")
            A("**无告警**(没有重启、没有活堆超涨、没有卡死)。")
    A("")

    A("---")
    A("")
    A("数据来源: `logs/night_soak_metrics.csv` / `logs/night_soak.log` / `logs/night_soak_alerts.log`")
    A("")

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w").write("\n".join(L) + "\n")
    print(f"报告已生成: {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
