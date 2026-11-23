#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""build_ebike_arch_fork9.py —— 造一个【与生产 ebike 模型结构完全一致】的 fork9 ONNX

★ 为什么要自己造, 而不能直接用现成的 fork9 ONNX
  仓库里现成的 fork9 ONNX(yolov8n_optimized.onnx / check0_base_optimize.onnx)是
  COCO 的 nc=80 / c3=80。生产 ebike 模型是 nc=2 / c3=64 —— 检测头 cls 分支不一样。

★ 生产 ebike 模型的结构是怎么确定的(不是猜的)
  用 RKNN 运行时的逐算子表(RKNN_QUERY_PERF_DETAIL, 见 audit/model_audit.py)
  直接读出生产模型 ebike_int8_rk2332.rknn 的每个卷积的权重形状:

    model.22/cv2.0/cv2.0.0/conv/Conv  (1,64,80,80),(64,64,3,3),(64) -> (1,64,80,80)
    model.22/cv2.0/cv2.0.1/conv/Conv  (1,64,80,80),(64,64,3,3),(64) -> (1,64,80,80)
    model.22/cv2.0/cv2.0.2/Conv       (1,64,80,80),(64,64,1,1),(64) -> (1,64,80,80)
    model.22/cv3.0/cv3.0.0/conv/Conv  (1,64,80,80),(64,64,3,3),(64) -> (1,64,80,80)
    model.22/cv3.0/cv3.0.1/conv/Conv  (1,64,80,80),(64,64,3,3),(64) -> (1,64,80,80)
    model.22/cv3.0/cv3.0.2/Conv       (1,64,80,80),(2,64,1,1),(2)   -> (1,2,80,80)
    (P4 同构 C_in=128, P5 同构 C_in=256)

  ⇒ 与模板(yolov8n)逐项对比, 【唯一差别是 cls 分支 cv3】:
       cv2 分支(出 64 通道原始 DFL logits): 完全一致
       cv3 分支:  c3 隐藏宽度 80 -> 64, 输出 nc 80 -> 2

  所以本脚本只重写 cv3 的 9 个权重(3 尺度 × 3 层) + 对应 bias, 图结构不动。

★ 权重取值的说明(重要)
  本脚本从模板 ONNX 里【截取】得到 cv3 的权重(取前 64 / 前 2 个通道),
  这只是让张量形状与生产 ebike 模型【完全一致】, 数值并不是算法组训练出来的
  ebike 权重(那个权重只存在于 best_split.onnx, 本机拿不到 —— 见
  docs/00_source_not_available.md)。

  ⇒ 本模型可以用来回答【结构性问题】(NPU 耗时、图结构、解码路径),
    不能用来回答【数值问题】(与生产 ebike 的精度对比)。
    数值问题要等 best_split.onnx 到位后重跑一遍 build_ebike_fork9.sh。

用法:
    build_ebike_arch_fork9.py <template_fork9.onnx> <out.onnx> [nc] [c3]
"""
import sys
import onnx
from onnx import helper, numpy_helper, TensorProto

DFL_LEN = 16


def shape_of(m, name):
    for vi in list(m.graph.output) + list(m.graph.value_info) + list(m.graph.input):
        if vi.name == name:
            return [d.dim_value for d in vi.type.tensor_type.shape.dim]
    return None


def main():
    src, dst = sys.argv[1], sys.argv[2]
    nc = int(sys.argv[3]) if len(sys.argv) > 3 else 2
    c3 = int(sys.argv[4]) if len(sys.argv) > 4 else 64
    m = onnx.load(src)
    g = m.graph
    inits = {i.name: i for i in g.initializer}

    # ---- 1) 确认是 fork9 形态 ----
    clss = []
    for o in g.output:
        s = shape_of(m, o.name)
        if s is None or len(s) != 4:
            raise SystemExit(f"输出 {o.name} 形状异常: {s}")
        c, h, w = s[1], s[2], s[3]
        if c not in (4 * DFL_LEN, 1):
            clss.append((h, w, o.name, c))
    if len(clss) != 3:
        raise SystemExit(f"不是 fork9 结构: cls 输出 {len(clss)} 个")
    cur_nc = clss[0][3]
    print(f"  模板 nc={cur_nc} -> 目标 nc={nc}, c3={c3}")

    # ---- 2) 重写 cv3 分支的 3 层权重 (3 尺度) ----
    #   cv3.{i}.0.conv.weight [c3_old, C_in, 3, 3] -> [c3, C_in, 3, 3]
    #   cv3.{i}.1.conv.weight [c3_old, c3_old, 3,3] -> [c3, c3, 3, 3]
    #   cv3.{i}.2.weight      [nc_old, c3_old, 1, 1] -> [nc, c3, 1, 1]
    rewritten = []
    for i in range(3):
        # (名字, {要截断的轴: 截到几}, 期望的完整形状, 标签)
        specs = [
            (f'model.22.cv3.{i}.0.conv.weight', {0: c3}, None, 'c3 第1层'),
            (f'model.22.cv3.{i}.0.conv.bias', {0: c3}, None, 'c3 第1层 bias'),
            (f'model.22.cv3.{i}.1.conv.weight', {0: c3, 1: c3}, None, 'c3 第2层'),
            (f'model.22.cv3.{i}.1.conv.bias', {0: c3}, None, 'c3 第2层 bias'),
            (f'model.22.cv3.{i}.2.weight', {0: nc, 1: c3}, None, 'c3 输出层'),
            (f'model.22.cv3.{i}.2.bias', {0: nc}, None, 'c3 输出层 bias'),
        ]
        for name, cut, _expect, tag in specs:
            if name not in inits:
                raise SystemExit(f"模板里没有 {name}")
            arr = onnx.numpy_helper.to_array(inits[name])
            sl = []
            for ax in range(arr.ndim):
                sl.append(slice(0, cut[ax]) if ax in cut else slice(None))
            new = arr[tuple(sl)].copy()
            # 校验: 第0维必须被截到预期宽度
            for ax, n in cut.items():
                if new.shape[ax] != n:
                    raise SystemExit(f"{name}: 轴{ax} 截取后是 {new.shape[ax]}, 期望 {n} (原 {arr.shape})")
            rewritten.append((name, inits[name], numpy_helper.from_array(new, name)))
            print(f"    {name}: {list(arr.shape)} -> {list(new.shape)}   [{tag}]")

    for name, old, new in rewritten:
        idx = list(g.initializer).index(old)
        g.initializer.remove(old)
        g.initializer.insert(idx, new)

    # ---- 3) 改写 cls 输出的 shape (【原地改】, 绝不能 remove/append ——
    #         输出的【顺序】是契约: index = s*3 + {0:box, 1:cls, 2:sum} ----
    cls_names = {c[2] for c in clss}
    for o in g.output:
        if o.name in cls_names:
            dims = o.type.tensor_type.shape.dim
            assert len(dims) == 4, (o.name, len(dims))
            dims[1].dim_value = nc
    del g.value_info[:]

    # ---- 4) graph.input 只留 images ----
    keep = [vi for vi in g.input if vi.name == 'images']
    del g.input[:]
    g.input.extend(keep)

    m = onnx.shape_inference.infer_shapes(m, strict_mode=False)
    onnx.checker.check_model(m)
    onnx.save(m, dst)

    print(f"  -> {dst}")
    for o in m.graph.output:
        print("  输出:", o.name, [d.dim_value for d in o.type.tensor_type.shape.dim])


if __name__ == '__main__':
    main()
