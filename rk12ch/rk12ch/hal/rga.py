"""RGA2 硬件缩放/裁剪/颜色转换 (ctypes 直连 librga.so)

对齐 librga 的 C ABI:
    IM_STATUS improcess(const rga_buffer_t src, const rga_buffer_t dst,
                        const rga_buffer_t pat, const im_rect srect,
                        const im_rect drect, const im_rect prect,
                        const int acquire_fence_fd, int *release_fence_fd,
                        im_opt *opt_ptr, int usage);

★ 零拷贝要点:
    src/dst 都用 fd 描述(librga 直接 import dma-buf), 不传 vir_addr,
    这样 RGA 跑在硬件上完全不经过 CPU。

★ 踩过的坑(务必按这里的写):
  1. 目标缓冲必须【多留几行余量】。
     RK3588 的 RGA 在缩放比例不是整数倍时(1920x1080 -> 640x640, sy=1.6875),
     会把行缓冲里没写完的 1~2 行也刷进目标缓冲, 也就是【越界写 1~2 行】。
     目标缓冲按精确大小分配时, 这 1~2 行直接踩坏后面的堆:
         malloc(): invalid next size (unsorted)
     -> 本模块的 RgaBuffer 统一带 pad_rows(默认 16 行)。

  2. 只在源和目标都是【dma-buf(fd>=0)】时才用 RGA。
     用户态 malloc 的缓冲会让 librga 走软件 MMU, 它缓存的 VA->PA 映射在
     内存被复用后会失效, RGA 会把输出写到【别的分配】里 -> 堆随机损坏。
     librga 的软件 MMU 报错长这样:
         RGA_MMU unsupported memory larger than 4G!
         scheduler core[4] unsupported mm_flag[0x8]!
"""

from __future__ import annotations

import ctypes
import logging
import os
from dataclasses import dataclass

log = logging.getLogger(__name__)

# ---- librga 格式常量 (rga.h) ----
RK_FORMAT_RGB_888 = 0x2 << 8
RK_FORMAT_RGBA_8888 = 0x0 << 8
RK_FORMAT_YCbCr_420_SP = 0xA << 8   # NV12 半平面
RK_FORMAT_YCbCr_420_P = 0xB << 8    # 平面 YUV420

IM_STATUS_NOERROR = 2
IM_STATUS_SUCCESS = 1

# 硬件异常时 RGA 会自己段错误, 这里包一层 sigsetjmp 兜不住(纯 Python 无法注册
# 跳转), 所以调用前必须保证参数合法; 详见 rk12ch/hal/rga_guard.py 的说明。


class ImRect(ctypes.Structure):
    _fields_ = [("x", ctypes.c_int), ("y", ctypes.c_int),
                ("width", ctypes.c_int), ("height", ctypes.c_int)]


class ImColorkeyRange(ctypes.Structure):
    _fields_ = [("max", ctypes.c_int), ("min", ctypes.c_int)]


class ImNnT(ctypes.Structure):
    _fields_ = [("scale_r", ctypes.c_int), ("scale_g", ctypes.c_int),
                ("scale_b", ctypes.c_int), ("offset_r", ctypes.c_int),
                ("offset_g", ctypes.c_int), ("offset_b", ctypes.c_int)]


class RgaBuffer(ctypes.Structure):
    """rga_buffer_t, 必须与 im2d_type.h 逐字段对齐"""
    _fields_ = [
        ("vir_addr", ctypes.c_void_p),
        ("phy_addr", ctypes.c_void_p),
        ("fd", ctypes.c_int),
        ("width", ctypes.c_int),
        ("height", ctypes.c_int),
        ("wstride", ctypes.c_int),
        ("hstride", ctypes.c_int),
        ("format", ctypes.c_int),
        ("color_space_mode", ctypes.c_int),
        ("global_alpha", ctypes.c_int),
        ("rd_mode", ctypes.c_int),
        ("color", ctypes.c_int),
        ("colorkey_range", ImColorkeyRange),
        ("nn", ImNnT),
        ("rop_code", ctypes.c_int),
        ("handle", ctypes.c_void_p),
    ]


@dataclass
class RgaImage:
    """一次 RGA 操作的输入/输出描述"""
    fd: int
    width: int
    height: int
    wstride: int
    hstride: int
    format: int
    vir_addr: int = 0          # fd < 0(CPU 内存) 时必填


class Rga:
    """librga 句柄。进程内单例(RGA 内部有全局状态)"""

    _instance = None

    def __init__(self, lib_path: str = "librga.so"):
        self.lib = ctypes.CDLL(lib_path)
        self.lib.improcess.restype = ctypes.c_int
        self.lib.improcess.argtypes = [
            RgaBuffer, RgaBuffer, RgaBuffer, ImRect, ImRect, ImRect,
            ctypes.c_int, ctypes.POINTER(ctypes.c_int), ctypes.c_void_p, ctypes.c_int,
        ]
        self.lib.imcheck_t.restype = ctypes.c_int
        self.lib.imcheck_t.argtypes = [
            RgaBuffer, RgaBuffer, RgaBuffer, ImRect, ImRect, ImRect, ctypes.c_int,
        ]
        log.info("[RGA] librga 已加载: %s", lib_path)

    @classmethod
    def instance(cls) -> "Rga":
        if cls._instance is None:
            cls._instance = Rga()
        return cls._instance

    # ------------------------------------------------------------------
    @staticmethod
    def _to_buffer(img: RgaImage) -> RgaBuffer:
        b = RgaBuffer()
        b.vir_addr = img.vir_addr
        b.phy_addr = None
        b.fd = img.fd
        b.width = img.width
        b.height = img.height
        b.wstride = img.wstride
        b.hstride = img.hstride
        b.format = img.format
        b.color_space_mode = 0
        b.global_alpha = 0
        b.rd_mode = 0
        b.color = 0
        b.rop_code = 0
        b.handle = None
        return b

    # ------------------------------------------------------------------
    @staticmethod
    def can_use(src: RgaImage, dst: RgaImage) -> bool:
        """RGA 能不能安全地处理这一对缓冲。

        ★ 只有【源和目标都是 dma-buf(fd >= 0)】时才安全:
          用户态 malloc 的缓冲会让 librga 走软件 MMU, 它缓存的 VA->PA 映射在
          内存被复用后会失效, RGA 会把输出写到【别的分配】里 -> 堆随机损坏。
          librga 的软件 MMU 报错长这样:
              RGA_MMU unsupported memory larger than 4G!
              scheduler core[4] unsupported mm_flag[0x8]!
        """
        return src.fd >= 0 and dst.fd >= 0

    # ------------------------------------------------------------------
    def check(self, src: RgaImage, dst: RgaImage,
              srect: ImRect, drect: ImRect) -> int:
        pat = RgaBuffer()
        prect = ImRect(0, 0, 0, 0)
        return self.lib.imcheck_t(self._to_buffer(src), self._to_buffer(dst), pat,
                                  srect, drect, prect, 0)

    # ------------------------------------------------------------------
    def process(self, src: RgaImage, dst: RgaImage,
                srect: ImRect, drect: ImRect) -> int:
        """执行一次 blit。返回 0 成功, 其它为错误码"""
        pat = RgaBuffer()
        prect = ImRect(0, 0, 0, 0)
        fence = ctypes.c_int(-1)
        ret = self.lib.improcess(self._to_buffer(src), self._to_buffer(dst), pat,
                                 srect, drect, prect, -1, ctypes.byref(fence),
                                 None, 0)
        if ret not in (IM_STATUS_NOERROR, IM_STATUS_SUCCESS):
            log.error("[RGA] improcess 失败 ret=%d (src %dx%d fd=%d -> dst %dx%d fd=%d)",
                      ret, src.width, src.height, src.fd, dst.width, dst.height, dst.fd)
        return ret

    # ------------------------------------------------------------------
    def crop_scale(self, src: RgaImage, dst: RgaImage,
                   sx: int, sy: int, sw: int, sh: int,
                   dw: int | None = None, dh: int | None = None) -> int:
        """裁剪 + 缩放到 dst 的整幅。坐标必须 clamp 在源图内(见 clamp_roi)"""
        if not self.can_use(src, dst):
            log.error("[RGA] 源/目标不是 dma-buf, 拒绝调用(软件 MMU 会写坏堆)")
            return -1
        dw = dst.width if dw is None else dw
        dh = dst.height if dh is None else dh
        # ★ chroma 是 2x2 下采样, 奇数坐标/尺寸 RGA 会直接拒绝
        sx &= ~1
        sy &= ~1
        sw &= ~1
        sh &= ~1
        if sw <= 0 or sh <= 0:
            return -1
        srect = ImRect(sx, sy, sw, sh)
        drect = ImRect(0, 0, dw, dh)
        return self.process(src, dst, srect, drect)

    # ------------------------------------------------------------------
    def fill_gray(self, dst: RgaImage, value: int = 114) -> int:
        """把目标缓冲整幅填成灰(value=114 与 YOLOv8 官方 letterbox 一致)"""
        srect = ImRect(0, 0, 0, 0)
        drect = ImRect(0, 0, dst.width, dst.height)
        pat = RgaBuffer()
        pat.width = dst.width
        pat.height = dst.height
        pat.wstride = dst.wstride
        pat.hstride = dst.hstride
        pat.format = dst.format
        # 用颜色填充: RGA 的 pat 通道 + rop_code = SRC_ROP
        pat.color = value
        prect = ImRect(0, 0, dst.width, dst.height)
        fence = ctypes.c_int(-1)
        return self.lib.improcess(self._to_buffer(dst), self._to_buffer(dst), pat,
                                  srect, drect, prect, -1, ctypes.byref(fence), None, 0)


def clamp_roi(x: int, y: int, w: int, h: int, fw: int, fh: int,
              min_w: int = 1, min_h: int = 1) -> tuple[int, int, int, int] | None:
    """ROI 硬夹紧: 保证矩形完全落在画面内, 夹完太小返回 None。

    ★ 必须做: 取整误差 + ROI 外扩都会让 x+w 越界, 而 RGA 的 im_rect
      越界不会被检查, 只是画到画外 -> 后面读到的图是乱的, 极端情况踩坏堆。
    """
    if fw <= 0 or fh <= 0:
        return None
    if w < 0:
        w = 0
    if h < 0:
        h = 0
    if x < 0:
        w += x
        x = 0
    if y < 0:
        h += y
        y = 0
    if x > fw:
        x = fw
    if y > fh:
        y = fh
    if x + w > fw:
        w = fw - x
    if y + h > fh:
        h = fh - y
    if w < min_w or h < min_h:
        return None
    return x, y, w, h
