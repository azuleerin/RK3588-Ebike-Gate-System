#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""summarize_npu_ab.py —— 把 audit/model_audit.py 的逐算子表汇总成 NPU 耗时 A/B 表

用法:
    summarize_npu_ab.py <logs 目录> <模型1.rknn> [模型2.rknn ...]
"""
import json
import os
import re
import sys


def load(model, logs):
    tag = os.path.basename(model).replace('.rknn', '')
    p = os.path.join(logs, f'npu_ab_{tag}.json')
    if not os.path.exists(p):
        raise FileNotFoundError(p)
    d = json.load(open(p))
    rt = d['runtime']
    ops = rt['ops']
    npu = [o for o in ops if o.get('target') == 'NPU']
    cpu = [o for o in ops if o.get('target') == 'CPU']
    return dict(
        tag=tag,
        n_ops=len(ops),
        n_npu=len(npu),
        n_cpu=len(cpu),
        npu_us=sum(int(o.get('time_us') or 0) for o in npu),
        cpu_us=sum(int(o.get('time_us') or 0) for o in cpu),
        run_us=rt.get('driver_run_us'),
        outputs=[(o['name'], o['dims']) for o in rt.get('outputs', [])],
        dfl_us=sum(int(o.get('time_us') or 0) for o in ops
                   if re.search(r'model\.22/(dfl|Slice_2sp|Sub|Add_1|Sub_1|Add_2|'
                                r'Div_1_2mul|Concat_4|Mul_2|Reshape_1_output_0_rs|'
                                r'Concat_3|Concat|Reshape|Split|Sigmoid)',
                                o.get('fullname') or '')),
    )


def main():
    logs = sys.argv[1]
    models = sys.argv[2:]
    rows = []
    for m in models:
        if not os.path.exists(m):
            continue
        tag = os.path.basename(m).replace('.rknn', '')
        p = os.path.join(logs, f'npu_ab_{tag}.json')
        if not os.path.exists(p):
            print(f'!! 缺少 {p}, 跳过 {tag}')
            continue
        rows.append(load(m, logs))
    if not rows:
        print('没有可汇总的模型')
        return 1

    base = rows[0]
    print('=' * 96)
    print('NPU 耗时 A/B   (NPU @1GHz, RKNN_QUERY_PERF_DETAIL, 同一块板)')
    print('=' * 96)
    print('%-34s %12s %12s %12s' % ('模型', '算子数', 'NPU算子(us)', '单次推理(us)'))
    for r in rows:
        print('%-34s %12d %12d %12s'
              % (r['tag'], r['n_ops'], r['npu_us'], r['run_us']))
    print('-' * 96)
    for r in rows[1:]:
        dn = r['npu_us'] - base['npu_us']
        dr = (r['run_us'] - base['run_us']) if (r['run_us'] and base['run_us']) else None
        print('Δ(%s → %s): NPU算子 %+d us (%+.1f%%)   单次推理 %s'
              % (base['tag'], r['tag'], dn,
                 100.0 * dn / max(1, base['npu_us']),
                 ('%+d us (%+.1f%%)' % (dr, 100.0 * dr / base['run_us'])) if dr else 'n/a'))
    print('-' * 96)
    for r in rows:
        print('%-34s CPU算子 %5d 个 / %6d us   输出 %d 个' %
              (r['tag'], r['n_cpu'], r['cpu_us'], len(r['outputs'])))
    print('=' * 96)
    return 0


if __name__ == '__main__':
    sys.exit(main())
