"""层间队列: 有界 + 丢帧策略 + 积压统计

★ 设计原则(12 路场景下的取舍):
    1. 队列必须有界。无界队列在下游卡顿时会把内存吃光, 最后 OOM。
    2. 丢【最旧】而不是丢【最新】: 实时监控场景里, 新帧永远比旧帧有价值。
    3. 队列深度要能看出积压: 满队列 = 下游跟不上, 是最重要的健康指标。
"""

from __future__ import annotations

import collections
import logging
import threading
import time
from dataclasses import dataclass, field

log = logging.getLogger(__name__)


def _release(item) -> None:
    """释放帧持有的底层资源(MPP frame deinit / dma-buf close)"""
    closer = getattr(item, "close", None)
    if callable(closer):
        try:
            closer()
        except Exception:
            log.exception("释放帧资源失败")


@dataclass
class QueueStats:
    name: str
    size: int = 0
    capacity: int = 0
    pushed: int = 0
    popped: int = 0
    dropped: int = 0
    max_depth: int = 0
    push_wait_total_ms: float = 0.0
    pop_wait_total_ms: float = 0.0

    @property
    def drop_rate(self) -> float:
        total = self.pushed + self.dropped
        return self.dropped / total if total else 0.0


class BoundedQueue:
    """线程安全有界队列(FIFO), 满时丢最旧"""

    def __init__(self, name: str, capacity: int = 8):
        self.name = name
        self.capacity = max(1, capacity)
        self._q: collections.deque = collections.deque()
        self._lock = threading.Lock()
        self._not_empty = threading.Condition(self._lock)
        self._not_full = threading.Condition(self._lock)
        self.stats = QueueStats(name=name, capacity=self.capacity)
        self._closed = False

    # ------------------------------------------------------------------
    def put(self, item, timeout: float = 0.0) -> bool:
        """入队。超时则返回 False。满时丢最旧的一帧"""
        t0 = time.perf_counter()
        with self._not_full:
            deadline = time.monotonic() + timeout
            while len(self._q) >= self.capacity and not self._closed:
                left = deadline - time.monotonic()
                if left <= 0:
                    break
                self._not_full.wait(left)
            if self._closed:
                return False
            if len(self._q) >= self.capacity:
                # ★ 丢最旧: 实时场景下旧帧没有价值, 新帧才有
                old = self._q.popleft()
                self.stats.dropped += 1
                # ★ 被丢的帧必须释放它持有的 dma-buf 引用(MPP frame / dma-buf fd),
                #   否则跑几分钟 fd 就耗尽了
                _release(old)
            self._q.append(item)
            self.stats.pushed += 1
            self.stats.size = len(self._q)
            self.stats.max_depth = max(self.stats.max_depth, len(self._q))
            self.stats.push_wait_total_ms += (time.perf_counter() - t0) * 1000.0
            self._not_empty.notify()
            return True

    # ------------------------------------------------------------------
    def get(self, timeout: float = 1.0):
        """出队。超时返回 None"""
        t0 = time.perf_counter()
        with self._not_empty:
            deadline = time.monotonic() + timeout
            while not self._q and not self._closed:
                left = deadline - time.monotonic()
                if left <= 0:
                    break
                self._not_empty.wait(left)
            if not self._q:
                return None
            item = self._q.popleft()
            self.stats.popped += 1
            self.stats.size = len(self._q)
            self.stats.pop_wait_total_ms += (time.perf_counter() - t0) * 1000.0
            self._not_full.notify()
            return item

    # ------------------------------------------------------------------
    def drain(self, max_items: int = 0) -> list:
        """一次性取走当前所有(用于组 batch)"""
        with self._lock:
            n = len(self._q) if max_items <= 0 else min(max_items, len(self._q))
            items = [self._q.popleft() for _ in range(n)]
            self.stats.popped += len(items)
            self.stats.size = len(self._q)
            self._not_full.notify_all()
            return items

    def qsize(self) -> int:
        with self._lock:
            return len(self._q)

    def close(self) -> None:
        with self._not_full:
            self._closed = True
            self._not_full.notify_all()
        with self._not_empty:
            self._not_empty.notify_all()

    @property
    def closed(self) -> bool:
        return self._closed


class FrameSlotPool:
    """输入缓冲池: 给 NPU 输入用, 避免每帧 malloc/free

    ★ 瓶颈提示: Python 的对象分配 + GC 在 12 路 x 10fps = 120 帧/秒时,
      如果每帧都新建大 bytearray, 会有明显的 GC 抖动。这里用池子复用。
    """

    def __init__(self, size: int, item_size: int):
        self.size = size
        self.item_size = item_size
        self._free: collections.deque = collections.deque()
        self._lock = threading.Lock()
        for _ in range(size):
            self._free.append(bytearray(item_size))

    def acquire(self) -> bytearray | None:
        with self._lock:
            if not self._free:
                return None
            return self._free.pop()

    def release(self, buf: bytearray) -> None:
        with self._lock:
            if len(self._free) < self.size:
                self._free.append(buf)

    @property
    def available(self) -> int:
        with self._lock:
            return len(self._free)
