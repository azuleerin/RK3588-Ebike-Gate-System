"""性能统计: 每路 FPS / 端到端延时 / NPU 利用率 / CPU 占用 / 队列积压

★ 端到端延时的定义(必须统一, 否则没有可比性):
    t0 = MPP 解码出帧的时刻(解码层打点)
    t1 = 业务层输出结果的时刻
    latency = t1 - t0
  这条链路上没有"帧被采集"的准确时刻(MPP 不给), 所以 t0 是【硬件解码完成】
  时刻, 已经是能拿到的最接近采集端的点。

★ NPU 利用率:
    rknn_query(RKNN_QUERY_PERF_DETAIL) 需要 rknn_init 时带
    RKNN_FLAG_COLLECT_PERF_MASK, 返回的是驱动层的详细调度信息(字符串)。
    更稳定/更轻量的做法是用 RKNN_QUERY_PERF_RUN 拿单次推理耗时,
    再用 推理耗时 / 统计周期 估算 NPU 忙碌率。
"""

from __future__ import annotations

import logging
import os
import threading
import time
from collections import defaultdict, deque
from dataclasses import dataclass, field

log = logging.getLogger(__name__)


class Ema:
    """指数滑动平均, 用于平滑抖动大的指标"""

    def __init__(self, alpha: float = 0.1):
        self.alpha = alpha
        self.value: float | None = None

    def update(self, x: float) -> float:
        self.value = x if self.value is None else self.alpha * x + (1 - self.alpha) * self.value
        return self.value


class LatencyHistogram:
    """固定桶的延时直方图(比存全部样本省内存)"""

    def __init__(self, buckets_ms: list[float]):
        self.buckets = sorted(buckets_ms)
        self.counts = [0] * (len(self.buckets) + 1)
        self.n = 0
        self.sum = 0.0
        self.max = 0.0
        self._lock = threading.Lock()

    def add(self, ms: float) -> None:
        with self._lock:
            self.n += 1
            self.sum += ms
            self.max = max(self.max, ms)
            for i, b in enumerate(self.buckets):
                if ms <= b:
                    self.counts[i] += 1
                    return
            self.counts[-1] += 1

    def summary(self) -> dict:
        with self._lock:
            avg = self.sum / self.n if self.n else 0.0
            p = {}
            cum = 0
            for i, b in enumerate(self.buckets):
                cum += self.counts[i]
                p[f"le{int(b)}ms"] = cum / self.n if self.n else 0.0
            return {"n": self.n, "avg_ms": avg, "max_ms": self.max, "percentile": p}


@dataclass
class ChannelStats:
    channel_id: int
    name: str
    decoded: int = 0
    detected: int = 0
    decoded_fps: Ema = field(default_factory=lambda: Ema(0.1))
    detect_fps: Ema = field(default_factory=lambda: Ema(0.1))
    latency: LatencyHistogram | None = None
    last_decode_ts: float = 0.0
    _ts_window: deque = field(default_factory=lambda: deque(maxlen=30))


class StatsCollector:
    """全局统计中心。所有模块通过它打点, 避免各自实现一套"""

    def __init__(self, buckets_ms: list[float] | None = None):
        self.buckets = buckets_ms or [5, 10, 20, 40, 80, 160, 320]
        self.channels: dict[int, ChannelStats] = {}
        self.queues: dict[str, object] = {}
        self.npu_busy_ema = Ema(0.15)
        self.cpu_usage_ema = Ema(0.15)
        self._lock = threading.Lock()
        self._last_cpu = self._read_cpu()
        self._last_sample_ts = time.monotonic()
        self._npu_run_us_total = 0
        self._npu_window_start = time.monotonic()

    # ------------------------------------------------------------------
    def register_channel(self, cid: int, name: str) -> None:
        with self._lock:
            self.channels[cid] = ChannelStats(channel_id=cid, name=name,
                                              latency=LatencyHistogram(self.buckets))

    def register_queue(self, q) -> None:
        self.queues[q.name] = q

    # ------------------------------------------------------------------
    def on_decoded(self, cid: int, ts: float) -> None:
        with self._lock:
            cs = self.channels.get(cid)
            if cs is None:
                return
            cs.decoded += 1
            cs._ts_window.append(ts)
            if len(cs._ts_window) > 1:
                span = cs._ts_window[-1] - cs._ts_window[0]
                if span > 0:
                    cs.decoded_fps.update(len(cs._ts_window) / span)

    def on_detected(self, cid: int, ts: float) -> None:
        with self._lock:
            cs = self.channels.get(cid)
            if cs is None:
                return
            cs.detected += 1

    def on_latency(self, cid: int, ms: float) -> None:
        with self._lock:
            cs = self.channels.get(cid)
            if cs and cs.latency:
                cs.latency.add(ms)

    def on_npu_run(self, duration_us: int) -> None:
        """累加 NPU 推理耗时, 用于估算利用率"""
        self._npu_run_us_total += duration_us

    # ------------------------------------------------------------------
    def _read_cpu(self) -> tuple[int, int]:
        """读 /proc/stat 的 cpu 累计时间(jiffies)"""
        try:
            with open("/proc/stat") as f:
                parts = f.readline().split()
            return int(parts[1]), int(parts[2]) + int(parts[3]) + int(parts[4])
        except (OSError, IndexError, ValueError):
            return 0, 0

    def sample_cpu(self) -> float:
        """CPU 总占用百分比(0~100)"""
        cur = self._read_cpu()
        prev = self._last_cpu
        self._last_cpu = cur
        dt = cur[0] - prev[0]
        db = cur[1] - prev[1]
        if dt <= 0:
            return self.cpu_usage_ema.value or 0.0
        usage = max(0.0, min(100.0, 100.0 * (1.0 - db / dt)))
        self.cpu_usage_ema.update(usage)
        return self.cpu_usage_ema.value or 0.0

    def sample_npu(self) -> float:
        """NPU 忙碌率估算: 统计窗口内 推理总耗时 / 窗口时长"""
        now = time.monotonic()
        window = now - self._npu_window_start
        if window <= 0:
            return 0.0
        busy = self._npu_run_us_total / 1e6 / window * 100.0
        self.npu_busy_ema.update(min(100.0, busy))
        self._npu_run_us_total = 0
        self._npu_window_start = now
        return self.npu_busy_ema.value or 0.0

    # ------------------------------------------------------------------
    def render(self) -> str:
        """打印一个统计周期"""
        npu = self.sample_npu()
        cpu = self.sample_cpu()
        lines = ["=" * 96,
                 f" 性能统计  {time.strftime('%H:%M:%S')}   CPU {cpu:5.1f}%   NPU {npu:5.1f}%",
                 "-" * 96,
                 f"{'路':<4}{'名称':<14}{'解码FPS':>9}{'检测FPS':>9}{'延时avg':>10}"
                 f"{'延时max':>10}{'P95':>9}{'帧数':>8}"]
        for cid, cs in sorted(self.channels.items()):
            lat = cs.latency.summary() if cs.latency else {}
            avg = lat.get("avg_ms", 0.0)
            mx = lat.get("max_ms", 0.0)
            p95 = 0.0
            if cs.latency:
                for k, v in sorted(cs.latency.summary()["percentile"].items()):
                    if v >= 0.95:
                        p95 = float(k.replace("le", "").replace("ms", ""))
                        break
            lines.append(f"{cid:<4}{cs.name:<14}{cs.decoded_fps.value or 0:>9.1f}"
                         f"{(cs.detect_fps.value or 0):>9.1f}{avg:>9.1f}ms"
                         f"{mx:>9.1f}ms{p95:>8.0f}ms{cs.decoded:>8}")
        lines.append("-" * 96)
        lines.append(f"{'队列':<24}{'深度':>8}{'容量':>8}{'入队':>10}{'出队':>10}{'丢弃':>10}{'丢弃率':>9}")
        for name, q in self.queues.items():
            st = q.stats
            lines.append(f"{name:<24}{st.size:>8}{st.capacity:>8}{st.pushed:>10}"
                         f"{st.popped:>10}{st.dropped:>10}{st.drop_rate * 100:>8.1f}%")
        lines.append("=" * 96)
        return "\n".join(lines)

    def dump(self, path: str | None = None) -> None:
        text = self.render()
        log.info("\n%s", text)
        if path:
            try:
                with open(path, "a") as f:
                    f.write(text + "\n")
            except OSError:
                pass
