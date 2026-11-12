"""RKNN Runtime 封装 (ctypes 直连 librknnrt.so, 对齐 Toolkit2 V2.5.0 官方 ABI)

对齐的官方接口(全部来自 rknn_api.h):
    rknn_init / rknn_destroy / rknn_query / rknn_run
    rknn_set_io_mem / rknn_create_mem_from_fd / rknn_destroy_mem
    rknn_outputs_get / rknn_outputs_release
    rknn_set_core_mask          -> NPU 分核
    rknn_set_batch_core_num     -> 单次 run 吃满多核

★ 零拷贝输入的正确姿势:
    rknn_create_mem_from_fd(ctx, fd, va, size, 0)   # 直接用 dma-buf fd
    rknn_set_io_mem(ctx, mem, attr)                   # attr 描述布局/类型
    rknn_run(ctx, NULL)
  这样 NPU 直接读物理页, CPU 不参与搬运。

★ 分核 + batch 的关系(容易搞错):
    rknn_set_core_mask(ctx, RKNN_CORE_0_1)  只影响【本 context 后续所有 run】,
    两个模型要各开一个 context(YOLO 一个、LPR 一个), 各自 set 自己的 mask。
    rknn_set_batch_core_num(ctx, n) 是让【单次 run】用 n 个核并行算 batch,
    和 set_core_mask 是叠加关系, 不要混用。
"""

from __future__ import annotations

import ctypes
import logging
import os
from dataclasses import dataclass, field

log = logging.getLogger(__name__)

# ---------------- 常量 ----------------
RKNN_MAX_DIMS = 16
RKNN_MAX_NAME_LEN = 256

RKNN_QUERY_IN_OUT_NUM = 0
RKNN_QUERY_INPUT_ATTR = 1
RKNN_QUERY_OUTPUT_ATTR = 2
RKNN_QUERY_PERF_DETAIL = 3
RKNN_QUERY_PERF_RUN = 4
RKNN_QUERY_SDK_VERSION = 5
RKNN_QUERY_NATIVE_NHWC_INPUT_ATTR = 10

RKNN_TENSOR_FLOAT32 = 0
RKNN_TENSOR_FLOAT16 = 1
RKNN_TENSOR_INT8 = 2
RKNN_TENSOR_UINT8 = 3
RKNN_TENSOR_INT16 = 4
RKNN_TENSOR_UINT16 = 5
RKNN_TENSOR_INT32 = 6
RKNN_TENSOR_UINT32 = 7
RKNN_TENSOR_INT64 = 8
RKNN_TENSOR_BOOL = 9

RKNN_TENSOR_NCHW = 0
RKNN_TENSOR_NHWC = 1

RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC = 2

RKNN_FLAG_PRIOR_HIGH = 0x00000000
RKNN_FLAG_COLLECT_PERF_MASK = 0x00000008

RKNN_NPU_CORE_AUTO = 0
RKNN_NPU_CORE_0 = 1
RKNN_NPU_CORE_1 = 2
RKNN_NPU_CORE_2 = 4
RKNN_NPU_CORE_0_1 = 3
RKNN_NPU_CORE_0_1_2 = 7

CORE_MASK_MAP = {
    "auto": RKNN_NPU_CORE_AUTO,
    "0": RKNN_NPU_CORE_0,
    "1": RKNN_NPU_CORE_1,
    "2": RKNN_NPU_CORE_2,
    "0_1": RKNN_NPU_CORE_0_1,
    "0_1_2": RKNN_NPU_CORE_0_1_2,
}

TENSOR_TYPE_NAME = {
    0: "FLOAT32", 1: "FLOAT16", 2: "INT8", 3: "UINT8", 4: "INT16",
    5: "UINT16", 6: "INT32", 7: "UINT32", 8: "INT64", 9: "BOOL",
}


# ---------------- ctypes 结构体(必须与 rknn_api.h 逐字段对齐) ----------------
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


class RknnTensorMem(ctypes.Structure):
    _fields_ = [
        ("virt_addr", ctypes.c_void_p),
        ("phys_addr", ctypes.c_uint64),
        ("fd", ctypes.c_int32),
        ("offset", ctypes.c_int32),
        ("size", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("priv_data", ctypes.c_void_p),
    ]


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


class RknnPerfRun(ctypes.Structure):
    _fields_ = [("run_duration", ctypes.c_int64)]


class RknnPerfDetail(ctypes.Structure):
    _fields_ = [("perf_data", ctypes.c_char_p), ("data_len", ctypes.c_uint64)]


# ---------------- 数据类 ----------------
@dataclass
class TensorInfo:
    index: int
    name: str
    type: int
    fmt: int
    dims: list[int]
    n_elems: int
    size: int
    qnt_type: int
    zp: int
    scale: float

    @property
    def type_name(self) -> str:
        return TENSOR_TYPE_NAME.get(self.type, f"TYPE{self.type}")

    @property
    def shape(self) -> tuple[int, ...]:
        return tuple(self.dims)


@dataclass
class ModelInfo:
    inputs: list[TensorInfo] = field(default_factory=list)
    outputs: list[TensorInfo] = field(default_factory=list)
    sdk_version: str = ""
    driver_version: str = ""


class RknnError(RuntimeError):
    pass


class RknnModel:
    """一个 RKNN 模型实例(= 一个 rknn_context)"""

    def __init__(self, model_path: str, lib_path: str = "librknnrt.so",
                 core_mask: str = "auto", collect_perf: bool = False):
        self.lib = ctypes.CDLL(lib_path)
        self._bind()
        self.ctx = ctypes.c_void_p(0)
        self.info = ModelInfo()
        self.core_mask = core_mask
        self._io_mem: list[RknnTensorMem] = []
        self._last_run_us = 0

        flags = RKNN_FLAG_PRIOR_HIGH
        if collect_perf:
            flags |= RKNN_FLAG_COLLECT_PERF_MASK

        size = os.path.getsize(model_path)
        with open(model_path, "rb") as f:
            data = f.read()
        ret = self.lib.rknn_init(ctypes.byref(self.ctx), data, size, flags, None)
        if ret != 0:
            raise RknnError(f"rknn_init 失败 ret={ret} model={model_path}")
        self._query_info()
        self.set_core_mask(core_mask)
        log.info("[RKNN] %s 已加载: 输入=%s 输出=%s core=%s",
                 os.path.basename(model_path),
                 [f"{i.name}{i.shape}:{i.type_name}" for i in self.info.inputs],
                 [f"{o.name}{o.shape}" for o in self.info.outputs], core_mask)

    # ------------------------------------------------------------------
    def _bind(self) -> None:
        lib = self.lib
        lib.rknn_init.restype = ctypes.c_int
        lib.rknn_init.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p,
                                  ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
        lib.rknn_destroy.restype = ctypes.c_int
        lib.rknn_destroy.argtypes = [ctypes.c_void_p]
        lib.rknn_query.restype = ctypes.c_int
        lib.rknn_query.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p,
                                    ctypes.c_uint32]
        lib.rknn_run.restype = ctypes.c_int
        lib.rknn_run.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        lib.rknn_set_io_mem.restype = ctypes.c_int
        lib.rknn_set_io_mem.argtypes = [ctypes.c_void_p, ctypes.POINTER(RknnTensorMem),
                                        ctypes.POINTER(RknnTensorAttr)]
        lib.rknn_create_mem_from_fd.restype = ctypes.POINTER(RknnTensorMem)
        lib.rknn_create_mem_from_fd.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                                ctypes.c_void_p, ctypes.c_uint32,
                                                ctypes.c_uint32]
        lib.rknn_destroy_mem.restype = ctypes.c_int
        lib.rknn_destroy_mem.argtypes = [ctypes.c_void_p, ctypes.POINTER(RknnTensorMem)]
        lib.rknn_outputs_get.restype = ctypes.c_int
        lib.rknn_outputs_get.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                         ctypes.POINTER(RknnOutput), ctypes.c_void_p]
        lib.rknn_outputs_release.restype = ctypes.c_int
        lib.rknn_outputs_release.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                             ctypes.POINTER(RknnOutput)]
        lib.rknn_inputs_set.restype = ctypes.c_int
        lib.rknn_inputs_set.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                        ctypes.POINTER(RknnInput)]
        lib.rknn_set_core_mask.restype = ctypes.c_int
        lib.rknn_set_core_mask.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.rknn_set_batch_core_num.restype = ctypes.c_int
        lib.rknn_set_batch_core_num.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    # ------------------------------------------------------------------
    def _query_info(self) -> None:
        num = RknnInputOutputNum()
        self.lib.rknn_query(self.ctx, RKNN_QUERY_IN_OUT_NUM, ctypes.byref(num),
                            ctypes.sizeof(num))
        for i in range(num.n_input):
            attr = RknnTensorAttr()
            attr.index = i
            self.lib.rknn_query(self.ctx, RKNN_QUERY_INPUT_ATTR, ctypes.byref(attr),
                                ctypes.sizeof(attr))
            self.info.inputs.append(TensorInfo(
                index=i, name=attr.name.decode(errors="ignore"), type=attr.type,
                fmt=attr.fmt, dims=list(attr.dims[:attr.n_dims]), n_elems=attr.n_elems,
                size=attr.size, qnt_type=attr.qnt_type, zp=attr.zp, scale=attr.scale))

        for i in range(num.n_output):
            attr = RknnTensorAttr()
            attr.index = i
            self.lib.rknn_query(self.ctx, RKNN_QUERY_OUTPUT_ATTR, ctypes.byref(attr),
                                ctypes.sizeof(attr))
            self.info.outputs.append(TensorInfo(
                index=i, name=attr.name.decode(errors="ignore"), type=attr.type,
                fmt=attr.fmt, dims=list(attr.dims[:attr.n_dims]), n_elems=attr.n_elems,
                size=attr.size, qnt_type=attr.qnt_type, zp=attr.zp, scale=attr.scale))

        class _Ver(ctypes.Structure):
            _fields_ = [("api_version", ctypes.c_char * 256),
                        ("drv_version", ctypes.c_char * 256)]
        v = _Ver()
        self.lib.rknn_query(self.ctx, RKNN_QUERY_SDK_VERSION, ctypes.byref(v),
                            ctypes.sizeof(v))
        self.info.sdk_version = v.api_version.decode(errors="ignore")
        self.info.driver_version = v.drv_version.decode(errors="ignore")

    # ------------------------------------------------------------------
    @property
    def T(self) -> int:
        """时序步数(CTC 输出 [1, T, C] 的 T)"""
        if not self.info.outputs:
            return 0
        d = self.info.outputs[0].dims
        return d[-2] if len(d) >= 2 else 0

    @property
    def C(self) -> int:
        """类别数([1, T, C] 的 C)"""
        if not self.info.outputs:
            return 0
        d = self.info.outputs[0].dims
        return d[-1] if len(d) >= 1 else 0

    # ------------------------------------------------------------------
    def set_core_mask(self, mask: str | int) -> int:
        """NPU 分核。YOLO 用 0_1, LPRNet 用 2"""
        m = CORE_MASK_MAP.get(mask, mask) if isinstance(mask, str) else mask
        ret = self.lib.rknn_set_core_mask(self.ctx, m)
        if ret != 0:
            log.warning("[RKNN] rknn_set_core_mask(%s) 失败 ret=%d", mask, ret)
        self.core_mask = mask
        return ret

    def set_batch_core_num(self, n: int) -> int:
        """单次 run 用 n 个核并行算 batch(与 set_core_mask 叠加)"""
        ret = self.lib.rknn_set_batch_core_num(self.ctx, n)
        if ret != 0:
            log.warning("[RKNN] rknn_set_batch_core_num(%d) 失败 ret=%d", n, ret)
        return ret

    # ------------------------------------------------------------------
    def bind_io_mem_from_fd(self, fd: int, va: int, size: int,
                            attr: TensorInfo, input_index: int = 0) -> RknnTensorMem:
        """把一块 dma-buf 绑成输入张量(零拷贝)。

        ★ attr.pass_through = 1 表示"布局/类型完全一致, 驱动不做任何转换",
          这是零拷贝的前提; 传 0 时驱动会按 attr 做转换, 可能产生 CPU 拷贝。
        """
        mem = self.lib.rknn_create_mem_from_fd(self.ctx, fd, ctypes.c_void_p(va),
                                               size, 0)
        if not mem:
            raise RknnError("rknn_create_mem_from_fd 失败")
        a = RknnTensorAttr()
        a.index = input_index
        a.fmt = attr.fmt
        a.type = attr.type
        a.n_dims = len(attr.dims)
        for i, d in enumerate(attr.dims):
            a.dims[i] = d
        a.pass_through = 1
        ret = self.lib.rknn_set_io_mem(self.ctx, mem, ctypes.byref(a))
        if ret != 0:
            self.lib.rknn_destroy_mem(self.ctx, mem)
            raise RknnError(f"rknn_set_io_mem 失败 ret={ret}")
        self._io_mem.append(mem)
        return mem

    def unbind_io_mem(self, mem: RknnTensorMem) -> None:
        if mem in self._io_mem:
            self.lib.rknn_destroy_mem(self.ctx, mem)
            self._io_mem.remove(mem)

    # ------------------------------------------------------------------
    def run(self) -> None:
        ret = self.lib.rknn_run(self.ctx, None)
        if ret != 0:
            raise RknnError(f"rknn_run 失败 ret={ret}")

    def get_outputs(self, n: int | None = None) -> list[RknnOutput]:
        n = n or len(self.info.outputs)
        outs = (RknnOutput * n)()
        for i in range(n):
            outs[i].want_float = 1
            outs[i].is_prealloc = 0
            outs[i].index = i
        ret = self.lib.rknn_outputs_get(self.ctx, n, outs, None)
        if ret != 0:
            raise RknnError(f"rknn_outputs_get 失败 ret={ret}")
        return list(outs)

    def release_outputs(self, outs: list[RknnOutput]) -> None:
        if outs:
            arr = (RknnOutput * len(outs))(*outs)
            self.lib.rknn_outputs_release(self.ctx, len(outs), arr)

    # ------------------------------------------------------------------
    def query_run_duration_us(self) -> int:
        """上次推理耗时(us), 用于 NPU 负载统计"""
        run = RknnPerfRun()
        ret = self.lib.rknn_query(self.ctx, RKNN_QUERY_PERF_RUN, ctypes.byref(run),
                                  ctypes.sizeof(run))
        self._last_run_us = run.run_duration if ret == 0 else 0
        return self._last_run_us

    def query_perf_detail(self) -> str:
        """NPU 详细负载(RKNN_FLAG_COLLECT_PERF_MASK 才有效)"""
        d = RknnPerfDetail()
        ret = self.lib.rknn_query(self.ctx, RKNN_QUERY_PERF_DETAIL, ctypes.byref(d),
                                  ctypes.sizeof(d))
        if ret != 0 or not d.perf_data:
            return ""
        return d.perf_data.decode(errors="ignore")

    # ------------------------------------------------------------------
    def close(self) -> None:
        for mem in list(self._io_mem):
            try:
                self.lib.rknn_destroy_mem(self.ctx, mem)
            except Exception:
                pass
        self._io_mem.clear()
        if self.ctx:
            self.lib.rknn_destroy(self.ctx)
            self.ctx = ctypes.c_void_p(0)

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass


# ----------------------------------------------------------------------
def quantize_uint8_to_int8(src_u8, scale: float, zp: int, out_i8: bytearray) -> None:
    """uint8 像素 -> int8 量化码: q = round((x/255)/scale) + zp

    ★ 这是 LPRNet 唯一正确的喂法(板端 A/B 七种喂法实测):
      模型输入 attr = INT8/NHWC, zp=0, scale=1/127.5,
      而 runtime 的 rknn_inputs_set 实际要 92160 字节;
      按 uint8 + 23040 喂会被 runtime 按 92160 字节读写 -> 越界踩坏堆。
    """
    inv = 1.0 / (255.0 * scale)
    for i, v in enumerate(src_u8):
        q = int(v * inv + 0.5) + zp
        if q > 127:
            q = 127
        elif q < -128:
            q = -128
        out_i8[i] = q
