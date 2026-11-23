#!/usr/bin/env python3
"""inspect_onnx.py —— 打印 ONNX 的输入/输出/末端节点结构 (只读)"""
import sys
import onnx
from onnx import shape_inference

path = sys.argv[1]
m = onnx.load(path)
try:
    m = shape_inference.infer_shapes(m)
except Exception as e:
    print("shape_inference 跳过:", e)

print("=== 输入 ===")
for i in m.graph.input:
    dims = [d.dim_value if d.dim_value else d.dim_param for d in i.type.tensor_type.shape.dim]
    print(f"  {i.name}: {dims}")

print("=== 输出 ===")
for o in m.graph.output:
    dims = [d.dim_value if d.dim_value else d.dim_param for d in o.type.tensor_type.shape.dim]
    print(f"  {o.name}: {dims}")

print(f"=== 节点数: {len(m.graph.node)} ===")
print("=== 最后 25 个节点 ===")
for n in m.graph.node[-25:]:
    print(f"  {n.op_type:14s} name={n.name[:52]:52s} in={list(n.input)[:4]} out={list(n.output)[:3]}")
