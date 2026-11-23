#!/usr/bin/env python3
"""merge_results.py —— 把多次压测的 JSONL 合并成一份

为什么要合并:
    这块板子可能被别的会话同时用来跑压测(实测遇到过: 另一个会话的
    RKNN 量化导出任务会【占用 NPU】), 于是某一次跑出来的样本会被拖慢。
    把多次跑的样本合到一起, 再用"干净窗口"规则挑一遍, 就能把
    干净样本凑齐 —— 这是仓库里 gate_ab_clean.py 用的同一套路。

用法:
    python3 merge_results.py out.jsonl in1.jsonl in2.jsonl ...

可选:
    --drop-case <name>   丢掉某个用例(可重复)
"""
from __future__ import annotations

import argparse
import json
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("--drop-case", action="append", default=[])
    args = ap.parse_args()

    kept = 0
    total = 0
    with open(args.out, "w") as out:
        for f in args.inputs:
            try:
                fh = open(f)
            except OSError:
                print(f"[跳过] 打不开 {f}", file=sys.stderr)
                continue
            with fh:
                for line in fh:
                    line = line.strip()
                    if not line.startswith("{"):
                        continue
                    total += 1
                    try:
                        r = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    case = r.get("case", "")
                    if case in args.drop_case:
                        continue
                    out.write(json.dumps(r, ensure_ascii=False) + "\n")
                    kept += 1
    print(f"合并完成: {kept}/{total} 个样本 -> {args.out}")
    if args.drop_case:
        print(f"丢弃的用例: {', '.join(args.drop_case)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
