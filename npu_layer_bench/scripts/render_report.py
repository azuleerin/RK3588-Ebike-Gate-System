#!/usr/bin/env python3
"""render_report.py —— 把 JSONL 结果渲染成 Markdown 报告

用法:
    python3 render_report.py results.jsonl out.md [--duration 30] [--repeats 3]

★ 重复次数的处理: 同一用例的 N 次重复取【均值】, 同时给出【极差】
  (max - min), 用来判断这组数据稳不稳。极差超过均值的 3% 会在表里标出来。
"""
from __future__ import annotations

import argparse
import json
import os
import statistics
import sys
from collections import OrderedDict, defaultdict


def load(path):
    rows = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return rows


def group(rows):
    """按 (layer, case) 分组, 保序。"""
    g = OrderedDict()
    for r in rows:
        key = (r.get("layer"), r.get("case"))
        g.setdefault(key, []).append(r)
    return g


# 一个样本的"单位工作耗时"(ms)。越小 = 越干净。
#
# ★ 为什么用这个口径来挑干净窗口:
#   这块板子可能被别的会话同时用来跑压测(实测遇到过), 那些任务会【占用 NPU】。
#   干扰只会让事情变慢, 不会变快 —— 所以"最慢的那几个样本"就是被干扰的,
#   而"最快的那几个"是干净的。这是仓库里 gate_ab_clean.py 用的同一套路
#   (剔除被别的进程抢过资源的时段)。
#
#   L1/L2/L3/EX : 单帧推理耗时(每帧一次)
#   L4/L5       : 每路检测等待(含 NPU 排队, 是生产链路的主指标)
def unit_ms(r):
    if r.get("layer") in ("L4", "L5"):
        v = r.get("frame_ms")
        return float(v) if isinstance(v, (int, float)) else None
    v = r.get("det_wall_ms")
    return float(v) if isinstance(v, (int, float)) else None


def foreign_load(r):
    """这个样本里, 【不属于我】的核上挂了多少负载(%)。

    ★ 这是最可靠的污染判据:
      每个样本都记了 ctx_per_core(我在这颗核上挂了几个上下文)和
      npu_load(驱动侧逐核占用)。如果某颗核上我一个上下文都没有,
      但它的占用却不是 0 —— 那就是别的任务在用这颗核, 一定污染了。
      实测: 干净的 core0 样本是 [84, 0, 0], 被污染的是 [95, 70, 70]。
    """
    load = r.get("npu_load")
    per = r.get("ctx_per_core")
    if not (isinstance(load, list) and isinstance(per, list)):
        return None
    worst = 0.0
    seen_unused = False
    for i in range(min(3, len(load), len(per))):
        if per[i] == 0:
            seen_unused = True
            try:
                v = float(load[i])
            except (TypeError, ValueError):
                continue
            if v > worst:
                worst = v
    return worst if seen_unused else None


def clean_window(rs, max_slowdown=1.25, foreign_limit=8.0):
    """挑出干净样本。

    两级判据, **两级都过**才算干净:
      ① 【外部负载判据】(最硬): 核上没有我的上下文, 但驱动报占用 > 8%
         -> 一定有别的任务在抢这颗核 -> 剔除。
         ★ 对"只用了 1~2 颗核"的用例是决定性的。
      ② 【耗时判据】: 单位耗时 > 最快样本 × max_slowdown -> 剔除。
         ★ 兜住"竞争者和我【在同一颗核】上"的情况 —— ① 在那种情况下看不见
           (空闲核是干净的), 只有耗时能暴露。
         ★ 也用于"三颗核全用上"的用例(那时没有空闲核可用 ① 判)。
    """
    # ① 外部负载判据
    with_free_core = [r for r in rs if foreign_load(r) is not None]
    if with_free_core:
        clean = [r for r in with_free_core if foreign_load(r) <= foreign_limit]
        if clean:
            rs = clean

    # ② 耗时判据
    usable = [(unit_ms(r), r) for r in rs]
    usable = [(u, r) for u, r in usable if u is not None and u > 0]
    if not usable:
        return rs, len(rs), len(rs)
    best = min(u for u, _ in usable)
    kept = [r for u, r in usable if u <= best * max_slowdown]
    return kept, len(kept), len(rs)


def agg(rs):
    """一组重复测量 -> 均值 + 极差。"""
    out = {"n": len(rs)}

    def m(field, sub=None):
        vals = []
        for r in rs:
            v = r.get(field)
            if sub is not None:
                v = v[sub] if isinstance(v, list) else None
            if isinstance(v, (int, float)):
                vals.append(float(v))
        if not vals:
            return 0.0, 0.0
        return statistics.fmean(vals), (max(vals) - min(vals))

    for f in ("fps", "det_wall_ms", "ocr_wall_ms", "rga_ms", "frame_ms",
              "det_npu_ms", "ocr_npu_ms", "queue_ms", "det_p50", "det_p99",
              "ocr_p50", "ocr_p99", "frame_p50", "frame_p99",
              "cpu_pct", "cpu_cores", "rss_mb"):
        out[f] = m(f)
    for f in ("core_app_pct", "core_drv_pct", "npu_load"):
        vals = []
        for r in rs:
            v = r.get(f)
            if isinstance(v, list) and len(v) == 3:
                vals.append([float(x) for x in v])
        if vals:
            out[f] = ([statistics.fmean(x[i] for x in vals) for i in range(3)],
                      max(max(x[i] for x in vals) - min(x[i] for x in vals)
                          for i in range(3)))
        else:
            out[f] = ([0.0, 0.0, 0.0], 0.0)
    out["rows"] = rs
    out["extra"] = {k: v for k, v in rs[0].items()
                    if k not in ("layer", "case", "repeat", "duration_s", "warmup_s")}
    return out


def fmt(v, nd=2):
    if v is None:
        return "-"
    return f"{v:.{nd}f}"


def spread_pct(a):
    """极差占均值的百分比。"""
    mean, rng = a
    if mean <= 0:
        return 0.0
    return 100.0 * rng / mean


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("jsonl")
    ap.add_argument("out")
    ap.add_argument("--duration", type=float, default=30.0)
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--max-slowdown", type=float, default=1.25,
                    help="干净窗口阈值: 单位耗时超过最快样本的这么多倍就剔除")
    ap.add_argument("--foreign-limit", type=float, default=8.0,
                    help="空闲核上的外部负载超过这个百分比就判定为污染")
    args = ap.parse_args()

    rows = load(args.jsonl)
    if not rows:
        print("没有可用数据", file=sys.stderr)
        return 1

    # ---- 干净窗口筛选 ----
    groups_raw = group(rows)
    groups = OrderedDict()
    dropped_total = 0
    for key, rs in groups_raw.items():
        kept, nk, nt = clean_window(rs, args.max_slowdown, args.foreign_limit)
        dropped_total += (nt - nk)
        groups[key] = kept if kept else rs

    L = []
    W = L.append

    W("# RK3588 NPU 分层压测报告")
    W("")
    W("> 工具: `npu_layer_bench`(独立压测台, **不接入主工程**, 不修改主工程任何代码)")
    W("> 板子: RK3588(3 核 NPU / 8 核 CPU)   运行时: RKNN Toolkit2 **v2.3.2**")
    W(f"> 每组: 预热 5s + 测量 **{args.duration:.0f}s**, 重复 **{args.repeats}** 次, 表格里是均值")
    W("")
    W(f"> **干净窗口**: 用驱动侧逐核占用判定污染 —— "
      f"【我没有任何上下文的那颗核】占用 > {args.foreign_limit:g}% 就说明有别的任务在抢 NPU; "
      f"三颗核全占满的用例退化为耗时判据(> 最快样本 × {args.max_slowdown:g})。"
      f"共剔除 {dropped_total} / {len(rows)} 个样本")
    W("")

    det_model = rows[0].get("det_model", "")
    ocr_model = next((r.get("ocr_model") for r in rows if r.get("ocr_model")), "")
    W("| 项 | 值 |")
    W("|---|---|")
    W(f"| 检测模型 | `{det_model}` |")
    if ocr_model:
        W(f"| 识别模型 | `{ocr_model}` |")
    W(f"| 采样点 | {len(rows)} |")
    W("")

    # ---------------------------------------------------------------- L1
    W("## 第 1 层 —— 纯 NPU 算力天花板(裸推理, 无前后处理)")
    W("")
    W("只跑 `rknn_run` + `rknn_outputs_get`。输入是预分配的零拷贝缓冲, "
      "启动时写一次图案, 热循环里对输入缓冲的写次数 = 0。")
    W("")
    l1 = [(k, v) for k, v in groups.items() if k[0] == "L1"]
    if l1:
        W("| 用例 | 配置 | 吞吐 FPS | 单帧推理 ms | P50 / P99 ms | 核占用 0/1/2 (应用侧) | 核占用 0/1/2 (驱动侧) | CPU% | 样本极差 |")
        W("|---|---|---|---|---|---|---|---|---|")
        for (layer, case), rs in l1:
            a = agg(rs)
            per = a["rows"][0].get("ctx_per_core", [0, 0, 0])
            nw = sum(per) if isinstance(per, list) else 0
            case_cfg = {
                "core0": "1 worker / Core0", "core1": "1 worker / Core1",
                "core2": "1 worker / Core2",
                "core0_nopq": "1 worker / Core0(不查驱动耗时)",
            }.get(case, f"{nw} worker, 每核 {per[0]}/{per[1]}/{per[2]}")
            cp = a["core_app_pct"][0]
            sp = spread_pct(a["fps"])
            flag = " ⚠️" if sp > 3.0 else ""
            W(f"| `{case}` | {case_cfg} | **{fmt(a['fps'][0])}** | {fmt(a['det_wall_ms'][0])} | "
              f"{fmt(a['det_p50'][0])} / {fmt(a['det_p99'][0])} | "
              f"{fmt(cp[0])} / {fmt(cp[1])} / {fmt(cp[2])} | "
              f"{fmt(a['core_drv_pct'][0][0])} / {fmt(a['core_drv_pct'][0][1])} / {fmt(a['core_drv_pct'][0][2])} | "
              f"{fmt(a['cpu_pct'][0])} | {fmt(sp, 2)}%{flag} |")
        W("")

    # ---------------------------------------------------------------- L2
    W("## 第 2 层 —— 检测 + OCR 双模型纯推理(同一线程内串行)")
    W("")
    l2 = [(k, v) for k, v in groups.items() if k[0] == "L2"]
    if l2:
        W("| 用例 | 配置 | 吞吐 FPS | 检测 ms | 识别 ms | 双模型合计 ms | 理论上限 FPS | 核占用 0/1/2 |")
        W("|---|---|---|---|---|---|---|---|")
        for (layer, case), rs in l2:
            a = agg(rs)
            det = a["det_wall_ms"][0]
            ocr = a["ocr_wall_ms"][0]
            total = det + ocr
            W(f"| `{case}` | {case} | **{fmt(a['fps'][0])}** | {fmt(det)} | {fmt(ocr)} | "
              f"**{fmt(total)}** | {fmt(1000.0 / total if total > 0 else 0)} | "
              f"{fmt(a['core_app_pct'][0][0])} / {fmt(a['core_app_pct'][0][1])} / {fmt(a['core_app_pct'][0][2])} |")
        W("")

    # ---------------------------------------------------------------- L3
    W("## 第 3 层 —— RGA 预处理 + 检测推理")
    W("")
    l3 = [(k, v) for k, v in groups.items() if k[0] == "L3"]
    if l3:
        W("| 用例 | 配置 | 吞吐 FPS | RGA ms | 推理 ms | 合计 ms | 核占用 0/1/2 |")
        W("|---|---|---|---|---|---|---|")
        for (layer, case), rs in l3:
            a = agg(rs)
            rga = a["rga_ms"][0]
            det = a["det_wall_ms"][0]
            W(f"| `{case}` | {case} | **{fmt(a['fps'][0])}** | {fmt(rga)} | {fmt(det)} | "
              f"{fmt(rga + det)} | "
              f"{fmt(a['core_app_pct'][0][0])} / {fmt(a['core_app_pct'][0][1])} / {fmt(a['core_app_pct'][0][2])} |")
        W("")

    # ---------------------------------------------------------------- L4/L5
    for lyr, title in (("L4", "第 4 层 —— 单路全链路(解码 + RGA + 检测 + OCR + 后处理)"),
                       ("L5", "第 5 层 —— 4 路全链路极限(解除限帧, 复现生产 93/95 FPS)")):
        ll = [(k, v) for k, v in groups.items() if k[0] == lyr]
        if not ll:
            continue
        W(f"## {title}")
        W("")
        W("这一层是**驱动生产二进制 `rk_gate`**(黑盒), 只改命令行参数, "
          "主工程代码一行未改; 数据来自它自己打的统计行。")
        W("")
        W("| 用例 | 总吞吐 FPS | 纯 NPU ms | 检测等待 ms | 排队 ms | 识别等待 ms | RGA ms | NPU 三核 (debugfs) | CPU% |")
        W("|---|---|---|---|---|---|---|---|---|")
        for (layer, case), rs in ll:
            a = agg(rs)
            npu = a["det_wall_ms"][0]
            detwait = a["frame_ms"][0]
            nl = a["npu_load"][0]
            W(f"| `{case}` | **{fmt(a['fps'][0])}** | {fmt(npu)} | {fmt(detwait)} | "
              f"{fmt(a['queue_ms'][0])} | {fmt(a['ocr_wall_ms'][0])} | {fmt(a['rga_ms'][0])} | "
              f"{fmt(nl[0])} / {fmt(nl[1])} / {fmt(nl[2])} | {fmt(a['cpu_pct'][0])} |")
        W("")
        # 每路明细
        chans = sorted({k.split("_")[1] for (l, c), rs in ll for k in rs[0]
                        if k.startswith("ch_") and k.count("_") >= 2})
        if chans:
            W("每路明细:")
            W("")
            W("| 用例 | 通道 | 交付 FPS | 纯 NPU ms | 检测等待 ms | 识别等待 ms | RGA ms |")
            W("|---|---|---|---|---|---|---|")

            def mv(rs, chan, suf):
                vals = [float(r.get(f"ch_{chan}_{suf}", 0) or 0) for r in rs]
                return statistics.fmean(vals) if vals else 0.0

            for (layer, case), rs in ll:
                for ch in chans:
                    d = mv(rs, ch, "deliv")
                    if d <= 0:
                        continue
                    W(f"| `{case}` | {ch} | **{fmt(d)}** | {fmt(mv(rs, ch, 'npu_ms'))} | "
                      f"{fmt(mv(rs, ch, 'detwait_ms'))} | {fmt(mv(rs, ch, 'ocrwait_ms'))} | "
                      f"{fmt(mv(rs, ch, 'rga_ms'))} |")
            W("")

    # ---------------------------------------------------------------- EX
    ex = [(k, v) for k, v in groups.items() if k[0] == "EX"]
    res_rows = [(k, v) for k, v in ex if k[1].startswith("res_")]
    op_rows = [(k, v) for k, v in ex if k[1].startswith("op_fallback_")]

    if res_rows:
        W("## 额外验证项 1 —— 分辨率扫描(640 / 576 / 512)")
        W("")
        W("用同一个 YOLOv8n ONNX 在板端重新编译成三种输入尺寸的 RKNN 模型 "
          "(INT8 卷积耗时只由形状决定, 与权重取值无关)。")
        W("")
        W("> ★ **口径说明**: 生产模型 `ebike_int8_rk2332.rknn` 是固定 640×640 编译的, "
          "改不了尺寸, 所以分辨率扫描用的是**同一架构的 1 类 YOLOv8n** 重新编译的三个模型"
          "(640 的那个与 `_bench` 的参考 INT8 模型字节数完全一致)。"
          "绝对帧率与生产模型不同(生产模型是 2 类 + 完整 DFL 头), "
          "但**\"分辨率 → 单帧耗时\"的缩放形状**才是这里要回答的问题。")
        W("")

        for tag, label in (("_1w", "单 worker / 单核(纯算力标尺)"),
                           ("_3w", "三核各 1 worker(总吞吐)")):
            sub = [(k, v) for k, v in res_rows if k[1].endswith(tag)]
            if not sub:
                continue
            W(f"### {label}")
            W("")
            W("| 分辨率 | 吞吐 FPS | 单帧推理 ms | 相对 640 的像素比 | 相对 640 的耗时比 | 相对 640 的 FPS 比 |")
            W("|---|---|---|---|---|---|")
            table = []
            for (layer, case), rs in sub:
                a = agg(rs)
                w = int(case.split("_")[1])
                table.append((w, a))
            table.sort()
            # 基准【永远取 640】, 不管表格按什么顺序排
            base = next(((w, a) for w, a in table if w == 640), table[0])
            for w, a in table:
                px = (w * w) / (base[0] * base[0])
                tr = a["det_wall_ms"][0] / base[1]["det_wall_ms"][0] if base[1]["det_wall_ms"][0] else 0
                fr = a["fps"][0] / base[1]["fps"][0] if base[1]["fps"][0] else 0
                W(f"| {w}×{w} | **{fmt(a['fps'][0])}** | {fmt(a['det_wall_ms'][0])} | "
                  f"{fmt(px, 3)} | {fmt(tr, 3)} | {fmt(fr, 3)} |")
            W("")

    if op_rows:
        W("## 额外验证项 2 —— 算子回落检查(CPU 回落统计)")
        W("")
        W("用 `RKNN_FLAG_COLLECT_PERF_MASK` + `RKNN_QUERY_PERF_DETAIL` 拿逐层明细, "
          "统计每个算子的执行目标。**该模式会逐层打点、显著拖慢推理, 只用于这一项。**")
        W("")
        W("| 模型 | 算子总数 | NPU | CPU 回落 | GPU | CPU 耗时 us | NPU 耗时 us | CPU 占比 | 回落算子类型 |")
        W("|---|---|---|---|---|---|---|---|---|")
        for (layer, case), rs in op_rows:
            e = rs[0]
            W(f"| `{case.replace('op_fallback_', '')}` | {e.get('total_ops', '-')} | "
              f"{e.get('npu_ops', '-')} | **{e.get('cpu_ops', '-')}** | {e.get('gpu_ops', '-')} | "
              f"{e.get('cpu_time_us', '-')} | {e.get('npu_time_us', '-')} | "
              f"{fmt(float(e.get('cpu_time_ratio_pct', 0)))}% | `{e.get('cpu_op_types', '')}` |")
        W("")

    # ---------------------------------------------------------------- 漏斗
    # ---------------------------------------------------------------- 漏斗
    W("## 分层效率漏斗 —— 每加一层, 掉多少")
    W("")
    W("把**同一条路径**(检测这一条主线)逐层叠上去, 就能直接看出瓶颈在哪一层。")
    W("")

    def best(cases, exclude=(), only=None):
        cand = []
        for (layer, case), rs in cases:
            if only and case not in only:
                continue
            if any(e in case for e in exclude):
                continue
            a = agg(rs)
            if a["fps"][0] > 0:
                cand.append((a["fps"][0], case, a))
        return max(cand) if cand else None

    funnel = []
    b = best(l1, exclude=("nopq",), only=("w9",))
    if b:
        funnel.append(("L1", "纯 NPU 裸推理 —— **算力天花板**", b[1], b[0], b[2]))
    b = best(l1, exclude=("nopq",), only=("w3",))
    if b:
        funnel.append(("L1", "纯 NPU, 每核 1 个 worker", b[1], b[0], b[2]))
    b = best(l3, only=("rga_3w",))
    if b:
        funnel.append(("L3", "+ RGA 预处理(1920x1080 NV12 -> 640x640 RGB888)", b[1], b[0], b[2]))
    if groups.get(("L5", "fullchain_4ch")):
        a = agg(groups[("L5", "fullchain_4ch")])
        funnel.append(("L5", "4 路全链路(解码 + RGA + 检测 + OCR + 后处理)",
                       "fullchain_4ch", a["fps"][0], a))
    if groups.get(("L4", "fullchain_1ch")):
        a = agg(groups[("L4", "fullchain_1ch")])
        funnel.append(("L4", "单路全链路(参照)", "fullchain_1ch", a["fps"][0], a))

    if funnel:
        W("| 层 | 含义 | 配置 | 吞吐 FPS | 相对天花板 |")
        W("|---|---|---|---|---|")
        top = funnel[0][3]
        for lyr, desc, case, fps, a in funnel:
            W(f"| **{lyr}** | {desc} | `{case}` | **{fmt(fps)}** | "
              f"{fmt(100.0 * fps / top, 1) if top else '-'}% |")
        W("")
        if l2:
            b = best(l2)
            if b:
                W(f"> **L2(检测 + OCR 双模型)是一条【分叉】, 不在上面的主线里**: "
                  f"它每帧要跑两次推理, 最优配置 `{b[1]}` = **{fmt(b[0])} FPS**"
                  f"(每帧 {fmt(b[2]['det_wall_ms'][0] + b[2]['ocr_wall_ms'][0])} ms = "
                  f"检测 {fmt(b[2]['det_wall_ms'][0])} + 识别 {fmt(b[2]['ocr_wall_ms'][0])})。")
                W("")

    # ---------------------------------------------------------------- 环境
    env_path = os.path.join(os.path.dirname(os.path.abspath(args.jsonl)), "env.txt")
    if os.path.exists(env_path):
        W("## 测量环境")
        W("")
        W("```")
        with open(env_path) as f:
            for line in f:
                W(line.rstrip())
        W("```")
        W("")

    W("---")
    W("")
    W(f"原始数据: `{args.jsonl}`")
    W("")

    with open(args.out, "w") as f:
        f.write("\n".join(L) + "\n")
    print(f"报告已写出: {args.out}  ({len(L)} 行)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
