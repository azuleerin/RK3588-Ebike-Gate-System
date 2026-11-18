#!/usr/bin/env python3
# ===========================================================================
# render_md.py —— 把 thread_bench 的 JSONL 结果渲染成 Markdown
#
# 用法:
#   render_md.py <run_dir> --out <out.md>     # 渲染目录下所有 *.jsonl
#   render_md.py --best <file.jsonl>          # 打印吞吐最优的 "T 策略"
#
# 每个 JSONL 渲染成一张表。表的顺序按文件名:
#   matrix -> saturate -> saturate_unlimited -> queue -> queue_unlimited -> dual
# ===========================================================================
import json
import os
import sys
import time

PHASE_ORDER = [
    ("matrix.jsonl", "① 真实负载 matrix", "按业务负载供给(默认 4 路 x 25 fps), 问: 固定负载下加线程会不会变差"),
    ("saturate.jsonl", "② 饱和负载 saturate", "供给拉到远超 NPU 能力, 问: 峰值吞吐随 T 怎么变"),
    ("saturate_unlimited.jsonl", "②b 饱和负载(unlimited 狂推)", "Producer 死循环狂推(8M/s), 交叉验证 ② 的结论"),
    ("queue.jsonl", "③ 队列容量边界 queue", "最优 T/策略下换队列容量, 问: 丢帧策略与缓冲深度的影响"),
    ("queue_unlimited.jsonl", "③b 队列容量边界(unlimited)", "狂推模式下的队列边界"),
    ("dual.jsonl", "④ 双模型串联 dual", "检测+识别串联在同一 worker, 问: DDR 带宽/调度是不是新瓶颈"),
    ("dual_business.jsonl", "④b 双模型(业务拓扑)", "识别集中 Core0, 复刻主工程 --det-core 1,2 --ocr-core 0"),
]


def load(path):
    rows = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    return rows


def fmt(v, digits=1, dash="-", allow_negative=False):
    if v is None:
        return dash
    try:
        f = float(v)
    except (TypeError, ValueError):
        return str(v)
    if f < 0 and not allow_negative:
        return dash
    return ("%." + str(digits) + "f") % f


def cores(arr):
    if not arr:
        return "-"
    return "/".join(fmt(v, 0) for v in arr)


def core_occupancy(r):
    """每核总占用: 优先用 core_total_pct(模型1+模型2 合并归一化),
    没有这个字段的旧数据退回 core_m1_pct(单模型时两者本来就相等)。"""
    tot = r.get("core_total_pct")
    if tot is None:
        tot = r.get("core_m1_pct")
    return cores(tot)


def table(rows):
    if not rows:
        return ""
    dual = any(r.get("dual_model") for r in rows)
    head = [
        "T", "策略", "q", "供给FPS", "吞吐FPS", "丢帧%",
        "e2e avg", "e2e P99", "e2e max",
        "M1 avg", "M1 P99",
    ]
    if dual:
        head += ["M2 avg", "M2 P99"]
    head += ["CPU%", "核占用0/1/2", "NPU算力%", "RSS峰值MB", "堆斜率KB/min", "错误"]

    out = ["| " + " | ".join(head) + " |",
           "|" + "|".join(["---"] * len(head)) + "|"]
    for r in rows:
        e2e = r.get("lat_e2e", {})
        m1 = r.get("lat_m1", {})
        m2 = r.get("lat_m2", {})
        cells = [
            str(r.get("threads", "?")),
            r.get("strategy", "?"),
            str(r.get("queue_size", "?")),
            fmt(r.get("arrival_fps"), 0),
            fmt(r.get("throughput_fps"), 1),
            fmt((r.get("drop_rate") or 0) * 100, 1),
            fmt(e2e.get("avg_ms"), 1),
            fmt(e2e.get("p99_ms"), 1),
            fmt(e2e.get("max_ms"), 1),
            fmt(m1.get("avg_ms"), 1),
            fmt(m1.get("p99_ms"), 1),
        ]
        if dual:
            cells += [fmt(m2.get("avg_ms"), 1), fmt(m2.get("p99_ms"), 1)]
        cells += [
            fmt(r.get("cpu_pct"), 1),
            core_occupancy(r),
            fmt(r.get("npu_compute_agg_pct"), 0),
            fmt((r.get("rss_peak_kb") or 0) / 1024.0, 0),
            # 堆斜率【负值是有意义的】: 负 = 窗口内在缩(没涨), 所以这里不吞负号
            fmt(r.get("heap_slope_kb_min"), 0, allow_negative=True),
            str(r.get("errors", 0)),
        ]
        out.append("| " + " | ".join(cells) + " |")
    return "\n".join(out)


def render(run_dir):
    parts = []
    parts.append("# thread_bench 实测结果")
    parts.append("")
    parts.append("- 硬件: Rockchip RK3588 (3 核 NPU / 8 核 CPU: 4xA76 + 4xA55)")
    parts.append("- 时长: %s" % time.strftime("%Y-%m-%d %H:%M:%S"))
    parts.append("- 口径: 吞吐 = 成功推理帧数 / 测量窗口(墙钟); e2e = 入队->结果就绪; "
                 "M1/M2 = 各模型 rknn_run+outputs_get; 核占用按该核 worker 数归一化(上限 100%)")
    parts.append("")

    for fname, title, desc in PHASE_ORDER:
        path = os.path.join(run_dir, fname)
        if not os.path.exists(path):
            continue
        rows = load(path)
        if not rows:
            continue
        parts.append("## %s" % title)
        parts.append("")
        parts.append(desc)
        parts.append("")
        parts.append(table(rows))
        parts.append("")
        best = max(rows, key=lambda r: r.get("throughput_fps") or 0)
        parts.append("> 本组吞吐最优: **T=%s / %s / q=%s → %.2f fps** (e2e P99 %.1f ms)"
                     % (best.get("threads"), best.get("strategy"), best.get("queue_size"),
                        best.get("throughput_fps", 0),
                        best.get("lat_e2e", {}).get("p99_ms", 0)))
        parts.append("")

    # 结论摘要: 按策略分组, 相对值以【同策略里最小的 T】为基准
    sat = os.path.join(run_dir, "saturate.jsonl")
    if os.path.exists(sat):
        rows = load(sat)
        if rows:
            parts.append("## 结论摘要(基于饱和负载)")
            parts.append("")
            by_st = {}
            for r in rows:
                by_st.setdefault(r.get("strategy", "?"), []).append(r)
            for st, rs in by_st.items():
                rs = sorted(rs, key=lambda x: x.get("threads") or 0)
                base = rs[0].get("throughput_fps") or 0
                parts.append("### 策略 `%s`" % st)
                parts.append("")
                parts.append("| T | 吞吐FPS | 相对最小T | 每核worker数 | M1 P99(ms) | e2e P99(ms) | 核占用0/1/2 | NPU算力% | CPU% |")
                parts.append("|---|---|---|---|---|---|---|---|---|")
                for r in rs:
                    rel = ((r.get("throughput_fps") or 0) / base * 100) if base else 0
                    parts.append("| %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
                        r.get("threads"),
                        fmt(r.get("throughput_fps"), 1),
                        fmt(rel, 0) + "%",
                        "/".join(str(v) for v in (r.get("workers_per_core1") or [])),
                        fmt(r.get("lat_m1", {}).get("p99_ms"), 1),
                        fmt(r.get("lat_e2e", {}).get("p99_ms"), 1),
                        core_occupancy(r),
                        fmt(r.get("npu_compute_agg_pct"), 0),
                        fmt(r.get("cpu_pct"), 1)))
                parts.append("")
    return "\n".join(parts)


def best_of(path):
    rows = load(path)
    if not rows:
        return ""
    b = max(rows, key=lambda r: r.get("throughput_fps") or 0)
    return "%s %s" % (b.get("threads"), b.get("strategy"))


def main():
    args = sys.argv[1:]
    out = None
    run_dir = None
    best_file = None
    i = 0
    while i < len(args):
        if args[i] == "--out":
            out = args[i + 1]
            i += 2
        elif args[i] == "--best":
            best_file = args[i + 1]
            i += 2
        else:
            run_dir = args[i]
            i += 1

    if best_file:
        print(best_of(best_file))
        return 0
    if not run_dir:
        print("用法: render_md.py <run_dir> [--out out.md] | --best <file.jsonl>", file=sys.stderr)
        return 2

    md = render(run_dir)
    if out:
        with open(out, "w", encoding="utf-8") as f:
            f.write(md)
        print("已写出: %s" % out)
    else:
        print(md)
    return 0


if __name__ == "__main__":
    sys.exit(main())
