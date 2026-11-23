#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""summarize_batch.py —— 把 run_batch_bench.sh 的原始 JSON 汇总成对比表

用法: ./summarize_batch.py <out_dir>
"""
import collections
import json
import os
import statistics
import sys


def load(outdir):
    rows = collections.defaultdict(list)
    for f in sorted(os.listdir(outdir)):
        if not f.endswith(".json") or f.startswith("summary"):
            continue
        try:
            d = json.load(open(os.path.join(outdir, f)))
        except Exception:
            continue
        if "batch_ms_mean" not in d:
            continue
        scen = "empty" if d.get("ddr_stress", 0) == 0 else "stress"
        key = (scen, d["batch"], d["mode"])
        rows[key].append(d)
    return rows


def med(vals):
    return statistics.median(vals) if vals else 0.0


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    rows = load(outdir)

    def show(scen, title):
        print("\n" + "=" * 100)
        print(title)
        print("=" * 100)
        print("%-7s %-9s %-11s %-11s %-11s %-10s %-11s %-8s %-9s" % (
            "场景", "模式", "Batch", "单Batch均ms", "单BatchP95", "单帧等效ms",
            "总FPS", "单帧延迟ms", "dmc%"))
        print("-" * 100)
        base = None
        for batch in (1, 2, 4):
            for mode in ("single", "multi3", "workers3"):
                ds = rows.get((scen, batch, mode))
                if not ds:
                    continue
                bm = med([d["batch_ms_mean"] for d in ds])
                bp = med([d["batch_ms_p95"] for d in ds])
                pf = med([d["per_frame_ms"] for d in ds])
                fps = med([d["fps"] for d in ds])
                lat = med([d["frame_latency_ms"] for d in ds])
                dmc = med([d["dmc_mean"] for d in ds])
                upl = med([d.get("upload_ms_mean", 0) for d in ds])
                if (scen, batch, mode) == (scen, 1, "single"):
                    base = (bm, fps)
                g1 = ""
                g2 = ""
                if base:
                    g1 = "(%+.1f%%)" % (100 * (bm / base[0] - 1))
                    g2 = "(%+.1f%%)" % (100 * (fps / base[1] - 1))
                print("%-7s %-9s %-9d %-11.3f %-11.3f %-11.3f %-10.2f %-11.3f %-8.1f  %s %s" % (
                    scen, mode, batch, bm, bp, pf, fps, lat, dmc, g1, g2))

    show("empty", "场景 A: 空载 (无额外 DDR 压力)")
    show("stress", "场景 B: 带 DDR 压力 (模拟 4 路 MPP 解码 + RGA 预处理)")

    # ---- 收益衰减 ----
    print("\n" + "=" * 100)
    print("空载 vs 带压: Batch 收益的衰减")
    print("=" * 100)
    print("%-9s %-9s %-12s %-12s %-10s %-12s %-12s %-10s" % (
        "模式", "Batch", "空载FPS", "带压FPS", "FPS衰减",
        "空载单帧ms", "带压单帧ms", "延迟上升"))
    print("-" * 100)
    for mode in ("single", "multi3"):
        for batch in (1, 2, 4):
            e = rows.get(("empty", batch, mode))
            s = rows.get(("stress", batch, mode))
            if not e or not s:
                continue
            fe = med([d["fps"] for d in e])
            fs = med([d["fps"] for d in s])
            pe = med([d["per_frame_ms"] for d in e])
            ps = med([d["per_frame_ms"] for d in s])
            print("%-9s %-9d %-12.2f %-12.2f %-10.1f%% %-12.3f %-12.3f %-10.1f%%" % (
                mode, batch, fe, fs, 100 * (fs / fe - 1) if fe else 0,
                pe, ps, 100 * (ps / pe - 1) if pe else 0))

    # ---- 零拷贝探测结论 ----
    print("\n" + "=" * 100)
    print("零拷贝 (rknn_set_io_mem) 在 batch>1 上的可用性")
    print("=" * 100)
    for batch in (1, 2, 4):
        for mode in ("single", "multi3"):
            ds = rows.get(("empty", batch, mode))
            if not ds:
                continue
            probes = set(d.get("zc_probe", "n/a") for d in ds)
            ios = set(d.get("io_used", "?") for d in ds)
            print("  Batch=%d %-8s : zc_probe=%-16s 实际使用通路=%s"
                  % (batch, mode, ",".join(probes), ",".join(ios)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
