#!/usr/bin/env python3
"""hw_load_mqtt.py —— 板端硬件负载采样器(需要 root), 每秒往 MQTT 发一包负载

为什么需要它(以及为什么它跑在【板子上】而不是 PC 上):
    网页控制台要显示 NPU / RGA / VPU 的真实负载, 而这些数字只有板子上才有:
      NPU 单核占用 : /sys/kernel/debug/rknpu/load   ← 需要 root
      RGA 占用     : /sys/kernel/debug/rkrga/load    ← 需要 root(注意是 rkrga)
      VPU(解码核)  : /proc/mpp_service/load         ← 不用 root
      DMC / GPU    : /sys/class/devfreq/*/load       ← 不用 root
    PC 端【读不到】板子的 sysfs —— 所以采集放在板子上, 通过 MQTT 发出去,
    网页端只做转发和画图。

    ⚠️ 主程序(rk_zero_copy)自己也会读一遍不用 root 的那几个接口
       (VPU/DMC/GPU/CPU), 报在 status 报文里。两边的数据会合并:
       哪个来源有值就用哪个, rknpu/rga 的真值优先。

用法(在板子上):
    sudo python3 scripts/hw_load_mqtt.py --host 192.168.31.214 --sn rk3588-01

话题:
    订阅: 不订阅(只发)
    发布: device/<sn>/message/up/load
          {"sn":..,"ts":..,"npu":..,"npu_core":..,"npu_src":"rknpu","rga":..,
           "rga_src":"debugfs","vpu":..,"cpu":..,"dmc":..,"gpu":..}
          读不到的字段是 -1(=未知), 并且 src 里注明来源。
"""
from __future__ import annotations

import argparse
import json
import socket
import struct
import sys
import time

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
    return struct.pack("!H", len(b)) + b


class MqttPub:
    """只会发 PUBLISH 的极简客户端(带自动重连)。"""

    def __init__(self, host: str, port: int, client_id: str, keepalive: int = 60):
        self.host = host
        self.port = port
        self.client_id = client_id
        self.keepalive = keepalive
        self.sock: socket.socket | None = None
        self.pid = 0

    def connect(self) -> bool:
        try:
            s = socket.create_connection((self.host, self.port), timeout=10)
            s.settimeout(5)
            var = (enc_str("MQTT") + bytes([4, 0x02])
                   + struct.pack("!H", self.keepalive) + enc_str(self.client_id))
            s.sendall(bytes([0x10]) + enc_len(len(var)) + var)
            # CONNACK: 0x20 0x02 <session> <rc>
            hdr = s.recv(1)
            if not hdr:
                return False
            rl = self._read_len(s)
            body = self._read_exact(s, rl)
            if len(body) < 2 or body[1] != 0:
                return False
            self.sock = s
            return True
        except Exception as e:  # noqa: BLE001
            print(f"[load] 连接 {self.host}:{self.port} 失败: {e}", flush=True)
            self.sock = None
            return False

    @staticmethod
    def _read_len(sock) -> int:
        mult, value = 1, 0
        while True:
            b = sock.recv(1)
            if not b:
                raise ConnectionError("连接断开")
            value += (b[0] & 0x7F) * mult
            mult *= 128
            if not (b[0] & 0x80):
                return value

    @staticmethod
    def _read_exact(sock, n: int) -> bytes:
        buf = b""
        while len(buf) < n:
            c = sock.recv(n - len(buf))
            if not c:
                raise ConnectionError("连接断开")
            buf += c
        return buf

    def publish(self, topic: str, payload: str, qos: int = 0) -> bool:
        if self.sock is None:
            return False
        try:
            var = enc_str(topic) + payload.encode("utf-8")
            self.sock.sendall(bytes([0x30 | (qos << 1)]) + enc_len(len(var)) + var)
            return True
        except Exception as e:  # noqa: BLE001
            print(f"[load] 发送失败: {e}", flush=True)
            self.sock = None
            return False

    def close(self) -> None:
        if self.sock is not None:
            try:
                self.sock.sendall(bytes([0xE0, 0x00]))
                self.sock.close()
            except Exception:  # noqa: BLE001
                pass
            self.sock = None


# ---------------------------------------------------------------- 负载采集

DEVICEFREQ_NPU = "/sys/class/devfreq/fdab0000.npu/load"
DEVICEFREQ_DMC = "/sys/class/devfreq/dmc/load"
DEVICEFREQ_GPU = "/sys/class/devfreq/fb000000.gpu-panthor/load"
RKNPU_LOAD = "/sys/kernel/debug/rknpu/load"
RGA_LOAD = "/sys/kernel/debug/rkrga/load"   # ★ 本内核是 rkrga, 不是 rga
MPP_LOAD = "/proc/mpp_service/load"


def read_text(path: str) -> str | None:
    try:
        with open(path, "r", errors="replace") as f:
            return f.read()
    except Exception:  # noqa: BLE001
        return None


def parse_devfreq(text: str) -> float:
    """'12@2400000000Hz' -> 12.0"""
    try:
        return float(text.split("@")[0].strip())
    except Exception:  # noqa: BLE001
        return -1.0


def parse_npu_cores(text: str) -> tuple[float, float, float]:
    """'Core0: 12% Core1: 5% Core2: 0%' -> (12, 5, 0)"""
    out = []
    for i in range(3):
        key = f"Core{i}:"
        pos = text.find(key)
        val = -1.0
        if pos >= 0:
            seg = text[pos + len(key):]
            num = ""
            for ch in seg:
                if ch.isdigit():
                    num += ch
                elif num:
                    break
            if num:
                val = float(num)
        out.append(val)
    return out[0], out[1], out[2]


def parse_rga(text: str) -> float:
    """RGA 的 load 行格式见过两种:
         'load: 12.50%'   (老内核)
         ' load = 3%'     (本内核的 rkrga/load, 逐行 "load = N%")
    后者不能靠 find("load:") —— 要按行找 "load" + 空白 + "=" + 数字, 取最后一个。
    """
    best = -1.0
    for line in text.splitlines():
        s = line.strip()
        if not s.startswith("load"):
            continue
        rest = s[4:].strip()
        if not rest.startswith("="):
            continue
        num = ""
        for ch in rest[1:]:
            if ch.isdigit() or ch == ".":
                num += ch
            elif num:
                break
        if num:
            try:
                best = max(best, float(num))
            except ValueError:
                pass
    if best >= 0:
        return best
    for key in ("load:", "Load:", "LOAD:"):
        pos = text.find(key)
        if pos < 0:
            continue
        seg = text[pos + len(key):]
        num = ""
        for ch in seg:
            if ch.isdigit() or ch == ".":
                num += ch
            elif num:
                break
        if num:
            try:
                return float(num)
            except ValueError:
                pass
    return -1.0


def parse_mpp(text: str) -> float:
    """取所有 rkvdec 核里最大的那个负载"""
    best = -1.0
    for line in text.splitlines():
        if "rkvdec" not in line:
            continue
        pos = line.find("load:")
        if pos < 0:
            continue
        seg = line[pos + 5:]
        num = ""
        for ch in seg:
            if ch.isdigit() or ch == ".":
                num += ch
            elif num:
                break
        if num:
            try:
                v = float(num)
                if v > best:
                    best = v
            except ValueError:
                pass
    return best


class CpuSampler:
    """/proc/stat 两次采样做差, 算 CPU 占用率"""

    def __init__(self):
        self.total = 0
        self.idle = 0
        self.primed = False

    def sample(self) -> float:
        text = read_text("/proc/stat") or ""
        for line in text.splitlines():
            if not line.startswith("cpu "):
                continue
            vals = [int(x) for x in line.split()[1:] if x.isdigit()]
            if len(vals) < 5:
                return -1.0
            total = sum(vals)
            idle = vals[3] + vals[4]
            pct = -1.0
            if self.primed and total > self.total:
                dt = total - self.total
                di = max(0, idle - self.idle)
                pct = max(0.0, min(100.0, 100.0 * (1.0 - di / dt)))
            self.total = total
            self.idle = idle
            self.primed = True
            return pct
        return -1.0


def collect(cpu: CpuSampler) -> dict:
    out = {
        "npu": -1.0, "npu_core": -1.0, "npu_src": "none",
        # ★ 分核占用: 网页上要画 Core0/Core1/Core2 三根柱子, 不能只给一个 max
        "npu_c0": -1.0, "npu_c1": -1.0, "npu_c2": -1.0,
        "rga": -1.0, "rga_src": "none",
        "vpu": -1.0, "cpu": -1.0, "dmc": -1.0, "gpu": -1.0,
    }

    # NPU: debugfs 的单核占用(最准), 读不到就退到 devfreq
    text = read_text(RKNPU_LOAD)
    if text:
        c0, c1, c2 = parse_npu_cores(text)
        if c0 >= 0 or c1 >= 0 or c2 >= 0:
            out["npu_core"] = max(c0, c1, c2)
            out["npu"] = out["npu_core"]
            out["npu_c0"], out["npu_c1"], out["npu_c2"] = c0, c1, c2
            out["npu_src"] = "rknpu"
    if out["npu"] < 0:
        text = read_text(DEVICEFREQ_NPU)
        if text:
            out["npu"] = parse_devfreq(text)
            out["npu_src"] = "devfreq" if out["npu"] >= 0 else "none"

    # RGA: 只有 debugfs 有
    text = read_text(RGA_LOAD)
    if text:
        out["rga"] = parse_rga(text)
        out["rga_src"] = "debugfs" if out["rga"] >= 0 else "none"

    # VPU(MPP 解码核)
    text = read_text(MPP_LOAD)
    if text:
        out["vpu"] = parse_mpp(text)

    out["cpu"] = cpu.sample()

    text = read_text(DEVICEFREQ_DMC)
    if text:
        out["dmc"] = parse_devfreq(text)
    text = read_text(DEVICEFREQ_GPU)
    if text:
        out["gpu"] = parse_devfreq(text)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.31.214", help="MQTT broker")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--sn", default="rk3588-01")
    ap.add_argument("--interval", type=float, default=1.0, help="采样周期(秒)")
    ap.add_argument("--qos", type=int, default=0)
    ap.add_argument("--duration", type=float, default=0, help="跑多少秒后退出(0=一直跑)")
    args = ap.parse_args()

    # 同时发到压测实例的 sn —— 网页上两个实例的负载都要有值
    topics = [f"device/{args.sn}/message/up/load",
              f"device/{args.sn}-stress/message/up/load"]
    topic = topics[0]
    pub = MqttPub(args.host, args.port, f"{args.sn}-load")
    cpu = CpuSampler()

    print(f"[load] 往 {args.host}:{args.port} 的 {topic} 发负载(周期 {args.interval}s)", flush=True)
    t_end = time.time() + args.duration if args.duration > 0 else 0
    n = 0
    while True:
        if not pub.connect():
            time.sleep(3)
            continue
        try:
            while True:
                data = collect(cpu)
                payload = json.dumps({"sn": args.sn, "ts": time.time(), **data},
                                     ensure_ascii=False)
                if not all(pub.publish(tp, payload, args.qos) for tp in topics):
                    break
                n += 1
                if n % 10 == 0:
                    print(f"[load] 已发 {n} 包  npu={data['npu']} rga={data['rga']} "
                          f"vpu={data['vpu']} cpu={data['cpu']}", flush=True)
                if t_end and time.time() > t_end:
                    pub.close()
                    return 0
                time.sleep(args.interval)
        except KeyboardInterrupt:
            pub.close()
            return 0
        except Exception as e:  # noqa: BLE001
            print(f"[load] 异常: {e}, 2 秒后重连", flush=True)
            time.sleep(2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
