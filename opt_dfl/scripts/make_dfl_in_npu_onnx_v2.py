#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""make_dfl_in_npu_onnx_v2.py —— v2 版: 把 DFL 解码留在 NPU 图里(softmax 轴放最后)

★ 为什么要 v2:
  v1 把 box logits 变形成 [1,4,16,H,W] 再对 axis=2 做 Softmax。
  ONNX (onnxruntime) 跑出来和 fork9+CPU 解码【完全一致】(max|Δ|=0.0000 px),
  但同一个 ONNX 转成 RKNN 之后框就全错了(匹配率 2.7%)。
  原因基本可以定位到【5 维张量的 softmax 轴】: RKNN 编译器会把前导维
  [1,4] 折叠成 batch 维 (4,16,H,W), 折叠后 axis=2 的含义从"16 个 bin"
  变成了"H 轴(80)", softmax 求错了轴 -> 距离全错。

  v2 的做法: 把 16 个 bin 放到【最后一维】, Softmax(axis=-1)。
  这样不管编译器怎么折叠前导维, softmax 的轴都不会被重新解释。
  中间量统一用 [1,H,W,4] 布局, 最后再 flatten 成 [1,8400,4]。

用法: make_dfl_in_npu_onnx_v2.py <fork9.onnx> <out.onnx>
"""
import sys
import numpy as np
import onnx
from onnx import helper, numpy_helper, TensorProto

DFL_LEN = 16
STRIDES = [8, 16, 32]


def shape_of(m, name):
    for vi in list(m.graph.output) + list(m.graph.value_info) + list(m.graph.input):
        if vi.name == name:
            return [d.dim_value for d in vi.type.tensor_type.shape.dim]
    return None


def main():
    src, dst = sys.argv[1], sys.argv[2]
    m = onnx.load(src)
    g = m.graph

    boxes, clss = [], []
    for o in g.output:
        s = shape_of(m, o.name)
        if s is None or len(s) != 4:
            continue
        c, h, w = s[1], s[2], s[3]
        if c == 4 * DFL_LEN:
            boxes.append((h, w, o.name))
        elif c != 1:
            clss.append((h, w, o.name, c))
    if len(boxes) != 3 or len(clss) != 3:
        raise SystemExit(f"不是 fork9 结构: box={len(boxes)} cls={len(clss)}")
    nc = clss[0][3]

    by_hw_box = {(h, w): n for h, w, n in boxes}
    by_hw_cls = {(h, w): n for h, w, n, _ in clss}

    nodes, inits = [], []
    box_flat, cls_flat = [], []

    def const(name, arr):
        inits.append(numpy_helper.from_array(np.asarray(arr, dtype=np.float32), name))
        return name

    def const_i(name, arr):
        inits.append(numpy_helper.from_array(np.asarray(arr, dtype=np.int64), name))
        return name

    for idx, stride in enumerate(STRIDES):
        h = w = 640 // stride
        bt = by_hw_box.get((h, w))
        ct = by_hw_cls.get((h, w))
        if bt is None or ct is None:
            raise SystemExit(f"stride={stride} 找不到对应输出")
        tag = f"s{idx}"

        # 1) [1,64,H,W] -> [1,4,16,H,W] -> [1,H,W,4,16]   (bin 放最后一维)
        shp = const_i(f"{tag}/shape_dfl", [1, 4, DFL_LEN, h, w])
        r0 = f"{tag}/rs5"
        add = nodes.append
        add(helper.make_node("Reshape", [bt, shp], [r0], name=f"{tag}/Reshape"))
        r1 = f"{tag}/rs5_hw"
        add(helper.make_node("Transpose", [r0], [r1], perm=[0, 3, 4, 1, 2],
                             name=f"{tag}/TransposeHW"))

        # 2) Softmax 在【最后一维】(16 个 bin) —— 轴不会被编译器重解释
        sm = f"{tag}/softmax"
        add(helper.make_node("Softmax", [r1], [sm], axis=4, name=f"{tag}/Softmax"))

        # 3) 与 [0..15] 加权求期望 -> [1,H,W,4]
        bins = const(f"{tag}/bins", np.arange(DFL_LEN, dtype=np.float32).reshape(1, 1, 1, 1, DFL_LEN))
        mul = f"{tag}/mul"
        add(helper.make_node("Mul", [sm, bins], [mul], name=f"{tag}/Mul"))
        axc = const_i(f"{tag}/axes", [4])
        red5 = f"{tag}/expect_5d"
        add(helper.make_node("ReduceSum", [mul, axc], [red5], name=f"{tag}/ReduceSum"))
        # opset13+ 的 ReduceSum 恒定 keepdims=1 -> [1,H,W,4,1], 要再 Squeeze 掉最后一轴
        sqc = const_i(f"{tag}/squeeze_axis", [4])
        red = f"{tag}/expect"
        add(helper.make_node("Squeeze", [red5, sqc], [red], name=f"{tag}/Squeeze"))

        # 4) 格点单位 -> 像素
        stc = const(f"{tag}/stride", float(stride))
        d = f"{tag}/dist_px"
        add(helper.make_node("Mul", [red, stc], [d], name=f"{tag}/MulStride"))

        # 5) 切出 l,t,r,b (最后一维), 各 [1,H,W,1]
        sl = []
        for c in range(4):
            a = const_i(f"{tag}/sl{c}_start", [c])
            b = const_i(f"{tag}/sl{c}_end", [c + 1])
            ax = const_i(f"{tag}/sl{c}_axes", [3])
            o = f"{tag}/sl{c}"
            add(helper.make_node("Slice", [d, a, b, ax], [o], name=f"{tag}/Slice{c}"))
            sl.append(o)

        # 6) 加回 anchor 格点位置: x1 = (j+0.5)*stride - d_l*stride
        gx = ((np.arange(w, dtype=np.float32) + 0.5) * stride).reshape(1, 1, w, 1)
        gy = ((np.arange(h, dtype=np.float32) + 0.5) * stride).reshape(1, h, 1, 1)
        grid_x = const(f"{tag}/grid_x", np.broadcast_to(gx, (1, h, w, 1)).copy())
        grid_y = const(f"{tag}/grid_y", np.broadcast_to(gy, (1, h, w, 1)).copy())

        x1 = f"{tag}/x1"; add(helper.make_node("Sub", [grid_x, sl[0]], [x1], name=f"{tag}/SubX1"))
        y1 = f"{tag}/y1"; add(helper.make_node("Sub", [grid_y, sl[1]], [y1], name=f"{tag}/SubY1"))
        x2 = f"{tag}/x2"; add(helper.make_node("Add", [grid_x, sl[2]], [x2], name=f"{tag}/AddX2"))
        y2 = f"{tag}/y2"; add(helper.make_node("Add", [grid_y, sl[3]], [y2], name=f"{tag}/AddY2"))

        half = const(f"{tag}/half", 0.5)
        pair = []
        for j, (a, b) in enumerate(((x1, x2), (y1, y2))):
            ssum = f"{tag}/pair{j}_sum"
            add(helper.make_node("Add", [a, b], [ssum], name=f"{tag}/Add{j}"))
            hm = f"{tag}/pair{j}_half"
            add(helper.make_node("Mul", [ssum, half], [hm], name=f"{tag}/MulHalf{j}"))
            pair.append(hm)
        dif = []
        for j, (a, b) in enumerate(((x1, x2), (y1, y2))):
            dd = f"{tag}/diff{j}"
            add(helper.make_node("Sub", [b, a], [dd], name=f"{tag}/Sub{j}"))
            dif.append(dd)

        xywh = f"{tag}/xywh"
        add(helper.make_node("Concat", pair + dif, [xywh], axis=3, name=f"{tag}/ConcatXYWH"))

        # 7) [1,H,W,4] -> [1,H*W,4]
        shb = const_i(f"{tag}/shape_boxflat", [1, h * w, 4])
        bf = f"{tag}/box_flat"
        add(helper.make_node("Reshape", [xywh, shb], [bf], name=f"{tag}/ReshapeBox"))
        box_flat.append(bf)

        # 8) cls [1,nc,H,W] -> [1,H,W,nc] -> [1,H*W,nc]
        tcp = f"{tag}/cls_t"
        add(helper.make_node("Transpose", [ct], [tcp], perm=[0, 2, 3, 1], name=f"{tag}/TransposeCls"))
        shc = const_i(f"{tag}/shape_clsflat", [1, h * w, nc])
        cf = f"{tag}/cls_flat"
        add(helper.make_node("Reshape", [tcp, shc], [cf], name=f"{tag}/ReshapeCls"))
        cls_flat.append(cf)

    bcat = "box_cat"; nodes.append(helper.make_node("Concat", box_flat, [bcat], axis=1, name="ConcatBox"))
    ccat = "cls_cat"; nodes.append(helper.make_node("Concat", cls_flat, [ccat], axis=1, name="ConcatCls"))
    bchw = "box_chw"; nodes.append(helper.make_node("Transpose", [bcat], [bchw], perm=[0, 2, 1], name="TransposeBoxCHW"))
    cchw = "cls_chw"; nodes.append(helper.make_node("Transpose", [ccat], [cchw], perm=[0, 2, 1], name="TransposeClsCHW"))

    n_anchor = sum((640 // s) ** 2 for s in STRIDES)
    del g.output[:]
    g.output.extend([
        helper.make_tensor_value_info(bchw, TensorProto.FLOAT, [1, 4, n_anchor]),
        helper.make_tensor_value_info(cchw, TensorProto.FLOAT, [1, nc, n_anchor]),
    ])

    g.node.extend(nodes)
    g.initializer.extend(inits)

    # DCE
    produced = {o.name for o in g.output}
    nodes_by_out = {}
    for n in g.node:
        for o in n.output:
            nodes_by_out.setdefault(o, n)
    keep, stack = set(), list(produced)
    while stack:
        nm = stack.pop()
        n = nodes_by_out.get(nm)
        if n is None or id(n) in keep:
            continue
        keep.add(id(n))
        stack.extend(n.input)
    dead = [n for n in g.node if id(n) not in keep]
    if dead:
        print(f"  DCE 掉 {len(dead)} 个无用节点")
        kn = [n for n in g.node if id(n) in keep]
        del g.node[:]
        g.node.extend(kn)

    used = set()
    for n in g.node:
        used.update(n.input)
    used.update(i.name for i in g.input)
    used.update(i.name for i in g.output)
    inits_new = [t for t in g.initializer if t.name in used]
    del g.initializer[:]
    g.initializer.extend(inits_new)

    del g.input[:]
    g.input.extend([helper.make_tensor_value_info("images", TensorProto.FLOAT, [1, 3, 640, 640])])

    m = onnx.shape_inference.infer_shapes(m)
    onnx.checker.check_model(m)
    onnx.save(m, dst)
    print(f"  -> {dst}")
    print(f"  输出: box[1,4,{n_anchor}] / cls[1,{nc},{n_anchor}]  节点数 {len(g.node)}")


if __name__ == "__main__":
    main()
