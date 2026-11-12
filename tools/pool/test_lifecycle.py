#!/usr/bin/env python3
"""dmapool 在流水线里的生命周期集成测试(独立, 不依赖 rk12ch)

用最小的队列/批次模拟, 验证三件事:
    ① 正常路径: 预处理借块 -> 队列 -> 推理用完 -> 块回池
    ② 丢帧路径: 队列满了丢最旧 -> 帧引用和池化的块都要归还
    ③ 帧额度: 额度随帧一起流转, 帧被释放后额度才回来
    ④ 内存上限: 12 路的缓冲总量由池锁死, 与在途帧数无关

跑法: python3 tools/pool/test_lifecycle.py
"""
from __future__ import annotations

import os
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from dmapool import DmaBufPool, FrameSlotPool  # noqa: E402


# ---------------------------------------------------------------------------
# 最小模拟: 有界队列(满时丢最旧并释放) + 批次
# ---------------------------------------------------------------------------
class MiniQueue:
    def __init__(self, capacity: int):
        self.capacity = capacity
        self._items = []
        self.dropped = 0
        self._lock = threading.Lock()

    def put(self, item):
        with self._lock:
            if len(self._items) >= self.capacity:
                old = self._items.pop(0)
                self.dropped += 1
                old.close()          # ★ 丢帧必须归还资源
            self._items.append(item)

    def get(self):
        with self._lock:
            return self._items.pop(0) if self._items else None

    def __len__(self):
        with self._lock:
            return len(self._items)


class FakeFrameLease:
    """假装是 MPP 的 FrameLease"""
    def __init__(self):
        self.closed = False

    def close(self):
        self.closed = True

    @property
    def fd(self):
        return -1

    @property
    def vaddr(self):
        return 0


class FakeRefLease:
    """带额度的帧引用(和 rk12ch 的 RefLease 语义一致)"""
    def __init__(self, lease, slot=None, slot_pool=None):
        self.lease = lease
        self.n = 1
        self._lock = threading.Lock()
        self.slot = slot
        self.slot_pool = slot_pool

    def retain(self):
        with self._lock:
            self.n += 1
        return self

    def release(self):
        with self._lock:
            self.n -= 1
            if self.n > 0:
                return
        self.lease.close()
        if self.slot_pool is not None and self.slot is not None:
            self.slot_pool.release(self.slot)
            self.slot = None


class FakeBatch:
    def __init__(self, pool, buf, items):
        self.pool = pool
        self.buf = buf
        self.items = items

    def close(self):
        for it in self.items:
            it.raw.release()
        if self.pool is not None and self.buf is not None:
            self.pool.release(self.buf)
            self.buf = None


def make_batch(pool: DmaBufPool, frame_pool: FrameSlotPool, n: int = 2) -> FakeBatch:
    buf = pool.acquire()
    assert buf is not None, "池不应该一开始就耗尽"
    items = []
    for i in range(n):
        slot = frame_pool.acquire()
        items.append(type("Item", (), {
            "raw": FakeRefLease(FakeFrameLease(), slot=slot, slot_pool=frame_pool)})())
    return FakeBatch(pool, buf, items)


# ---------------------------------------------------------------------------
def test_normal_path_returns_buffer():
    pool = DmaBufPool("t", 4096, 2, prefer_dmabuf=False)
    fp = FrameSlotPool("f", 4)
    q = MiniQueue(8)

    batch = make_batch(pool, fp, n=2)
    assert pool.in_use == 1 and fp.stats.in_use == 2
    q.put(batch)

    got = q.get()
    assert got is batch
    got.pool.release(got.buf)          # 推理用完 -> 归还池化的块
    got.buf = None
    assert pool.in_use == 0, "推理完成后块必须回池"
    assert pool.available == 2
    for item in got.items:             # 帧引用释放 -> 额度归还
        item.raw.release()
    assert fp.stats.in_use == 0, "帧释放后额度必须回来"
    pool.close()
    print("  ✓ 正常路径: 块与额度都回到池里")


def test_drop_path_releases_everything():
    pool = DmaBufPool("t", 4096, 4, prefer_dmabuf=False)
    fp = FrameSlotPool("f", 8)
    q = MiniQueue(2)

    b1 = make_batch(pool, fp, n=1)
    b2 = make_batch(pool, fp, n=1)
    b3 = make_batch(pool, fp, n=1)
    q.put(b1)
    q.put(b2)
    assert pool.in_use == 3, pool.in_use       # b3 也已经借走了块和额度
    assert fp.stats.in_use == 3

    q.put(b3)                                  # 队列满 -> 丢最旧的 b1
    assert q.dropped == 1
    assert b1.items[0].raw.lease.closed, "被丢的帧必须 deinit"
    assert pool.in_use == 2, "被丢的那批的块也必须回池"
    assert fp.stats.in_use == 2, "额度也必须跟着回"

    while True:
        b = q.get()
        if b is None:
            break
        b.pool.release(b.buf)
        b.buf = None
        for item in b.items:
            item.raw.release()
    assert pool.in_use == 0
    assert pool.available == 4
    assert fp.stats.in_use == 0
    pool.close()
    print("  ✓ 丢帧路径: 帧引用和块都归还, 池不泄漏")


def test_pool_exhaustion_drops_batch():
    pool = DmaBufPool("t", 4096, 1, prefer_dmabuf=False)
    fp = FrameSlotPool("f", 4)

    b1 = make_batch(pool, fp, n=1)
    assert pool.available == 0
    assert pool.acquire() is None, "池耗尽必须返回 None(上层据此跳帧)"
    assert pool.stats.exhausted_count == 1
    assert pool.count == 1, "池耗尽绝不扩容"

    b1.pool.release(b1.buf)
    b1.buf = None
    for item in b1.items:
        item.raw.release()
    assert pool.available == 1
    pool.close()
    print("  ✓ 池耗尽: 返回空而不是扩容/崩溃")


def test_memory_is_flat_across_channels():
    pool = DmaBufPool("t", 1024 * 1024, 36, prefer_dmabuf=False)
    assert pool.count == 36, "36 块 = 12 路 x 2 并发 x 1.5 安全系数"
    held = [pool.acquire() for _ in range(24)]      # 12 路 x 2 在途
    assert all(h is not None for h in held)
    assert pool.in_use == 24 and pool.available == 12
    for h in held:
        pool.release(h)
    assert pool.in_use == 0
    pool.close()
    print("  ✓ 12 路内存上限由池锁死(36 块), 与在途帧数无关")


def main() -> int:
    print("=" * 62)
    print(" 池化生命周期集成测试")
    print("=" * 62)
    tests = [test_normal_path_returns_buffer,
             test_drop_path_releases_everything,
             test_pool_exhaustion_drops_batch,
             test_memory_is_flat_across_channels]
    failed = 0
    for t in tests:
        try:
            t()
        except AssertionError as e:
            print(f"  ✗ {t.__name__}: {e}")
            failed += 1
        except Exception as e:  # noqa: BLE001
            print(f"  ✗ {t.__name__}: {type(e).__name__}: {e}")
            failed += 1
    print("=" * 62)
    print(f" {len(tests) - failed}/{len(tests)} 通过")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
