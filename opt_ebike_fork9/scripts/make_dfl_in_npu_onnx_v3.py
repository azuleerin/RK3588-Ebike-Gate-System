#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""make_dfl_in_npu_onnx_v3.py —— 由瑞芯微 fork9 ONNX 反向构造「DFL 解码留在 NPU 里」的 ONNX

★ 为什么这样做(而不是重新跑 ultralytics 导出):
  同一个模型的两臂必须【权重完全相同、量化集完全相同、转换配方完全相同】,
  唯一变量才是「DFL 解码跑在 NPU 还是 CPU」。
  用 fork9 的 ONNX 当底, 原地把 DFL 解码塞回图里, 天然保证只有这一个变量。

  fork9 ONNX 的 9 个输出 = 3 尺度 × [box(原始 DFL logits), cls(概率), sum(概率和)]
  本脚本把每个尺度的 box logits 在图内解码成 cx,cy,w,h(像素), 再拼成
  形态 C 的两个输出: box[1,4,8400] / cls[1,nc,8400] —— 与生产模型
  ebike_int8_rk2332.rknn 的形态完全一致。

用法:
    make_dfl_in_npu_onnx.py <fork9.onnx> <out.onnx>
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
    _opset = max(o.version for o in m.opset_import if o.domain in ('', 'ai.onnx'))
    print(f"  源 ONNX opset = {_opset}")

    # ---- 1) 按形状把 9 个输出归类 ----
    boxes, clss, sums = [], [], []
    for o in g.output:
        s = shape_of(m, o.name)
        if s is None or len(s) != 4:
            raise SystemExit(f"输出 {o.name} 形状异常: {s}")
        c, h, w = s[1], s[2], s[3]
        if c == 4 * DFL_LEN:
            boxes.append((h, w, o.name))
        elif c == 1:
            sums.append((h, w, o.name))
        else:
            clss.append((h, w, o.name, c))
    if len(boxes) != 3 or len(clss) != 3:
        raise SystemExit(f"不是 fork9 结构: box={len(boxes)} cls={len(clss)}")
    nc = clss[0][3]
    if any(c[3] != nc for c in clss):
        raise SystemExit("三个尺度的 nc 不一致")

    by_hw_box = {(h, w): n for h, w, n in boxes}
    by_hw_cls = {(h, w): n for h, w, n, _ in clss}

    nodes, inits = [], []
    box_flat, cls_flat = [], []

    def add(n):
        nodes.append(n)

    def const(name, arr):
        t = numpy_helper.from_array(np.asarray(arr, dtype=np.float32), name)
        inits.append(t)
        return name

    def const_i(name, arr):
        """形状/轴这类索引常量必须是整型 (Reshape/Slice/ReduceSum 的输入类型要求)"""
        t = numpy_helper.from_array(np.asarray(arr, dtype=np.int64), name)
        inits.append(t)
        return name

    for idx, stride in enumerate(STRIDES):
        # 尺度对应关系: 640/stride = 特征图边长
        h = w = 640 // stride
        bt = by_hw_box.get((h, w))
        ct = by_hw_cls.get((h, w))
        if bt is None or ct is None:
            raise SystemExit(f"stride={stride} 找不到对应的 box/cls 输出")
        tag = f"s{idx}"

        # 1) [1,64,H,W] -> [1,4,16,H,W]
        rs = f"{tag}/reshape_dfl"
        shape_c = const_i(f"{tag}/shape_dfl", [1, 4, DFL_LEN, h, w])
        add(helper.make_node("Reshape", [bt, shape_c], [rs], name=f"{tag}/Reshape"))

        # 2) softmax over 16 bins
        # ★★ opset <= 12 的 Softmax 有【legacy 2D 强制转换】: ONNX 规定 opset 1~12 的
        #    Softmax 会先把输入压成 [d0*..*d(axis-1), d(axis)*..*d(n-1)] 再对第 1 维做
        #    softmax。对 5 维张量 axis=2 来说, 实际是在 16*80*80 = 102400 个元素上做
        #    softmax -> 每个元素都趋近 0(实测 2.7e-06), DFL 期望值直接变成 0,
        #    框全是零宽, 而且【全程不报任何错】。
        #    opset 13 起这个强制转换被取消, axis 就是 axis。
        #    ⇒ 对 opset <= 12: 先把 16 个 bin 转到【最后一维】再 softmax, 再转回来。
        sm = f"{tag}/softmax"
        if _opset <= 12:
            tr_a = f"{tag}/bins_last"
            sm_last = f"{tag}/softmax_last"
            add(helper.make_node("Transpose", [rs], [tr_a], perm=[0, 1, 3, 4, 2],
                                 name=f"{tag}/TransposeToLast"))
            add(helper.make_node("Softmax", [tr_a], [sm_last], axis=-1,
                                 name=f"{tag}/Softmax"))
            add(helper.make_node("Transpose", [sm_last], [sm], perm=[0, 1, 4, 2, 3],
                                 name=f"{tag}/TransposeBack"))
        else:
            add(helper.make_node("Softmax", [rs], [sm], axis=2, name=f"{tag}/Softmax"))

        # 3) 与 [0..15] 加权 -> 求期望
        # [1,1,16,1,1] —— 广播到 softmax 输出的第 2 轴(16 个 bin); 不能用 [16]
        #   (ONNX 广播从右对齐, [16] 会去对齐最后一轴 W)
        binv = const(f"{tag}/bins", np.arange(DFL_LEN, dtype=np.float32).reshape(1, 1, DFL_LEN, 1, 1))
        mul = f"{tag}/mul_bins"
        add(helper.make_node("Mul", [sm, binv], [mul], name=f"{tag}/Mul"))
        axc = const_i(f"{tag}/axes", [2])
        rs2_5d = f"{tag}/dfl_expect_5d"
        # ★ ReduceSum 的 axes 形态随 opset 变:
        #     opset <= 12 : axes 是【属性】
        #     opset 13~17 : axes 是【第二个输入】
        #     opset >= 18 : axes 又是【属性】
        #   本仓库模板 yolov8n_optimized.onnx 是 opset 12 -> 必须用属性形式。
        if 13 <= _opset <= 17:
            add(helper.make_node("ReduceSum", [mul, axc], [rs2_5d],
                                 name=f"{tag}/ReduceSum"))
        else:
            add(helper.make_node("ReduceSum", [mul], [rs2_5d], axes=[2],
                                 name=f"{tag}/ReduceSum"))
        # opset13+ 的 ReduceSum 恒定 keepdims=1 -> [1,4,1,H,W], 要再 Squeeze 掉第 2 轴
        sqc = const_i(f"{tag}/squeeze_axis", [2])
        rs2 = f"{tag}/dfl_expect"
        # ★ Squeeze 的 axes 形态同 ReduceSum: opset<=12 是属性, >=13 是输入
        if _opset >= 13:
            add(helper.make_node("Squeeze", [rs2_5d, sqc], [rs2], name=f"{tag}/Squeeze"))
        else:
            add(helper.make_node("Squeeze", [rs2_5d], [rs2], axes=[2],
                                 name=f"{tag}/Squeeze"))

        # 4) grid 单位 -> 像素 (乘 stride)
        stc = const(f"{tag}/stride", float(stride))
        dp = f"{tag}/dist_px"
        add(helper.make_node("Mul", [rs2, stc], [dp], name=f"{tag}/MulStride"))

        # 5) l,t,r,b -> cx,cy,w,h   (切片用 Slice 节点)
        sl = []
        for c in range(4):
            a = const_i(f"{tag}/sl{c}_start", [c])
            b = const_i(f"{tag}/sl{c}_end", [c + 1])
            ax = const_i(f"{tag}/sl{c}_axes", [1])   # 通道维
            o = f"{tag}/sl{c}"
            add(helper.make_node("Slice", [dp, a, b, ax], [o], name=f"{tag}/Slice{c}"))
            sl.append(o)

        # ★ 关键: DFL 解出来的是【相对 anchor 的距离】, 必须加回 anchor 自身的格点位置:
        #     x1 = (j + 0.5) * stride - d_l * stride
        #     x2 = (j + 0.5) * stride + d_r * stride
        #   (ultralytics 口径: anchor 中心在格点 (j+0.5), 距离是格点单位)
        gx_np = ((np.arange(w, dtype=np.float32) + 0.5) * stride).reshape(1, 1, 1, w)
        gy_np = ((np.arange(h, dtype=np.float32) + 0.5) * stride).reshape(1, 1, h, 1)
        grid_x = const(f"{tag}/grid_x", np.broadcast_to(gx_np, (1, 1, h, w)).copy())
        grid_y = const(f"{tag}/grid_y", np.broadcast_to(gy_np, (1, 1, h, w)).copy())

        # x1 = grid_x - d_l ; x2 = grid_x + d_r ; y1 = grid_y - d_t ; y2 = grid_y + d_b
        x1 = f"{tag}/x1"
        add(helper.make_node("Sub", [grid_x, sl[0]], [x1], name=f"{tag}/SubX1"))
        x2 = f"{tag}/x2"
        add(helper.make_node("Add", [grid_x, sl[2]], [x2], name=f"{tag}/AddX2"))
        y1 = f"{tag}/y1"
        add(helper.make_node("Sub", [grid_y, sl[1]], [y1], name=f"{tag}/SubY1"))
        y2 = f"{tag}/y2"
        add(helper.make_node("Add", [grid_y, sl[3]], [y2], name=f"{tag}/AddY2"))

        half = const(f"{tag}/half", 0.5)
        pair = []
        for j, (a, b) in enumerate(((x1, x2), (y1, y2))):
            ssum = f"{tag}/pair{j}_sum"
            add(helper.make_node("Add", [a, b], [ssum], name=f"{tag}/Add{j}"))
            halfm = f"{tag}/pair{j}_half"
            add(helper.make_node("Mul", [ssum, half], [halfm], name=f"{tag}/MulHalf{j}"))
            pair.append(halfm)
        dif = []
        for j, (a, b) in enumerate(((x1, x2), (y1, y2))):
            d = f"{tag}/diff{j}"
            add(helper.make_node("Sub", [b, a], [d], name=f"{tag}/Sub{j}"))
            dif.append(d)

        xywh = f"{tag}/xywh"
        add(helper.make_node("Concat", pair + dif, [xywh], axis=1, name=f"{tag}/ConcatXYWH"))

        # 6) [1,4,H,W] -> [1,4,H*W]  (纯 reshape, 不做 transpose)
        shp = const_i(f"{tag}/shape_boxflat", [1, 4, h * w])
        bf = f"{tag}/box_flat"
        add(helper.make_node("Reshape", [xywh, shp], [bf], name=f"{tag}/ReshapeBox"))

        # 7) cls [1,nc,H,W] -> [1,nc,H*W]  (纯 reshape)
        shc = const_i(f"{tag}/shape_clsflat", [1, nc, h * w])
        cf = f"{tag}/cls_flat"
        add(helper.make_node("Reshape", [ct, shc], [cf], name=f"{tag}/ReshapeCls"))

        box_flat.append(bf)
        cls_flat.append(cf)

    # ---- 2) 三尺度拼起来 -> [1,8400,4] / [1,8400,nc] ----
    bcat = "box_cat"
    add(helper.make_node("Concat", box_flat, [bcat], axis=2, name="ConcatBox"))
    ccat = "cls_cat"
    add(helper.make_node("Concat", cls_flat, [ccat], axis=2, name="ConcatCls"))

    # ---- 3) 转成形态C 的 CHW 布局: box[1,4,8400] / cls[1,nc,8400] ----
    # ★ 不做收尾 Transpose: 收尾的 Transpose 会让 RKNN 编译器把
    #   [1,4,8400] 当成 NHWC 存储, 运行期再按 NCHW 读 -> 数据顺序全乱。
    #   直接让输出张量物理上就是 NCHW。
    bchw = bcat
    cchw = ccat

    n_anchor = sum((640 // s) ** 2 for s in STRIDES)
    del g.output[:]
    g.output.extend([
        helper.make_tensor_value_info(bchw, TensorProto.FLOAT, [1, 4, n_anchor]),
        helper.make_tensor_value_info(cchw, TensorProto.FLOAT, [1, nc, n_anchor]),
    ])

    # ---- 4) 挂上新节点 + DCE 掉没人用的老节点(sum 那条分支) ----
    g.node.extend(nodes)
    g.initializer.extend(inits)

    produced = {o.name for o in g.output}
    keep = set()
    stack = list(produced)
    nodes_by_out = {}
    for n in g.node:
        for o in n.output:
            nodes_by_out.setdefault(o, n)
    while stack:
        name = stack.pop()
        n = nodes_by_out.get(name)
        if n is None or id(n) in keep:
            continue
        keep.add(id(n))
        stack.extend(n.input)

    dead = [n for n in g.node if id(n) not in keep]
    if dead:
        print(f"  DCE 掉 {len(dead)} 个无用节点 "
              f"(例: {[n.op_type for n in dead[:6]]})")
        keep_nodes = [n for n in g.node if id(n) in keep]
        del g.node[:]
        g.node.extend(keep_nodes)

    # 清理不再被引用的 initializer
    used = set()
    for n in g.node:
        used.update(n.input)
    used.update(i.name for i in g.input)
    used.update(i.name for i in g.output)
    inits_new = [t for t in g.initializer if t.name in used]
    print(f"  initializer: {len(g.initializer)} -> {len(inits_new)}")
    del g.initializer[:]
    g.initializer.extend(inits_new)

    # ---- 5) graph.input 里除了 images 都是 initializer, 去掉(省得 toolkit 当输入处理) ----
    del g.input[:]
    g.input.extend([helper.make_tensor_value_info("images", TensorProto.FLOAT, [1, 3, 640, 640])])

    m = onnx.shape_inference.infer_shapes(m)
    onnx.checker.check_model(m)
    onnx.save(m, dst)
    print(f"  -> {dst}")
    print(f"  输出: box[1,4,{n_anchor}] (cx,cy,w,h 像素) / cls[1,{nc},{n_anchor}] (概率)")
    print(f"  节点数 {len(g.node)}, opset {m.opset_import[0].version}")


if __name__ == "__main__":
    main()
