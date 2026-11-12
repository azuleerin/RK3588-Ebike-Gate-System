"""配置加载与校验

★ 原则: 代码里不出现任何硬编码的路径/阈值/路数, 全部从 YAML 来。
  校验要在启动时就做, 不能等到跑起来才报错(12 路跑起来再崩代价太大)。
"""

from __future__ import annotations

import os
from typing import Any

import yaml


REQUIRED_TOP = ("system", "channels", "decode", "preprocess", "inference",
                "models", "business", "stats")


class ConfigError(ValueError):
    pass


def load_config(path: str) -> dict:
    if not os.path.exists(path):
        raise ConfigError(f"配置文件不存在: {path}")
    with open(path, "r", encoding="utf-8") as f:
        cfg = yaml.safe_load(f)
    if not isinstance(cfg, dict):
        raise ConfigError("配置文件根节点必须是字典")
    validate_config(cfg)
    return cfg


def validate_config(cfg: dict) -> None:
    for k in REQUIRED_TOP:
        if k not in cfg:
            raise ConfigError(f"配置缺少必需段: {k}")

    chans = cfg["channels"]
    if not isinstance(chans, list) or not chans:
        raise ConfigError("channels 必须是非空列表")
    ids = set()
    for ch in chans:
        for key in ("id", "name", "url"):
            if key not in ch:
                raise ConfigError(f"channel 缺少字段 {key}: {ch}")
        if ch["id"] in ids:
            raise ConfigError(f"channel id 重复: {ch['id']}")
        ids.add(ch["id"])
        tf = ch.get("target_fps", 0)
        if tf < 0:
            raise ConfigError(f"channel {ch['id']} 的 target_fps 不能为负")

    yolo = cfg["models"]["yolo"]
    for key in ("path", "labels", "input_w", "input_h", "conf_threshold", "nms_iou"):
        if key not in yolo:
            raise ConfigError(f"models.yolo 缺少字段 {key}")
    if not os.path.exists(yolo["path"]):
        raise ConfigError(f"YOLO 模型不存在: {yolo['path']}")
    if yolo.get("labels") and not os.path.exists(yolo["labels"]):
        raise ConfigError(f"标签文件不存在: {yolo['labels']}")

    lpr = cfg["models"]["lpr"]
    for key in ("path", "input_w", "input_h", "charset"):
        if key not in lpr:
            raise ConfigError(f"models.lpr 缺少字段 {key}")
    if not os.path.exists(lpr["path"]):
        raise ConfigError(f"LPRNet 模型不存在: {lpr['path']}")
    if lpr.get("feed", "int8_code") not in ("int8_code", "uint8", "float32"):
        raise ConfigError("models.lpr.feed 只能是 int8_code / uint8 / float32")

    inf = cfg["inference"]
    if inf.get("yolo_batch", 1) < 1 or inf.get("lpr_batch", 1) < 1:
        raise ConfigError("batch 必须 >= 1")
    for key in ("yolo_cores", "lpr_cores"):
        if key not in inf:
            raise ConfigError(f"inference 缺少字段 {key}")


def dump_channels_template(count: int, base_url: str = "rtsp://192.168.31.200:8554/h264",
                            start_port: int = 8554) -> list[dict]:
    """生成 N 路通道模板(给 12 路部署脚本用)"""
    out = []
    for i in range(count):
        out.append({
            "id": i,
            "name": f"ch{i}",
            "url": base_url.replace(str(start_port), str(start_port + i)),
            "fps": 25,
            "target_fps": 10,
            "width": 1920,
            "height": 1080,
            "codec": "h264",
        })
    return out


def merge_overrides(cfg: dict, overrides: dict[str, Any]) -> dict:
    """命令行覆盖: --set models.yolo.conf_threshold=0.3"""
    for k, v in overrides.items():
        node = cfg
        parts = k.split(".")
        for p in parts[:-1]:
            node = node.setdefault(p, {})
        old = node.get(parts[-1])
        if isinstance(old, bool):
            node[parts[-1]] = v.lower() in ("1", "true", "yes", "on")
        elif isinstance(old, int) and not isinstance(old, bool):
            node[parts[-1]] = int(v)
        elif isinstance(old, float):
            node[parts[-1]] = float(v)
        else:
            node[parts[-1]] = v
    return cfg
