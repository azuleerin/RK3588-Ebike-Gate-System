#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_fork9_structure.py —— fork9 ONNX 的结构自检

必须满足:
  · 恰好 9 个输出
  · 顺序是 index = s*3 + {0:box, 1:cls, 2:sum}, s = 0/1/2 = stride 8/16/32
  · box  = [1, 4*DFL_LEN, H, W]   (原始 DFL logits, 未解码)
  · cls  = [1, nc, H, W]          (概率)
  · sum  = [1, 1, H, W]
  · 三个尺度的 H,W = 640/8, 640/16, 640/32
  · 三个尺度的 nc 一致

用法:
    verify_fork9_structure.py <fork9.onnx> [--imgsz 640]
"""
import argparse
import sys

import onnx

DFL_LEN = 16


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('--imgsz', type=int, default=640)
    ap.add_argument('--dfl-len', type=int, default=DFL_LEN)
    args = ap.parse_args()

    m = onnx.load(args.src)
    outs = []
    for o in m.graph.output:
        d = [x.dim_value for x in o.type.tensor_type.shape.dim]
        outs.append((o.name, d))

    ok = True
    print('输出数: %d (期望 9)' % len(outs))
    if len(outs) != 9:
        print('  ✘ 结构不对')
        return 1

    nc = None
    for s in range(3):
        stride = 8 * (1 << s)
        hw = args.imgsz // stride
        for k, role in enumerate(('box', 'cls', 'sum')):
            name, d = outs[s * 3 + k]
            if len(d) != 4:
                print('  ✘ 输出[%d] %s 维度数不对: %s' % (s * 3 + k, role, d))
                ok = False
                continue
            if d[0] != 1 or d[2] != hw or d[3] != hw:
                print('  ✘ 输出[%d] %s 形状不对: %s (期望 [1,?,%d,%d])'
                      % (s * 3 + k, role, d, hw, hw))
                ok = False
                continue
            if role == 'box':
                if d[1] != 4 * args.dfl_len:
                    print('  ✘ 输出[%d] box 通道数 %d != 4*DFL_LEN(%d)'
                          % (s * 3 + k, d[1], 4 * args.dfl_len))
                    ok = False
            elif role == 'cls':
                if nc is None:
                    nc = d[1]
                elif d[1] != nc:
                    print('  ✘ 输出[%d] cls 通道数 %d 与第一个尺度 %d 不一致'
                          % (s * 3 + k, d[1], nc))
                    ok = False
            else:
                if d[1] != 1:
                    print('  ✘ 输出[%d] sum 通道数 %d != 1' % (s * 3 + k, d[1]))
                    ok = False
            print('  输出[%d] %-4s %-22s %s' % (s * 3 + k, role, name, d))

    print('nc = %s' % nc)
    print('=' * 60)
    print('VERDICT: %s' % ('PASS ✔  fork9 结构正确' if ok else 'FAIL ✘'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
