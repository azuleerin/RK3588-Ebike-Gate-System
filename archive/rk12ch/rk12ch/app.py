"""12 路 1080P RTSP 电动车检测 + 车牌识别 —— 主程序

四层流水线:
    解码层(每路一线程, MPP 硬解)
        | Q1
    预处理层(RGA 裁剪/缩放/颜色转换, 零拷贝)
        | Q2 (batch)
    推理层(YOLO 2 核 + LPRNet 1 核)
        | Q3 / Q5
    业务层(跟踪 + 车牌匹配 + 输出)
        | Q4 (车牌裁剪请求) -> 预处理层(车牌) -> 推理层(LPRNet)

启动:
    python3 -m rk12ch.app --config config/system.yaml
    python3 -m rk12ch.app --config config/system.yaml --set models.yolo.conf_threshold=0.3
"""

from __future__ import annotations

import argparse
import logging
import os
import signal
import sys
import threading
import time

from .config import load_config, merge_overrides
from .pipeline.business import BusinessLayer
from .pipeline.decode import DecodeLayer
from .pipeline.infer import LprWorker, YoloWorker
from .pipeline.preprocess import PlatePreprocessLayer, PreprocessLayer
from .pipeline.queues import BoundedQueue
from .stats import StatsCollector

log = logging.getLogger("rk12ch")


def setup_logging(level: str, log_dir: str | None = None) -> None:
    handlers = [logging.StreamHandler(sys.stdout)]
    if log_dir:
        try:
            os.makedirs(log_dir, exist_ok=True)
            handlers.append(logging.FileHandler(os.path.join(log_dir, "rk12ch.log")))
        except OSError:
            pass
    logging.basicConfig(
        level=getattr(logging, level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)-5s [%(threadName)-16s] %(name)s: %(message)s",
        handlers=handlers,
    )


class Application:
    def __init__(self, cfg: dict):
        self.cfg = cfg
        self.stop_event = threading.Event()
        self.stats = StatsCollector(cfg["stats"].get("latency_buckets"))

        # ---- 队列 ----
        self.q_decoded = None      # 解码 -> 预处理
        self.q_batch = None        # 预处理 -> 推理
        self.q_yolo_res = None     # 推理 -> 业务
        self.q_plate_req = None    # 业务 -> 预处理(车牌裁剪请求)
        self.q_plate_img = None    # 预处理 -> 推理(车牌小图 batch)
        self.q_plate_out = None    # 推理 -> 业务(车牌识别结果)

        self.decode_layer = None
        self.pre = None
        self.pre_plate = None
        self.yolo = None
        self.lpr = None
        self.business = None
        self._stat_thread = None

    # ------------------------------------------------------------------
    def build(self) -> None:
        cfg = self.cfg
        inf = cfg["inference"]

        # 解码层自带 Q1
        self.decode_layer = DecodeLayer(cfg, self.stats)
        self.q_decoded = self.decode_layer.q_decoded

        self.q_batch = BoundedQueue("Q2:batch", inf.get("queue_size", 8))
        self.q_yolo_res = BoundedQueue("Q3:yolo-result", inf.get("queue_size", 8))
        self.q_plate_req = BoundedQueue("Q4:plate-req", inf.get("queue_size", 8))
        self.q_plate_img = BoundedQueue("Q5:plate-img", inf.get("queue_size", 8))
        self.q_plate_out = BoundedQueue("Q6:plate-out", inf.get("queue_size", 8))
        for q in (self.q_batch, self.q_yolo_res, self.q_plate_req,
                  self.q_plate_img, self.q_plate_out):
            self.stats.register_queue(q)

        self.pre = PreprocessLayer(cfg, self.q_decoded, self.q_batch,
                                   self.stats, self.stop_event)
        self.pre_plate = PlatePreprocessLayer(cfg, self.q_plate_req, self.q_plate_img,
                                               self.stop_event)
        self.yolo = YoloWorker(cfg, self.q_batch, self.q_yolo_res, self.stats,
                               self.stop_event, pool=self.pre.pool)
        self.lpr = LprWorker(cfg, self.q_plate_img, self.q_plate_out, self.stats,
                             self.stop_event, pool=self.pre_plate.pool)
        self.business = BusinessLayer(cfg, self.q_yolo_res, self.q_plate_req,
                                      self.q_plate_out, self.stats, self.stop_event)

    # ------------------------------------------------------------------
    def start(self) -> None:
        self.decode_layer.start()
        self.pre.start()
        self.pre_plate.start()
        self.yolo.start()
        self.lpr.start()
        self.business.start()
        self._stat_thread = threading.Thread(target=self._stat_loop,
                                              name="stats", daemon=True)
        self._stat_thread.start()
        log.info("[app] 流水线已启动: %d 路 / YOLO核=%s / LPR核=%s",
                 len(self.cfg["channels"]),
                 self.cfg["inference"]["yolo_cores"],
                 self.cfg["inference"]["lpr_cores"])

    # ------------------------------------------------------------------
    def _stat_loop(self) -> None:
        interval = self.cfg["system"].get("stats_interval_sec", 5.0)
        while not self.stop_event.wait(interval):
            if self.cfg["stats"].get("enabled", True):
                self.stats.dump()

    # ------------------------------------------------------------------
    def stop(self) -> None:
        log.info("[app] 正在停止 ...")
        self.stop_event.set()
        for q in (self.q_batch, self.q_yolo_res, self.q_plate_req,
                  self.q_plate_img, self.q_plate_out):
            q.close()
        if self.business:
            self.business.stop()
        if self.yolo:
            self.yolo.join(timeout=3.0)
        if self.lpr:
            self.lpr.join(timeout=3.0)
        if self.pre:
            self.pre.stop()
        if self.pre_plate:
            self.pre_plate.stop()
        if self.decode_layer:
            self.decode_layer.stop()
        if self.cfg["stats"].get("enabled", True):
            self.stats.dump()

    def wait(self) -> None:
        try:
            while not self.stop_event.is_set():
                time.sleep(0.5)
        except KeyboardInterrupt:
            pass


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="RK3588 12 路 RTSP 电动车检测 + 车牌识别")
    ap.add_argument("--config", default="config/system.yaml")
    ap.add_argument("--set", action="append", default=[],
                    help="配置覆盖, 例: --set models.yolo.conf_threshold=0.3")
    ap.add_argument("--duration", type=int, default=0,
                    help="运行多少秒后自动退出(0=常驻)")
    args = ap.parse_args(argv)

    cfg = load_config(args.config)
    overrides = {}
    for item in args.set:
        if "=" not in item:
            print(f"--set 参数格式错误: {item}", file=sys.stderr)
            return 2
        k, v = item.split("=", 1)
        overrides[k] = v
    cfg = merge_overrides(cfg, overrides)

    setup_logging(cfg["system"].get("log_level", "INFO"),
                  cfg["system"].get("log_dir"))

    app = Application(cfg)
    app.build()
    app.start()

    def _sig(signum, frame):
        app.stop_event.set()
    signal.signal(signal.SIGINT, _sig)
    signal.signal(signal.SIGTERM, _sig)

    if args.duration > 0:
        t = threading.Timer(args.duration, app.stop_event.set)
        t.daemon = True
        t.start()
    app.wait()
    app.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
