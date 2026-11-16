"""流水线各层之间传递的数据结构

时间戳统一用 time.monotonic()(纳秒), 跨层不换算, 避免时钟源不一致。
"""

from __future__ import annotations

import threading
import time
from dataclasses import dataclass, field


class RefLease:
    """MPP frame 的引用计数包装。

    ★ 一帧里可能有多个车 -> 多个车牌裁剪请求共享同一帧。
      谁最后释放谁负责 mpp_frame_deinit, 否则会重复释放(崩)或泄漏(fd 耗尽)。
    """

    __slots__ = ("lease", "n", "_lock")

    def __init__(self, lease):
        self.lease = lease
        self.n = 1
        self._lock = threading.Lock()

    def retain(self) -> "RefLease":
        with self._lock:
            self.n += 1
        return self

    def release(self) -> None:
        with self._lock:
            self.n -= 1
            if self.n <= 0:
                self.lease.close()

    @property
    def fd(self) -> int:
        return self.lease.fd

    @property
    def va(self) -> int:
        return self.lease.va


@dataclass
class RawFrame:
    """解码层 -> 预处理层: 一帧 NV12 (持有 MPP lease 的引用计数)"""
    channel_id: int
    ref: RefLease
    frame_id: int
    t_decode: float = field(default_factory=time.monotonic)   # 解码完成时刻
    pts_us: int = 0
    width: int = 0
    height: int = 0
    hor_stride: int = 0
    ver_stride: int = 0

    @property
    def lease(self):
        return self.ref.lease

    @property
    def fd(self) -> int:
        return self.ref.fd

    @property
    def va(self) -> int:
        return self.ref.va

    def close(self) -> None:
        self.ref.release()


@dataclass
class BatchItem:
    """预处理层 -> 推理层: batch 里的一个样本"""
    channel_id: int
    frame_id: int
    t_decode: float
    # batch 内的下标; 帧数据已经由 RGA 写进 batch 缓冲的对应偏移
    slot: int = 0
    meta: dict = field(default_factory=dict)
    # ★ 原始帧的引用: 车牌裁剪要用【原图】, 不能用 640x640 的模型输入
    #   (缩到 640 再裁 160x48, 车牌像素只剩 1/4, 识别率会崩)
    raw: RefLease | None = None
    frame_w: int = 0
    frame_h: int = 0
    hor_stride: int = 0
    ver_stride: int = 0


@dataclass
class Batch:
    """一个 batch: RGA 已经把各帧写进同一块连续缓冲"""
    items: list[BatchItem]
    fd: int = -1          # batch 缓冲的 dma-buf fd
    va: int = 0
    n: int = 0            # 实际样本数
    t_prepare: float = field(default_factory=time.monotonic)
    buf: object = None    # BatchBufferPool 的 DmaBuf(用完归还)

    def close(self) -> None:
        pass


@dataclass
class Detection:
    cls_id: int
    score: float
    x1: float
    y1: float
    x2: float
    y2: float
    track_id: int = -1
    plate: str = ""


@dataclass
class TrackResult:
    track_id: int
    channel_id: int
    frame_id: int
    box: tuple[float, float, float, float]
    score: float
    cls_id: int
    t_decode: float
    speed_kmh: float = 0.0
    speed_valid: bool = False
    plate: str = ""


@dataclass
class PlateRequest:
    """业务层 -> 预处理层(车牌裁剪): 要裁的车牌 ROI"""
    channel_id: int
    frame_id: int
    track_id: int
    ref: RefLease              # 原始帧的引用(裁剪后 release)
    t_decode: float
    box: tuple[float, float, float, float]   # 原图坐标系的 ROI
    frame_w: int
    frame_h: int
    src_geometry: tuple[int, int, int, int]  # (hor_stride, ver_stride, w, h)


@dataclass
class PlateResult:
    channel_id: int
    frame_id: int
    track_id: int
    plate: str
    score: float
    t_decode: float
    t_plate: float = field(default_factory=time.monotonic)
