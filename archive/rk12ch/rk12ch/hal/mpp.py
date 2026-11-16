"""RKMPP 硬解封装 (ctypes 直连 librockchip_mpp.so)

对齐官方 MPP API(rk_mpi.h / mpp_task.h):
    mpp_create / mpp_init / mpp_destroy
    MppApi.decode_put_packet / decode_get_frame / control / reset
    mpp_packet_init / mpp_packet_set_pts / mpp_packet_set_size / mpp_packet_deinit
    mpp_frame_get_buffer / mpp_frame_get_width / mpp_frame_get_hor_stride ...
    mpp_buffer_get_fd_with_caller / mpp_buffer_get_ptr_with_caller

★ MPP 的 API 是一个【函数指针表】(MppApi), mpp_create() 把表指针吐出来,
  后面所有调用都走这张表 -> ctypes 里要用 CFUNCTYPE 逐个声明, 顺序必须
  和 rk_mpi.h 完全一致, 少一个/多一个后面全部错位。

★ 帧的生命周期(最容易踩的坑):
    mpp_frame_get_buffer() 拿到的 buffer 是 MPP 内部引用计数管理的,
    用完必须 mpp_frame_deinit(), 否则 dma-buf fd 泄漏, 跑几分钟就
    "dma-buf 耗尽 / open /dev/dri 失败"。
    本模块的 MppFrameHolder 用 with 语句保证释放。
"""

from __future__ import annotations

import ctypes
import logging
import os
from dataclasses import dataclass

log = logging.getLogger(__name__)

# ---- 枚举 ----
MPP_CTX_DEC = 1
MPP_CTX_ENC = 2

MPP_VIDEO_CodingAVC = 7
MPP_VIDEO_CodingHEVC = 16

MPP_DEC_SET_PARSER_SPLIT_MODE = 101
MPP_DEC_SET_PRESENT_TIME_ORDER = 100
MPP_SET_INPUT_TIMEOUT = 76
MPP_SET_OUTPUT_TIMEOUT = 77
MPP_DEC_SET_INFO_CHANGE_READY = 99
MPP_DEC_SET_ENABLE_FAST_PLAY = 117

MPP_PORT_INPUT = 0
MPP_PORT_OUTPUT = 1

MPP_POLL_BLOCK = -1
MPP_POLL_NON_BLOCK = 0
MPP_POLL_READY = 1


# ---- ctypes 类型 ----
MppCtx = ctypes.c_void_p
MppPacket = ctypes.c_void_p
MppFrame = ctypes.c_void_p
MppBuffer = ctypes.c_void_p
MppTask = ctypes.c_void_p

RET = ctypes.c_int

# ★★ MppApi 里每个函数的【参数个数不一样】, 必须逐个声明正确的 CFUNCTYPE:
#     decode_put_packet(ctx, packet)          -> 2
#     decode_get_frame(ctx, frame*)            -> 2
#     control(ctx, cmd, param)                 -> 3
#     reset(ctx)                               -> 1
#   全部用同一个 3 参的 CFUNCTYPE 会报
#     "this function takes at least 3 arguments (2 given)"
#   —— 而且这种错误只在那条路径第一次被调用时才炸, 极难定位。
FN1 = ctypes.CFUNCTYPE(RET, ctypes.c_void_p)
FN2 = ctypes.CFUNCTYPE(RET, ctypes.c_void_p, ctypes.c_void_p)
FN3 = ctypes.CFUNCTYPE(RET, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)
FN_CTRL = ctypes.CFUNCTYPE(RET, ctypes.c_void_p, ctypes.c_int, ctypes.c_uint64)


class MppApi(ctypes.Structure):
    """必须与 rk_mpi.h 的 MppApi_t 逐字段对齐(顺序 + 每项的函数签名)"""
    _fields_ = [
        ("size", ctypes.c_uint32),
        ("version", ctypes.c_uint32),
        ("decode", FN3),              # (ctx, packet, frame*)
        ("decode_put_packet", FN2),    # (ctx, packet)
        ("decode_get_frame", FN2),    # (ctx, frame*)
        ("encode", FN3),              # (ctx, frame, packet*)
        ("encode_put_frame", FN2),    # (ctx, frame)
        ("encode_get_packet", FN2),   # (ctx, packet*)
        ("isp", FN3),                 # (ctx, dst, src)
        ("isp_put_frame", FN2),
        ("isp_get_frame", FN2),
        ("poll", FN3),                # (ctx, type, timeout)
        ("dequeue", FN3),             # (ctx, type, task*)
        ("enqueue", FN3),             # (ctx, type, task)
        ("reset", FN1),               # (ctx)
        ("control", FN_CTRL),         # (ctx, cmd, param)
        ("reserv", ctypes.c_uint32 * 16),
    ]


@dataclass
class DecodedFrame:
    """一帧 NV12 硬件解码结果(纯数据, 不持有 MPP 引用)"""
    fd: int                # dma-buf fd(RGA/NPU 直接 import)
    va: int                # CPU 映射地址(需要 CPU 读时用)
    width: int
    height: int
    hor_stride: int
    ver_stride: int
    pts: int
    frame_id: int = 0      # 由上层维护


class FrameLease:
    """持有 MPP frame 引用, close() 时 mpp_frame_deinit。

    ★ MPP 的 buffer 是引用计数管理的: decode_get_frame 拿到之后,
      必须保持 MppFrame 存活, 整条流水线用完才能 deinit。
      提前 deinit -> 后续 RGA/NPU 读到的是被复用的内存 -> 花屏甚至踩坏堆。
    """

    __slots__ = ("_decoder", "_frame", "info")

    def __init__(self, decoder: "MppDecoder", frame: MppFrame, info: DecodedFrame):
        self._decoder = decoder
        self._frame = frame
        self.info = info

    # 便捷属性
    @property
    def fd(self) -> int:
        return self.info.fd

    @property
    def va(self) -> int:
        return self.info.va

    @property
    def width(self) -> int:
        return self.info.width

    @property
    def height(self) -> int:
        return self.info.height

    @property
    def hor_stride(self) -> int:
        return self.info.hor_stride

    @property
    def ver_stride(self) -> int:
        return self.info.ver_stride

    @property
    def pts(self) -> int:
        return self.info.pts

    @property
    def frame_id(self) -> int:
        return self.info.frame_id

    def close(self) -> None:
        if self._frame:
            try:
                self._decoder.lib.mpp_frame_deinit(ctypes.byref(self._frame))
            except Exception:
                pass
            self._frame = MppFrame()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass


def mpp_driver_ready() -> bool:
    """VPU 硬解驱动是否就绪。

    ★ 为什么必须先查: VPU 驱动没起来时, MPP 的解码路径会【直接 SIGSEGV】
      (内部解引用空的驱动句柄), Python 根本 catch 不住, 整个进程会被带走。
      所以必须在建解码器之前挡住。

    判据: Rockchip 的 VPU 驱动加载后一定有 /proc/umap/ 节点(里面能看到 vpu)。
    """
    if not os.path.isdir("/proc/umap"):
        return False
    try:
        for name in os.listdir("/proc/umap"):
            if "vpu" in name.lower():
                return True
    except OSError:
        return False
    return False


def mpp_driver_hint() -> str:
    """驱动没就绪时给运维的处置建议"""
    if not os.path.isdir("/proc/umap"):
        return ("/proc/umap 不存在 -> Rockchip VPU 驱动没加载\n"
                "        处置: modprobe rkvdec (或换带 rkvdec 的内核镜像)\n"
                "        没有 VPU 驱动时 MPP 硬解不可用, 且一旦调用会 SIGSEGV")
    return ("/proc/umap 下没有 vpu 节点 -> VPU 驱动没起来\n"
            "        处置: modprobe rkvdec / 检查 dmesg 里的 vpu 初始化日志")


class MppDecoder:
    """一路 RTSP 的 MPP 硬解上下文"""

    def __init__(self, codec: str = "h264", input_timeout_ms: int = 200,
                 output_timeout_ms: int = 200, lib_path: str = "librockchip_mpp.so",
                 require_driver: bool = True):
        # ★ 先挡一道: VPU 驱动没就绪时 MPP 解码会 SIGSEGV, 整个进程被带走
        if require_driver and not mpp_driver_ready():
            raise RuntimeError("VPU 硬解驱动未就绪: " + mpp_driver_hint())
        self.lib = ctypes.CDLL(lib_path)
        self._bind()
        self.ctx = MppCtx()
        mpi_ptr = ctypes.POINTER(MppApi)()
        ret = self.lib.mpp_create(ctypes.byref(self.ctx), ctypes.byref(mpi_ptr))
        if ret != 0:
            raise RuntimeError(f"mpp_create 失败 ret={ret}")
        self.mpi = mpi_ptr.contents

        coding = MPP_VIDEO_CodingAVC if codec == "h264" else MPP_VIDEO_CodingHEVC
        # ★ 必须先 control 再 init(SPLIT_MODE 之类要求 init 之前设置)
        self.mpi.control(self.ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, 1)
        self.mpi.control(self.ctx, MPP_DEC_SET_PRESENT_TIME_ORDER, 1)
        self.mpi.control(self.ctx, MPP_SET_INPUT_TIMEOUT, input_timeout_ms)
        self.mpi.control(self.ctx, MPP_SET_OUTPUT_TIMEOUT, output_timeout_ms)

        ret = self.lib.mpp_init(self.ctx, MPP_CTX_DEC, coding)
        if ret != 0:
            raise RuntimeError(f"mpp_init 失败 ret={ret}")
        self.mpi.control(self.ctx, MPP_DEC_SET_INFO_CHANGE_READY, 1)
        self._frame_id = 0
        log.info("[MPP] 解码器就绪 codec=%s", codec)

    # ------------------------------------------------------------------
    def _bind(self) -> None:
        lib = self.lib
        lib.mpp_create.restype = RET
        lib.mpp_create.argtypes = [ctypes.POINTER(MppCtx), ctypes.POINTER(ctypes.POINTER(MppApi))]
        lib.mpp_init.restype = RET
        lib.mpp_init.argtypes = [MppCtx, ctypes.c_int, ctypes.c_int]
        lib.mpp_destroy.restype = RET
        lib.mpp_destroy.argtypes = [MppCtx]
        lib.mpp_packet_init.restype = RET
        lib.mpp_packet_init.argtypes = [ctypes.POINTER(MppPacket), ctypes.c_void_p, ctypes.c_size_t]
        lib.mpp_packet_deinit.restype = RET
        lib.mpp_packet_deinit.argtypes = [ctypes.POINTER(MppPacket)]
        lib.mpp_packet_set_pts.restype = None
        lib.mpp_packet_set_pts.argtypes = [MppPacket, ctypes.c_int64]
        lib.mpp_packet_set_size.restype = None
        lib.mpp_packet_set_size.argtypes = [MppPacket, ctypes.c_size_t]
        lib.mpp_frame_deinit.restype = RET
        lib.mpp_frame_deinit.argtypes = [ctypes.POINTER(MppFrame)]
        lib.mpp_frame_get_buffer.restype = MppBuffer
        lib.mpp_frame_get_buffer.argtypes = [MppFrame]
        lib.mpp_frame_get_width.restype = ctypes.c_uint32
        lib.mpp_frame_get_width.argtypes = [MppFrame]
        lib.mpp_frame_get_height.restype = ctypes.c_uint32
        lib.mpp_frame_get_height.argtypes = [MppFrame]
        lib.mpp_frame_get_hor_stride.restype = ctypes.c_uint32
        lib.mpp_frame_get_hor_stride.argtypes = [MppFrame]
        lib.mpp_frame_get_ver_stride.restype = ctypes.c_uint32
        lib.mpp_frame_get_ver_stride.argtypes = [MppFrame]
        lib.mpp_frame_get_pts.restype = ctypes.c_int64
        lib.mpp_frame_get_pts.argtypes = [MppFrame]
        lib.mpp_buffer_get_fd_with_caller.restype = ctypes.c_int
        lib.mpp_buffer_get_fd_with_caller.argtypes = [MppBuffer, ctypes.c_char_p]
        lib.mpp_buffer_get_ptr_with_caller.restype = ctypes.c_void_p
        lib.mpp_buffer_get_ptr_with_caller.argtypes = [MppBuffer, ctypes.c_char_p]

    # ------------------------------------------------------------------
    def decode(self, data: bytes, pts_us: int) -> list["FrameLease"]:
        """喂一个 H.264 访问单元, 返回本次能解出的所有帧(可能是 0 帧)

        ★ 返回的是 lease: 调用方负责 close()。队列丢帧时也要 close。
        """
        frames: list[FrameLease] = []
        packet = MppPacket()
        buf = ctypes.create_string_buffer(data, len(data))
        # ★ mpp_packet_init 会【拷贝】data, 这里必须自己保证 data 的生命周期
        #   到 decode 完成。生产环境建议用 mpp_buffer + mpp_packet_init_with_buffer
        #   做零拷贝(见 README 的优化点)。
        ret = self.lib.mpp_packet_init(ctypes.byref(packet), buf, len(data))
        if ret != 0:
            return frames
        self.lib.mpp_packet_set_pts(packet, pts_us)
        self.lib.mpp_packet_set_size(packet, len(data))

        try:
            self.mpi.decode_put_packet(self.ctx, packet)
            # 排空解码队列
            while True:
                frame = MppFrame()
                ret = self.mpi.decode_get_frame(self.ctx, ctypes.byref(frame))
                if ret != 0 or not frame:
                    break
                frames.append(self._wrap(frame))
                if len(frames) >= 8:   # ★ 防止异常流把队列灌爆
                    break
        finally:
            self.lib.mpp_packet_deinit(ctypes.byref(packet))
        return frames

    # ------------------------------------------------------------------
    def _wrap(self, frame: MppFrame) -> "FrameLease":
        buf = self.lib.mpp_frame_get_buffer(frame)
        fd = self.lib.mpp_buffer_get_fd_with_caller(buf, b"mpp") if buf else -1
        va = self.lib.mpp_buffer_get_ptr_with_caller(buf, b"mpp") if buf else 0
        self._frame_id += 1
        info = DecodedFrame(
            fd=fd,
            va=va or 0,
            width=self.lib.mpp_frame_get_width(frame),
            height=self.lib.mpp_frame_get_height(frame),
            hor_stride=self.lib.mpp_frame_get_hor_stride(frame),
            ver_stride=self.lib.mpp_frame_get_ver_stride(frame),
            pts=self.lib.mpp_frame_get_pts(frame),
            frame_id=self._frame_id,
        )
        return FrameLease(self, frame, info)

    # ------------------------------------------------------------------
    def reset(self) -> None:
        self.mpi.reset(self.ctx)

    def close(self) -> None:
        if self.ctx:
            try:
                self.lib.mpp_destroy(self.ctx)
            except Exception:
                pass
            self.ctx = MppCtx()


class RtspH264Demux:
    """RTSP -> AnnexB H.264 拆包。

    MPP 只解 H.264/H.265 码流, 不做 RTSP 协议, 所以要先把 RTSP 解成裸流。
    这里用 ffmpeg 子进程输出裸 h264(AnnexB, 带 in-band SPS/PPS)。

    ★ 瓶颈: 这里有一次【用户态拷贝】(socket -> pipe -> MPP)。
      12 路 1080P 每路 4~8 Mbps, 12 路合计 ~60 Mbps, CPU 拷贝约占 1 核的 3~5%。
      优化方向: 换 GStreamer(rtspsrc ! h264parse ! appsink) 或
                自研 RTSP 客户端直接拿到 SDP 的 sprop-parameter-sets,
                用 mpp_packet_init_with_buffer 做零拷贝。
    """

    def __init__(self, url: str, codec: str = "h264", ffmpeg_bin: str = "ffmpeg",
                 extra_args: list[str] | None = None, read_size: int = 256 * 1024):
        import subprocess
        args = [ffmpeg_bin, "-rtsp_transport", "tcp", "-i", url, "-an", "-sn",
                "-f", "h264" if codec == "h264" else "hevc", "-"]
        if extra_args:
            args[1:1] = extra_args
        self.proc = subprocess.Popen(args, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, bufsize=0)
        self.read_size = read_size

    def read_packet(self) -> bytes | None:
        if self.proc.stdout is None:
            return None
        data = self.proc.stdout.read(self.read_size)
        return data if data else None

    def close(self) -> None:
        if self.proc.poll() is None:
            self.proc.kill()
        if self.proc.stdout:
            self.proc.stdout.close()
