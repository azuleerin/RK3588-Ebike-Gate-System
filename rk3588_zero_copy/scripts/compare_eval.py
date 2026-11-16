#!/usr/bin/env python3
"""compare_eval.py —— 前后两次 rk_eval 的逐图对比(框数/分数/坐标必须完全一致)

用法:
    python3 scripts/compare_eval.py <before.json> <after.json> [名称]

判定标准(任务四的回归要求):
    · 逐图框数完全一致
    · 框数总和一致
    · (更严) 每个框的类别/分数/坐标逐项一致
"""
import json
import sys


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    a_path, b_path = sys.argv[1], sys.argv[2]
    name = sys.argv[3] if len(sys.argv) > 3 else f"{a_path} vs {b_path}"

    a = load(a_path)
    b = load(b_path)

    ra = {r["file"]: r.get("dets", []) for r in a.get("results", [])}
    rb = {r["file"]: r.get("dets", []) for r in b.get("results", [])}

    total_a = sum(len(v) for v in ra.values())
    total_b = sum(len(v) for v in rb.values())

    only_a = set(ra) - set(rb)
    only_b = set(rb) - set(ra)

    diff_count = []
    diff_detail = []
    for f in sorted(set(ra) & set(rb)):
        da, db = ra[f], rb[f]
        if len(da) != len(db):
            diff_count.append((f, len(da), len(db)))
            continue
        for i, (x, y) in enumerate(zip(da, db)):
            if x.get("c") != y.get("c"):
                diff_detail.append((f, i, "cls", x.get("c"), y.get("c")))
            if abs(float(x.get("s", 0)) - float(y.get("s", 0))) > 1e-6:
                diff_detail.append((f, i, "score", x.get("s"), y.get("s")))
            bx, by = x.get("b", []), y.get("b", [])
            if len(bx) != len(by):
                diff_detail.append((f, i, "box-len", bx, by))
            else:
                for k in range(len(bx)):
                    if abs(float(bx[k]) - float(by[k])) > 1e-3:
                        diff_detail.append((f, i, f"box[{k}]", bx[k], by[k]))

    print("=" * 68)
    print(f" 前后对比: {name}")
    print("=" * 68)
    print(f"  改动前: {len(ra)} 张图, {total_a} 个检测   ({a_path})")
    print(f"  改动后: {len(rb)} 张图, {total_b} 个检测   ({b_path})")
    if only_a:
        print(f"  ⚠ 只有改动前有的图: {len(only_a)} 张")
    if only_b:
        print(f"  ⚠ 只有改动后有的图: {len(only_b)} 张")

    n_diff = len(diff_count) + len(diff_detail)
    if n_diff == 0 and not only_a and not only_b and total_a == total_b:
        print("\n  ✅ 完全一致: 逐图框数、框数总和、每个框的类别/分数/坐标全部相同")
        print("     (cls 空间判别对现有模型零影响 —— 符合预期)")
        return 0

    print(f"\n  ❌ 发现 {n_diff} 处差异:")
    for f, ca, cb in diff_count[:20]:
        print(f"     框数不同 {f}: {ca} -> {cb}")
    for row in diff_detail[:20]:
        print(f"     {row[0]} #{row[1]} {row[2]}: {row[3]} -> {row[4]}")
    if n_diff > 40:
        print(f"     ... 还有 {n_diff - 40} 处")
    return 1


if __name__ == "__main__":
    sys.exit(main())
