# tools/ —— 排查 .rknn 容器用的探针脚本(只读)

这两个脚本是本阶段**逆向排查 RKNN 容器格式**时写的, 结论已经写进
`docs/00_source_not_available.md` 第三节(结论: .rknn 里没有 ONNX,
是私有 FlatBuffer + 编译后的算子表 + int8 权重)。

* `probe_rknn_onnx.py`  —— 扫 ModelProto 头 / zlib 头 / initializer 名清单
* `probe_rknn_onnx2.py` —— 扫容器内嵌的张量名(ONNX 节点名), 用来还原图结构

保留它们是为了让"为什么不能直接从 .rknn 反推 ONNX"这件事**可复现**。
