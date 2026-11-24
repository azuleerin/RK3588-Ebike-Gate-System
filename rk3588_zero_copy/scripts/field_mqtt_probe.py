#!/usr/bin/env python3
"""
field_mqtt_probe.py —— 现场预检用的极简 MQTT 3.1.1 探针

为什么自己写而不用 mosquitto_pub:
    板上没有 mosquitto 客户端(paho-mqtt 也没装), 而现场预检要能在
    【不装任何东西、不需要 root】的前提下验证 "broker 到底通不通、
    发布到底能不能出去"。

★ 只用 Python 标准库(socket/struct), 没有第三方依赖。

用法:
    python3 field_mqtt_probe.py --host 192.168.8.21 --port 1883
    python3 field_mqtt_probe.py --host H --port P --qos 1 \
        --topic device/SN/message/up/preflight_test --message hello

退出码:
    0 = TCP 通 + CONNACK 接受 + (指定发布时) 发布成功
    1 = 失败(逐项打印失败原因)
    2 = 用法错
"""
import argparse
import socket
import struct
import sys
import time


# ---------------------------------------------------------------- MQTT 打包
def _encode_remaining_length(n: int) -> bytes:
    """MQTT 的"剩余长度"是变长整数(最多 4 字节)。"""
    out = bytearray()
    while True:
        byte = n % 128
        n //= 128
        if n > 0:
            byte |= 0x80
        out.append(byte)
        if n == 0:
            break
    return bytes(out)


def _encode_string(s: str) -> bytes:
    raw = s.encode("utf-8")
    return struct.pack(">H", len(raw)) + raw


def build_connect(client_id: str, keepalive: int = 10) -> bytes:
    """CONNECT 包: 协议名 MQTT / 级别 4 (3.1.1) / clean session。"""
    variable_header = (
        _encode_string("MQTT")
        + bytes([0x04])                       # Protocol Level = 4
        + bytes([0x02])                       # Connect Flags: Clean Session
        + struct.pack(">H", keepalive)
    )
    payload = _encode_string(client_id)
    body = variable_header + payload
    return bytes([0x10]) + _encode_remaining_length(len(body)) + body


def build_publish(topic: str, payload: bytes, qos: int, packet_id: int) -> bytes:
    body = _encode_string(topic)
    if qos > 0:
        body += struct.pack(">H", packet_id)
    body += payload
    header = 0x30 | (qos << 1)
    return bytes([header]) + _encode_remaining_length(len(body)) + body


def _read_exactly(sock: socket.socket, n: int, deadline: float) -> bytes:
    buf = b""
    while len(buf) < n:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"等 {n} 字节超时(已收 {len(buf)})")
        sock.settimeout(remaining)
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError(f"连接被对端关闭(已收 {len(buf)}/{n} 字节)")
        buf += chunk
    return buf


def _read_packet(sock: socket.socket, deadline: float) -> tuple:
    """读一个完整的 MQTT 包, 返回 (type, payload)。"""
    header = _read_exactly(sock, 1, deadline)[0]
    mult = 1
    length = 0
    while True:
        b = _read_exactly(sock, 1, deadline)[0]
        length += (b & 127) * mult
        mult *= 128
        if not (b & 0x80):
            break
        if mult > 128 ** 3:
            raise ValueError("剩余长度非法")
    payload = _read_exactly(sock, length, deadline) if length else b""
    return (header & 0xF0, payload)


# ---------------------------------------------------------------- 主流程
def main() -> int:
    ap = argparse.ArgumentParser(description="MQTT 3.1.1 连通性/发布探针")
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--qos", type=int, default=0, choices=[0, 1])
    ap.add_argument("--topic", default=None, help="要发布到的主题(不给就不发布)")
    ap.add_argument("--message", default="rk-gate field preflight test")
    ap.add_argument("--client-id", default="rk-field-preflight")
    ap.add_argument("--timeout", type=float, default=5.0)
    args = ap.parse_args()

    deadline = time.monotonic() + args.timeout

    # ---- ① TCP 连通 ----
    try:
        sock = socket.create_connection((args.host, args.port), timeout=args.timeout)
    except OSError as e:
        print(f"tcp_ok=0")
        print(f"tcp_error={e}")
        return 1
    print(f"tcp_ok=1")
    print(f"tcp_peer={args.host}:{args.port}")

    try:
        # ---- ② MQTT CONNECT / CONNACK ----
        sock.sendall(build_connect(args.client_id))
        ptype, payload = _read_packet(sock, deadline)
        if ptype != 0x20:
            print(f"connack=0")
            print(f"mqtt_error=期望 CONNACK, 收到类型 0x{ptype:02X}")
            return 1
        rc = payload[1] if len(payload) >= 2 else -1
        rc_names = {
            0: "连接已接受", 1: "协议版本不支持", 2: "客户端标识被拒绝",
            3: "服务不可用", 4: "用户名或密码错误", 5: "未授权",
        }
        print(f"connack=1")
        print(f"connack_rc={rc}")
        print(f"connack_msg={rc_names.get(rc, '未知返回码')}")
        if rc != 0:
            return 1

        # ---- ③ PUBLISH ----
        if args.topic:
            pid = int(time.time() * 1000) & 0xFFFF
            sock.sendall(build_publish(args.topic, args.message.encode("utf-8"),
                                       args.qos, pid))
            if args.qos > 0:
                ptype, _ = _read_packet(sock, deadline)
                if ptype != 0x40:
                    print(f"published=0")
                    print(f"mqtt_error=期望 PUBACK, 收到类型 0x{ptype:02X}")
                    return 1
            print(f"published=1")
            print(f"publish_topic={args.topic}")
            print(f"publish_qos={args.qos}")
            print(f"publish_bytes={len(args.message.encode('utf-8'))}")
    except (OSError, TimeoutError, ValueError, ConnectionError) as e:
        print(f"mqtt_error={e}")
        return 1
    finally:
        try:
            sock.close()
        except OSError:
            pass

    return 0


if __name__ == "__main__":
    sys.exit(main())
