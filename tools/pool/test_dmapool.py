#!/usr/bin/env python3
"""dmapool 单元测试(独立, 不依赖 rk12ch)

覆盖三条核心语义 + 边界:
    ① 借还 1:1
    ② 引用计数归零才可复用
    ③ 池耗尽返回空(不崩、不扩容)
    + 重复归还只记数不崩 / 并发互斥 / fd-vaddr 终身不变 / 容量公式

跑法:
    python3 tools/pool/test_dmapool.py
"""
from __future__ import annotations

import os
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from dmapool import (BufferLease, DmaBufPool, FrameSlotPool,  # noqa: E402
                     block_size_for, build_pools, pool_size)


def new_pool(count: int = 4, size: int = 4096) -> DmaBufPool:
    # 单元测试用 CPU 内存: 池的借用/引用计数语义与底层内存无关,
    # 必须能在没有 dma-buf 的环境里独立验证。
    return DmaBufPool(f"test-{count}", size, count, prefer_dmabuf=False)


# ---------------------------------------------------------------------------
def test_borrow_return_one_to_one():
    p = new_pool(4)
    assert p.available == 4
    a, b = p.acquire(), p.acquire()
    assert a is not None and b is not None
    assert a is not b, "两次 acquire 必须拿到不同的块"
    assert p.available == 2 and p.in_use == 2
    p.release(a)
    assert p.available == 3
    p.release(b)
    assert p.available == 4 and p.in_use == 0
    assert p.stats.acquire_count == 2 and p.stats.release_count == 2
    p.close()
    print("  ✓ 借还 1:1")


def test_refcount_blocks_reuse_until_zero():
    p = new_pool(2)
    lease = BufferLease(p, p.acquire())
    assert lease.ref_count == 1

    lease2 = lease.retain()                 # 交给第二个使用方
    assert lease2 is not lease, "转交必须返回新的 lease"
    assert lease.ref_count == 2

    lease.release()                          # 第一个使用方用完
    assert lease.ref_count == 1, "还有人在用 -> 不能回池"
    assert p.available == 1
    assert p.in_use == 1

    lease2.release()                         # 最后一个使用方用完
    assert lease.ref_count == 0
    assert p.available == 2, "归零后回到空闲链表"
    assert p.in_use == 0
    p.close()
    print("  ✓ 引用计数归零才可复用")


def test_fd_and_vaddr_never_change():
    p = new_pool(3)
    first = {}
    for _ in range(5):
        # 一次借满 -> 三个 index 都会出现
        # (池是 LIFO: 借一块还一块时永远复用同一块, 对 cache 局部性是好事)
        held = [p.acquire() for _ in range(3)]
        assert all(b is not None for b in held)
        for buf in held:
            if buf.index not in first:
                first[buf.index] = (buf.fd, buf.vaddr, buf.size)
            else:
                assert first[buf.index] == (buf.fd, buf.vaddr, buf.size), \
                    "同一块内存的 fd/vaddr 不允许变化"
            p.release(buf)
    assert len(first) == 3, f"3 块内存, 借还 5 轮, 实际见过 {len(first)} 块"
    assert p.available == 3
    p.close()
    print("  ✓ fd/vaddr 终身不变")


def test_exhausted_returns_none_not_crash():
    p = new_pool(2)
    a, b = p.acquire(), p.acquire()
    assert p.acquire() is None, "池耗尽必须返回 None"
    assert p.available == 0
    assert p.stats.exhausted_count == 1
    assert p.count == 2, "池耗尽绝不扩容"
    p.release(a)
    p.release(b)
    assert p.acquire() is not None, "有块归还后必须能再借到"
    p.close()
    print("  ✓ 池耗尽返回空而不是崩溃")


def test_double_release_is_counted_not_crashed():
    p = new_pool(2)
    buf = p.acquire()
    assert p.release(buf) is True
    assert p.release(buf) is False
    assert p.stats.double_release_count == 1
    assert p.available == 2, "重复归还不能把块放回两次"
    p.close()
    print("  ✓ 重复归还只记数不崩溃")


def test_multithread_no_double_borrow():
    pool = new_pool(8)
    seen = []
    lock = threading.Lock()

    def worker():
        for _ in range(200):
            buf = pool.acquire()
            if buf is None:
                continue
            with lock:
                seen.append(buf.index)
            threading.Event().wait(0.0005)
            pool.release(buf)

    ts = [threading.Thread(target=worker) for _ in range(8)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    assert pool.in_use == 0
    assert pool.available == 8
    assert len(seen) > 0
    pool.close()
    print(f"  ✓ 多线程并发借还({len(seen)} 次借出), 无重复占用")


def test_concurrent_acquire_exclusive():
    pool = new_pool(4)
    bad = []
    lock = threading.Lock()
    active = set()

    def worker():
        for _ in range(300):
            buf = pool.acquire()
            if buf is None:
                continue
            with lock:
                if buf.index in active:
                    bad.append(buf.index)
                active.add(buf.index)
            with lock:
                active.discard(buf.index)
            pool.release(buf)

    ts = [threading.Thread(target=worker) for _ in range(6)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    assert not bad, f"同一块被同时借出: {bad[:5]}"
    pool.close()
    print("  ✓ 并发 acquire 互斥(同一块不会被两个线程同时持有)")


def test_frame_slot_pool():
    p = FrameSlotPool("frame", 3)
    s1, s2, s3 = p.acquire(), p.acquire(), p.acquire()
    assert s1 is not None and s2 is not None and s3 is not None
    assert p.acquire() is None, "额度用完必须返回 None"
    assert p.stats.exhausted_count == 1
    p.release(s1)
    assert p.acquire() is not None
    print("  ✓ 帧额度池(并发准入控制)")


def test_pool_size_formula():
    # 每路并发 2 x 12 路 = 24 块, 安全系数 1.5 -> 36
    assert pool_size(2, 12, 1.5, batch=1) == 36
    # batch=4: 24 帧 -> 6 块 -> x1.5 -> 9 块
    assert pool_size(2, 12, 1.5, batch=4) == 9
    assert pool_size(1, 1, 1.0, batch=1) == 2
    print("  ✓ 池容量公式(每路并发 x 路数 x 安全系数)")


def test_block_size():
    size = block_size_for(640, 640, batch=4)
    assert size == (4 * 640 + 16) * 640 * 3
    assert size > 640 * 640 * 3 * 4, "必须留 RGA 余量行"
    print("  ✓ 块大小(含 RGA 余量行)")


def test_build_pools_12_channels():
    b = build_pools(channels=12, yolo_in=(640, 640), lpr_in=(160, 48),
                    yolo_batch=4, lpr_batch=8,
                    per_channel_concurrency=2, safety_factor=1.5)
    assert b.yolo.count == 9 and b.lpr.count == 5 and b.frame.count == 36
    assert b.yolo.block_size != b.lpr.block_size, "不同分辨率必须是不同的池"
    total = (b.yolo.count * b.yolo.block_size + b.lpr.count * b.lpr.block_size)
    print(f"  ✓ 12 路建池: {b.yolo.describe()}")
    print(f"     {b.lpr.describe()}")
    print(f"     {b.frame.describe()}")
    print(f"     缓冲总内存 ≈ {total / 1024 / 1024:.1f}MB (与在途帧数无关)")
    b.yolo.close()
    b.lpr.close()


def main() -> int:
    tests = [test_borrow_return_one_to_one,
             test_refcount_blocks_reuse_until_zero,
             test_fd_and_vaddr_never_change,
             test_exhausted_returns_none_not_crash,
             test_double_release_is_counted_not_crashed,
             test_multithread_no_double_borrow,
             test_concurrent_acquire_exclusive,
             test_frame_slot_pool,
             test_pool_size_formula,
             test_block_size,
             test_build_pools_12_channels]
    print("=" * 62)
    print(" dmapool 单元测试")
    print("=" * 62)
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
