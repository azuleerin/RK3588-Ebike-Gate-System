"""YOLOv8n 后处理(双输出形态 C)

模型输出:
    输出0 = box [n, 4, num_anchors]  (cx, cy, w, h, 单位是 640 输入图的像素)
    输出1 = cls [n, nc, num_anchors]  (已经是 sigmoid 之后的概率)

★ 为什么不能直接用官方 yolov8 的后处理:
    官方实现默认 "cxcywh -> xyxy" + NMS, 这里的坐标是【模型输入坐标系】的,
    要按 mapper 还原到原图(拉伸或 letterbox 两种模式)。

★ 性能: 8400 个 anchor 的过滤用 numpy 向量化, 纯 Python 循环会慢 100 倍。
"""

from __future__ import annotations

import numpy as np


class YoloPost:
    """双输出形态 C 的后处理"""

    def __init__(self, input_w: int, input_h: int, conf: float = 0.25,
                 nms_iou: float = 0.45, letterbox: bool = False,
                 class_names: dict | None = None):
        self.input_w = input_w
        self.input_h = input_h
        self.conf = conf
        self.nms_iou = nms_iou
        self.letterbox = letterbox
        self.class_names = class_names or {}

    # ------------------------------------------------------------------
    def decode(self, box_out: np.ndarray, cls_out: np.ndarray,
               batch_index: int = 0,
               img_w: int = 1920, img_h: int = 1080,
               scale: float = 1.0, pad_x: float = 0.0, pad_y: float = 0.0
               ) -> list[tuple[int, float, float, float, float, float]]:
        """返回 [(cls_id, score, x1, y1, x2, y2)], 坐标已经还原到原图系

        box_out: [n, 4, A]  cls_out: [n, nc, A]
        """
        # batch 维度可能是 1(模型固化 batch=1)也可能等于 batch
        b = box_out.reshape(-1, box_out.shape[-2], box_out.shape[-1])
        c = cls_out.reshape(-1, cls_out.shape[-2], cls_out.shape[-1])
        bi = min(batch_index, b.shape[0] - 1)
        boxes = b[bi]            # [4, A]
        scores = c[bi]           # [nc, A]

        # ---- 置信度筛选(向量化) ----
        scores_max = scores.max(axis=0)              # [A]
        cls_ids = scores.argmax(axis=0)              # [A]
        keep = scores_max >= self.conf
        if not keep.any():
            return []
        cls_ids = cls_ids[keep]
        scores_max = scores_max[keep]
        cx, cy, w, h = boxes[0][keep], boxes[1][keep], boxes[2][keep], boxes[3][keep]

        # ---- 模型输入坐标 -> 原图坐标 ----
        if self.letterbox:
            x1 = (cx - w / 2 - pad_x) / scale
            y1 = (cy - h / 2 - pad_y) / scale
            x2 = (cx + w / 2 - pad_x) / scale
            y2 = (cy + h / 2 - pad_y) / scale
        else:
            sx = img_w / self.input_w
            sy = img_h / self.input_h
            x1 = (cx - w / 2) * sx
            y1 = (cy - h / 2) * sy
            x2 = (cx + w / 2) * sx
            y2 = (cy + h / 2) * sy

        # ---- 裁到画面内(ROI 越界的根因之一) ----
        x1 = np.clip(x1, 0, img_w)
        y1 = np.clip(y1, 0, img_h)
        x2 = np.clip(x2, 0, img_w)
        y2 = np.clip(y2, 0, img_h)

        dets = [(int(cls_ids[i]), float(scores_max[i]), float(x1[i]), float(y1[i]),
                 float(x2[i]), float(y2[i])) for i in range(len(cls_ids))]
        return self.nms(dets)

    # ------------------------------------------------------------------
    def nms(self, dets: list) -> list:
        """按类做 NMS(贪心, numpy 加速)"""
        if not dets:
            return []
        arr = np.array(dets, dtype=np.float32)
        out = []
        for cls in np.unique(arr[:, 0]):
            idx = np.where(arr[:, 0] == cls)[0]
            boxes = arr[idx, 2:]
            scores = arr[idx, 1]
            order = scores.argsort()[::-1]
            keep = []
            while order.size > 0:
                i = order[0]
                keep.append(i)
                if order.size == 1:
                    break
                xx1 = np.maximum(boxes[i, 0], boxes[order[1:], 0])
                yy1 = np.maximum(boxes[i, 1], boxes[order[1:], 1])
                xx2 = np.minimum(boxes[i, 2], boxes[order[1:], 2])
                yy2 = np.minimum(boxes[i, 3], boxes[order[1:], 3])
                inter = np.clip(xx2 - xx1, 0, None) * np.clip(yy2 - yy1, 0, None)
                area_i = (boxes[i, 2] - boxes[i, 0]) * (boxes[i, 3] - boxes[i, 1])
                area_o = (boxes[order[1:], 2] - boxes[order[1:], 0]) * \
                         (boxes[order[1:], 3] - boxes[order[1:], 1])
                iou = inter / np.maximum(area_i + area_o - inter, 1e-6)
                order = order[1:][iou <= self.nms_iou]
            for k in keep:
                d = arr[idx[k]]
                out.append((int(d[0]), float(d[1]), float(d[2]), float(d[3]),
                            float(d[4]), float(d[5])))
        return out
