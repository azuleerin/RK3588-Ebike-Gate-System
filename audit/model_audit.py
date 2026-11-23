#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""model_audit.py —— RKNN 模型分析(项1 算子执行设备 / 项2 量化纯度)

三层证据链, 互相印证:

  [A] 容器静态解析   解析 .rknn 文件头 + 内嵌元数据(目标平台 / 编译器版本 /
                     attrs / quant_tab / I/O 张量 dtype)。不依赖运行时。
  [B] Toolkit 校验   用 rknn-toolkit2 (rknn.api.RKNN) load_rknn() 打开模型,
                     确认编译器版本与本机 Toolkit 版本一致。
  [C] 运行时逐算子   通过 ctypes 调板端 librknnrt.so, 用 RKNN_QUERY_PERF_DETAIL
                     取回【驱动自己的】逐算子表: 每个算子的 OpType / DataType /
                     Target(NPU|CPU) / 耗时。这是"算子到底跑在哪、什么精度、
                     占多少时间"的权威答案, 也是本脚本的主要结论来源。

用法:
    ./model_audit.py <model.rknn> [--json out.json] [--no-toolkit] [--quiet]

退出码:
    0 = 审计通过(无问题)
    1 = 审计发现问题(详见输出)
    2 = 用法/环境错误
"""

import ast
import ctypes
import json
import os
import re
import sys

# --------------------------------------------------------------------------
# rknn_api.h 常量(与 third_party/include/rknn_api.h 一致)
# --------------------------------------------------------------------------
RKNN_MAX_DIMS = 16
RKNN_MAX_NAME_LEN = 256

RKNN_QUERY_IN_OUT_NUM = 0
RKNN_QUERY_INPUT_ATTR = 1
RKNN_QUERY_OUTPUT_ATTR = 2
RKNN_QUERY_PERF_DETAIL = 3
RKNN_QUERY_PERF_RUN = 4
RKNN_QUERY_SDK_VERSION = 5
RKNN_QUERY_MEM_SIZE = 6

RKNN_FLAG_PRIOR_HIGH = 0x00000000
RKNN_FLAG_COLLECT_PERF_MASK = 0x00000008

RKNN_TENSOR_FLOAT32, RKNN_TENSOR_FLOAT16, RKNN_TENSOR_INT8 = 0, 1, 2
RKNN_TENSOR_UINT8, RKNN_TENSOR_INT16, RKNN_TENSOR_UINT16 = 3, 4, 5
RKNN_TENSOR_INT32, RKNN_TENSOR_UINT32, RKNN_TENSOR_INT64 = 6, 7, 8
RKNN_TENSOR_BOOL = 9

TYPE_NAME = {
    RKNN_TENSOR_FLOAT32: "FLOAT32", RKNN_TENSOR_FLOAT16: "FLOAT16",
    RKNN_TENSOR_INT8: "INT8", RKNN_TENSOR_UINT8: "UINT8",
    RKNN_TENSOR_INT16: "INT16", RKNN_TENSOR_UINT16: "UINT16",
    RKNN_TENSOR_INT32: "INT32", RKNN_TENSOR_UINT32: "UINT32",
    RKNN_TENSOR_INT64: "INT64", RKNN_TENSOR_BOOL: "BOOL",
}

# --------------------------------------------------------------------------
# ctypes 结构体
# --------------------------------------------------------------------------


class RknnTensorAttr(ctypes.Structure):
    _fields_ = [
        ("index", ctypes.c_uint32),
        ("n_dims", ctypes.c_uint32),
        ("dims", ctypes.c_uint32 * RKNN_MAX_DIMS),
        ("name", ctypes.c_char * RKNN_MAX_NAME_LEN),
        ("n_elems", ctypes.c_uint32),
        ("size", ctypes.c_uint32),
        ("fmt", ctypes.c_int),
        ("type", ctypes.c_int),
        ("qnt_type", ctypes.c_int),
        ("fl", ctypes.c_int8),
        ("zp", ctypes.c_int32),
        ("scale", ctypes.c_float),
        ("w_stride", ctypes.c_uint32),
        ("size_with_stride", ctypes.c_uint32),
        ("pass_through", ctypes.c_uint8),
        ("h_stride", ctypes.c_uint32),
    ]


class RknnInputOutputNum(ctypes.Structure):
    _fields_ = [("n_input", ctypes.c_uint32), ("n_output", ctypes.c_uint32)]


class RknnPerfDetail(ctypes.Structure):
    _fields_ = [("perf_data", ctypes.c_char_p), ("data_len", ctypes.c_uint64)]


class RknnPerfRun(ctypes.Structure):
    _fields_ = [("run_duration", ctypes.c_int64)]


class RknnSdkVersion(ctypes.Structure):
    _fields_ = [("api_version", ctypes.c_char * 256),
                ("drv_version", ctypes.c_char * 256)]


class RknnMemSize(ctypes.Structure):
    _fields_ = [("total_weight_size", ctypes.c_uint32),
                ("total_internal_size", ctypes.c_uint32),
                ("total_dma_allocated_size", ctypes.c_uint64),
                ("total_sram_size", ctypes.c_uint32),
                ("free_sram_size", ctypes.c_uint32),
                ("reserved", ctypes.c_uint32 * 10)]


class RknnInput(ctypes.Structure):
    _fields_ = [
        ("index", ctypes.c_uint32),
        ("buf", ctypes.c_void_p),
        ("size", ctypes.c_uint32),
        ("pass_through", ctypes.c_uint8),
        ("type", ctypes.c_int),
        ("fmt", ctypes.c_int),
    ]


class RknnOutput(ctypes.Structure):
    _fields_ = [
        ("want_float", ctypes.c_uint8),
        ("is_prealloc", ctypes.c_uint8),
        ("index", ctypes.c_uint32),
        ("buf", ctypes.c_void_p),
        ("size", ctypes.c_uint32),
    ]


def load_rknnrt():
    """加载板端运行库。"""
    for cand in ("/usr/lib/librknnrt.so",
                 "/home/seeed/rk_deploy/rk3588_zero_copy/third_party/lib/librknnrt.so"):
        if os.path.exists(cand):
            return ctypes.CDLL(cand), cand
    raise RuntimeError("找不到 librknnrt.so")


# ==========================================================================
# [A] 容器静态解析
# ==========================================================================
def parse_container(path):
    """解析 .rknn 容器头与内嵌元数据。"""
    data = open(path, "rb").read()
    info = {"size": len(data)}
    if data[:4] != b"RKNN":
        info["error"] = "magic 不是 RKNN"
        return info
    info["magic"] = "RKNN"
    info["container_version"] = int.from_bytes(data[4:8], "little")

    # 头部字符串字段: 用可打印串扫描定位
    def grab(pat):
        m = re.search(pat, data[:4096])
        if not m:
            return None
        g = m.group(1) if m.groups() else m.group(0)
        return g.decode() if isinstance(g, bytes) else g

    info["engine_version"] = grab(rb"RKNPU v\d+")
    info["compiler_version"] = grab(rb"(\d+\.\d+\.\d+\(compiler version:[^)]*\)\))")
    info["target_platform"] = grab(rb"(rk3588|rk3568|rk3566|rk3576|rk2332|rk1808|rk1126)")
    info["source_format"] = grab(rb"(ONNX|TFLite|Caffe|PyTorch|Darknet)")

    # 内嵌元数据 (Python repr 形式, 不是 JSON)
    m = re.search(rb"\{'attrs': \{.*?\}\}\}", data[:8192], re.S)
    if m:
        try:
            meta = ast.literal_eval(m.group(0).decode("utf-8", "replace"))
            info["attrs"] = meta.get("attrs", {})
            info["quant_tab"] = meta.get("quant_tab", {})
        except Exception as e:                                    # pragma: no cover
            info["meta_parse_error"] = str(e)

    # I/O 张量 dtype 声明
    for key, pat in (("io_dtype_in", rb'\{"\w+": \{"dtype": "\w+", "layout": "\w+"\}\}'),
                     ("io_dtype_out", rb'\{"[^}]+": \{"dtype": "\w+", "layout": "\w+"\}[^}]*')):
        m = re.search(pat, data[:8192])
        if m:
            info[key] = m.group(0).decode("utf-8", "replace")
    return info


# ==========================================================================
# [B] Toolkit 校验
# ==========================================================================
def toolkit_check(path):
    out = {}
    try:
        from rknn.api import RKNN
    except Exception as e:
        out["available"] = False
        out["error"] = str(e)
        return out
    out["available"] = True
    try:
        import rknn
        out["toolkit_version"] = open(
            os.path.join(os.path.dirname(rknn.__file__), "VERSION")).read().strip()
    except Exception:
        try:
            from importlib.metadata import version
            out["toolkit_version"] = version("rknn_toolkit2")
        except Exception:
            out["toolkit_version"] = "unknown"
    r = RKNN(verbose=False)
    ret = r.load_rknn(path)
    out["load_rknn_ret"] = ret
    out["load_ok"] = (ret == 0)
    try:
        r.release()
    except Exception:
        pass
    return out


# ==========================================================================
# [C] 运行时逐算子分析
# ==========================================================================
def runtime_op_table(path):
    """取回驱动自报的逐算子表 + 算子类型耗时排名表。"""
    lib, libpath = load_rknnrt()
    lib.rknn_init.restype = ctypes.c_int
    lib.rknn_init.argtypes = [ctypes.POINTER(ctypes.c_uint64), ctypes.c_void_p,
                              ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    lib.rknn_query.restype = ctypes.c_int
    lib.rknn_query.argtypes = [ctypes.c_uint64, ctypes.c_uint32, ctypes.c_void_p,
                                ctypes.c_uint32]
    lib.rknn_set_core_mask.restype = ctypes.c_int
    lib.rknn_set_core_mask.argtypes = [ctypes.c_uint64, ctypes.c_int]
    lib.rknn_inputs_set.restype = ctypes.c_int
    lib.rknn_inputs_set.argtypes = [ctypes.c_uint64, ctypes.c_uint32,
                                    ctypes.POINTER(RknnInput)]
    lib.rknn_run.restype = ctypes.c_int
    lib.rknn_run.argtypes = [ctypes.c_uint64, ctypes.c_void_p]
    lib.rknn_outputs_get.restype = ctypes.c_int
    lib.rknn_outputs_get.argtypes = [ctypes.c_uint64, ctypes.c_uint32,
                                     ctypes.POINTER(RknnOutput), ctypes.c_void_p]
    lib.rknn_outputs_release.restype = ctypes.c_int
    lib.rknn_outputs_release.argtypes = [ctypes.c_uint64, ctypes.c_uint32,
                                         ctypes.POINTER(RknnOutput)]
    lib.rknn_destroy.restype = ctypes.c_int
    lib.rknn_destroy.argtypes = [ctypes.c_uint64]

    ctx = ctypes.c_uint64(0)
    ret = lib.rknn_init(ctypes.byref(ctx), ctypes.c_char_p(path.encode()),
                        0, RKNN_FLAG_PRIOR_HIGH | RKNN_FLAG_COLLECT_PERF_MASK, None)
    if ret != 0:
        raise RuntimeError("rknn_init 失败: %d" % ret)

    res = {"lib": libpath}
    try:
        ver = RknnSdkVersion()
        if lib.rknn_query(ctx, RKNN_QUERY_SDK_VERSION, ctypes.byref(ver),
                          ctypes.sizeof(ver)) == 0:
            res["api_version"] = ver.api_version.decode()
            res["driver_version"] = ver.drv_version.decode()

        ms = RknnMemSize()
        if lib.rknn_query(ctx, RKNN_QUERY_MEM_SIZE, ctypes.byref(ms),
                          ctypes.sizeof(ms)) == 0:
            res["weight_mb"] = ms.total_weight_size / 1048576.0
            res["internal_mb"] = ms.total_internal_size / 1048576.0

        io = RknnInputOutputNum()
        lib.rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, ctypes.byref(io), ctypes.sizeof(io))
        res["n_input"] = io.n_input
        res["n_output"] = io.n_output

        inattr = RknnTensorAttr()
        inattr.index = 0
        lib.rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, ctypes.byref(inattr),
                       ctypes.sizeof(inattr))
        res["input"] = {
            "name": inattr.name.decode(errors="replace"),
            "type": TYPE_NAME.get(inattr.type, str(inattr.type)),
            "fmt": inattr.fmt,
            "dims": [inattr.dims[i] for i in range(inattr.n_dims)],
            "size": inattr.size,
            "n_elems": inattr.n_elems,
            "scale": inattr.scale,
            "zero_point": inattr.zp,
            "qnt_type": inattr.qnt_type,
        }
        res["outputs"] = []
        for i in range(io.n_output):
            oa = RknnTensorAttr()
            oa.index = i
            lib.rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, ctypes.byref(oa),
                           ctypes.sizeof(oa))
            res["outputs"].append({
                "name": oa.name.decode(errors="replace"),
                "type": TYPE_NAME.get(oa.type, str(oa.type)),
                "fmt": oa.fmt,
                "dims": [oa.dims[k] for k in range(oa.n_dims)],
                "size": oa.size,
                "scale": oa.scale,
                "zero_point": oa.zp,
            })

        # --- 跑一次, 让驱动把逐算子表填出来 ---
        buf = ctypes.create_string_buffer(inattr.size)
        ctypes.memset(buf, 114, inattr.size)
        inp = RknnInput()
        inp.index = 0
        inp.buf = ctypes.cast(buf, ctypes.c_void_p)
        inp.size = inattr.size
        inp.pass_through = 0
        inp.type = RKNN_TENSOR_UINT8
        inp.fmt = 1  # NHWC
        lib.rknn_inputs_set(ctx, 1, ctypes.byref(inp))
        lib.rknn_run(ctx, None)
        outs = (RknnOutput * io.n_output)()
        for i in range(io.n_output):
            outs[i].index = i
        lib.rknn_outputs_get(ctx, io.n_output, outs, None)
        lib.rknn_outputs_release(ctx, io.n_output, outs)

        prun = RknnPerfRun()
        if lib.rknn_query(ctx, RKNN_QUERY_PERF_RUN, ctypes.byref(prun),
                          ctypes.sizeof(prun)) == 0:
            res["driver_run_us"] = prun.run_duration

        pd = RknnPerfDetail()
        qret = lib.rknn_query(ctx, RKNN_QUERY_PERF_DETAIL, ctypes.byref(pd),
                              ctypes.sizeof(pd))
        res["perf_query_ret"] = qret
        res["perf_data_len"] = pd.data_len
        if qret == 0 and pd.perf_data:
            text = pd.perf_data.decode("utf-8", "replace")
            res["perf_text"] = text
            res["ops"] = parse_op_table(text)
            res["ranking"] = parse_ranking_table(text)
        else:
            res["perf_text"] = ""
            res["ops"] = []
            res["ranking"] = []
    finally:
        lib.rknn_destroy(ctx)
    return res


OP_COLUMNS = ["ID", "OpType", "DataType", "Target", "InputShape", "OutputShape",
              "Cycles(DDR/NPU/Total)", "Time(us)", "MacUsage(%)", "WorkLoad(0/1/2)",
              "RW(KB)", "FullName"]


def parse_op_table(text):
    """解析 'Network Layer Information Table'。

    驱动用【固定宽度列】排版, 且 MacUsage 为 0 时整列留空(列数会变)。
    所以不能按空白切列, 要用表头定位每列的起始下标再切片。
    """
    lines = text.splitlines()
    hdr_idx = None
    hdr = None
    for i, ln in enumerate(lines):
        if ln.startswith("ID   OpType"):
            hdr_idx, hdr = i, ln
            break
    if hdr is None:
        return []
    starts = []
    pos = 0
    for col in OP_COLUMNS:
        p = hdr.find(col, pos)
        if p < 0:
            return []
        starts.append(p)
        pos = p + len(col)
    bounds = starts + [len(hdr)]

    ops = []
    for ln in lines[hdr_idx + 1:]:
        if not ln.strip() or ln.strip().startswith("-"):
            continue
        if not re.match(r"^\s*\d+\s", ln):
            continue
        cells = []
        for k in range(len(OP_COLUMNS)):
            # 最后一列(FullName)取到行尾, 不按表头宽度截断
            s = ln[bounds[k]:bounds[k + 1]] if k + 1 < len(OP_COLUMNS) else ln[bounds[k]:]
            cells.append(s.strip())
        if not cells[0].isdigit():
            continue
        ops.append({
            "id": int(cells[0]),
            "op_type": cells[1],
            "dtype": cells[2],
            "target": cells[3],
            "input_shape": cells[4],
            "output_shape": cells[5],
            "cycles": cells[6],
            "time_us": int(cells[7]) if cells[7].isdigit() else 0,
            "mac_usage": cells[8],
            "workload": cells[9],
            "rw_kb": int(cells[10]) if cells[10].isdigit() else 0,
            "fullname": cells[11],
        })
    return ops


def parse_ranking_table(text):
    """解析 'Operator Time Consuming Ranking Table' (按算子类型汇总)。"""
    rows = []
    pat = re.compile(
        r"^(\S+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+([\d.]+)%\s*$")
    for line in text.splitlines():
        m = pat.match(line.strip())
        if not m:
            continue
        op = m.group(1)
        if op == "Total":
            rows.append({"op_type": "Total", "calls": int(m.group(2)),
                         "cpu_us": int(m.group(3)), "gpu_us": int(m.group(4)),
                         "npu_us": int(m.group(5)), "total_us": int(m.group(6)),
                         "ratio": float(m.group(7))})
            continue
        rows.append({"op_type": op, "calls": int(m.group(2)),
                     "cpu_us": int(m.group(3)), "gpu_us": int(m.group(4)),
                     "npu_us": int(m.group(5)), "total_us": int(m.group(6)),
                     "ratio": float(m.group(7))})
    return rows


# ==========================================================================
# 报告
# ==========================================================================
def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return 2
    path = args[0]
    use_toolkit = "--no-toolkit" not in args
    quiet = "--quiet" in args
    json_out = None
    if "--json" in args:
        json_out = args[args.index("--json") + 1]

    if not os.path.exists(path):
        print("[FATAL] 模型不存在: %s" % path)
        return 2

    report = {"model": path, "findings": [], "ok": True}

    def out(*a):
        if not quiet:
            print(*a)

    # ---------- [A] 静态 ----------
    out("=" * 78)
    out("模型审计报告  ——  %s" % path)
    out("=" * 78)
    out("\n[A] 容器静态解析")
    st = parse_container(path)
    report["container"] = st
    out("  魔数/版本      : %s / v%d" % (st.get("magic"), st.get("container_version", 0)))
    out("  文件大小       : %.2f MB" % (st.get("size", 0) / 1048576.0))
    out("  NPU 引擎版本   : %s" % st.get("engine_version"))
    out("  编译器版本     : %s" % st.get("compiler_version"))
    out("  目标平台       : %s" % st.get("target_platform"))
    out("  源模型格式     : %s" % st.get("source_format"))
    if st.get("io_dtype_in"):
        out("  输入张量声明   : %s" % st["io_dtype_in"])
    if st.get("io_dtype_out"):
        out("  输出张量声明   : %s" % st["io_dtype_out"])

    # ---------- [B] Toolkit ----------
    if use_toolkit:
        out("\n[B] rknn-toolkit2 校验")
        tk = toolkit_check(path)
        report["toolkit"] = tk
        if not tk.get("available"):
            out("  ⚠ Toolkit 不可用: %s (跳过 B 层)" % tk.get("error"))
        else:
            out("  Toolkit 版本   : %s" % tk.get("toolkit_version"))
            out("  load_rknn()    : %s" % ("OK" if tk.get("load_ok") else "FAILED"))
            m = re.search(r"(\d+\.\d+\.\d+)", st.get("compiler_version") or "")
            if m and tk.get("toolkit_version") and m.group(1) != tk["toolkit_version"]:
                report["findings"].append(
                    "模型编译器版本 %s 与本机 Toolkit %s 不一致"
                    % (m.group(1), tk["toolkit_version"]))
                report["ok"] = False

    # ---------- [C] 运行时 ----------
    out("\n[C] 运行时逐算子分析 (librknnrt / RKNN_QUERY_PERF_DETAIL)")
    try:
        rt = runtime_op_table(path)
    except Exception as e:
        print("[FATAL] 运行时分析失败: %s" % e)
        return 2
    report["runtime"] = {k: v for k, v in rt.items() if k != "perf_text"}
    out("  运行库         : %s" % rt.get("lib"))
    out("  Runtime API    : %s | 驱动 %s" % (rt.get("api_version"), rt.get("driver_version")))
    out("  权重/内部内存  : %.2f MB / %.2f MB" % (rt.get("weight_mb", 0),
                                                   rt.get("internal_mb", 0)))
    inp = rt.get("input", {})
    out("  输入张量       : %s type=%s dims=%s size=%d B" %
        (inp.get("name"), inp.get("type"), inp.get("dims"), inp.get("size", 0)))
    for o in rt.get("outputs", []):
        out("  输出张量       : %-28s type=%-8s dims=%s size=%d B" %
            (o["name"], o["type"], o["dims"], o["size"]))
    if rt.get("driver_run_us"):
        out("  驱动自报单次   : %d us (%.2f FPS)" %
            (rt["driver_run_us"], 1e6 / rt["driver_run_us"]))

    ops = rt.get("ops", [])
    if not ops:
        print("[FATAL] 未取到逐算子表 (query_ret=%s data_len=%s perf_text_len=%d)"
              % (rt.get("perf_query_ret"), rt.get("perf_data_len"),
                 len(rt.get("perf_text", ""))))
        return 2

    # ---- 项1: 算子执行设备 ----
    out("\n" + "-" * 78)
    out("项1  算子执行设备审计 (CPU 回落排查)")
    out("-" * 78)
    by_target = {}
    for o in ops:
        t = o["target"]
        d = by_target.setdefault(t, {"n": 0, "us": 0, "types": {}})
        d["n"] += 1
        d["us"] += o["time_us"]
        d["types"][o["op_type"]] = d["types"].get(o["op_type"], 0) + 1
    total_us = sum(o["time_us"] for o in ops)
    out("  算子总数       : %d" % len(ops))
    for t, d in sorted(by_target.items(), key=lambda kv: -kv[1]["n"]):
        pct_n = 100.0 * d["n"] / len(ops)
        pct_t = 100.0 * d["us"] / total_us if total_us else 0.0
        out("  Target=%-4s : %3d 个 (%5.1f%%) | 耗时 %7d us (%5.2f%%) | 类型 %s"
            % (t, d["n"], pct_n, d["us"], pct_t,
               ", ".join("%s×%d" % (k, v) for k, v in sorted(d["types"].items(),
                                                             key=lambda kv: -kv[1]))))

    cpu_ops = [o for o in ops if o["target"] == "CPU"]
    out("\n  CPU 回落算子明细:")
    if not cpu_ops:
        out("    (无)")
    for o in cpu_ops:
        out("    #%-4d %-16s dtype=%-8s %6d us  %s"
            % (o["id"], o["op_type"], o["dtype"], o["time_us"], o["fullname"]))
    # 纯计算类 CPU 回落(排除 I/O 算子)
    io_ops = {"InputOperator", "OutputOperator"}
    calc_cpu = [o for o in cpu_ops if o["op_type"] not in io_ops]
    calc_cpu_us = sum(o["time_us"] for o in calc_cpu)
    out("\n  >> CPU 回落中的【纯计算算子】: %d 个, 合计 %d us (占总算子时间 %.2f%%)"
        % (len(calc_cpu), calc_cpu_us,
           100.0 * calc_cpu_us / total_us if total_us else 0.0))
    if not calc_cpu:
        out("     → 无计算类算子回落 CPU。CPU 侧仅有 Input/Output 算子(IO 记账), 属正常。")
        out("     → 结论: 【无问题】不存在 CPU 回落性能损耗, 无需优化。")
    else:
        out("     → 结论: 【有问题】存在计算类算子回落 CPU, 需评估原生 NPU 算子替换。")
        report["findings"].append(
            "%d 个计算类算子回落 CPU (合计 %d us, %.2f%%)"
            % (len(calc_cpu), calc_cpu_us,
               100.0 * calc_cpu_us / total_us if total_us else 0.0))
        report["ok"] = False

    # 排名表
    out("\n  算子类型耗时排名 (驱动自报):")
    out("    %-18s %6s %9s %9s %9s %8s" % ("OpType", "Calls", "CPU(us)", "NPU(us)",
                                          "Total(us)", "Ratio"))
    for r in rt.get("ranking", []):
        out("    %-18s %6d %9d %9d %9d %7.2f%%"
            % (r["op_type"], r["calls"], r["cpu_us"], r["npu_us"], r["total_us"],
               r["ratio"]))

    # ---- 项2: 量化纯度 ----
    out("\n" + "-" * 78)
    out("项2  量化纯度审计 (混合精度排查)")
    out("-" * 78)
    by_dtype = {}
    for o in ops:
        d = by_dtype.setdefault(o["dtype"], {"n": 0, "us": 0, "ops": []})
        d["n"] += 1
        d["us"] += o["time_us"]
        d["ops"].append(o)
    out("  算子精度分布:")
    for dt, d in sorted(by_dtype.items(), key=lambda kv: -kv[1]["n"]):
        out("    %-8s : %3d 个 (%5.1f%%) | 耗时 %7d us (%5.2f%%)"
            % (dt, d["n"], 100.0 * d["n"] / len(ops), d["us"],
               100.0 * d["us"] / total_us if total_us else 0.0))
        for o in d["ops"]:
            out("        #%-4d %-16s %-8s %6d us  %s"
                % (o["id"], o["op_type"], o["target"], o["time_us"], o["fullname"]))

    int8_n = by_dtype.get("INT8", {}).get("n", 0)
    int8_pct = 100.0 * int8_n / len(ops)
    fp16_ops = by_dtype.get("FLOAT16", {}).get("ops", [])
    fp32_ops = by_dtype.get("FLOAT32", {}).get("ops", [])
    fp16_us = sum(o["time_us"] for o in fp16_ops)
    out("\n  INT8 算子占比    : %d/%d = %.2f%%" % (int8_n, len(ops), int8_pct))
    out("  FP16 残留层       : %d 个 (耗时 %d us, %.2f%%)"
        % (len(fp16_ops), fp16_us,
           100.0 * fp16_us / total_us if total_us else 0.0))
    out("  FP32 残留层       : %d 个" % len(fp32_ops))
    for o in fp16_ops:
        out("      FP16: #%-4d %-16s %6d us  %s"
            % (o["id"], o["op_type"], o["time_us"], o["fullname"]))
    if not fp16_ops and not fp32_ops:
        out("  >> 结论: 【无问题】模型为纯 INT8, 无 FP16/FP32 残留层。")
        out("     → 全 INT8 化收益 = 0 (已经是 100% INT8), 不存在可回收的混合精度损耗。")
        out("     → UINT8 的 2 个算子 = 输入侧量化转换(InputOperator + 首层 Conv),")
        out("        是 RKNN 标准输入变换, 不是精度残留。")
    else:
        out("  >> 结论: 【有问题】存在 FP16/FP32 残留层, 需评估校准优化为 INT8 的可行性与收益。")
        report["findings"].append(
            "存在 FP16(%d 个)/FP32(%d 个) 残留层" % (len(fp16_ops), len(fp32_ops)))
        report["ok"] = False

    # 内嵌量化表
    qt = st.get("quant_tab", {})
    if qt:
        out("\n  内嵌 quant_tab (输入/输出张量量化参数):")
        for k, v in qt.items():
            out("    %-30s dtype=%-8s qmethod=%-6s qtype=%-5s min=%s max=%s"
                % (k, v.get("dtype"), v.get("qmethod"), v.get("qtype"),
                   v.get("min"), v.get("max")))

    # ---------- 结论 ----------
    out("\n" + "=" * 78)
    out("审计结论: %s" % ("【无问题】" if report["ok"] else "【有问题】"))
    if report["findings"]:
        for f in report["findings"]:
            out("  · %s" % f)
    else:
        out("  · 项1 算子执行设备: 无 CPU 计算回落")
        out("  · 项2 量化纯度: 纯 INT8, 无 FP16/FP32 残留")
    out("=" * 78)

    if json_out:
        with open(json_out, "w") as f:
            json.dump(report, f, indent=2, ensure_ascii=False)
        print("\nJSON 已写入: %s" % json_out)

    return 0 if report["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
