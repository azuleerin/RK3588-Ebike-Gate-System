#!/usr/bin/env python3
"""mqtt_alarm.py —— 往 MQTT broker 发一条告警报文

★ 不重复造 MQTT 实现: 直接复用同目录 hw_load_mqtt.py 里的 MqttPub
  (极简 MQTT 3.1.1 客户端, 只会发 PUBLISH, 带自动重连)。
  mqtt_sub.py 是订阅端, hw_load_mqtt.py 是发布端 —— 两个都在仓库里, 直接用。

用法:
    python3 scripts/mqtt_alarm.py --host 192.168.31.30 --port 1883 \
        --sn RK3588-GATE-01 --level 2 --title "系统已被手动停止" \
        --msg "rk_gate 服务处于 inactive 且无维护标记"

topic 约定(与板端既有报文一致, 看板订阅 device/#):
    device/<sn>/message/up/alarm

告警分级(本项目约定):
    Level 1 = 致命   —— 服务不可用, 需要立即介入
    Level 2 = 警告   —— 状态异常, 需要人工确认是否预期
    Level 3 = 提示   —— 供参考, 无需立即处理

退出码:
    0 = 发送成功
    1 = 参数/连接/发送失败(原因打 stderr)
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    from hw_load_mqtt import MqttPub
except Exception as e:  # noqa: BLE001
    print(f"[alarm] 复用 hw_load_mqtt.MqttPub 失败: {e}", file=sys.stderr)
    sys.exit(1)

LEVEL_NAMES = {1: "致命", 2: "警告", 3: "提示"}


def main() -> int:
    ap = argparse.ArgumentParser(description="发一条 MQTT 告警")
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--sn", default="rk3588-01")
    ap.add_argument("--level", type=int, default=2, choices=[1, 2, 3])
    ap.add_argument("--title", required=True)
    ap.add_argument("--msg", required=True)
    ap.add_argument("--topic", default="", help="默认 device/<sn>/message/up/alarm")
    ap.add_argument("--qos", type=int, default=0,
                    help="默认 0 —— 告警是尽力而为的通知, 不依赖 PUBACK 确认;"
                         "QoS=1 需要 broker 回 PUBACK, 现场 broker 行为不一致时反而更脆")
    ap.add_argument("--extra", default="", help="额外的 JSON 字段(k=v,k=v)")
    args = ap.parse_args()

    topic = args.topic or f"device/{args.sn}/message/up/alarm"

    payload = {
        "sn": args.sn,
        "ts": int(time.time()),
        "ts_str": time.strftime("%Y-%m-%d %H:%M:%S"),
        "type": "alarm",
        "level": args.level,
        "level_name": LEVEL_NAMES.get(args.level, "未知"),
        "title": args.title,
        "msg": args.msg,
        "source": "gate_health.sh",
    }
    # --extra k=v,k=v
    if args.extra:
        for kv in args.extra.split(","):
            if "=" in kv:
                k, v = kv.split("=", 1)
                payload[k.strip()] = v.strip()

    body = json.dumps(payload, ensure_ascii=False)

    pub = MqttPub(args.host, args.port, client_id=f"{args.sn}-alarm",
                  keepalive=30)
    if not pub.connect():
        print(f"[alarm] 连不上 broker {args.host}:{args.port}", file=sys.stderr)
        return 1
    try:
        ok = pub.publish(topic, body, qos=args.qos)
    finally:
        pub.close()
    if not ok:
        print(f"[alarm] 发送失败: {topic}", file=sys.stderr)
        return 1
    print(f"[alarm] 已发 Level {args.level} 告警 -> {args.host}:{args.port} {topic}")
    print(f"[alarm] {body}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
