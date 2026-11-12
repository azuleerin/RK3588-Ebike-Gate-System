"""dmapool —— 独立的 DMA-BUF 池化内存块模块

★ 为什么是独立模块(2026-11-12):
    rk12ch/ 目前处于 QUARANTINE(冻结)状态 —— 根因未定位、单路侧还没过 24h 回归,
    冻结树一个字节都不许改(会破坏"带病复现"的对照关系)。
    所以池化先以【独立工具】的形式落地并验证, 解冻(step/08)时再接线。
    ★ 本模块【不依赖 rk12ch 的任何内部模块】: 自带 dma-heap 分配器,
      单元测试可以在没有 rk12ch 的环境下单独跑。

★★★ 三个核心决策(冻结, 不再讨论) ★★★

① 按分辨率分池, 绝不混用
    MPP 解码输出是 1080P NV12, RGA 输出是 640x640 RGB, LPR 裁剪是 160x48。
    三者尺寸差异巨大, 必须三个独立的池各自管理对应尺寸的块。
    混在一个池里 -> 借用逻辑要处理"尺寸不匹配", 复杂度指数上升。

② 池大小 = 每路并发深度 x 路数 x 安全系数
    例: 12 路, 每路同时最多 2 帧在飞行(1 帧在 RGA, 1 帧在 NPU 排队)
        -> 至少 12 x 2 = 24 块; 乘安全系数 1.5 -> 36 块
        (640x640 RGB 每块 1.2MB -> 约 43MB, 完全可控)
    ★ 池耗尽时【绝对不能无限扩容】: acquire() 返回 None, 上层跳帧或阻塞等待。
      池是内存上限的硬性锁死。

③ 用引用计数替代"用完就还"
    一块 DMA-BUF 可能同时被 RGA 的输出和 NPU 的输入引用。
    RGA 写完就还池 -> NPU 可能还在读 -> 被下一帧覆盖。
    所以每块带一个引用计数: 借出 +1, 每个使用方 -1, 归零才回空闲链表。

★★★ fd / vaddr 终身不变 ★★★
    MPP 解码输出、RGA import、RKNN 绑定都只需要 fd;
    fd 在池初始化时确定, 之后每帧只【传同一个 fd】,
    彻底消除"悬垂 fd / 重复 close / 忘记 close"这一类问题的土壤。
    对比逐帧 new/delete DMA-BUF: 必然在某条异常路径上泄漏 fd,
    跑几分钟后 dma-buf 耗尽 -> 整条流水线哑掉。
"""

from __future__ import annotations

import ctypes
import fcntl
import math
import mmap
import os
import struct
import threading
from dataclasses import dataclass, field

# ---------------------------------------------------------------------------
# dma-heap 分配器(自带, 不依赖 rk12ch)
#
# ★ 两个不同的 ioctl, magic 不对会 EINVAL 静默失败:
#       ① Rockchip 版: _IOWR('R', 0x00, struct rk_dma_heap_alloc_data)
#       ② 标准版:     _IOWR('H', 0x00, struct rk_dma_heap_alloc_data)
#   两者结构体布局相同, 只是 magic 不同。
# ★ 分配成功后【必须 mmap】才能拿到 CPU 地址; 本板内核对某些 heap 会
#   返回 Permission denied(见 README 的环境限制), 所以必须能降级到 CPU 内存。
# ---------------------------------------------------------------------------


def _IOC(direction: int, magic: int, nr: int, size: int) -> int:
    """Linux ioctl 编码: _IOC(dir, type, nr, size)"""
    return (direction << 30) | (size << 16) | (magic << 8) | nr


_IOWR = 3
RK_DMA_HEAP_IOCTL_ALLOC_R = _IOC(_IOWR, ord('R'), 0x00, 24)
DMA_HEAP_IOCTL_ALLOC_H = _IOC(_IOWR, ord('H'), 0x00, 24)
RK_DMA_HEAP_UNCACHED = 1 << 0

DEFAULT_HEAPS = ("/dev/dma_heap/system", "/dev/dma_heap/rk3588-system",
                 "/dev/dma_heap/system-uncached", "/dev/dma_heap/rknpu",
                 "/dev/dma_heap/video")


class _HeapAllocData(ctypes.Structure):
    """struct rk_dma_heap_alloc_data (24 字节)"""
    _fields_ = [
        ("len", ctypes.c_uint64),
        ("fd", ctypes.c_uint32),
        ("fd_flags", ctypes.c_uint32),
        ("heap_flags", ctypes.c_uint64),
    ]


@dataclass
class RawBuffer:
    """底层一块缓冲(fd + CPU 映射地址)"""
    fd: int = -1
    vaddr: int = 0
    size: int = 0
    _mm: object = None

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


def alloc_dmaheap(size: int, heaps=DEFAULT_HEAPS) -> RawBuffer | None:
    """从 dma-heap 分配一块并 mmap。失败返回 None"""
    for heap in heaps:
        try:
            hfd = os.open(heap, os.O_RDWR | os.O_CLOEXEC)
        except OSError:
            continue
        try:
            # ① Rockchip 版 + uncached  ② Rockchip 版 + flags=0  ③ 标准版
            for req, heap_flags in ((RK_DMA_HEAP_IOCTL_ALLOC_R, RK_DMA_HEAP_UNCACHED),
                                    (RK_DMA_HEAP_IOCTL_ALLOC_R, 0),
                                    (DMA_HEAP_IOCTL_ALLOC_H, 0)):
                data = _HeapAllocData()
                data.len = size
                data.heap_flags = heap_flags
                try:
                    fcntl.ioctl(hfd, req, data, True)
                except OSError:
                    continue
                if data.fd <= 0:
                    continue
                try:
                    mm = mmap.mmap(data.fd, size, mmap.MAP_SHARED,
                                   mmap.PROT_READ | mmap.PROT_WRITE)
                    va = ctypes.addressof(ctypes.c_char.from_buffer(mm))
                except (OSError, ValueError):
                    os.close(data.fd)
                    continue
                return RawBuffer(data.fd, va, size, mm)
        finally:
            os.close(hfd)
    return None


def alloc_cpu(size: int) -> RawBuffer:
    """降级路径: 普通内存(没有 fd, RGA 只能走 CPU 拷贝)"""
    mm = mmap.mmap(-1, size)
    va = ctypes.addressof(ctypes.c_char.from_buffer(mm))
    return RawBuffer(-1, va, size, mm)


def default_allocator(size: int, prefer_dmabuf: bool = True) -> RawBuffer:
    if prefer_dmabuf:
        buf = alloc_dmaheap(size)
        if buf is not None:
            return buf
    return alloc_cpu(size)


# ---------------------------------------------------------------------------
# 池
# ---------------------------------------------------------------------------
@dataclass
class PoolStats:
    total: int = 0
    in_use: int = 0
    high_water: int = 0           # 历史峰值占用
    acquire_count: int = 0        # 累计借出
    release_count: int = 0        # 累计归还
    exhausted_count: int = 0      # 池耗尽次数(上层据此跳帧/告警)
    double_release_count: int = 0  # 重复归还(说明上层有 bug, 只记不崩)


class PooledBuffer:
    """池里的一块内存。fd / vaddr 在池的生命周期内【终身不变】"""

    __slots__ = ("fd", "vaddr", "size", "index", "_pool", "_ref", "_lock")

    def __init__(self, fd: int, vaddr: int, size: int, index: int, pool: "DmaBufPool"):
        self.fd = fd
        self.vaddr = vaddr
        self.size = size
        self.index = index
        self._pool = pool
        self._ref = 0
        self._lock = threading.Lock()

    @property
    def ref_count(self) -> int:
        with self._lock:
            return self._ref

    @property
    def free(self) -> bool:
        with self._lock:
            return self._ref == 0

    def __repr__(self) -> str:
        return (f"<PooledBuffer #{self.index} fd={self.fd} va=0x{self.vaddr:x} "
                f"size={self.size} ref={self._ref}>")


class DmaBufPool:
    """固定块数、固定块大小的缓冲池

    借用语义:
        acquire() -> PooledBuffer | None   None = 池耗尽(不扩容, 不崩)
        release(buf) -> bool                True = 归零回池, False = 还有人在用
        retain(buf) -> bool                 ref+1, 交给另一个使用方
    """

    def __init__(self, name: str, block_size: int, count: int,
                 prefer_dmabuf: bool = True, allocator=None):
        if block_size <= 0 or count <= 0:
            raise ValueError("block_size / count 必须 > 0")
        self.name = name
        self.block_size = block_size
        self.count = count
        self._blocks: list[PooledBuffer] = []
        self._raw: list[RawBuffer] = []
        self._free: list[int] = []
        self._lock = threading.Lock()
        self.stats = PoolStats(total=count)
        self.dmabuf = prefer_dmabuf
        self._allocator = allocator or (lambda sz: default_allocator(sz, prefer_dmabuf))

        for i in range(count):
            raw = self._allocator(block_size)
            if raw.fd < 0:
                self.dmabuf = False
            self._raw.append(raw)
            self._blocks.append(PooledBuffer(raw.fd, raw.vaddr, raw.size, i, self))
            self._free.append(i)

    # ------------------------------------------------------------------
    def acquire(self) -> PooledBuffer | None:
        """借出一块。池耗尽返回 None(上层跳帧或阻塞), 绝不扩容"""
        with self._lock:
            if not self._free:
                self.stats.exhausted_count += 1
                return None
            idx = self._free.pop()
            buf = self._blocks[idx]
            buf._ref = 1
            self.stats.in_use += 1
            self.stats.acquire_count += 1
            if self.stats.in_use > self.stats.high_water:
                self.stats.high_water = self.stats.in_use
            return buf

    # ------------------------------------------------------------------
    def retain(self, buf: PooledBuffer) -> bool:
        """把引用转交给另一个使用方(ref+1)"""
        if buf is None or buf._pool is not self:
            return False
        with buf._lock:
            if buf._ref <= 0:
                return False
            buf._ref += 1
        return True

    # ------------------------------------------------------------------
    def release(self, buf: PooledBuffer) -> bool:
        """归还一次引用。归零才真正回空闲链表。

        返回 True = 这块已经归零(回到池里); False = 还有人用着 / 重复归还。
        """
        if buf is None or buf._pool is not self:
            return False
        with buf._lock:
            if buf._ref <= 0:
                # 重复归还: 只记数不崩(这类 bug 上层一定会踩到, 崩了就没现场了)
                self.stats.double_release_count += 1
                return False
            buf._ref -= 1
            if buf._ref > 0:
                return False
        with self._lock:
            self._free.append(buf.index)
            self.stats.in_use -= 1
            self.stats.release_count += 1
        return True

    # ------------------------------------------------------------------
    @property
    def available(self) -> int:
        with self._lock:
            return len(self._free)

    @property
    def in_use(self) -> int:
        with self._lock:
            return self.stats.in_use

    def close(self) -> None:
        """关掉所有底层 fd。池化期间 fd 绝不能被提前关掉"""
        with self._lock:
            raws, self._raw = self._raw, []
        for raw in raws:
            raw.close()

    def describe(self) -> str:
        return (f"{self.name}: {self.count} 块 x {self.block_size // 1024}KB "
                f"(可用 {self.available}, 在用 {self.in_use}, "
                f"峰值 {self.stats.high_water}, 耗尽 {self.stats.exhausted_count} 次)")


class BufferLease:
    """池化 buffer 的引用计数租约(跨层传递用)

    队列里存的是这个 lease; 队列丢帧 / 消费完成时调用 release()。
    ★ "用完就还"和"引用计数"的差别就在这里:
      RGA 写完 != 可以还池(NPU 可能还在读)。
    """

    __slots__ = ("pool", "buf", "_lock", "_released")

    def __init__(self, pool: DmaBufPool, buf: PooledBuffer):
        self.pool = pool
        self.buf = buf
        self._lock = threading.Lock()
        self._released = False

    @property
    def fd(self) -> int:
        return self.buf.fd

    @property
    def vaddr(self) -> int:
        return self.buf.vaddr

    @property
    def size(self) -> int:
        return self.buf.size

    @property
    def ref_count(self) -> int:
        return self.buf.ref_count

    def retain(self) -> "BufferLease":
        """再交给一个使用方 —— 返回【一个新的 lease】。

        ★ 每次转交都要多一份引用, 所以必须返回新 lease:
          如果返回自己, 第二个使用方归还时会和第一个抢同一个计数,
          引用数对不上 -> 块提前回池 -> NPU 还在读就被覆盖。
        """
        with self._lock:
            if self._released:
                return self
            if not self.pool.retain(self.buf):
                return self
            return BufferLease(self.pool, self.buf)

    def release(self) -> None:
        with self._lock:
            if self._released:
                return
            self._released = True
        self.pool.release(self.buf)

    def close(self) -> None:
        self.release()

    def __enter__(self) -> "BufferLease":
        return self

    def __exit__(self, *exc) -> None:
        self.release()


class FrameSlotPool:
    """帧额度池(不占内存, 只做并发准入控制)

    ★ MPP 自己管理解码缓冲, Python 这边没法直接把它接到我们的内存池上
      (真要接得用 MPP_DEC_SET_EXT_BUF_GROUP + mpp_buffer_group)。
      但"同一时刻有多少帧在流水线里飞行"才是内存上限的决定因素,
      所以用额度池把在途帧数卡死: 借不到额度 -> 直接跳过这一帧。
    """

    def __init__(self, name: str, count: int):
        self.name = name
        self.count = count
        self._free = list(range(count))
        self._lock = threading.Lock()
        self.stats = PoolStats(total=count)

    def acquire(self) -> int | None:
        with self._lock:
            if not self._free:
                self.stats.exhausted_count += 1
                return None
            slot = self._free.pop()
            self.stats.in_use += 1
            self.stats.acquire_count += 1
            if self.stats.in_use > self.stats.high_water:
                self.stats.high_water = self.stats.in_use
            return slot

    def release(self, slot: int) -> bool:
        with self._lock:
            if slot is None or slot < 0:
                return False
            self._free.append(slot)
            self.stats.in_use -= 1
            self.stats.release_count += 1
            return True

    @property
    def available(self) -> int:
        with self._lock:
            return len(self._free)

    def describe(self) -> str:
        return (f"{self.name}: {self.count} 个帧额度 (可用 {self.available}, "
                f"在用 {self.stats.in_use}, 峰值 {self.stats.high_water}, "
                f"耗尽 {self.stats.exhausted_count} 次)")


# ---------------------------------------------------------------------------
# 容量计算
# ---------------------------------------------------------------------------
def pool_size(per_channel_concurrency: int, channels: int,
              safety_factor: float = 1.5, batch: int = 1) -> int:
    """池大小 = 每路并发深度 x 路数 x 安全系数

    batch > 1 时(一个块装 N 帧), 先把总并发折算成块数再乘安全系数。
    """
    if per_channel_concurrency <= 0 or channels <= 0:
        raise ValueError("per_channel_concurrency / channels 必须 > 0")
    total = per_channel_concurrency * channels
    blocks = math.ceil(total / max(1, batch))
    return max(2, int(math.ceil(blocks * safety_factor)))


def block_size_for(in_w: int, in_h: int, batch: int = 1,
                   pad_rows: int = 16) -> int:
    """一块的大小: batch 个 (in_w x in_h x 3) + RGA 余量行

    ★ pad_rows 不能省: RGA 在缩放比例不整除时会往目标缓冲【多写 1~2 行】,
      目标缓冲按精确大小分配就会越界踩坏堆(实测: malloc(): invalid next size)。
    """
    frames = max(1, batch)
    return (frames * in_h + pad_rows) * in_w * 3


@dataclass
class PoolBundle:
    """三个分辨率的池 + 帧额度池"""
    yolo: DmaBufPool
    lpr: DmaBufPool
    frame: FrameSlotPool
    spec: dict = field(default_factory=dict)

    def describe(self) -> str:
        return "\n".join(["内存池:",
                          "  " + self.yolo.describe(),
                          "  " + self.lpr.describe(),
                          "  " + self.frame.describe()])


def build_pools(channels: int, yolo_in: tuple[int, int], lpr_in: tuple[int, int],
                yolo_batch: int, lpr_batch: int,
                per_channel_concurrency: int = 2, safety_factor: float = 1.5,
                prefer_dmabuf: bool = True) -> PoolBundle:
    """按分辨率建三个池(每个池对应一个分辨率, 不混用)

    例: 12 路, 每路并发 2, 安全系数 1.5, yolo_batch=4
        yolo : ceil(24/4)=6 -> 9 块 x (4*640+16)*640*3 = 4.72MB  -> 42.5MB
        lpr  : ceil(24/8)=3 -> 5 块 x (8*48+16)*160*3 = 187KB    -> 0.9MB
        frame: 36 个帧额度(不占内存)
    """
    y_size = block_size_for(yolo_in[0], yolo_in[1], yolo_batch)
    l_size = block_size_for(lpr_in[0], lpr_in[1], lpr_batch)
    yolo_pool = DmaBufPool("yolo-rgb", y_size,
                           pool_size(per_channel_concurrency, channels, safety_factor, yolo_batch),
                           prefer_dmabuf)
    lpr_pool = DmaBufPool("lpr-rgb", l_size,
                           pool_size(per_channel_concurrency, channels, safety_factor, lpr_batch),
                           prefer_dmabuf)
    frame_pool = FrameSlotPool("frame-nv12",
                                pool_size(per_channel_concurrency, channels, safety_factor, 1))
    return PoolBundle(yolo=yolo_pool, lpr=lpr_pool, frame=frame_pool,
                      spec={"channels": channels,
                            "per_channel_concurrency": per_channel_concurrency,
                            "safety_factor": safety_factor,
                            "yolo": {"block": y_size, "count": yolo_pool.count},
                            "lpr": {"block": l_size, "count": lpr_pool.count}})
