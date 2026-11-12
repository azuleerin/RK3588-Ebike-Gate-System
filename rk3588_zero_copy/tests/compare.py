#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""compare.py —— 对比 C++ 与 Python 两份输出(带浮点容差)。

用法: python3 compare.py out_cpp.txt out_py.txt
退出码 0 = 通过, 1 = 有差异
"""
import sys


def parse(path):
    rows = []
    for line in open(path, encoding="utf-8"):
        parts = line.split()
        if parts:
            rows.append(parts)
    return rows


def main():
    if len(sys.argv) < 3:
        print("用法: compare.py <cpp.txt> <py.txt>")
        return 2
    a = parse(sys.argv[1])
    b = parse(sys.argv[2])

    if len(a) != len(b):
        print(f"[FAIL] 行数不同: cpp={len(a)} py={len(b)}")
        # 打印第一处不同的行, 方便定位
        for i in range(min(len(a), len(b))):
            if a[i] != b[i]:
                print(f"       第 {i+1} 行开始不同: cpp={a[i]} py={b[i]}")
                break
        return 1

    tol = 1e-4          # 相对/绝对容差
    bad = 0
    for i, (ra, rb) in enumerate(zip(a, b)):
        if ra[0] != rb[0]:
            print(f"[FAIL] 第 {i+1} 行 tag 不同: {ra[0]} vs {rb[0]}")
            bad += 1
            continue
        if len(ra) != len(rb):
            print(f"[FAIL] 第 {i+1} 行字段数不同: {ra} vs {rb}")
            bad += 1
            continue
        for j in range(1, len(ra)):
            try:
                va, vb = float(ra[j]), float(rb[j])
            except ValueError:
                if ra[j] != rb[j]:
                    print(f"[FAIL] 第 {i+1} 行字段 {j} 不同: {ra[j]} vs {rb[j]}")
                    bad += 1
                continue
            denom = max(abs(va), abs(vb), 1.0)
            if abs(va - vb) / denom > tol:
                print(f"[FAIL] 第 {i+1} 行字段 {j} 数值超差: {va} vs {vb} "
                      f"(相对 {abs(va-vb)/denom:.3e})")
                bad += 1

    if bad:
        print(f"\n[FAIL] 共 {bad} 处差异")
        return 1
    print(f"[PASS] {len(a)} 行全部一致 (容差 {tol:g})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
