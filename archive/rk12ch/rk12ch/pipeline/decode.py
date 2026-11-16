"""第 1 层: 解码层 —— 每路一个线程, RKMPP 硬解 RTSP

线程模型:
    12 路 = 12 个解码线程, 每个线程:
        RTSP(ffmpeg 拆包) -> MPP 硬解 -> 拿到 NV12 dma-buf -> 丢进 Q1

★ 抽帧率在这里做(而不是在 NPU 前面):
    12 路 x 25fps = 300 帧/秒进解码, 但检测只需要 12 x 10 = 120 帧/秒。
    在解码层丢掉 60% 的帧, 能同时省掉 RGA 和 NPU 的开销, 而且省的是
    【整条链路】的开销(不是只省 NPU)。
    ★ 注意: 必须【解码后再丢】, 不能在 RTSP 层丢 —— 硬解是按 GOP 依赖的,
      丢包会导致参考帧缺失, 花屏甚至解不出后续帧。
"""

from __future__ import annotations

import logging
import threading
import time

from ..hal.mpp import MppDecoder
from ..hal.rtsp import RtspH264Client
from ..stats import StatsCollector
from .queues import BoundedQueue
from .types import RawFrame, RefLease

log = logging.getLogger(__name__)


class DecodeWorker(threading.Thread):
    """一路 RTSP 的解码线程"""

    def __init__(self, cfg: dict, out_queue: BoundedQueue, stats: StatsCollector,
                 stop_event: threading.Event):
        super().__init__(name=f"decode-{cfg['name']}", daemon=True)
        self.cfg = cfg
        self.out_q = out_queue
        self.stats = stats
        self.stop_event = stop_event
        self.channel_id = cfg["id"]
        self.name = cfg["name"]
        self.target_fps = float(cfg.get("target_fps", 0) or 0)
        self._min_interval = 1.0 / self.target_fps if self.target_fps > 0 else 0.0
        self._last_emit = 0.0

    # ------------------------------------------------------------------
    def run(self) -> None:
        # ★ 驱动没就绪时不要退出, 退避重试 —— 运维加载模块后能自动恢复
        while not self.stop_event.is_set():
            try:
                client = RtspH264Client(self.cfg["url"])
                client.connect()
                decoder = MppDecoder(codec=self.cfg.get("codec", "h264"))
                break
            except Exception as e:
                log.warning("[decode:%s] 初始化失败(将退避重试): %s", self.name, e)
                if self.stop_event.wait(5.0):
                    return
                client = None
        else:
            return
        if client is None:
            return

        log.info("[decode:%s] 开始拉流 %s", self.name, self.cfg["url"])
        pts = 0
        while not self.stop_event.is_set():
            try:
                data = client.read_frame()
            except Exception:
                log.exception("[decode:%s] 读流异常", self.name)
                break
            if not data:
                continue
            pts += int(len(data) * 8 / 25)  # 按 25Mbps 粗略折算 pts

            try:
                leases = decoder.decode(data, pts)
            except Exception:
                log.exception("[decode:%s] MPP 解码异常", self.name)
                continue

            for lease in leases:
                now = time.monotonic()
                # ★ 抽帧: 丢掉间隔内的帧(解码仍然每一帧都做, 保证参考帧完整)
                if self._min_interval > 0:
                    if now - self._last_emit < self._min_interval:
                        lease.close()
                        continue
                    self._last_emit = now
                frame = RawFrame(
                    channel_id=self.channel_id,
                    ref=RefLease(lease),
                    frame_id=lease.frame_id,
                    t_decode=now,
                    pts_us=lease.pts,
                    width=lease.width,
                    height=lease.height,
                    hor_stride=lease.hor_stride,
                    ver_stride=lease.ver_stride,
                )
                self.stats.on_decoded(self.channel_id, now)
                # ★ put 超时时间给 0: 解码线程不能被下游拖住, 宁可丢帧
                self.out_q.put(frame, timeout=0.0)

        # 收尾: 把队列里属于本路的帧释放掉
        log.info("[decode:%s] 退出", self.name)
        if decoder:
            decoder.close()
        if client:
            client.close()


class DecodeLayer:
    """解码层管理器: 统一管理 12 个解码线程"""

    def __init__(self, cfg: dict, stats: StatsCollector):
        self.cfg = cfg
        self.stats = stats
        self.stop_event = threading.Event()
        self.q_decoded = BoundedQueue("Q1:decoded", cfg["decode"]["decode_queue_size"])
        self.workers: list[DecodeWorker] = []
        stats.register_queue(self.q_decoded)

    def start(self) -> None:
        for ch in self.cfg["channels"]:
            self.stats.register_channel(ch["id"], ch["name"])
            w = DecodeWorker(ch, self.q_decoded, self.stats, self.stop_event)
            w.start()
            self.workers.append(w)
        log.info("[decode] %d 路解码线程已启动", len(self.workers))

    def stop(self) -> None:
        self.stop_event.set()
        for w in self.workers:
            w.join(timeout=3.0)
        self.q_decoded.close()
