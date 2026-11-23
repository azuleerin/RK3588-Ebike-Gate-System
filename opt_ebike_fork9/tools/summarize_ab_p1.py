#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""summarize_ab_p1.py —— 汇总 P1 交错 A/B 的 JSON(帧计数器差分口径)

用法: summarize_ab_p1.py <out/ab 目录>
"""
import glob
import json
import os
import statistics
import sys


def load(d):
    out = []
    for f in sorted(glob.glob(os.path.join(d, "*.json"))):
        with open(f) as fh:
            out.append((os.path.basename(f)[:-5], json.load(fh)))
    return out


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "out/ab"
    rows = load(d)
    if not rows:
        print("没有结果 JSON:", d)
        return 1

    groups = {}
    for name, r in rows:
        key = "口径① max-fps=20" if r.get("max_fps") == 20 else "口径② max-fps=0"
        groups.setdefault(key, {"base": [], "new": []})["base" if name.startswith("base") else "new"].append(r)

    for key in ("口径① max-fps=20", "口径② max-fps=0"):
        g = groups.get(key)
        if not g:
            continue
        print("=" * 96)
        print("%s" % key)
        for arm in ("base", "new"):
            rs = g[arm]
            if not rs:
                continue
            fps = [r["total_fps"] for r in rs]
            npu = []
            dw = []
            ow = []
            cores = []
            heaps = []
            rss = []
            for r in rs:
                for ch, c in r["channels"].items():
                    npu.append(c["npu_ms"])
                    dw.append(c["detwait_ms"])
                    if c.get("ocrwait_ms"):
                        ow.append(c["ocrwait_ms"])
                if "npu_core_pct" in r:
                    cores.append(r["npu_core_pct"])
                if "heap_global" in r:
                    heaps.append(r["heap_global"]["slope_kb_per_min"])
                if "rss_global" in r:
                    rss.append(r["rss_global"]["slope_kb_per_min"])
            print("  %-5s 轮次=%d  总吞吐(帧计数器差分)= %s  均值 %.2f FPS"
                  % (arm, len(rs), ["%.2f" % f for f in fps], statistics.mean(fps)))
            print("        npu  %.2f ms | detwait %.2f ms | ocrwait %.2f ms"
                  % (statistics.mean(npu), statistics.mean(dw),
                     statistics.mean(ow) if ow else 0.0))
            if cores:
                ck = sorted(cores[0].keys())
                print("        逐核 NPU%%: %s"
                      % "  ".join("%s=%.1f" % (k, statistics.mean(c[k] for c in cores)) for k in ck))
            if heaps:
                print("        活堆斜率 %.2f KB/min | RSS 斜率 %.2f KB/min"
                      % (statistics.mean(heaps), statistics.mean(rss) if rss else 0.0))
            print("        起跑前逐核: %s"
                  % ", ".join(str(r.get("npu_idle_before")) for r in rs))
        if g["base"] and g["new"]:
            fb = statistics.mean(r["total_fps"] for r in g["base"])
            fn = statistics.mean(r["total_fps"] for r in g["new"])
            print("  ==> new − base = %+.2f FPS (%+.2f%%)" % (fn - fb, 100.0 * (fn - fb) / fb))
    print()
    print("原始 JSON 在:", os.path.abspath(d))
    return 0


if __name__ == "__main__":
    sys.exit(main())
