#!/usr/bin/env python3
# ===========================================================================
# ab_refactor_report.py —— 汇总 NPU 调度重构的 A/B 结果
#
# 用法: ab_refactor_report.py <logs/ab_refactor 的目录>
#
# 数据来源:
#   <leg>/load.csv   每秒一行: t,npu0,npu1,npu2,vpu,rga,cpu_pct,rss_mb,fps1..fps4
#                    (npu0..2 需要 root 才能读 debugfs, 读不到就是 0)
#   <leg>/gate.log   应用自报的每帧统计(deliv=/npu=/detwait=/ocrwait=)
#                    和收尾的"每核 NPU 占用"
# ===========================================================================
import os
import re
import sys


def read_load(path):
    """load.csv -> (每列的均值, 采样点数)"""
    if not os.path.exists(path):
        return None, 0
    rows = []
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            parts = line.strip().split(",")
            if len(parts) < 12:
                continue
            try:
                rows.append([float(x) for x in parts])
            except ValueError:
                continue
    if not rows:
        return None, 0
    n = len(rows)
    cols = list(zip(*rows))
    avg = [sum(c) / len(c) if c else 0.0 for c in cols]
    return avg, n


def read_gate(path):
    """从 gate.log 里抽: 交付帧率 / npu 耗时 / detwait / ocrwait / 每核占用"""
    if not os.path.exists(path):
        return {}
    txt = open(path, "r", encoding="utf-8", errors="ignore").read()
    out = {}

    # 每帧统计: [ch1][frame 200] fps=.. deliv=.. src=.. rga=..ms npu=..ms detwait=..ms ocrwait=..ms
    per = {}
    pat = re.compile(
        r"\[(ch\d)\]\[frame\s+(\d+)\]\s+fps=([\d.]+)\s+deliv=([\d.]+)\s+src=([\d.]+)"
        r"\s+rga=([\d.]+)ms\s+npu=([\d.]+)ms\s+detwait=([\d.]+)ms\s+ocrwait=([\d.]+)ms")
    for m in pat.finditer(txt):
        ch = m.group(1)
        d = per.setdefault(ch, {"deliv": [], "npu": [], "detwait": [], "ocrwait": []})
        d["deliv"].append(float(m.group(4)))
        d["npu"].append(float(m.group(7)))
        d["detwait"].append(float(m.group(8)))
        d["ocrwait"].append(float(m.group(9)))
    out["per_ch"] = per

    # 每核占用: "  检测   mask=0x1  任务    396  NPU占用  68.02%  含后处理  69.03%"
    cores = []
    pat2 = re.compile(
        r"\s+(检测|识别)\s+mask=0x([0-9a-f]+)\s+任务\s+(\d+)\s+NPU占用\s+([\d.]+)%\s+含后处理\s+([\d.]+)%")
    for m in pat2.finditer(txt):
        cores.append({"kind": m.group(1), "mask": m.group(2), "tasks": int(m.group(3)),
                      "npu": float(m.group(4)), "total": float(m.group(5))})
    out["cores"] = cores

    # 识别到的车牌条数
    out["plates"] = len(re.findall(r"\[车牌\]", txt))
    # 错误
    out["errors"] = len(re.findall(r"\[错误\]|\[PlateRec\] 失败|rknn_run.*失败", txt))
    return out


def avg(xs):
    return sum(xs) / len(xs) if xs else 0.0


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "logs/ab_refactor"
    legs = sorted(d for d in os.listdir(root) if os.path.isdir(os.path.join(root, d)))
    if not legs:
        print("没有找到任何一条腿的数据")
        return 1

    print("\n%-16s %-26s %8s %8s %8s %8s %8s %8s" % (
        "腿", "配置", "ch1", "ch2", "ch3", "ch4", "CPU%", "RSS(MB)"))
    print("-" * 96)

    rows = []
    for leg in legs:
        ld = os.path.join(root, leg)
        avg_row, n = read_load(os.path.join(ld, "load.csv"))
        g = read_gate(os.path.join(ld, "gate.log"))
        per = g.get("per_ch", {})

        def fps(ch):
            return avg(per.get(ch, {}).get("deliv", []))

        cpu = avg_row[6] if avg_row else 0.0
        rss = avg_row[7] if avg_row else 0.0
        # load.csv 的列: t,npu0,npu1,npu2,vpu,rga,cpu_pct,rss_mb,fps1..fps4
        print("%-16s %-26s %8.2f %8.2f %8.2f %8.2f %8.1f %8.1f" % (
            leg, "", fps("ch1"), fps("ch2"), fps("ch3"), fps("ch4"), cpu, rss))
        rows.append((leg, g, per, avg_row))

    # 延迟指标
    print("\n%-16s %10s %10s %10s %10s %10s" % (
        "腿", "npu(ms)", "detwait", "ocrwait", "识别总数", "每核占用(检测)"))
    print("-" * 72)
    for leg, g, per, avg_row in rows:
        npu = avg([v for d in per.values() for v in d["npu"]])
        detw = avg([v for d in per.values() for v in d["detwait"]])
        ocrw = avg([v for d in per.values() for v in d["ocrwait"]])
        det_cores = [c for c in g.get("cores", []) if c["kind"] == "检测"]
        ocr_cores = [c for c in g.get("cores", []) if c["kind"] == "识别"]
        det_s = "/".join("%.0f" % c["npu"] for c in det_cores)
        ocr_s = "/".join("%.0f" % c["npu"] for c in ocr_cores)
        print("%-16s %10.1f %10.1f %10.1f %10d   检测:%s 识别:%s" % (
            leg, npu, detw, ocrw, g.get("plates", 0), det_s or "-", ocr_s or "-"))

    # debugfs 侧(需要 root 才有数)
    if rows and rows[0][3]:
        print("\n%-16s %10s %10s %10s %10s %10s" % (
            "腿", "npu0%", "npu1%", "npu2%", "VPU%", "RGA%"))
        print("-" * 72)
        for leg, g, per, avg_row in rows:
            if not avg_row:
                continue
            print("%-16s %10.1f %10.1f %10.1f %10.1f %10.1f" % (
                leg, avg_row[1], avg_row[2], avg_row[3], avg_row[4], avg_row[5]))
        if abs(rows[0][3][1]) < 0.01:
            print("(debugfs 的 NPU load 需要 root 才读得到, 这几列是 0 —— "
                  "用 sudo 再跑一次就有数了)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
