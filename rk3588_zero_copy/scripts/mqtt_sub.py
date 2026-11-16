#!/usr/bin/env python3
"""mqtt_sub.py —— 极简 MQTT 3.1.1 订阅端(不依赖任何第三方库)

板子上没装 paho-mqtt, 而 PC 端(.30)的 broker 是现成的 —— 所以自己按
MQTT 3.1.1 的报文格式实现一个订阅端, 把板端推上来的 JSON 全部落盘。

用法:
    python3 scripts/mqtt_sub.py --host 192.168.8.21 --port 1883 \
        --topic 'device/#' --out /tmp/mqtt_capture.jsonl [--seconds 60]

输出:
    /tmp/mqtt_capture.jsonl —— 每行一条 {topic, payload, ts}
    终端打印收到的前几条(截断显示)
"""
from __future__ import annotations

import argparse
import json
import socket
import struct
import sys
import time


def enc_len(n: int) -> bytes:
    out = b""
    while True:
        b = n % 128
        n //= 128
        if n:
            out += bytes([b | 0x80])
        else:
            out += bytes([b])
            break
    return out


def enc_str(s: str) -> bytes:
    b = s.encode("utf-8")
    return struct.pack("!H", len(b)) + b


def read_len(sock) -> int:
    mult, value = 1, 0
    while True:
        b = sock.recv(1)
        if not b:
            raise ConnectionError("连接断开")
        value += (b[0] & 0x7F) * mult
        mult *= 128
        if not (b[0] & 0x80):
            return value


def read_exact(sock, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("连接断开")
        buf += chunk
    return buf


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.8.21")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--topic", default="#")
    ap.add_argument("--out", default="/tmp/mqtt_capture.jsonl")
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--client-id", default="zcode_sub")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    sock = socket.create_connection((args.host, args.port), timeout=10)
    sock.settimeout(2.0)

    # ---- CONNECT ----
    payload = enc_str(args.client_id)
    var = enc_str("MQTT") + bytes([4, 0x02]) + struct.pack("!H", 30) + payload
    sock.sendall(bytes([0x10]) + enc_len(len(var)) + var)

    # ---- 读 CONNACK ----
    hdr = sock.recv(1)
    if not hdr or hdr[0] != 0x20:
        print("没有收到 CONNACK", file=sys.stderr)
        return 1
    rl = read_len(sock)
    ack = read_exact(sock, rl)
    print(f"[mqtt] 已连接 {args.host}:{args.port}, CONNACK session_present={ack[0]} code={ack[1]}")
    if ack[1] != 0:
        print(f"连接被拒绝, code={ack[1]}", file=sys.stderr)
        return 1

    # ---- SUBSCRIBE ----
    sub = struct.pack("!H", 1) + enc_str(args.topic) + bytes([0])
    sock.sendall(bytes([0x82]) + enc_len(len(sub)) + sub)

    end = time.time() + args.seconds
    n = 0
    with open(args.out, "w") as f:
        while time.time() < end:
            try:
                hdr = sock.recv(1)
            except socket.timeout:
                continue
            if not hdr:
                break
            ptype = hdr[0] & 0xF0
            rl = read_len(sock)
            body = read_exact(sock, rl) if rl else b""
            if ptype != 0x30:            # 只处理 PUBLISH
                continue
            # 变长头: topic (+ packet id, 当 QoS>0)
            tlen = struct.unpack("!H", body[:2])[0]
            topic = body[2:2 + tlen].decode("utf-8", "replace")
            off = 2 + tlen
            qos = (hdr[0] & 0x06) >> 1
            if qos > 0:
                off += 2
            data = body[off:]
            n += 1
            rec = {"ts": time.time(), "topic": topic, "payload": data.decode("utf-8", "replace")}
            f.write(json.dumps(rec, ensure_ascii=False) + "\n")
            f.flush()
            if not args.quiet and n <= 5:
                txt = rec["payload"]
                if len(txt) > 400:
                    txt = txt[:400] + "..."
                print(f"[mqtt] #{n} {topic}\n        {txt}")
    print(f"[mqtt] 共收到 {n} 条, 已写入 {args.out}")
    sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
