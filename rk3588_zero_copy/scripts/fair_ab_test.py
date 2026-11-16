#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""fair_ab_test.py —— 公平 A/B/C 对比压测(检测+识别满负荷下, 只关一个硬件单元)

★ 为什么要有这个脚本(针对"对比逻辑不公平"的质疑)
  之前测"关掉 MPP / RGA"的时候, 压测实例【没开车牌识别】, 于是对比的是
  "只跑检测"的场景 —— 跟真实业务(检测 + 识别)不是一回事, 结论会被质疑。
  另外有一组是"关掉 NPU 只跑解码", 那个组没有业务含义, 绝不能进对比。

  本脚本的硬约束:
    ① 同一个进程、同一份视频、同一套参数, 只用【运行时开关】改一个硬件单元;
    ② 三组都保持【检测 + PP-OCR 车牌识别】全开(满负荷业务);
    ③ 绝不用"关掉 NPU"的组参与对比;
    ④ 每组先等【稳态】(切换后前几秒还在追帧/重排解码器), 再采样;
    ⑤ 开关走的是和网页完全一样的链路(MQTT 下行 → 板端原子开关 → 下一帧生效),
       所以这组数据同时也是"开关真的在干预底层硬件"的证据;
    ⑥ 每组都记下板端【自己上报的】engine 状态, 证明开关真的生效了。

★ 三组
    A 全硬件加速 : MPP 硬解 + RGA + NPU(检测+识别) 全开   ← 基线
    B 关硬解     : 只关 MPP(切 FFmpeg 软解), RGA + NPU 保持开
    C 关 RGA     : 只关 RGA 预处理(切 CPU), MPP + NPU 保持开

用法(在板子上):
    python3 scripts/fair_ab_test.py --host 192.168.8.21 --sn rk3588-01
    python3 scripts/fair_ab_test.py --groups A,B --settle 10 --sample 20
输出:
    /tmp/fair_ab/result.json    原始数据(画图脚本吃这个)
    终端直接打一张对比表
"""
from __future__ import annotations

import argparse
import json
import os
import re
import socket
import statistics
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)

# ---------------------------------------------------------------- MQTT 最小实现
# (照抄 stress_agent.py / web_console.py 里已经跑通的写法 —— MQTT 的包长
#  处理很容易写错, 这里不重新发明)


def enc_len(n: int) -> bytes:
    out = b""
    while True:
        b = n % 128
        n //= 128
        out += bytes([b | 0x80]) if n else bytes([b])
        if not n:
            return out


def enc_str(s: str) -> bytes:
    b = s.encode("utf-8")
    return struct.pack("!H", len(b)) + b


class Mqtt:
    """最小 MQTT 3.1.1 客户端(订阅 + 发布)。

    ★ 为什么这里必须自己按【包】读, 不能"读到什么算什么":
      broker 在 SUBACK 之前就可能把 PUBLISH 甩过来(订阅一成立就开始推),
      如果按"读 4 字节就是 CONNACK、读 1+1+n 就是 SUBACK"的固定套路走,
      就会把 PUBLISH 的字节当成 SUBACK 吃掉, 之后整条流错位 -> 解析出垃圾。
      所以这里统一按包读(type + remaining length + payload),
      PUBLISH 先进队列, SUBACK/PINGRESP 就地处理。
    """

    def __init__(self, host, port, cid):
        self.host, self.port, self.cid = host, port, cid
        self.sock = None
        self.buf = b""
        self.queue: list[tuple[str, str]] = []

    def connect(self):
        self.sock = socket.create_connection((self.host, self.port), timeout=10)
        self.sock.settimeout(2.0)
        self.buf = b""
        var = enc_str("MQTT") + bytes([4, 0x02]) + struct.pack("!H", 60) + enc_str(self.cid)
        self.sock.sendall(bytes([0x10]) + enc_len(len(var)) + var)
        time.sleep(0.4)
        self._read_exact(4)                      # CONNACK(4 字节) —— 只读这一个, 别多读

    def subscribe(self, topic, qos=0):
        body = struct.pack("!H", 1) + enc_str(topic) + bytes([qos])
        self.sock.sendall(bytes([0x82]) + enc_len(len(body)) + body)
        # 等 SUBACK; 中间可能先来 PUBLISH, 塞进队列
        deadline = time.time() + 5.0
        while time.time() < deadline:
            pkt = self._read_packet(timeout=2.0)
            if pkt is None:
                continue
            ptype, body = pkt
            if ptype == 0x90:                     # SUBACK
                return
            if ptype == 0x30:                     # PUBLISH
                self.queue.append(self._decode_publish(body))

    def publish(self, topic, payload, qos=0):
        var = enc_str(topic) + payload.encode("utf-8")
        self.sock.sendall(bytes([0x30 | (qos << 1)]) + enc_len(len(var)) + var)

    # ---------------------------------------------------------------- 读包
    def _read_exact(self, n, timeout=2.0):
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

    def _read_packet(self, timeout=2.0):
        """读一个完整的 MQTT 包 -> (首字节类型, payload); 超时返回 None"""
        try:
            b0 = self._read_exact(1, timeout=timeout)[0]
            mult, val = 1, 0
            for _ in range(4):
                b = self._read_exact(1, timeout=timeout)[0]
                val += (b & 0x7F) * mult
                mult *= 128
                if not (b & 0x80):
                    break
            body = self._read_exact(val, timeout=timeout) if val else b""
            return b0, body
        except socket.timeout:
            return None

    @staticmethod
    def _decode_publish(body: bytes, qos: int = 0) -> tuple[str, str]:
        tlen = struct.unpack("!H", body[:2])[0]
        topic = body[2:2 + tlen].decode("utf-8", "replace")
        off = 2 + tlen
        # QoS>0 的 PUBLISH 中间有 2 字节 packet id(主程序用 QoS 1 上报!)
        if qos > 0:
            off += 2
        payload = body[off:] if off <= len(body) else b""
        return topic, payload.decode("utf-8", "replace")

    def recv_msg(self, timeout=2.0):
        if self.queue:
            return self.queue.pop(0)
        pkt = self._read_packet(timeout=timeout)
        if pkt is None:
            return None
        ptype, body = pkt
        if ptype != 0x30:
            return None
        if len(body) < 2:
            return None
        qos = (ptype >> 1) & 0x03
        return self._decode_publish(body, qos)


# ---------------------------------------------------------------- 压测实例

def find_agent() -> int | None:
    out = subprocess.run(["pgrep", "-f", "stress_agent.py"],
                         capture_output=True, text=True).stdout.split()
    return int(out[0]) if out else None


def start_agent(host: str, port: int, sn: str, video: str, ppocr: str) -> int:
    cmd = [sys.executable, os.path.join(HERE, "stress_agent.py"),
           "--host", host, "--port", str(port), "--sn", sn,
           "--video", video, "--ppocr", ppocr]
    print("[fair] 起压测代理:", " ".join(cmd), flush=True)
    os.makedirs("/tmp/console", exist_ok=True)
    log = open("/tmp/console/stress_agent.log", "ab", buffering=0)
    subprocess.Popen(cmd, stdout=log, stderr=log, stdin=subprocess.DEVNULL,
                     start_new_session=True)
    for _ in range(30):
        time.sleep(0.5)
        if find_agent():
            return find_agent()
    raise RuntimeError("压测代理没起来")


def find_stress_pid(sn: str) -> int | None:
    """找压测实例的进程(它的 --sn 是 <base>-stress, 这里传【完整】的 sn)"""
    want = f"--sn {sn}"
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as f:
                cmd = f.read().replace(b"\0", b" ").decode("utf-8", "replace")
        except OSError:
            continue
        if "rk_zero_copy" in cmd and want in cmd:
            return int(pid)
    return None


def proc_cpu_ticks(pid: int) -> int | None:
    try:
        with open(f"/proc/{pid}/stat") as f:
            data = f.read()
    except OSError:
        return None
    # 字段 14/15 = utime/stime; comm 里可能有空格, 要从最后一个 ')' 之后切
    tail = data[data.rfind(")") + 2:].split()
    if len(tail) < 13:
        return None
    return int(tail[11]) + int(tail[12])


def clk_tck() -> int:
    try:
        return os.sysconf("SC_CLK_TCK")
    except (ValueError, OSError):
        return 100


def vpu_load() -> float:
    """VPU(MPP 解码核)占用 —— 不用 root, /proc/mpp_service/load 是全局可读的"""
    try:
        with open("/proc/mpp_service/load") as f:
            for line in f:
                if "rkvdec" in line:
                    m = re.search(r"load:\s*([0-9.]+)%", line)
                    if m:
                        return float(m.group(1))
    except OSError:
        pass
    return -1.0


# ---------------------------------------------------------------- 采样

GROUPS = {
    "A": {"mpp": True,  "rga": True,  "npu": True,
          "name": "A 全硬件加速",
          "desc": "MPP 硬解 + RGA + NPU(检测+识别) 全开"},
    "B": {"mpp": False, "rga": True,  "npu": True,
          "name": "B 关硬解",
          "desc": "只关 MPP(FFmpeg 软解), RGA + NPU 保持开"},
    "C": {"mpp": True,  "rga": False, "npu": True,
          "name": "C 关 RGA",
          "desc": "只关 RGA 预处理(CPU), MPP + NPU 保持开"},
}


def median(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None


def run_group(mq: Mqtt, ctl_topic: str, status_topic: str, key: str,
              settle: float, sample: float, stress_sn: str, pid_hint=None) -> dict:
    g = GROUPS[key]
    print(f"\n===== {g['name']} ({g['desc']}) =====", flush=True)

    # ---- 下发开关, 并且【等板端确认】----
    # ★ 必须确认: 指令是走 MQTT 下行 -> 板端原子开关 -> 下一帧生效,
    #   如果连接断了/订阅没了, 指令会【静默丢掉】 —— 那样测出来的三组数据
    #   全是同一组, 看起来"开关没影响", 其实是指令根本没到。
    want = {"mpp": g["mpp"], "rga": g["rga"], "npu": g["npu"]}
    payload = json.dumps({"cmd": "set_engine", **want})
    confirmed = False
    for attempt in range(1, 4):
        mq.publish(ctl_topic, payload)
        print(f"[fair] 已下发(第 {attempt} 次): {payload}", flush=True)
        deadline = time.time() + 8.0
        while time.time() < deadline:
            msg = mq.recv_msg(timeout=1.0)
            if not msg or msg[0] != status_topic:
                continue
            try:
                st = json.loads(msg[1])
            except Exception:      # noqa: BLE001
                continue
            eng = st.get("engine") or {}
            if all(eng.get(k) == want[k] for k in ("mpp", "rga", "npu")):
                confirmed = True
                break
        if confirmed:
            break
        print(f"[fair] 板端没确认(第 {attempt} 次), 重发", flush=True)
    if not confirmed:
        print(f"[fair] ★ 警告: {key} 组开关没被板端确认, 数据可能不可信", flush=True)

    # ---- 稳态等待(切换后前几秒还在追帧/重排解码器) ----
    t0 = time.time()
    while time.time() - t0 < settle:
        mq.recv_msg(timeout=0.5)
    print(f"[fair] 稳态等待 {settle:.0f}s 完成, 开始采样 {sample:.0f}s", flush=True)

    samples = []
    pid = pid_hint or find_stress_pid(stress_sn)
    ticks0 = proc_cpu_ticks(pid) if pid else None
    wall0 = time.time()
    while time.time() - wall0 < sample:
        msg = mq.recv_msg(timeout=1.0)
        if not msg:
            continue
        topic, body = msg
        if topic != status_topic:
            continue
        try:
            obj = json.loads(body)
        except Exception:      # noqa: BLE001
            continue
        samples.append(obj)
    wall = time.time() - wall0
    ticks1 = proc_cpu_ticks(pid) if pid else None
    cpu_pct = None
    if ticks0 is not None and ticks1 is not None:
        cpu_pct = 100.0 * (ticks1 - ticks0) / (wall * clk_tck())

    def med(field, default=None):
        v = median([s.get(field) for s in samples])
        return default if v is None else v

    engines = [s.get("engine") or {} for s in samples]
    eng_seen = {}
    for e in engines:
        for k in ("mpp", "rga", "npu"):
            if k in e:
                eng_seen[k] = e[k]

    res = {
        "group": key,
        "name": g["name"],
        "desc": g["desc"],
        "request": {"mpp": g["mpp"], "rga": g["rga"], "npu": g["npu"]},
        "engine_reported": eng_seen,        # 板端自己上报的开关状态(证明真生效了)
        "n_samples": len(samples),
        "fps": med("fps"),
        "dec_ms": med("dec_ms"),
        "rga_ms": med("rga_ms"),
        "npu_ms": med("npu_ms"),
        "post_ms": med("post_ms"),
        "total_ms": med("total_ms"),
        "wait_ms": med("wait_ms"),
        "n_objs": med("n_objs"),
        "decoder": samples[-1].get("decoder") if samples else None,
        "rga_mode": samples[-1].get("rga_mode") if samples else None,
        "vpu_pct": vpu_load(),
        "cpu_pct": cpu_pct,                 # 压测实例自己的 CPU 占用
        "raw": samples,
    }
    print(f"[fair] FPS={res['fps']}  解码={res['dec_ms']}ms  预处理={res['rga_ms']}ms  "
          f"NPU={res['npu_ms']}ms  总={res['total_ms']}ms  "
          f"CPU={cpu_pct if cpu_pct is None else round(cpu_pct,1)}%  VPU={res['vpu_pct']}%  "
          f"后端={res['decoder']}/{res['rga_mode']}  板端上报引擎={eng_seen}", flush=True)
    return res


# ---------------------------------------------------------------- 命令行对照跑法

# ★ 三个组的命令行: 【只有一处不同】
CLI_GROUPS = {
    "A": ["--decoder", "mpp"],
    "B": ["--decoder", "ffmpeg"],
    "C": ["--decoder", "mpp", "--rga", "0"],
}
CLI_DESC = {
    "A": "MPP 硬解 + RGA + NPU(检测+识别) 全开",
    "B": "只关 MPP(FFmpeg 软解), RGA + NPU 保持开",
    "C": "只关 RGA 预处理(CPU), MPP + NPU 保持开",
}


def parse_frame_lines(text: str) -> list[dict]:
    """从 [frame N] 行里取出各项指标"""
    out = []
    rx = re.compile(r"\[frame (\d+)\] fps=([\d.]+) deliv=([\d.]+) src=([\d.]+) gap=([\d.]+)ms "
                    r"wait=([\d.]+)ms dec=([\d.]+)ms rga=([\d.]+)ms npu=([\d.]+)ms"
                    r"\(run([\d.]+)/out([\d.]+)\) post=([\d.]+)ms draw=([\d.]+)ms total=([\d.]+)ms")
    for m in rx.finditer(text):
        out.append({
            "frame": int(m.group(1)), "fps": float(m.group(2)), "deliv": float(m.group(3)),
            "src": float(m.group(4)), "gap": float(m.group(5)), "wait": float(m.group(6)),
            "dec": float(m.group(7)), "rga": float(m.group(8)), "npu": float(m.group(9)),
            "run": float(m.group(10)), "out": float(m.group(11)),
            "post": float(m.group(12)), "draw": float(m.group(13)), "total": float(m.group(14)),
        })
    return out


def run_cli_group(key: str, args) -> dict:
    """跑一组: 同一条命令, 只改一个参数"""
    extra = CLI_GROUPS[key]
    cmd = [args.binary, args.video, args.model,
           "--labels", args.labels,
           "--no-show", "--no-speed",
           "--frames", str(args.frames),
           "--stats-interval", str(max(1, args.frames // 4)),
           "--conf", "0.15", "--nms", "0.45",
           "--ppocr", args.ppocr, "--plate-cls", "1",
           "--ppocr-ttl", str(args.ppocr_ttl)]
    if args.pipe:
        cmd += ["--npu-pipe", str(args.pipe)]
    if not args.paced:
        cmd += ["--no-sync-src"]
    cmd += extra
    print(f"\n===== {key} {CLI_DESC[key]} =====", flush=True)
    print("[fair] " + " ".join(cmd), flush=True)

    t0 = time.time()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    out = proc.stdout.read()
    # ★ 用 os.wait4 拿这个子进程的 rusage:
    #   进程一退出 /proc/<pid>/stat 就没了, 事后去读只能读到 0。
    _, _, rusage = os.wait4(proc.pid, 0)
    wall = time.time() - t0
    cpu_s = rusage.ru_utime + rusage.ru_stime
    cpu_pct = 100.0 * cpu_s / wall if wall > 0 else 0.0

    frames = parse_frame_lines(out)
    # ★ 只取【后一半】的行: 前半段是预热(模型加载/MPP 初始化/第一批帧)
    tail = frames[len(frames) // 2:] if frames else []
    if not tail:
        tail = frames

    def med(f):
        v = median([x[f] for x in tail])
        return v if v is not None else 0.0

    res = {
        "group": key,
        "name": f"{key} {CLI_DESC[key]}",
        "desc": CLI_DESC[key],
        "cmd": " ".join(cmd),
        "n_frames_parsed": len(frames),
        "frames": args.frames,
        "fps": med("deliv"),
        "dec_ms": med("dec"),
        "rga_ms": med("rga"),
        "npu_ms": med("npu"),
        "run_ms": med("run"),
        "post_ms": med("post"),
        "total_ms": med("total"),
        "cpu_pct": cpu_pct,
        "wall_s": wall,
        "raw": tail,
    }
    print(f"[fair] FPS={res['fps']:.2f} 解码={res['dec_ms']:.2f}ms 预处理={res['rga_ms']:.2f}ms "
          f"NPU={res['npu_ms']:.2f}ms 总={res['total_ms']:.2f}ms CPU={cpu_pct:.1f}%", flush=True)
    return res


def print_summary(results: list[dict], note: str = "") -> None:
    print("\n" + "=" * 104)
    print(" 公平 A/B/C 对比 —— 满负荷业务(检测 + PP-OCR 车牌识别全开), 只单独关闭某一个硬件单元")
    if note:
        print(f" {note}")
    print("=" * 104)
    print(f"{'组':<4}{'配置':<28}{'FPS':>8}{'解码ms':>9}{'预处理ms':>10}{'NPUms':>8}"
          f"{'总ms':>8}{'CPU%':>8}")
    print("-" * 104)
    for r in results:
        cpu = "-" if r.get("cpu_pct") is None else f"{r['cpu_pct']:.1f}"
        print(f"{r['group']:<4}{r['desc'][:26]:<28}{r['fps']:>8.2f}{r['dec_ms']:>9.2f}"
              f"{r['rga_ms']:>10.2f}{r['npu_ms']:>8.2f}{r['total_ms']:>8.2f}{cpu:>8}")
    print("-" * 104)
    base = next((r for r in results if r["group"] == "A" and r.get("fps")), None)
    if base:
        for r in results:
            if r["group"] == "A" or not r.get("fps"):
                continue
            cpu_ratio = ""
            if base.get("cpu_pct") and r.get("cpu_pct"):
                cpu_ratio = f" | CPU {r['cpu_pct']/base['cpu_pct']:.1f}×"
            print(f"  {r['group']} vs A: FPS {r['fps']/base['fps']*100:.1f}% "
                  f"({r['fps']-base['fps']:+.2f}) | 解码 {r['dec_ms']/max(base['dec_ms'],1e-9):.1f}× | "
                  f"预处理 {r['rga_ms']/max(base['rga_ms'],1e-9):.1f}× | "
                  f"总耗时 {r['total_ms']/base['total_ms']:.1f}×{cpu_ratio}")
    print()


def run_cli_mode(args) -> int:
    """命令行对照跑法: 三组各跑一个独立进程, 参数只差一个, 帧数相同(内容完全一致)"""
    print("=" * 104)
    print(" 公平 A/B/C 对比 —— 命令行对照跑法")
    print(f" 视频={os.path.basename(args.video)}  每组帧数={args.frames}  "
          f"npu-pipe={args.pipe or 1}  检测+PP-OCR 识别全程开启")
    print("=" * 104)
    results = []
    for key in [k.strip().upper() for k in args.groups.split(",") if k.strip()]:
        if key not in CLI_GROUPS:
            print(f"[fair] 未知组 {key}, 跳过", flush=True)
            continue
        results.append(run_cli_group(key, args))
        time.sleep(2)          # 两组之间让板子喘口气(频率/缓存都稳一稳)

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as f:
        json.dump({"ts": time.time(), "mode": "cli", "frames": args.frames,
                   "ppocr_ttl": args.ppocr_ttl, "pipe": args.pipe,
                   "video": os.path.basename(args.video), "results": results}, f,
                  ensure_ascii=False, indent=1)
    print_summary(results, "同一条命令只差一个参数; 三组跑的是【同样的帧】(同一段画面), 逐项可比")
    print(f"原始数据: {args.out}")
    return 0


# ---------------------------------------------------------------- 主流程

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", default="cli", choices=["cli", "live"],
                    help="cli=三组各跑一个独立进程(内容/帧数完全一致, 最严格); "
                         "live=走 MQTT 运行时开关(网页演示用的那条链路)")
    ap.add_argument("--host", default="192.168.8.21", help="MQTT broker(live 模式用)")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--sn", default="rk3588-01")
    ap.add_argument("--groups", default="A,B,C")
    ap.add_argument("--settle", type=float, default=10.0, help="每组稳态等待(秒, live 模式)")
    ap.add_argument("--sample", type=float, default=20.0, help="每组采样时长(秒, live 模式)")
    ap.add_argument("--frames", type=int, default=600, help="每组跑多少帧(cli 模式)")
    ap.add_argument("--pipe", type=int, default=1, help="--npu-pipe(cli 模式; 0=不并行)")
    ap.add_argument("--paced", action="store_true", help="按源帧率播放(默认不限速, 压满算力)")
    ap.add_argument("--ppocr-ttl", type=int, default=5000,
                    help="车牌识别缓存 TTL(ms): 5000=生产配置; 0=每帧都识别(最重负载)")
    ap.add_argument("--video", default="/home/seeed/testvideo3.mp4")
    ap.add_argument("--ppocr", default=f"{PROJ}/model/ppocrv4_rec_fp16.rknn")
    ap.add_argument("--model", default=f"{PROJ}/model/ebike_int8_rk2332.rknn")
    ap.add_argument("--labels", default=f"{PROJ}/model/labels_ebike.txt")
    ap.add_argument("--binary", default=f"{PROJ}/build/rk_zero_copy")
    ap.add_argument("--out", default="/tmp/fair_ab/result.json")
    ap.add_argument("--keep", action="store_true", help="跑完不停压测实例(live 模式)")
    args = ap.parse_args()

    if args.mode == "cli":
        return run_cli_mode(args)

    stress_sn = f"{args.sn}-stress"
    ctl_topic = f"device/{stress_sn}/message/down/control"
    status_topic = f"device/{stress_sn}/message/up/status"

    # 压测代理必须在, 否则没人接 stress 指令
    if not find_agent():
        start_agent(args.host, args.port, args.sn, args.video, args.ppocr)
    else:
        print(f"[fair] 压测代理已在跑 (PID {find_agent()})", flush=True)

    mq = Mqtt(args.host, args.port, f"fair-ab-{os.getpid()}-{int(time.time()) % 100000}")
    mq.connect()
    mq.subscribe(status_topic)
    print(f"[fair] 订阅 {status_topic}", flush=True)

    # 先停再起: 保证压测实例的控制通道是【全新的】。
    #   (老实例的控制订阅可能早就失效了 —— broker 重启/客户端顶号都会,
    #    而失效是静默的: 指令发出去没人收, 三组数据看起来"开关没影响"。)
    # ★ 必须等老进程【真的退出】再起新的: 代理是异步收指令的, 停+起间隔太短
    #   会让两个实例同时跑, 抢同一批 NPU 核 -> 三组数据一起被腰斩(踩过)。
    mq.publish(ctl_topic, json.dumps({"cmd": "stress", "on": False}))
    for _ in range(40):
        if not find_stress_pid(stress_sn):
            break
        time.sleep(0.5)
    print(f"[fair] 老实例已退出, 准备起新的", flush=True)
    mq.publish(ctl_topic, json.dumps({"cmd": "stress", "on": True}))
    print("[fair] 已请求启动压测实例, 等它出状态...", flush=True)

    pid = None
    t0 = time.time()
    while time.time() - t0 < 90:
        msg = mq.recv_msg(timeout=1.0)
        if msg and msg[0] == status_topic:
            try:
                st = json.loads(msg[1])
            except Exception:      # noqa: BLE001
                continue
            if float(st.get("fps") or 0) > 1.0:     # ★ 要真的跑起来了(预热完成)
                pid = find_stress_pid(stress_sn)
                if pid:
                    print(f"[fair] 压测实例已就绪 (PID {pid}, fps={st.get('fps')})", flush=True)
                    break
    if not pid:
        print("[fair] 压测实例没起来, 放弃", flush=True)
        return 2

    results = []
    for key in [k.strip().upper() for k in args.groups.split(",") if k.strip()]:
        if key not in GROUPS:
            print(f"[fair] 未知组 {key}, 跳过", flush=True)
            continue
        results.append(run_group(mq, ctl_topic, status_topic, key,
                                 args.settle, args.sample, stress_sn, pid_hint=pid))

    if not args.keep:
        mq.publish(ctl_topic, json.dumps({"cmd": "stress", "on": False}))
        print("\n[fair] 已停掉压测实例", flush=True)

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as f:
        json.dump({"ts": time.time(), "sn": args.sn, "results": results}, f,
                  ensure_ascii=False, indent=1)

    print_summary(results, "live 模式: 走网页同一条 MQTT 链路, 运行时切换开关(板端已逐组确认生效)")
    print(f"原始数据: {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
