#!/usr/bin/env python3
"""id_stability.py —— 分析跟踪器的 ID 稳定性(抽帧推理用)

输入: 主程序 --trace-ids 打出来的 TRACE 行, 一行一帧:
    TRACE f=<帧号> t=<时刻> <id>:<cx>,<cy> <id>:<cx>,<cy> ...

判定"ID 漂移"的规则:
    一个【新出现】的 ID, 如果在最近 W 帧内, 有一个【刚消失】的 ID,
    而两者最后/首次出现的位置相距不到 D 像素 —— 那就是同一个目标换了个 ID。

为什么这样判:
    · 目标真的离开画面再回来(隔了很久)不算漂移 —— W 窗口把它排除掉;
    · 两个不同的目标挨得很近也不会算 —— 位置差超过 D 就不算;
    · 同一个目标换了 ID, 位置一定还在附近(车不会瞬移) —— 这才是漂移。

输出:
    漂移次数、漂移率(每 100 帧)、每条轨迹的平均存活帧数、ID 总数。
"""
from __future__ import annotations

import argparse
import re
import sys
from collections import defaultdict

RE_TRACE = re.compile(r"^TRACE f=(\d+) t=([\d.]+)(.*)$")


def parse(path: str) -> list[dict]:
    frames = []
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = RE_TRACE.match(line.strip())
            if not m:
                continue
            fid = int(m.group(1))
            t = float(m.group(2))
            items = []
            for tok in m.group(3).split():
                if ":" not in tok:
                    continue
                idc, pos = tok.split(":", 1)
                try:
                    cx, cy = pos.split(",")
                    items.append((int(idc), float(cx), float(cy)))
                except ValueError:
                    continue
            frames.append({"f": fid, "t": t, "items": items})
    return frames


def analyze(frames: list[dict], window: int, dist: float) -> dict:
    # 每个 ID 的出现/消失
    first_seen: dict[int, int] = {}
    last_seen: dict[int, int] = {}
    last_pos: dict[int, tuple[float, float]] = {}
    seen_order: list[int] = []

    for fr in frames:
        ids = {i for i, _, _ in fr["items"]}
        for i, cx, cy in fr["items"]:
            if i not in first_seen:
                first_seen[i] = fr["f"]
                seen_order.append(i)
            last_seen[i] = fr["f"]
            last_pos[i] = (cx, cy)

    # 消失事件: 一个 ID 在 last_seen 之后再也没出现
    gone_at = {}
    for i, lf in last_seen.items():
        gone_at[i] = lf

    # 逐个新 ID 找"最近消失的、位置又很近的"旧 ID
    switches = []
    for i in seen_order:
        f0 = first_seen[i]
        best = None
        for j in seen_order:
            if j == i or j not in gone_at:
                continue
            if f0 - gone_at[j] < 0 or f0 - gone_at[j] > window:
                continue
            if j in first_seen and first_seen[j] > gone_at[j]:
                continue          # j 还没消失过
            if j in first_seen and first_seen[j] >= f0:
                continue          # j 不是"旧的"
            pj = last_pos.get(j)
            pi = last_pos.get(i)
            if pj is None or pi is None:
                continue
            d = ((pj[0] - pi[0]) ** 2 + (pj[1] - pi[1]) ** 2) ** 0.5
            if d <= dist and (best is None or gone_at[j] > best[1]):
                best = (j, gone_at[j], d)
        if best:
            switches.append({"new_id": i, "old_id": best[0], "frame": f0,
                             "gap": f0 - best[1], "dist": round(best[2], 1)})

    n_frames = len(frames)
    n_ids = len(seen_order)
    # 每条轨迹的平均存活帧数(用 hits 的近似: last_seen - first_seen + 1)
    lives = [last_seen[i] - first_seen[i] + 1 for i in seen_order]
    return {
        "frames": n_frames,
        "ids": n_ids,
        "switches": len(switches),
        "switch_per_100f": round(100.0 * len(switches) / max(1, n_frames), 2),
        "avg_life": round(sum(lives) / max(1, len(lives)), 1),
        "max_life": max(lives) if lives else 0,
        "min_life": min(lives) if lives else 0,
        "detail": switches[:12],
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("trace", help="TRACE 日志文件")
    ap.add_argument("--window", type=int, default=30,
                    help="漂移判定窗口(帧); 默认 30")
    ap.add_argument("--dist", type=float, default=60.0,
                    help="位置相近阈值(像素); 默认 60")
    args = ap.parse_args()

    frames = parse(args.trace)
    if not frames:
        print("没解析到 TRACE 行", file=sys.stderr)
        return 1
    r = analyze(frames, args.window, args.dist)
    print(f"帧数={r['frames']}  ID总数={r['ids']}  漂移次数={r['switches']}  "
          f"漂移率={r['switch_per_100f']}/100帧  轨迹寿命 平均{r['avg_life']}帧 "
          f"(最短{r['min_life']}/最长{r['max_life']})")
    if r["detail"]:
        print("  前几条漂移:", r["detail"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
