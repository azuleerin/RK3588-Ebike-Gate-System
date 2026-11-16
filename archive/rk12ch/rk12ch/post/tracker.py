"""目标跟踪(轻量 ByteTrack 风格)

★ 为什么不用 SORT/DeepSORT:
    12 路 x 10fps = 120 track/s, Python 里跑卡尔曼滤波 + 匈牙利匹配会吃掉
    1~2 个核, 得不偿失。这里用【两级阈值 + IoU 贪心匹配】的简化版:
        高分框(>=high_threshold)参与匹配 -> 维持已有 track
        低分框(>=low_threshold)只用于给未匹配的 track 续命
    对"电动车从门口经过"这种场景足够, 而且车牌的关联靠 track_id, 够用。

★ 优化点: 真要提高精度, 把这一层换成 C++ 实现(挂在独立的 gRPC/共享内存
  通道上), Python 只做业务编排。
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field


@dataclass
class Track:
    track_id: int
    box: tuple[float, float, float, float]
    score: float
    cls_id: int
    last_ts: float
    hits: int = 1
    age: int = 0
    plate: str = ""
    plate_ts: float = 0.0
    velocity: tuple[float, float] = (0.0, 0.0)

    @property
    def center(self) -> tuple[float, float]:
        x1, y1, x2, y2 = self.box
        return (x1 + x2) / 2.0, (y1 + y2) / 2.0


def iou(a: tuple[float, float, float, float],
        b: tuple[float, float, float, float]) -> float:
    ax1, ay1, ax2, ay2 = a
    bx1, by1, bx2, by2 = b
    ix1, iy1 = max(ax1, bx1), max(ay1, by1)
    ix2, iy2 = min(ax2, bx2), min(ay2, by2)
    iw, ih = max(0.0, ix2 - ix1), max(0.0, iy2 - iy1)
    inter = iw * ih
    if inter <= 0:
        return 0.0
    area_a = max(0.0, ax2 - ax1) * max(0.0, ay2 - ay1)
    area_b = max(0.0, bx2 - bx1) * max(0.0, by2 - by1)
    union = area_a + area_b - inter
    return inter / union if union > 0 else 0.0


class ByteTracker:
    """单路跟踪器"""

    def __init__(self, high_threshold: float = 0.6, low_threshold: float = 0.25,
                 match_iou: float = 0.5, max_age: int = 30):
        self.high = high_threshold
        self.low = low_threshold
        self.match_iou = match_iou
        self.max_age = max_age
        self.tracks: dict[int, Track] = {}
        self._next_id = 1

    # ------------------------------------------------------------------
    def update(self, detections: list, ts: float) -> list[Track]:
        """detections: [(cls_id, score, x1, y1, x2, y2)]"""
        high = [d for d in detections if d[1] >= self.high]
        low = [d for d in detections if self.low <= d[1] < self.high]

        unmatched_tracks = list(self.tracks.values())
        matched_det = set()

        # ---- 一级: 高分框与已有 track 匹配 ----
        pairs = []
        for ti, tr in enumerate(unmatched_tracks):
            for di, det in enumerate(high):
                if det[0] != tr.cls_id:
                    continue
                i = iou(tr.box, (det[2], det[3], det[4], det[5]))
                if i >= self.match_iou:
                    pairs.append((i, ti, di))
        pairs.sort(reverse=True)
        used_tr, used_det = set(), set()
        for _, ti, di in pairs:
            if ti in used_tr or di in used_det:
                continue
            used_tr.add(ti)
            used_det.add(di)
            self._apply(unmatched_tracks[ti], high[di], ts)

        # ---- 二级: 低分框给未匹配的 track 续命 ----
        for ti, tr in enumerate(unmatched_tracks):
            if ti in used_tr:
                continue
            best_i, best_di = 0.0, -1
            for di, det in enumerate(low):
                if det[0] != tr.cls_id:
                    continue
                i = iou(tr.box, (det[2], det[3], det[4], det[5]))
                if i > best_i:
                    best_i, best_di = i, di
            if best_di >= 0 and best_i >= self.match_iou * 0.6:
                self._apply(tr, low[best_di], ts)

        # ---- 新建 track ----
        for di, det in enumerate(high):
            if di in used_det:
                continue
            tid = self._next_id
            self._next_id += 1
            self.tracks[tid] = Track(
                track_id=tid,
                box=(det[2], det[3], det[4], det[5]),
                score=det[1],
                cls_id=det[0],
                last_ts=ts,
            )

        # ---- 清理老 track ----
        for tid in [tid for tid, tr in self.tracks.items()
                    if ts - tr.last_ts > self.max_age / 10.0]:
            del self.tracks[tid]

        return list(self.tracks.values())

    # ------------------------------------------------------------------
    def _apply(self, tr: Track, det, ts: float) -> None:
        cls_id, score, x1, y1, x2, y2 = det
        cx, cy = (x1 + x2) / 2.0, (y1 + y2) / 2.0
        pcx, pcy = tr.center
        dt = max(1e-3, ts - tr.last_ts)
        # 指数平滑的速度(像素/秒), 业务层可以用它做粗略测速
        tr.velocity = (0.7 * tr.velocity[0] + 0.3 * ((cx - pcx) / dt),
                       0.7 * tr.velocity[1] + 0.3 * ((cy - pcy) / dt))
        tr.box = (x1, y1, x2, y2)
        tr.score = score
        tr.cls_id = cls_id
        tr.last_ts = ts
        tr.hits += 1
        tr.age += 1


class PlateCache:
    """track_id -> 车牌 的缓存(车牌不会每帧变)"""

    def __init__(self, cache_ms: int = 2000):
        self.cache_ms = cache_ms
        self._cache: dict[int, tuple[str, float]] = {}

    def get(self, track_id: int, now: float) -> str:
        item = self._cache.get(track_id)
        if not item:
            return ""
        plate, ts = item
        if (now - ts) * 1000.0 > self.cache_ms:
            return ""
        return plate

    def put(self, track_id: int, plate: str, now: float) -> None:
        if plate:
            self._cache[track_id] = (plate, now)

    def evict(self, alive_ids: set[int]) -> None:
        for tid in [k for k in self._cache if k not in alive_ids]:
            self._cache.pop(tid, None)
