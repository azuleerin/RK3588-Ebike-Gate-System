"""第 2 层: 预处理层 —— RGA 硬件裁剪/缩放/颜色转换, 全程零拷贝

数据流:
    Q1(NV12 帧) -> [RGA] -> batch 缓冲(dma-buf) -> Q2 -> 推理层

★ 为什么把 batch 组装放在这一层:
    NPU 的 batch 输入要求【一块连续的内存】(rknn_set_io_mem 只能绑一块)。
    如果在推理层组 batch, 就得把各路已经准备好的图再 memcpy 进 batch 缓冲
    (12 路 x 640x640x3 = 4.9MB/batch), 这一下就把"零拷贝"打破了。
    在 RGA 之前组装 -> RGA 直接把第 i 帧写进 batch 缓冲的第 i 个槽位
    (drect.y = i * input_h), NPU 直接读同一块物理页, 全程零次 CPU 拷贝。

★ RGA 的两个硬性要求(踩过坑):
    1. 目标缓冲要多留几行余量: RGA 在缩放比例不整除时会多写 1~2 行。
    2. 源/目标都必须是 dma-buf(fd>=0): 用户态内存会让 librga 走软件 MMU,
       缓存的 VA->PA 失效后会把输出写到别的分配里 -> 堆随机损坏。
"""

from __future__ import annotations

import ctypes
import logging
import threading
import time
from collections import deque

from ..hal.dmabuf import DmaBuf, alloc_buffer, alloc_cpu_buffer
from ..hal.rga import Rga, RgaImage, clamp_roi
from .queues import BoundedQueue
from .types import Batch, BatchItem, PlateRequest, RawFrame

log = logging.getLogger(__name__)

# RGA 目标缓冲的余量行(RGA 会多刷 1~2 行)
RGA_PAD_ROWS = 16


def cpu_nv12_to_rgb(va: int, width: int, height: int, hor_stride: int,
                    ver_stride: int, sx: int, sy: int, sw: int, sh: int,
                    dw: int, dh: int) -> bytes:
    """CPU 兜底: 从【CPU 可映射】的 NV12 源裁一块 -> 缩放 -> RGB888 字节流。

    ★ 只在源有 CPU 映射时可用(VA != 0)。
      颜色空间按 BT.601 limited range 处理; 如果解码出来的是 yuvj420p(全范围),
      颜色会偏暗 —— 这条路径只是"RGA 不可用时的保命"方案, 上生产建议修好 dma-buf。
    """
    import numpy as np
    src = np.frombuffer((ctypes.c_uint8 * (hor_stride * ver_stride * 3 // 2))
                         .from_address(va), dtype=np.uint8)
    y = src[:hor_stride * ver_stride].reshape(ver_stride, hor_stride)[:height, :width]
    uv_base = hor_stride * ver_stride
    uv = src[uv_base:uv_base + (hor_stride * ver_stride // 2)]
    uv = uv[:((height + 1) // 2) * hor_stride].reshape((height + 1) // 2, hor_stride)
    u = uv[:(height + 1) // 2, :width // 2].astype(np.int16)
    v = uv[:(height + 1) // 2, :width // 2].astype(np.int16)
    u = np.repeat(np.repeat(u, 2, axis=0), 2, axis=1)[:height, :width]
    v = np.repeat(np.repeat(v, 2, axis=0), 2, axis=1)[:height, :width]
    yy = y.astype(np.int16)
    r = (298 * (yy - 16) + 409 * (v - 128) + 128) >> 8
    g = (298 * (yy - 16) - 100 * (u - 128) - 208 * (v - 128) + 128) >> 8
    b = (298 * (yy - 16) + 516 * (u - 128) + 128) >> 8
    rgb = np.stack([r, g, b], axis=-1).clip(0, 255).astype(np.uint8)
    roi = rgb[sy:sy + sh, sx:sx + sw]
    # 最近邻缩放到 dw x dh(够用, 精度要求高的话换 cv2)
    yi = (np.arange(dh) * sh // dh).clip(0, sh - 1)
    xi = (np.arange(dw) * sw // dw).clip(0, sw - 1)
    out = roi[yi][:, xi]
    return out.tobytes()


class BatchBufferPool:
    """batch 输入缓冲池(每块都是一块 dma-buf)"""

    def __init__(self, count: int, batch: int, in_w: int, in_h: int):
        self.batch = batch
        self.in_w = in_w
        self.in_h = in_h
        self.frame_bytes = in_w * in_h * 3
        # + 余量行, 防止 RGA 越界写
        self.alloc_bytes = (batch * in_h + RGA_PAD_ROWS) * in_w * 3
        self._pool: deque[DmaBuf] = deque()
        self._lock = threading.Lock()
        self.dmabuf = True
        for _ in range(count):
            buf = alloc_buffer(self.alloc_bytes)
            if buf is None:
                # ★ 没有 dma-buf 也能跑: 退化成 CPU 内存。
                #   代价是 RGA 用不了(软件 MMU 会写坏堆), 走预处理层的 CPU 兜底。
                self.dmabuf = False
                buf = alloc_cpu_buffer(self.alloc_bytes)
            self._pool.append(buf)
        log.info("[preprocess] batch 缓冲池: %d 块 x %d 帧 (%d KB/块)%s",
                 count, batch, self.alloc_bytes // 1024,
                 "" if self.dmabuf else "  [无 dma-buf -> 走 CPU 转换]")

    def acquire(self) -> DmaBuf | None:
        with self._lock:
            return self._pool.popleft() if self._pool else None

    def release(self, buf: DmaBuf) -> None:
        with self._lock:
            self._pool.append(buf)

    def close(self) -> None:
        with self._lock:
            for b in self._pool:
                b.close()
            self._pool.clear()


class PreprocessLayer:
    """YOLO 输入预处理: 组 batch + RGA 拉伸到模型输入尺寸"""

    def __init__(self, cfg: dict, in_queue: BoundedQueue, out_queue: BoundedQueue,
                 stats, stop_event: threading.Event):
        self.cfg = cfg
        self.in_q = in_queue
        self.out_q = out_queue
        self.stats = stats
        self.stop_event = stop_event
        self.rga = Rga.instance()

        m = cfg["models"]["yolo"]
        self.in_w = m["input_w"]
        self.in_h = m["input_h"]
        self.batch = cfg["inference"]["yolo_batch"]
        self.timeout = cfg["inference"]["yolo_timeout_ms"] / 1000.0

        self.pool = BatchBufferPool(count=2, batch=self.batch,
                                     in_w=self.in_w, in_h=self.in_h)
        self._thread = threading.Thread(target=self._run, name="preprocess-yolo", daemon=True)

    # ------------------------------------------------------------------
    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self.stop_event.set()
        self._thread.join(timeout=3.0)
        self.pool.close()

    # ------------------------------------------------------------------
    def _run(self) -> None:
        """组装线程: 攒够一个 batch 就做一次 RGA"""
        pending: list[RawFrame] = []
        while not self.stop_event.is_set():
            frame = self.in_q.get(timeout=0.2)
            if frame is not None:
                pending.append(frame)
                # 一次最多攒 batch_size 帧
                while len(pending) < self.batch:
                    extra = self.in_q.get(timeout=self.timeout if not pending else 0.0)
                    if extra is None:
                        break
                    pending.append(extra)
            if not pending:
                continue
            self._process_batch(pending)
            pending = []

    # ------------------------------------------------------------------
    def _process_batch(self, frames: list[RawFrame]) -> None:
        buf = self.pool.acquire()
        if buf is None:
            for f in frames:
                f.close()
            return

        items: list[BatchItem] = []
        for slot, frame in enumerate(frames):
            src = RgaImage(
                fd=frame.fd,
                width=frame.width,
                height=frame.height,
                wstride=frame.hor_stride,
                hstride=frame.ver_stride,
                format=0xA << 8,       # RK_FORMAT_YCbCr_420_SP
                vir_addr=frame.va,
            )
            dst = RgaImage(
                fd=buf.fd,
                width=self.in_w,
                height=self.in_h,
                wstride=self.in_w * 3,
                hstride=self.batch * self.in_h,     # 整个 batch 的行数
                format=0x2 << 8,       # RK_FORMAT_RGB_888
                vir_addr=buf.va,
            )
            # drect.y = slot * in_h -> 直接写进 batch 缓冲的对应槽位
            if not self.rga.can_use(src, dst):
                # CPU 兜底: 没有 dma-buf 时 RGA 不能用(软件 MMU 会写坏堆)
                ret = 0
                if frame.va:
                    try:
                        rgb = cpu_nv12_to_rgb(frame.va, frame.width, frame.height,
                                              frame.hor_stride, frame.ver_stride,
                                              0, 0, frame.width, frame.height,
                                              self.in_w, self.in_h)
                        ctypes.memmove(buf.va + slot * self.in_h * self.in_w * 3,
                                       rgb, len(rgb))
                    except Exception:
                        log.exception("[preprocess] CPU 兜底转换失败")
                        ret = -1
                else:
                    ret = -1
            else:
                ret = self.rga.crop_scale(src, dst, 0, 0, frame.width, frame.height,
                                          dw=self.in_w, dh=self.in_h)
            # ★ 失败的帧【也要占位】: batch 的第 i 个样本必须对应缓冲的第 i 个槽位,
            #   少一个就会让后面所有样本的 batch 下标全部错位(框对不上帧)。
            if ret != 0:
                log.warning("[preprocess] RGA 失败 ch=%d slot=%d ret=%d",
                            frame.channel_id, slot, ret)
            items.append(BatchItem(
                channel_id=frame.channel_id,
                frame_id=frame.frame_id,
                t_decode=frame.t_decode,
                slot=slot,
                raw=frame.ref,
                frame_w=frame.width,
                frame_h=frame.height,
                hor_stride=frame.hor_stride,
                ver_stride=frame.ver_stride,
                meta={"valid": ret == 0},
            ))

        # ★ 源帧【不能】在这里释放: 车牌裁剪要用原图(而不是 640x640 的模型输入)。
        #   帧的引用交给业务层/车牌预处理层去释放(RefLease 计数)。

        if not items:
            self.pool.release(buf)
            return
        batch = Batch(items=items, fd=buf.fd, va=buf.va, n=len(items), buf=buf)
        self.out_q.put(batch, timeout=0.0)


class PlatePreprocessLayer:
    """车牌裁剪: 从原帧按 ROI 裁出 160x48, 再组 batch"""

    def __init__(self, cfg: dict, in_queue: BoundedQueue, out_queue: BoundedQueue,
                 stop_event: threading.Event):
        self.cfg = cfg
        self.in_q = in_queue
        self.out_q = out_queue
        self.stop_event = stop_event
        self.rga = Rga.instance()

        m = cfg["models"]["lpr"]
        self.in_w = m["input_w"]
        self.in_h = m["input_h"]
        self.batch = cfg["inference"]["lpr_batch"]
        self.pool = BatchBufferPool(count=2, batch=self.batch,
                                     in_w=self.in_w, in_h=self.in_h)
        self._thread = threading.Thread(target=self._run, name="preprocess-lpr", daemon=True)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self.stop_event.set()
        self._thread.join(timeout=3.0)
        self.pool.close()

    # ------------------------------------------------------------------
    def _run(self) -> None:
        pending: list[PlateRequest] = []
        while not self.stop_event.is_set():
            req = self.in_q.get(timeout=0.2)
            if req is not None:
                pending.append(req)
                while len(pending) < self.batch:
                    extra = self.in_q.get(timeout=0.0)
                    if extra is None:
                        break
                    pending.append(extra)
            if not pending:
                continue
            self._process_batch(pending)
            pending = []

    # ------------------------------------------------------------------
    def _process_batch(self, reqs: list[PlateRequest]) -> None:
        buf = self.pool.acquire()
        if buf is None:
            for r in reqs:
                r.lease.close()
            return

        items = []
        for slot, req in enumerate(reqs):
            x, y, w, h = req.box
            # ★ ROI 必须 clamp 到画面内: 取整误差 + 外扩都会让 x+w 越界,
            #   RGA 越界不会被检查 -> 画到画外 / 踩坏堆
            roi = clamp_roi(int(x), int(y), int(w), int(h), req.frame_w, req.frame_h, 8, 6)
            if roi is None:
                req.lease.close()
                continue
            sx, sy, sw, sh = roi
            hor_stride, ver_stride, fw, fh = req.src_geometry
            src = RgaImage(
                fd=req.lease.fd,
                width=fw,
                height=fh,
                wstride=hor_stride,
                hstride=ver_stride,
                format=0xA << 8,
                vir_addr=req.lease.va,
            )
            dst = RgaImage(
                fd=buf.fd,
                width=self.in_w,
                height=self.in_h,
                wstride=self.in_w * 3,
                hstride=self.batch * self.in_h,
                format=0x2 << 8,
                vir_addr=buf.va,
            )
            if not self.rga.can_use(src, dst):
                # CPU 兜底: 没有 dma-buf 时 RGA 不能用(软件 MMU 会写坏堆)
                ret = 0
                if req.lease.va:
                    try:
                        rgb = cpu_nv12_to_rgb(req.lease.va, fw, fh, hor_stride, ver_stride,
                                              sx, sy, sw, sh, self.in_w, self.in_h)
                        ctypes.memmove(buf.va + slot * self.in_h * self.in_w * 3,
                                       rgb, len(rgb))
                    except Exception:
                        log.exception("[preprocess-lpr] CPU 兜底转换失败")
                        ret = -1
                else:
                    ret = -1
            else:
                ret = self.rga.crop_scale(src, dst, sx, sy, sw, sh,
                                          dw=self.in_w, dh=self.in_h)
            if ret != 0:
                log.warning("[preprocess-lpr] RGA 失败 ret=%d box=%s", ret, req.box)
            items.append(BatchItem(
                channel_id=req.channel_id,
                frame_id=req.frame_id,
                t_decode=req.t_decode,
                slot=slot,
                meta={"track_id": req.track_id, "valid": ret == 0},
            ))

        for r in reqs:
            r.lease.close()

        if not items:
            self.pool.release(buf)
            return
        self.out_q.put(Batch(items=items, fd=buf.fd, va=buf.va, n=len(items), buf=buf),
                       timeout=0.0)
