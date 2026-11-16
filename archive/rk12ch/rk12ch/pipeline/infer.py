"""第 3 层: 推理层 —— NPU 分核调度 + 批量推理

两个独立的 worker, 各自一个 rknn_context:
    YoloWorker:  core mask = 0_1 (两个核), 跑 YOLOv8n
    LprWorker:   core mask = 2   (一个核), 跑 LPRNet

★ 分核的正确做法:
    rknn_set_core_mask 是【context 级】的, 所以必须【两个模型两个 context】,
    否则后 set 的会把前面的覆盖掉。
    同一个 context 内再用 rknn_set_batch_core_num(n) 让单次 run 用 n 个核并行
    算 batch —— 这两个是叠加关系, 不要混用。

★ batch 的硬约束(不踩这个坑 batch 根本不生效):
    模型转换时必须带 batch 维(rknn.config(batch_size=N), 且 ONNX 的 batch 轴
    是动态的)。查询模型输入 attr 的 dims[0]:
        dims[0] > 1  -> 支持 batch, 生效 batch = min(cfg_batch, dims[0])
        dims[0] == 1 -> 模型被固化成 batch=1, 喂 batch>1 会被 runtime 拒绝
                        (本工程现在的 ebike_int8_rk2332.rknn 就是 batch=1)
"""

from __future__ import annotations

import ctypes
import logging
import threading
import time

import numpy as np

from ..hal.rknn import RknnError, RknnModel, TensorInfo
from .queues import BoundedQueue
from .types import Batch, BatchItem

log = logging.getLogger(__name__)


def _out_numpy(out) -> np.ndarray:
    """rknn_output.buf -> numpy(不拷贝)"""
    n = out.size // 4
    arr = (ctypes.c_float * n).from_address(out.buf)
    return np.frombuffer(arr, dtype=np.float32, count=n)


class BaseInferWorker(threading.Thread):
    def __init__(self, name: str, model: RknnModel, in_queue: BoundedQueue,
                 out_queue: BoundedQueue, stats, stop_event: threading.Event):
        super().__init__(name=name, daemon=True)
        self.model = model
        self.in_q = in_queue
        self.out_q = out_queue
        self.stats = stats
        self.stop_event = stop_event

    def run(self) -> None:
        while not self.stop_event.is_set():
            batch = self.in_q.get(timeout=0.2)
            if batch is None:
                continue
            try:
                self.process(batch)
            except Exception:
                log.exception("[%s] 推理异常", self.name)
            finally:
                self.on_batch_done(batch)

    def process(self, batch: Batch) -> None:
        raise NotImplementedError

    def on_batch_done(self, batch: Batch) -> None:
        pass


class YoloWorker(BaseInferWorker):
    """YOLOv8n 检测(2 个 NPU 核)"""

    def __init__(self, cfg, in_queue, out_queue, stats, stop_event, pool=None):
        m = cfg["models"]["yolo"]
        model = RknnModel(m["path"], core_mask=cfg["inference"]["yolo_cores"],
                          collect_perf=cfg["stats"].get("npu_perf", False))
        super().__init__("infer-yolo", model, in_queue, out_queue, stats, stop_event)
        self._pool = pool          # batch 缓冲池, 用完要归还

        self.in_w = m["input_w"]
        self.in_h = m["input_h"]
        self.cfg_batch = cfg["inference"]["yolo_batch"]

        attr = model.info.inputs[0]
        self.model_batch_dim = attr.dims[0] if attr.dims else 1
        # ★ 模型固化 batch=1 时, 只能用 batch=1
        self.effective_batch = self.cfg_batch if self.model_batch_dim > 1 else 1
        if self.cfg_batch > 1 and self.model_batch_dim <= 1:
            log.warning(
                "[infer-yolo] 模型 batch 维是 1(转换时没带 batch_size), "
                "实际 batch 退化为 1!\n"
                "           想要真 batch: 用 rknn.config(batch_size=%d) 重新转换模型"
                "(且 ONNX batch 轴要动态)", self.cfg_batch)
        self._io_mem = None
        self._mem = None

    # ------------------------------------------------------------------
    def _bind(self, batch: Batch) -> None:
        """把 batch 缓冲绑成 NPU 输入(零拷贝)"""
        model = self.model
        if self._mem is not None:
            try:
                model.unbind_io_mem(self._mem)
            except Exception:
                pass
            self._mem = None

        attr: TensorInfo = model.info.inputs[0]
        size = batch.n * self.in_h * self.in_w * 3
        mem = model.lib.rknn_create_mem_from_fd(model.ctx, batch.fd,
                                                ctypes.c_void_p(batch.va), size, 0)
        if not mem:
            raise RknnError("rknn_create_mem_from_fd 失败")
        self._mem = mem

        # attr: 用 UINT8 + NHWC + pass_through=0, 让驱动在读 DMA 时顺带做
        #       uint8->int8 量化(模型输入 zp=-128 scale=1/255 的标准约定)
        a = model.lib.rknn_set_io_mem
        from ..hal.rknn import RknnTensorAttr
        ta = RknnTensorAttr()
        ta.index = 0
        ta.fmt = 1                      # RKNN_TENSOR_NHWC
        ta.type = 3                    # RKNN_TENSOR_UINT8
        ta.n_dims = 4
        ta.dims[0] = batch.n
        ta.dims[1] = self.in_h
        ta.dims[2] = self.in_w
        ta.dims[3] = 3
        ta.pass_through = 0
        ret = a(model.ctx, mem, ctypes.byref(ta))
        if ret != 0:
            raise RknnError(f"rknn_set_io_mem 失败 ret={ret}")

    # ------------------------------------------------------------------
    def process(self, batch: Batch) -> None:
        t0 = time.monotonic()
        self._bind(batch)
        self.model.run()
        outs = self.model.get_outputs()
        try:
            run_us = self.model.query_run_duration_us()
            self.stats.on_npu_run(run_us)
            data = [_out_numpy(o) for o in outs]
            for item in batch.items:
                self.out_q.put((item, data), timeout=0.0)
        finally:
            self.model.release_outputs(outs)
        log.debug("[infer-yolo] batch=%d 耗时 %.2fms", batch.n, (time.monotonic() - t0) * 1000)

    def on_batch_done(self, batch: Batch) -> None:
        # 批缓冲交回池子(推理完才能复用, 否则会覆盖正在被 NPU 读的数据)
        if self._pool is not None and batch.buf is not None:
            self._pool.release(batch.buf)


class LprWorker(BaseInferWorker):
    """LPRNet 车牌识别(1 个 NPU 核)

    ★ 这里【不用】set_io_mem 零拷贝, 而是 rknn_inputs_set + CPU 缓冲:
      1. 模型的输入张量是 INT8(zp=0, scale=1/127.5), 运行时对它做 uint8
         输入会按 FP32 尺寸(92160 字节)展开 -> 越界踩坏堆
         (runtime 原话: "param input size(23040) < model input size(92160)")
      2. 正确喂法是 int8 量化码, size=23040
      3. 车牌小图只有 160x48x3 = 23KB, batch=8 也才 184KB, CPU 拷贝可忽略
    """

    def __init__(self, cfg, in_queue, out_queue, stats, stop_event, pool=None):
        m = cfg["models"]["lpr"]
        model = RknnModel(m["path"], core_mask=cfg["inference"]["lpr_cores"])
        super().__init__("infer-lpr", model, in_queue, out_queue, stats, stop_event)
        self.in_w = m["input_w"]
        self.in_h = m["input_h"]
        self.attr = model.info.inputs[0]
        self.scale = self.attr.scale if self.attr.scale > 0 else 1.0
        self.zp = self.attr.zp
        self.n_elems = self.attr.n_elems
        self.charset = m.get("charset", "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ")
        self.batch = cfg["inference"]["lpr_batch"]
        # CPU 输入缓冲(batch 个 int8 量化码)
        self._cpu_buf = (ctypes.c_int8 * (self.batch * self.n_elems))()
        self._pool = pool

    # ------------------------------------------------------------------
    def process(self, batch: Batch) -> None:
        from ..hal.rknn import RknnInput
        # RGA 写出来的是 RGB888(uint8), 这里在 CPU 上量化成 int8 码
        inv = 1.0 / (255.0 * self.scale)
        for item in batch.items:
            off = item.slot * self.n_elems
            # batch 缓冲里的 RGB888 数据在 va + slot*in_h*in_w*3
            base = batch.va + item.slot * self.in_h * self.in_w * 3
            src = (ctypes.c_uint8 * self.n_elems).from_address(base)
            dst = ctypes.cast(ctypes.byref(self._cpu_buf, off),
                              ctypes.POINTER(ctypes.c_int8))
            for i in range(self.n_elems):
                q = int(src[i] * inv + 0.5) + self.zp
                if q > 127:
                    q = 127
                elif q < -128:
                    q = -128
                dst[i] = q

        # 每个样本单独 set 一次(runtime 的 batch 输入要求一次喂整批)
        for item in batch.items:
            off = item.slot * self.n_elems
            inp = RknnInput()
            inp.index = 0
            inp.buf = ctypes.cast(ctypes.byref(self._cpu_buf, off), ctypes.c_void_p)
            inp.size = self.n_elems
            inp.pass_through = 0
            inp.type = 2          # RKNN_TENSOR_INT8
            inp.fmt = 1          # RKNN_TENSOR_NHWC
            ret = self.model.lib.rknn_inputs_set(self.model.ctx, 1, ctypes.byref(inp))
            if ret != 0:
                log.error("[infer-lpr] rknn_inputs_set 失败 ret=%d", ret)
                continue
            self.model.run()
            outs = self.model.get_outputs()
            try:
                logits = _out_numpy(outs[0])
                plate = self._decode(logits)
                self.out_q.put((item, plate), timeout=0.0)
            finally:
                self.model.release_outputs(outs)

    def on_batch_done(self, batch: Batch) -> None:
        if self._pool is not None and batch.buf is not None:
            self._pool.release(batch.buf)

    # ------------------------------------------------------------------
    def _decode(self, logits: np.ndarray) -> str:
        """CTC 贪心解码。索引偏移: 类别 k(1..N) -> charset[k-1]"""
        T = self.model.T
        C = self.model.C
        out = []
        prev = -1
        for t in range(T):
            row = logits[t * C:(t + 1) * C]
            best = int(np.argmax(row))
            if best != prev and best != 0:
                if 1 <= best <= len(self.charset):
                    out.append(self.charset[best - 1])
            prev = best
        return "".join(out)
