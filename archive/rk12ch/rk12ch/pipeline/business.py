"""第 4 层: 业务层 —— 跟踪 / 车牌 ROI 计算 / 车牌匹配 / 结果输出

处理两条输入流:
    Q3(YOLO 结果) -> 跟踪 -> 算车牌 ROI -> Q4(车牌裁剪请求)
    Q5(车牌识别结果) -> 按 track_id 匹配回 track -> 输出

★ 为什么要把业务层独立成线程:
    跟踪和上报都是 CPU 活(尤其 MQTT IO), 放在推理线程里会直接把 NPU 饿死。
    这里独立线程 + 队列, 推理线程只管 put。

★ 车牌 ROI 的几何先验:
    板端只有"车体框(ebike)", 没有单独的车牌框 —— 车牌挂在车尾(框的下部)。
    所以 ROI 用【相对车体框】的比例切, 而不是绝对像素:
        宽 = 框宽 * ratio, 顶边 = 框顶 + 框高 * roi_y + 框高 * roi_below
        高 = 框高 * roi_height
    切完还要外扩(pad), 外扩完【必须 clamp 回画面内】。
"""

from __future__ import annotations

import json
import logging
import queue
import threading
import time

from ..post.tracker import ByteTracker, PlateCache
from ..post.yolo import YoloPost
from .queues import BoundedQueue
from .types import BatchItem, PlateRequest, TrackResult

log = logging.getLogger(__name__)


def plate_roi_from_box(box: tuple[float, float, float, float],
                       ratio: float, roi_y: float, roi_height: float,
                       shift: float, below: float, pad: float,
                       fw: int, fh: int):
    """从车体框算出车牌 ROI(原图坐标系), 返回 (x, y, w, h) 或 None

    ★ 越界保护: 取整误差 + 外扩都可能让 x+w 超出画面, 必须在最后 clamp 一次。
    """
    x1, y1, x2, y2 = box
    bw, bh = x2 - x1, y2 - y1
    if bw < 4 or bh < 4:
        return None
    w = max(8.0, bw * ratio)
    h = max(6.0, bh * roi_height)
    cx = 0.5 * (x1 + x2) + bw * shift
    left = cx - w * 0.5
    top = y1 + bh * roi_y + bh * below
    # 外扩
    dx, dy = w * pad, h * pad
    left -= dx
    top -= dy
    w += dx * 2
    h += dy * 2
    # ★ clamp 到画面内
    if left < 0:
        w += left
        left = 0
    if top < 0:
        h += top
        top = 0
    if left + w > fw:
        w = fw - left
    if top + h > fh:
        h = fh - top
    if w < 8 or h < 6:
        return None
    return int(left), int(top), int(w), int(h)


class ResultSink:
    """输出基类"""

    def emit(self, payload: dict) -> None:
        raise NotImplementedError

    def close(self) -> None:
        pass


class StdoutSink(ResultSink):
    def emit(self, payload: dict) -> None:
        print(json.dumps(payload, ensure_ascii=False), flush=True)


class FileSink(ResultSink):
    def __init__(self, path: str):
        self.path = path
        self._lock = threading.Lock()

    def emit(self, payload: dict) -> None:
        with self._lock:
            with open(self.path, "a") as f:
                f.write(json.dumps(payload, ensure_ascii=False) + "\n")


class MqttSink(ResultSink):
    """MQTT 上行。★ 阻塞 IO 一定放在业务线程, 不能进推理线程"""

    def __init__(self, host: str, port: int, topic: str, qos: int = 1,
                 client_id: str = "rk12ch"):
        self.topic = topic
        self.qos = qos
        self._client = None
        try:
            import paho.mqtt.client as mqtt
            self._mqtt = mqtt
            self._client = mqtt.Client(client_id=client_id)
            self._client.connect(host, port, 60)
            self._client.loop_start()
        except Exception:
            log.exception("[business] MQTT 连接失败, 该 sink 退化为丢弃")

    def emit(self, payload: dict) -> None:
        if self._client is None:
            return
        try:
            self._client.publish(self.topic.format(sn=payload.get("sn", "dev")),
                                 json.dumps(payload, ensure_ascii=False), qos=self.qos)
        except Exception:
            log.exception("[business] MQTT 发送失败")

    def close(self) -> None:
        if self._client is not None:
            try:
                self._client.loop_stop()
                self._client.disconnect()
            except Exception:
                pass


class BusinessLayer:
    """业务层: 一个线程吃 YOLO 结果, 一个线程吃车牌结果"""

    def __init__(self, cfg: dict, yolo_queue: BoundedQueue, plate_req_queue: BoundedQueue,
                 plate_res_queue: BoundedQueue, stats, stop_event: threading.Event):
        self.cfg = cfg
        self.yolo_q = yolo_queue
        self.plate_req_q = plate_req_queue
        self.plate_res_q = plate_res_queue
        self.stats = stats
        self.stop_event = stop_event

        m = cfg["models"]["yolo"]
        self.post = YoloPost(
            input_w=m["input_w"], input_h=m["input_h"],
            conf=m["conf_threshold"], nms_iou=m["nms_iou"],
            class_names=m.get("class_names"),
        )
        t = cfg["business"]["tracker"]
        self.trackers: dict[int, ByteTracker] = {}
        self.plate_cache = PlateCache(cfg["business"]["plate_match"]["cache_ms"])

        lm = cfg["models"]["lpr"]
        self.roi_cfg = lm["roi"]
        self.plate_cls_ids = set()
        for name, cid in (m.get("class_names") or {}).items():
            if "plate" in name.lower():
                self.plate_cls_ids.add(int(cid))
        # 允许配置里直接指定哪些类是车牌
        for cid in (cfg.get("business", {}).get("plate_cls_ids") or []):
            self.plate_cls_ids.add(int(cid))

        self.sinks: list[ResultSink] = []
        self._build_sinks(cfg)

        self._threads = [
            threading.Thread(target=self._yolo_loop, name="business-yolo", daemon=True),
            threading.Thread(target=self._plate_loop, name="business-plate", daemon=True),
        ]

    # ------------------------------------------------------------------
    def _build_sinks(self, cfg: dict) -> None:
        out = cfg["business"]["output"]
        for kind in out.get("sinks", ["stdout"]):
            if kind == "stdout":
                self.sinks.append(StdoutSink())
            elif kind == "file":
                self.sinks.append(FileSink(out.get("file_path", "/tmp/rk12ch.jsonl")))
            elif kind == "mqtt":
                c = out.get("mqtt", {})
                self.sinks.append(MqttSink(c.get("host", "127.0.0.1"), c.get("port", 1883),
                                           c.get("topic", "device/{sn}/message/up/ivs_result"),
                                           c.get("qos", 1),
                                           f"rk12ch-{cfg['system']['device_sn']}"))

    def start(self) -> None:
        for t in self._threads:
            t.start()

    def stop(self) -> None:
        self.stop_event.set()
        for t in self._threads:
            t.join(timeout=3.0)
        for s in self.sinks:
            s.close()

    # ------------------------------------------------------------------
    def _yolo_loop(self) -> None:
        """吃 YOLO 结果 -> 跟踪 -> 算车牌 ROI -> 发裁剪请求 -> 输出"""
        while not self.stop_event.is_set():
            item = self.yolo_q.get(timeout=0.2)
            if item is None:
                continue
            batch_item, outputs = item
            try:
                self._handle_detection(batch_item, outputs)
            except Exception:
                log.exception("[business] 处理检测结果异常")
            finally:
                # ★ 这一帧的原始帧引用: 如果没有产生车牌裁剪请求, 在这里释放
                if batch_item.raw is not None:
                    batch_item.raw.release()

    # ------------------------------------------------------------------
    def _handle_detection(self, item: BatchItem, outputs) -> None:
        if len(outputs) < 2:
            return
        if not item.meta.get("valid", True):
            return      # RGA 失败的槽位, 跳过
        box_out, cls_out = outputs[0], outputs[1]
        dets = self.post.decode(box_out, cls_out, batch_index=item.slot,
                                img_w=item.frame_w, img_h=item.frame_h)
        if not dets:
            return

        tracker = self.trackers.get(item.channel_id)
        if tracker is None:
            t = self.cfg["business"]["tracker"]
            tracker = ByteTracker(t["high_threshold"], t["low_threshold"],
                                  t["match_iou"], t["max_age"])
            self.trackers[item.channel_id] = tracker

        now = time.monotonic()
        tracks = tracker.update(dets, now)
        self.stats.on_detected(item.channel_id, now)

        results = []
        for tr in tracks:
            plate = self.plate_cache.get(tr.track_id, now)
            tr.plate = plate
            results.append(TrackResult(
                track_id=tr.track_id, channel_id=item.channel_id,
                frame_id=item.frame_id, box=tr.box, score=tr.score,
                cls_id=tr.cls_id, t_decode=item.t_decode, plate=plate,
            ))

        # ---- 车牌 ROI -> 裁剪请求 ----
        for tr in tracks:
            if self.plate_cls_ids and tr.cls_id not in self.plate_cls_ids:
                continue
            if self.plate_cache.get(tr.track_id, now):
                continue    # 已经有车牌了, 不重复识别(省 NPU)
            if item.raw is None:
                continue
            roi = plate_roi_from_box(
                tr.box,
                ratio=self.roi_cfg["ratio"], roi_y=self.roi_cfg["y"],
                roi_height=self.roi_cfg["height"], shift=self.roi_cfg["shift"],
                below=self.roi_cfg["below"], pad=self.roi_cfg["pad"],
                fw=item.frame_w, fh=item.frame_h,
            )
            if roi is None:
                continue
            item.raw.retain()          # ★ 裁剪请求持有一份引用
            req = PlateRequest(
                channel_id=item.channel_id, frame_id=item.frame_id,
                track_id=tr.track_id, ref=item.raw, t_decode=item.t_decode,
                box=roi, frame_w=item.frame_w, frame_h=item.frame_h,
                src_geometry=(item.hor_stride, item.ver_stride,
                              item.frame_w, item.frame_h),
            )
            self.plate_req_q.put(req, timeout=0.0)

        # ---- 上报 ----
        if results:
            payload = {
                "sn": self.cfg["system"]["device_sn"],
                "ch": item.channel_id,
                "frame_id": item.frame_id,
                "ts": int(time.time() * 1000),
                "objects": [
                    {
                        "track_id": r.track_id, "cls": r.cls_id,
                        "score": round(r.score, 3),
                        "box": [round(v, 1) for v in r.box],
                        "plate": r.plate,
                    } for r in results
                ],
            }
            for s in self.sinks:
                try:
                    s.emit(payload)
                except Exception:
                    log.exception("[business] sink 输出失败")
            # 端到端延时: 从解码完成到业务输出
            self.stats.on_latency(item.channel_id,
                                  (time.monotonic() - item.t_decode) * 1000.0)

    # ------------------------------------------------------------------
    def _plate_loop(self) -> None:
        """吃车牌识别结果 -> 匹配回 track -> 更新缓存"""
        while not self.stop_event.is_set():
            item = self.plate_res_q.get(timeout=0.2)
            if item is None:
                continue
            batch_item, plate = item
            now = time.monotonic()
            self.plate_cache.put(batch_item.meta.get("track_id", -1), plate, now)
            self.stats.on_latency(batch_item.channel_id,
                                  (now - batch_item.t_decode) * 1000.0)
