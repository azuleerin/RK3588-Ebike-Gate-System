#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""hybrid_step2.py —— PP-OCR 混合量化第 2 步: 指定要保留 FP16 的层, 建最终模型

step1 产出的 quantization.cfg 是个 YAML, 结构:
    custom_quantize_layers:      <- 工具建议保留 float16 的层(带敏感度分数注释)
        <layer>: float16         # <sensitivity>
        ...
    quantize_parameters:         <- 逐层量化参数(原样保留)
        ...

本脚本只改写 custom_quantize_layers 那一段:
  · 按敏感度分数挑前 N 个层保留 float16, 其余全部走 int8
  · 然后 hybrid_quantization_step2(model, data, cfg) 建最终模型并导出

用法: hybrid_step2.py <step1_dir> <out.rknn> <keep_n>
       keep_n = 0  -> 纯 INT8(全部层都量化)
"""
import os
import re
import sys

LAYER_RE = re.compile(r"^\s+(\S+):\s*(\S+)\s*(?:#\s*([0-9.eE+-]+))?\s*$")
SECT = "quantize_parameters:"


def split_cfg(path):
    txt = open(path).read()
    i = txt.index(SECT)
    return txt[:i], txt[i:]


def parse_layers(head):
    """解析 custom_quantize_layers 段 -> [(name, score or None)]"""
    out = []
    for line in head.splitlines():
        if not line.startswith("    ") or line.strip().startswith("#"):
            continue
        m = LAYER_RE.match(line)
        if m and ":" in line:
            out.append((m.group(1), float(m.group(3)) if m.group(3) else None))
    return out


def main():
    step1, out_rknn, keep_n = sys.argv[1], sys.argv[2], int(sys.argv[3])
    work = os.path.join(step1, "step1_work")
    cfg_src = os.path.join(work, "ppocrv4_rec.quantization.cfg")
    model_in = os.path.join(work, "ppocrv4_rec.model")
    data_in = os.path.join(work, "ppocrv4_rec.data")
    for p in (cfg_src, model_in, data_in):
        if not os.path.exists(p):
            sys.exit(f"缺文件: {p}")

    head, tail = split_cfg(cfg_src)
    layers = parse_layers(head)
    print(f">>> 候选层 {len(layers)} 个")
    scored = [(s, n) for n, s in layers if s is not None]
    # 敏感度高的排前面; 没分数的排最后(但工具本来就选了它们)
    scored.sort(key=lambda t: -t[0])
    rest = [n for n, s in layers if s is None]
    chosen = [n for s, n in scored[:keep_n]]
    if keep_n > len(scored):
        chosen += rest[:keep_n - len(scored)]

    print(f">>> 保留 FP16 的层 {len(chosen)} 个 (敏感度前 {min(keep_n, len(scored))} + 无分数 {max(0, keep_n - len(scored))})")
    for n in chosen[:8]:
        print(f"      {n}")
    if len(chosen) > 8:
        print(f"      ... 还有 {len(chosen) - 8} 个")

    new_head = "custom_quantize_layers:\n"
    for n in chosen:
        new_head += f"    {n}: float16\n"
    new_cfg = os.path.join(step1, f"hybrid_cfg_keep{keep_n}.cfg")
    with open(new_cfg, "w") as f:
        f.write(new_head + tail)
    print(f">>> 写出配置: {new_cfg}")

    from rknn.api import RKNN
    rknn = RKNN(verbose=True)
    if rknn.config(mean_values=[[127.5, 127.5, 127.5]],
                   std_values=[[127.5, 127.5, 127.5]],
                   target_platform="rk3588") != 0:
        sys.exit("config failed")
    print(">>> hybrid_quantization_step2 ...")
    if rknn.hybrid_quantization_step2(model_in, data_in, new_cfg) != 0:
        sys.exit("step2 failed")
    if rknn.export_rknn(out_rknn) != 0:
        sys.exit("export failed")
    rknn.release()
    print(f">>> OK {out_rknn} ({os.path.getsize(out_rknn)} 字节)")


if __name__ == "__main__":
    main()
