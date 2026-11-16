#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""stress_agent.py —— 板端"压力测试"代理(网页控制台的压测按钮背后就是它)

为什么需要它:
    网页控制台上那个【压力测试】按钮, 要让 FPS 瞬间飙到 100+,
    好让观众当场把 MPP 关掉, 亲眼看到 FPS 暴跌、CPU 狂飙。
    但控制台可能跑在 PC 上, PC 没法直接起板端的进程。
    所以: 网页 -> MQTT 下行指令 -> 【本代理在板端】-> 起极限并发的进程。

做什么:
    订阅 device/<sn>/message/down/control, 收到
        {"cmd":"stress","on":true}  -> 起一个极限并发的 rk_zero_copy
        {"cmd":"stress","on":false} -> 把它停掉
    压力进程用【同一个 sn】, 于是它的状态报文走同一个 up/status 主题,
    网页上的曲线会立刻变成它的; 它也订阅同一个 control 主题,
    所以网页上关 MPP/RGA/NPU 对它是生效的 —— 正是演示要的。

压力进程怎么跑到 100+ fps:
    --sync-src 0   不按源帧率限速, 流水线全速跑
    --npu-pipe 3   检测用三个 NPU 核一起跑(实测 1080p 三实例 89.9 交付 FPS)

用法(在板子上):
    python3 scripts/stress_agent.py --host 192.168.8.21 --sn rk3588-01
    # 或 sudo python3 ... (要 root 才能读 rknpu/load, 但本代理本身不需要)
"""
from __future__ import annotations

import argparse
import json
import os
import signal
import socket
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)

# ---------------------------------------------------------------- MQTT 最小实现


def enc_len(n: int) -> bytes:
    out = b""
    while True:
        b = n % 128
        n //= 128
        out += bytes([b | 0x80]) if n else bytes([b])
        if not n:
            break
    return out


def enc_str(s: str) -> bytes:
    b = s.encode("utf-8")
    return struct.pack(">H", len(b)) + b


class Mqtt:
    """最小 MQTT 3.1.1 客户端。

    ★ 必须用【带缓冲】的读法: 直接在 socket 上 recv 到一半超时,
      会把 TCP 流"读断", 下一次从半截包里接着读 -> 后面全乱,
      表现出来就是 agent 莫名其妙退出、压测进程被一起带走(退出钩子会 stop 它)。
      所以所有读都先进 self.buf, 跨调用保留。
    """

    def __init__(self, host, port, cid):
        self.host, self.port, self.cid = host, port, cid
        self.sock = None
        self.buf = b""

    def connect(self):
        self.sock = socket.create_connection((self.host, self.port), timeout=10)
        self.sock.settimeout(2.0)
        self.buf = b""
        body = enc_str("MQTT") + bytes([4, 0x02]) + struct.pack(">H", 30) + enc_str(self.cid)
        self.sock.sendall(bytes([0x10]) + enc_len(len(body)) + body)
        time.sleep(0.4)
        self._read_exact(4)          # CONNACK

    def subscribe(self, topic, qos=0):
        body = struct.pack(">H", 1) + enc_str(topic) + bytes([qos])
        self.sock.sendall(bytes([0x82]) + enc_len(len(body)) + body)

    def _read_exact(self, n, timeout=2.0):
        """读满 n 字节(先进缓冲区, 跨调用保留)"""
        deadline = time.time() + timeout
        while len(self.buf) < n:
            left = deadline - time.time()
            if left <= 0:
                raise socket.timeout("read timeout")
            self.sock.settimeout(left)
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("broker 关闭了连接")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv_msg(self, timeout=2.0):
        """收一条 PUBLISH, 返回 (topic, payload); 超时返回 None"""
        deadline = time.time() + timeout
        try:
            b0 = self._read_exact(1, timeout=max(0.2, deadline - time.time()))
            if (b0[0] & 0xF0) != 0x30:          # 不是 PUBLISH(比如 PINGRESP)
                return None
            mult, val = 1, 0
            for _ in range(4):
                b = self._read_exact(1, timeout=max(0.2, deadline - time.time()))
                val += (b[0] & 0x7F) * mult
                mult *= 128
                if not (b[0] & 0x80):
                    break
            body = self._read_exact(val, timeout=max(0.2, deadline - time.time()))
            tlen = struct.unpack(">H", body[:2])[0]
            topic = body[2:2 + tlen].decode("utf-8", "replace")
            off = 2 + tlen
            if (b0[0] & 0x06) >> 1:
                off += 2
            return topic, body[off:].decode("utf-8", "replace")
        except socket.timeout:
            return None


# ---------------------------------------------------------------- 压测进程

class Stress:
    def __init__(self, binary, model, labels, video, sn, extra, mqtt_host="", mqtt_port=1883,
                 ppocr="", plate_cls=1, ppocr_ttl=5000):
        self.binary, self.model, self.labels = binary, model, labels
        self.video, self.sn, self.extra = video, sn, extra
        self.mqtt_host, self.mqtt_port = mqtt_host, mqtt_port
        self.ppocr, self.plate_cls, self.ppocr_ttl = ppocr, plate_cls, ppocr_ttl
        self.proc = None

    def start(self):
        if self.proc and self.proc.poll() is None:
            print("[stress] 已经在跑, 不重复起", flush=True)
            return
        cmd = [self.binary, self.video, self.model,
               "--decoder", "mpp", "--no-show", "--labels", self.labels,
               "--sync-src", "0", "--npu-pipe", "3",
               # ★ 必须 --loop-src: 不限速跑的话, 一个 3 万帧的文件几秒钟就跑完了,
               #   进程直接退出 -> 网页上的 FPS 曲线掉回生产实例, 演示就断了。
               "--loop-src",
               "--sn", self.sn, "--stats-interval", "50",
               "--no-speed"]
        # ★★★ 满负荷业务: 检测 + 车牌识别必须一直开着 ★★★
        #   之前的压测实例【没开 PP-OCR】, 于是"关掉 MPP"对比的是"只跑检测"，
        #   跟真实业务(检测+识别)不是一回事, 结论会被质疑。
        #   现在压测实例和生产实例一样: 检测 + PP-OCR 识别全开,
        #   A/B/C 三组只允许"单独关掉某一个硬件单元"。
        if self.ppocr:
            cmd += ["--ppocr", self.ppocr,
                    "--plate-cls", str(self.plate_cls),
                    "--ppocr-ttl", str(self.ppocr_ttl)]
        # ★ 必须给 --mqtt-host: 主程序只有开了 MQTT 才会每秒发状态报文,
        #   网页上的 FPS/负载曲线就是从那个主题来的。
        if self.mqtt_host:
            cmd += ["--mqtt-host", self.mqtt_host, "--mqtt-port", str(self.mqtt_port)]
        cmd += self.extra
        print("[stress] 起:", " ".join(cmd), flush=True)
        #  child's output -> file, 方便看它是不是起来就退了
        log = open("/tmp/console/stress_child.log", "ab", buffering=0)
        self.proc = subprocess.Popen(cmd, stdout=log, stderr=log,
                                     stdin=subprocess.DEVNULL)

    def stop(self):
        if not self.proc:
            return
        if self.proc.poll() is None:
            print("[stress] 停掉压测进程", flush=True)
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.8.21")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--sn", default="rk3588-01")
    ap.add_argument("--binary", default=f"{PROJ}/build/rk_zero_copy")
    ap.add_argument("--model", default=f"{PROJ}/model/ebike_int8_rk2332.rknn")
    ap.add_argument("--labels", default=f"{PROJ}/model/labels_ebike.txt")
    ap.add_argument("--video", default="/home/seeed/testvideo2.mp4")
    ap.add_argument("--ppocr", default=f"{PROJ}/model/ppocrv4_rec_fp16.rknn",
                    help="车牌识别模型(留空=压测不跑识别, 但那样对比就不公平了)")
    ap.add_argument("--plate-cls", type=int, default=1)
    ap.add_argument("--ppocr-ttl", type=int, default=5000)
    ap.add_argument("--frames", type=int, default=0, help="0 = 一直跑")
    ap.add_argument("--extra", default="", help="额外的 rk_zero_copy 参数(空格分隔)")
    args = ap.parse_args()

    # ★ 用【独立的 sn】: 压测实例的状态/负载跟生产实例分开上报,
    #   网页上就不会被生产实例的 25fps 报文顶掉(演示效果会打折)。
    stress_sn = f"{args.sn}-stress"
    topic = f"device/{stress_sn}/message/down/control"
    st = Stress(args.binary, args.model, args.labels, args.video, stress_sn,
                args.extra.split() if args.extra else [],
                mqtt_host=args.host, mqtt_port=args.port,
                ppocr=args.ppocr, plate_cls=args.plate_cls, ppocr_ttl=args.ppocr_ttl)

    def on_exit(*_):
        st.stop()
        sys.exit(0)

    signal.signal(signal.SIGINT, on_exit)
    signal.signal(signal.SIGTERM, on_exit)

    print(f"[stress] 订阅 {args.host}:{args.port} {topic}", flush=True)
    mq = Mqtt(args.host, args.port, f"stress-agent-{args.sn}")
    while True:
        try:
            mq.connect()
        except OSError as e:
            print(f"[stress] 连不上 broker: {e}, 3 秒后重试", flush=True)
            time.sleep(3)
            continue
        mq.subscribe(topic)
        print("[stress] 就绪, 等网页的压测指令", flush=True)
        while True:
            msg = mq.recv_msg(timeout=2.0)
            if not msg:
                continue
            _, payload = msg
            try:
                j = json.loads(payload)
            except Exception:                      # noqa: BLE001
                continue
            cmd = j.get("cmd", "")
            if cmd != "stress":
                continue
            on = bool(j.get("on", True))
            if on:
                st.start()
            else:
                st.stop()


if __name__ == "__main__":
    main()
