"""dma-buf 分配/映射 (ctypes 直连 dma-heap ioctl)

零拷贝链路的地基:
    MPP 解码 -> dma-buf(fd)
        -> RGA  import fd 直接读写
            -> RKNN set_io_mem(fd)  NPU 直接读物理页
    全程没有任何一次 CPU memcpy。

⚠️ 两个必须记住的坑(和 C++ 版 dmabuf.cc 完全一致):

1. dma-heap 有【两个】不同的 ioctl, magic 不对会 EINVAL 静默失败:
       ① Rockchip 版:  _IOWR('R', 0x00, struct rk_dma_heap_allocation_data)
       ② 标准版:      _IOWR('H', 0x00, struct dma_heap_allocation_data)
   两者结构体布局相同, 只是 magic 不同。

2. 本板子内核【没有实现 dma-buf 的 cache 同步 ioctl】,
   CPU 侧写完必须自己 memcpy 一次(或走 RGA), 不要指望 DMA 写完 CPU 能立刻看到。
"""

from __future__ import annotations

import ctypes
import fcntl
import logging
import mmap
import os
import struct
from dataclasses import dataclass

log = logging.getLogger(__name__)

# ---- ioctl 号 (与 <linux/dma-heap.h> / rockchip 私有版本一致) ----
# ★ Python 的 fcntl 没有 IOWR 宏, 得自己按 Linux 的 _IOC 编码算:
#       _IOC(dir, type, nr, size) = (dir<<30) | (size<<16) | (type<<8) | nr
#       _IOWR = (read|write) = 3
def _IOC(direction: int, magic: int, nr: int, size: int) -> int:
    return (direction << 30) | (size << 16) | (magic << 8) | nr


_IOWR = 3
RK_DMA_HEAP_IOCTL_ALLOC_R = _IOC(_IOWR, ord('R'), 0x00, 24)   # Rockchip 版
DMA_HEAP_IOCTL_ALLOC_H = _IOC(_IOWR, ord('H'), 0x00, 24)       # 标准版
RK_DMA_BUF_IOCTL_SYNC = _IOC(_IOWR, ord('P'), 0x02, 16)       # cache 同步(本板内核未实现)

RK_DMA_HEAP_UNCACHED = 1 << 0      # Rockchip 版 uncached 标志


class _HeapAllocData(ctypes.Structure):
    """struct rk_dma_heap_alloc_data (24 字节, 两个 magic 共用同一布局)

        u64 len; u32 fd; u32 fd_flags; u64 heap_flags;
    """
    _fields_ = [
        ("len", ctypes.c_uint64),
        ("fd", ctypes.c_uint32),
        ("fd_flags", ctypes.c_uint32),
        ("heap_flags", ctypes.c_uint64),
    ]


class _BufSyncData(ctypes.Structure):
    """struct rk_dma_buf_sync (16 字节)"""
    _fields_ = [
        ("flags", ctypes.c_uint32),
        ("start", ctypes.c_uint64),
        ("end", ctypes.c_uint64),
    ]


class DmaBuf:
    """一块 dma-buf: fd + CPU 映射地址 + 物理地址由内核管理"""

    __slots__ = ("fd", "size", "_mm", "va")

    def __init__(self, fd: int, size: int, mm: mmap.mmap | None, va: int):
        self.fd = fd
        self.size = size
        self._mm = mm
        self.va = va

    @property
    def valid(self) -> bool:
        return self.fd >= 0

    def sync(self) -> bool:
        """DMA 写 -> CPU 读的 cache 同步(本板子内核可能不支持, 失败就忽略)"""
        if self.fd < 0:
            return False
        data = _BufSyncData()
        data.flags = 0x00000000 | 0x00000004   # START | READ
        data.start = 0
        data.end = self.size
        try:
            fcntl.ioctl(self.fd, RK_DMA_BUF_IOCTL_SYNC, data, True)
            return True
        except OSError:
            return False      # 本板内核没实现这个 ioctl

    def close(self) -> None:
        if self._mm is not None:
            try:
                self._mm.close()
            except Exception:
                pass
            self._mm = None
        if self.fd >= 0:
            try:
                os.close(self.fd)
            except OSError:
                pass
            self.fd = -1

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def alloc_dmabuf(size: int, heaps: tuple[str, ...] = (
        "/dev/dma_heap/rk3588-system", "/dev/dma_heap/system",
        "/dev/dma_heap/rknpu", "/dev/dma_heap/video")) -> DmaBuf | None:
    """分配一块 dma-buf。全部 heap 都失败时返回 None(调用方需降级到 CPU 内存)"""
    for heap in heaps:
        try:
            hfd = os.open(heap, os.O_RDWR | os.O_CLOEXEC)
        except OSError:
            continue
        try:
            # ① Rockchip 版 + uncached  ② Rockchip 版 + heap_flags=0  ③ 标准版
            attempts = [
                (RK_DMA_HEAP_IOCTL_ALLOC_R, RK_DMA_HEAP_UNCACHED),
                (RK_DMA_HEAP_IOCTL_ALLOC_R, 0),
                (DMA_HEAP_IOCTL_ALLOC_H, 0),
            ]
            for req, heap_flags in attempts:
                data = _HeapAllocData()
                data.len = size
                data.heap_flags = heap_flags
                try:
                    fcntl.ioctl(hfd, req, data, True)
                except OSError:
                    continue
                if data.fd <= 0:
                    continue
                fd = data.fd
                try:
                    mm = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
                    va = ctypes.addressof(ctypes.c_char.from_buffer(mm))
                except (OSError, ValueError):
                    os.close(fd)
                    continue
                log.info("[dmabuf] %s 分配成功 fd=%d size=%d va=0x%x", heap, fd, size, va)
                return DmaBuf(fd, size, mm, va)
        finally:
            os.close(hfd)
    log.warning("[dmabuf] 所有 dma-heap 都分配失败(%d 字节), 降级到 CPU 内存", size)
    return None


def alloc_cpu_buffer(size: int) -> DmaBuf:
    """降级路径: 普通内存(无 fd)。RGA/RKNN 只能走 CPU 拷贝, 延时明显变差"""
    mm = mmap.mmap(-1, size)
    va = ctypes.addressof(ctypes.c_char.from_buffer(mm))
    return DmaBuf(-1, size, mm, va)


def alloc_buffer(size: int, prefer_dmabuf: bool = True) -> DmaBuf:
    return alloc_dmabuf(size) if prefer_dmabuf else alloc_cpu_buffer(size)


def find_dmabuf_fd_for_addr(addr: int) -> int:
    """给定 CPU 虚拟地址, 反查它是不是某个 dma-buf 的映射(拿 fd 喂 RGA)"""
    try:
        with open("/proc/self/maps", "r") as f:
            for line in f:
                parts = line.split()
                if len(parts) < 6:
                    continue
                if not parts[5].startswith("/dev/dri"):
                    continue
                lo, hi = (int(x, 16) for x in parts[0].split("-"))
                if lo <= addr < hi:
                    return _fd_from_maps(parts[0])
    except OSError:
        pass
    return -1


def _fd_from_maps(range_str: str) -> int:
    # /proc/self/maps 的第 6 列是路径, fd 要从 /proc/self/fd 反查
    inode = None
    with open("/proc/self/maps", "r") as f:
        for line in f:
            if line.split()[0] == range_str:
                inode = line.split()[5].split("[")[1].split("]")[0]
                break
    if inode is None:
        return -1
    for fd in range(0, 1024):
        p = f"/proc/self/fd/{fd}"
        try:
            target = os.readlink(p)
        except OSError:
            continue
        if target.startswith("/dev/dri"):
            try:
                with open(p) as fh:
                    st = os.fstat(fh.fileno())
                if str(st.st_ino) == inode:
                    return fd
            except OSError:
                continue
    return -1
