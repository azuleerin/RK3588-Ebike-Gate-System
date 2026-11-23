# 03 · ★ 踩到并修掉的一个静默坑: opset ≤ 12 的 Softmax「legacy 2D 强制转换」

> 这是本阶段**新发现**的问题, 上一个阶段(task2)的 `make_dfl_in_npu_onnx_v3.py`
> 没踩到 —— 因为它当时跑在 opset 13 的 ONNX 上。
> 本阶段的模板 `yolov8n_optimized.onnx` 是 **opset 12**, 于是踩中了。
>
> **危害: 静默失败。** `onnx.checker` 通过、RKNN 编译器正常编译、程序跑起来
> 不报任何错, 但**每一个框都是零宽**。

---

## 一、现象

用 `make_dfl_in_npu_onnx_v3.py` 把 fork9 ONNX 改造成「形态C(DFL 留图内)」之后,
两条臂的框**完全对不上**:

```
形态C(孪生):  f00002.jpg, 1, cls=0, 0.307774, x1=0.822678 y1=0.423036 x2=0.822678 y2=0.426935
fork9(CPU解):  f00002.jpg, 1, cls=0, 0.307774, x1=0.779000 y1=0.352670 x2=0.860988 y2=0.580121
```

* **分数一模一样**(0.307774) ⇒ cls 路径没问题, 问题在 box 路径;
* **x1 == x2** ⇒ 框宽 = 0; y 方向也几乎贴在一起。

匹配率 0.00%, IoU 全对不上。

## 二、定位

把形态C ONNX 的每一个中间张量加成输出, 用 onnxruntime 跑一遍:

```
s0/reshape_dfl     (1, 4, 16, 80, 80)   [0]=[61.2774 10.2514 24.4973 10.8228]   ← 原始 logits, 正常
s0/softmax         (1, 4, 16, 80, 80)   [0]=[2.7407e-06 1.8947e-28 2.9140e-22 ...] ← ★ 全是 0 量级
s0/mul_bins        (1, 4, 16, 80, 80)   [0]=[0. 0. 0. 0.]
s0/dfl_expect      (1, 4, 80, 80)        [0]=[5.8266e-06 9.0578e-26 2.2441e-20 ...]
s0/sl0..sl3        (1, 1, 80, 80)        ← 四个距离全是 ~0
```

**Softmax 的输出不对**: 对 16 个 bin 做 softmax, 最大那个应该≈1, 实际是 2.7e-06。

## 三、根因

**ONNX 的 Softmax 在 opset 1~12 有「legacy 2D 强制转换」**:
规范规定要先���输入压成 2D `[d0*..*d(axis-1), d(axis)*..*d(n-1)]`,
再对第 1 维做 softmax。

对 5 维张量 `[1, 4, 16, H, W]` 且 `axis=2` 来说, 压平后是
`[1*4, 16*H*W]` ⇒ **softmax 是在 16×80×80 = 102400 个元素上做的**,
不是 16 个 bin 上做的 ⇒ 每个元素都趋近 1/102400 量级 ⇒
DFL 期望值 `Σ p_b · b` ≈ 0 ⇒ 框全是零宽。

**opset 13 起这个强制转换被取消**(`axis` 就是 `axis`),
所以 task2 当年跑的 opset 13 ONNX 是对的 —— **同一个脚本, 换个 opset 就静默坏掉**。

> 对照表:
> | opset | Softmax 的 axes 语义 | ReduceSum 的 axes | Squeeze 的 axes |
> |---|---|---|---|
> | ≤ 12 | **属性**, 且有 legacy 2D 强制转换 | 属性 | 属性 |
> | 13~17 | 属性, **无**强制转换 | **输入** | 输入 |
> | ≥ 18 | 属性, **无**强制转换 | 属性 | 属性 |

## 四、修法

`opt_ebike_fork9/scripts/make_dfl_in_npu_onnx_v3.py`(沙盒副本):

```python
# ★★ opset <= 12 的 Softmax 有【legacy 2D 强制转换】...
#    ⇒ 对 opset <= 12: 先把 16 个 bin 转到【最后一维】再 softmax, 再转回来。
sm = f"{tag}/softmax"
if _opset <= 12:
    add(helper.make_node("Transpose", [rs], [tr_a], perm=[0, 1, 3, 4, 2], ...))
    add(helper.make_node("Softmax", [tr_a], [sm_last], axis=-1, ...))
    add(helper.make_node("Transpose", [sm_last], [sm], perm=[0, 1, 4, 2, 3], ...))
else:
    add(helper.make_node("Softmax", [rs], [sm], axis=2, ...))
```

顺带把 ReduceSum / Squeeze 的 axes 形态也按 opset 做了适配(见上表)。

## 五、防复发: 加了数值级自检

**`scripts/verify_formC_equiv.py`** —— 把 fork9 的原始 DFL logits 在 CPU 上手工解码,
和形态C 图内解出来的框逐像素对比:

```
  trial0 stride=8   max|Δ(cx,cy,w,h)| = 0.000066 px
  trial0 stride=16  max|Δ(cx,cy,w,h)| = 0.000093 px
  trial0 stride=32  max|Δ(cx,cy,w,h)| = 0.000097 px
  ...
VERDICT: PASS ✔  形态C 图内 DFL 与 CPU 解码逐像素一致 (max|Δ|=0.000127)
```

**⇒ 以后任何人造「形态C」模型, 必须先过这个自检。**
`build_ebike_fork9.sh` 里已经把它接在转换之前, 不通过就不往下走。

## 六、这个坑对上一阶段的结论有没有影响?

**没有。** task2 的 Arm A(v3) 是在 **opset 13** 的 fork9 ONNX 上做的,
那条路没有 legacy 强制转换, 而且它当时**做过同等的数值验证**
(`verify_dfl_equiv.py`: "单位正确版: max|Δ|=0.0000 px")。
本次发现的坑只是说明:**那段脚本对 opset 是敏感的**,
换个 opset 就得重新验证 —— 所以本次给它配了强制自检。
