#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""rtsp_fanout_server.py —— 真实相机的【1 拉 N 播】本地零转码中继(服务端)

★ 为什么需要它(2026-11-26 Phase 1 实测结论, 全部有实测出处):
    rk_gate 的 4 路通道【直连远端相机】时, 4 路里只有最先启动的那一路能持续
    出帧, 其余路的帧号永不推进 -> 看门狗 L2 判定卡死 -> 每 ~90 秒重启一次服务。

    逐项排除(全是实测, 不是推测):
      · 相机 RTSP 协议层无罪: 原生协议并发 4 路, 接入间隔 0/0.3/1.0s 三档,
        4 路收字节增量完全一致, 30 秒持续不断;
      · 相机不会踢旧连接: A 连 8 秒后接 B, A/B 仍同时稳定收流;
      · libav 无罪: 4 路并发 field_probe 抓帧全部成功;
      · 4xMPP 无罪: 4 路连【本地】RTSP 服务器时 4 路全部满速 deliv=20.0;
      · MP4 源无罪: 4 路 MP4 走同一二进制 4/4 正常。
    => 只有"FFmpeg RTSP 客户端直连远端相机"这条路有问题。

★ 真正的机理(实测): 【SPS/PPS 拿不到】。
    这台相机(LIVE555)的 SDP 里【没有 sprop-parameter-sets】, SPS/PPS 只在
    关键帧里带。客户端(FFmpeg)接入时如果错过关键帧, 就一路报
      "non-existing PPS 0 referenced" / "no frame!" —— 帧号永远不推进。
    本地 replay 服务器之所以 4 路全通, 是因为它每个访问单元都带 SPS/PPS。

★ 本中继怎么解决(两招一起上):
    1) 在【每个 IDR 前面补插 SPS/PPS】—— 任何时刻接入的客户端, 只要等到
       下一个关键帧就能立刻解码, 不会因为错过带内参数集而永久卡住;
    2) 把抓到的 SPS/PPS 以 base64 写进本地 SDP 的 sprop-parameter-sets ——
       客户端一连上就知道参数集, 连等关键帧都不必。

★ 零转码: H.264 【不解码、不重编码】, 码流 payload 一个字节都不改。
    只在 RTP 层做两件事: 补插 SPS/PPS、重写 seq/ts/ssrc。

用法:
    python3 scripts/rtsp_fanout_server.py --port 8555 --path /live \\
        --source-file /home/seeed/.config/rk-gate/real_rtsp.url
"""
from __future__ import annotations

import argparse
import base64
import collections
import os
import re
import socket
import struct
import sys
import threading
import time

RTP_HDR = 12
DEFAULT_PORT = 8555
DEFAULT_PATH = "/live"
QUEUE_MAX = 1200          # 队列里最多留多少个 RTP 包(约 20 秒 @25fps)
MTU = 1400

NAL_SPS, NAL_PPS, NAL_IDR = 7, 8, 5


# ---------------------------------------------------------------------------
# 基础工具
# ---------------------------------------------------------------------------
def interleave(payload: bytes, channel: int = 0) -> bytes:
    """RTSP over TCP(interleaved) 帧头: '$' + 通道 + 2 字节长度(大端)。"""
    return b"$" + bytes([channel]) + struct.pack(">H", len(payload)) + payload


def split_url(url: str):
    """rtsp://[user:pass@]host[:port][/path] -> (cred, host, port, path)

    ★ 必须先剥掉 userinfo: 带账号密码的地址里 host 段是
      "用户名:密码@主机地址", 不剥的话 host/port 解析全错
      (踩过: 正则 group() 返回 None 直接抛异常)。
      ★★ 注释里【不许】写真实凭据 —— 仓库是 Git 跟踪的, 写了就等于泄露。
    """
    cred = ""
    m = re.match(r"rtsp://([^@/]*)@(.*)$", url)
    if m:
        cred = m.group(1)
        url = "rtsp://" + m.group(2)      # ★ 保留 scheme, 后面的正则还要用
    m = re.match(r"rtsp://([^/:]+)(?::(\d+))?(/.*)?$", url)
    if not m:
        raise ValueError("解析不了 RTSP 地址: "
                         + re.sub(r"://[^@/]*@", "://***:***@", url))
    return cred, m.group(1), int(m.group(2) or 554), (m.group(3) or "/")


def read_interleaved(sock, buf: bytearray):
    """从 sock 读出下一个 interleaved 帧。返回 (channel, payload) 或 None。"""
    while True:
        while True:
            i = buf.find(b"$")
            if i >= 0:
                break
            if len(buf) > 4096:            # 前面全是垃圾, 丢到下一个 '$'
                del buf[:len(buf) - 1]
            chunk = sock.recv(65536)
            if not chunk:
                return None
            buf += chunk
        if i > 0:
            del buf[:i]
        while len(buf) < 4:
            chunk = sock.recv(65536)
            if not chunk:
                return None
            buf += chunk
        ch = buf[1]
        ln = (buf[2] << 8) | buf[3]
        while len(buf) < 4 + ln:
            chunk = sock.recv(65536)
            if not chunk:
                return None
            buf += chunk
        payload = bytes(buf[4:4 + ln])
        del buf[:4 + ln]
        return ch, payload


def recv_rtsp_response(sock, buf: bytearray):
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(65536)
        if not chunk:
            raise IOError("RTSP 连接被关闭")
        buf += chunk
    head_b, _, rest = bytes(buf).partition(b"\r\n\r\n")
    buf.clear()
    buf += rest
    head = head_b.decode("utf-8", "replace")
    m = re.search(r"Content-Length:\s*(\d+)", head, re.I)
    need = int(m.group(1)) if m else 0
    while len(buf) < need:
        chunk = sock.recv(65536)
        if not chunk:
            break
        buf += chunk
    body = bytes(buf[:need])
    del buf[:need]
    return head, body


def rtsp_request(sock, method: str, url: str, cseq: int, extra: str = "") -> None:
    sock.sendall((f"{method} {url} RTSP/1.0\r\nCSeq: {cseq}\r\n"
                  f"User-Agent: RkFanout/1.0\r\n{extra}\r\n").encode())


def nal_type(payload: bytes) -> int:
    return payload[0] & 0x1F if payload else 0


def starts_idr(payload: bytes) -> bool:
    """这个 RTP 包是否【开始】一个 IDR NAL(单 NAL 或 FU-A 的首片)。"""
    if not payload:
        return False
    t = payload[0] & 0x1F
    if t == NAL_IDR:
        return True
    if t == 28:                                   # FU-A
        if len(payload) < 2:
            return False
        fu_hdr = payload[1]
        return bool(fu_hdr & 0x80) and (fu_hdr & 0x1F) == NAL_IDR
    return False


def make_rtp(payload: bytes, seq: int, ts: int, ssrc: int, marker: bool) -> bytes:
    b1 = 96 | (0x80 if marker else 0x00)
    return struct.pack("!BBHII", 0x80, b1, seq & 0xFFFF, ts & 0xFFFFFFFF, ssrc) \
        + payload


# ===========================================================================
# 拉流侧: 从相机拉 1 路 -> 补插 SPS/PPS -> 放进共享队列
# ===========================================================================
class CameraPuller(threading.Thread):
    def __init__(self, source: str, queue: collections.deque, qlock: threading.Lock,
                 state: dict):
        super().__init__(daemon=True)
        self.source = source
        self.q = queue
        self.lock = qlock
        self.state = state
        self.stop = threading.Event()
        self.sps = None            # 缓存的 SPS RTP payload
        self.pps = None            # 缓存的 PPS RTP payload
        self.seq = 0
        self.ssrc = 0x5EED0001

    # -- SDP 里找视频轨的 control(跳过音频轨) --
    @staticmethod
    def video_control(sdp: str) -> str:
        in_video = False
        ctrls = []
        for line in sdp.replace("\r\n", "\n").split("\n"):
            if line.startswith("m="):
                in_video = line.startswith("m=video")
                continue
            if in_video and line.startswith("a=control:"):
                ctrls.append(line.split(":", 1)[1].strip())
        if not ctrls:
            raise RuntimeError("相机 SDP 里没有视频轨的 a=control")
        return ctrls[0]

    def run(self):
        while not self.stop.is_set():
            try:
                self._session()
            except Exception as e:  # noqa: BLE001
                self.state["err"] = f"{type(e).__name__}: {e}"
                time.sleep(2.0)

    def _session(self):
        cred, host, port, path = split_url(self.source)
        auth = ""
        if cred:
            auth = ("Authorization: Basic "
                    f"{base64.b64encode(cred.encode()).decode()}\r\n")
        s = socket.create_connection((host, port), timeout=8)
        s.settimeout(10)
        buf = bytearray()
        base = f"rtsp://{host}:{port}{path}"

        rtsp_request(s, "OPTIONS", base, 1, auth)
        recv_rtsp_response(s, buf)
        rtsp_request(s, "DESCRIBE", base, 2, auth + "Accept: application/sdp\r\n")
        _head, body = recv_rtsp_response(s, buf)
        sdp = body.decode("utf-8", "replace")
        self.state["cam_sdp"] = sdp
        track = self.video_control(sdp)

        rtsp_request(s, "SETUP", f"{base}/{track}", 3,
                     auth + "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
        head, _ = recv_rtsp_response(s, buf)
        if " 200 " not in head.split("\r\n")[0]:
            raise RuntimeError("SETUP 失败: " + head.split("\r\n")[0])
        sm = re.search(r"Session:\s*([^;\r\n]+)", head)
        sid = sm.group(1).strip() if sm else ""

        rtsp_request(s, "PLAY", base, 4,
                     auth + f"Session: {sid}\r\nRange: npt=0.000-\r\n")
        head, _ = recv_rtsp_response(s, buf)
        if " 200 " not in head.split("\r\n")[0]:
            raise RuntimeError("PLAY 失败: " + head.split("\r\n")[0])

        self.state["conn"] = True
        self.state["err"] = ""
        print(f"[fanout] 已连上相机 {host}:{port}{path}  track={track}", flush=True)
        try:
            while not self.stop.is_set():
                item = read_interleaved(s, buf)
                if item is None:
                    break
                ch, payload = item
                if ch != 0 or len(payload) < RTP_HDR:
                    continue
                ts = int.from_bytes(payload[4:8], "big")
                marker = bool(payload[1] & 0x80)
                # ★ payload 是【整个 RTP 包】(12 字节头 + 载荷);
                #   NAL 分析必须只用【载荷部分】, 否则 nal_type() 读到的是
                #   RTP 头的 0x80 -> type 0, 于是 SPS/PPS 永远识别不出来。
                #   (2026-11-26 实测踩到: 补插逻辑形同虚设。)
                self._on_packet(payload[RTP_HDR:], ts, marker)
                self.state["rtp"] += 1
                self.state["last"] = time.time()
        finally:
            self.state["conn"] = False
            try:
                s.close()
            except OSError:
                pass
            print("[fanout] 相机连接断开, 3 秒后重连", flush=True)
            time.sleep(3.0)

    def _on_packet(self, pl: bytes, ts: int, marker: bool) -> None:
        """处理一个 RTP 【载荷】: 缓存 SPS/PPS, 在 IDR 前补插, 然后入队。"""
        t = nal_type(pl)
        if t == NAL_SPS:
            self.sps = pl
            self.state["sps"] = pl
        elif t == NAL_PPS:
            self.pps = pl
            self.state["pps"] = pl

        out = []
        # ★ 在 IDR 之前补插 SPS/PPS(带内参数集) —— 这是中继的核心价值:
        #   相机只在关键帧里带 SPS/PPS, 客户端错过就永远解不出来
        #   ("non-existing PPS 0 referenced" / "no frame!")。
        if starts_idr(pl) and self.sps and self.pps:
            out.append((self.sps, ts, False))
            out.append((self.pps, ts, False))
        out.append((pl, ts, marker))

        with self.lock:
            for payload, tstamp, mk in out:
                self.seq = (self.seq + 1) & 0xFFFF
                # ★ 队列元素是【三元组】(序号, RTP 包, 时间戳) ——
                #   _send_media 按 s_no/payload/cam_ts 解包。少一个元素
                #   发送线程会直接抛 ValueError 退出, 客户端就永远收不到数据
                #   (2026-11-26 实测踩到: "unspecified size" +
                #    "Failed reading RTSP data")。
                self.q.append((self.seq,
                               make_rtp(payload, self.seq, tstamp, self.ssrc, mk),
                               tstamp))
            while len(self.q) > QUEUE_MAX:
                self.q.popleft()


# ===========================================================================
# 分发侧
# ===========================================================================
def build_sdp(path: str, cam_sdp: str, sps: bytes, pps: bytes) -> bytes:
    """把相机的 SDP 裁成【只有视频轨】的本地 SDP。

    ★ 为什么要裁: 相机的 SDP 混着会话级 PCMA 音频属性和会话级 a=control:* ,
      FFmpeg 解析这种 SDP 会直接报 "Invalid data found when processing input"
      (实测)。
    ★ 其它一律照抄相机的(rtpmap/fmtp/packetization-mode), 保证编码格式一致。
    ★ sprop-parameter-sets: 把抓到的 SPS/PPS 以 base64 写进去 —— 客户端
      一连上就知道参数集, 不用等关键帧。
    """
    if isinstance(cam_sdp, bytes):
        cam_sdp = cam_sdp.decode("utf-8", "replace")
    lines = [l for l in cam_sdp.replace("\r\n", "\n").split("\n") if l.strip()]
    out = ["v=0", "o=- 0 0 IN IP4 127.0.0.1", "s=rk-fanout",
           "c=IN IP4 127.0.0.1", "t=0 0", f"a=control:{path}"]
    in_video = False
    for l in lines:
        if l.startswith("m="):
            in_video = l.startswith("m=video")
            if in_video:
                out.append("m=video 0 RTP/AVP 96")
            continue
        if not in_video:
            continue
        if l.startswith("a=control:"):
            out.append(f"a=control:{path}/trackID=0")
            continue
        if l.startswith("c="):
            continue
        if l.startswith("a=fmtp:") and "profile-level-id" in l:
            # 相机写的 profile-level-id 是 "000042" —— profile_idc=0, 非法值
            # (标准 Baseline 应是 42e01f)。实测码流是 H.264 Main Profile,
            # 相机只是把这一行写错了, 按 Main 修正。
            l = re.sub(r"profile-level-id=[0-9a-fA-F]+",
                       "profile-level-id=4d001f", l)
        out.append(l)
    if sps and pps:
        sp = base64.b64encode(sps).decode()
        pp = base64.b64encode(pps).decode()
        # fmtp 行可能已经被裁掉了, 补一条
        if not any(x.startswith("a=fmtp:96") for x in out):
            out.append("a=fmtp:96 packetization-mode=1")
        out.append(f"a=fmtp:96 sprop-parameter-sets={sp},{pp}")
    return ("\r\n".join(out) + "\r\n").encode()


def rtsp_response(method: str, cseq: str, path: str, sdp: bytes) -> bytes:
    if method == "OPTIONS":
        body = b"Public: DESCRIBE, SETUP, PLAY, TEARDOWN\r\n"
        return (f"RTSP/1.0 200 OK\r\nCSeq: {cseq}\r\n"
                f"Content-Length: {len(body)}\r\n\r\n").encode() + body
    if method == "DESCRIBE":
        return (f"RTSP/1.0 200 OK\r\nCSeq: {cseq}\r\n"
                f"Content-Type: application/sdp\r\n"
                f"Content-Length: {len(sdp)}\r\n\r\n").encode() + sdp
    # ★ SETUP 的响应【必须带 Transport 头】——
    #   FFmpeg 的 RTSP 客户端在 SETUP 之后要从响应里解析传输方式,
    #   缺了它就直接 avformat_open_input 失败:
    #     "Invalid data found when processing input", 而且【连 PLAY 都不会发】。
    if method == "SETUP":
        return (f"RTSP/1.0 200 OK\r\nCSeq: {cseq}\r\nSession: 87654321\r\n"
                f"Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
                f"Content-Length: 0\r\n\r\n").encode()
    return (f"RTSP/1.0 200 OK\r\nCSeq: {cseq}\r\n"
            f"Session: 87654321\r\n"
            f"Content-Length: 0\r\n\r\n").encode()


def handle_client(conn: socket.socket, args, state: dict, q: collections.deque,
                  qlock: threading.Lock) -> None:
    conn.settimeout(120)
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    buf = bytearray()
    try:
        while True:
            try:
                chunk = conn.recv(8192)
            except socket.timeout:
                break
            if not chunk:
                break
            buf += chunk
            while b"\r\n\r\n" in buf:
                head_b, _, rest = bytes(buf).partition(b"\r\n\r\n")
                buf.clear()
                buf += rest
                text = head_b.decode("utf-8", "replace")
                method = text.split(" ", 1)[0].upper()
                cseq = "1"
                for line in text.splitlines():
                    if line.lower().startswith("cseq:"):
                        cseq = line.split(":", 1)[1].strip()
                sdp = build_sdp(args.path, state.get("cam_sdp", ""),
                                state.get("sps"), state.get("pps"))
                conn.sendall(rtsp_response(method, cseq, args.path, sdp))
                if method == "PLAY":
                    threading.Thread(target=_send_media, daemon=True,
                                     args=(conn, args, q, qlock)).start()
                    _pump(conn)
                    return
    except Exception as e:  # noqa: BLE001
        print(f"[fanout] 客户端连接结束: {e}", flush=True)
    finally:
        try:
            conn.close()
        except OSError:
            pass


def _pump(conn) -> None:
    """PLAY 之后保持连接, 只负责把客户端发来的数据读掉(等它断开)。"""
    try:
        while True:
            if not conn.recv(4096):
                break
    except OSError:
        pass


def _send_media(conn, args, q, qlock) -> None:
    """把队列里的 RTP 包转发给这个客户端(重写 seq/ts/ssrc)。

    ★ payload 一个字节都不改 —— 真正的"零转码"。
    ★ 游标用【单调递增的全局序号】而不是队列下标: 队列满了会从左边丢元素,
      下标会错位, 序号不会。
    """
    seq = 1000
    ts_base = None
    ssrc = 0x5EED0000 | (os.getpid() & 0xFFFF)
    last_seen = None
    try:
        while True:
            item = None
            with qlock:
                if last_seen is None:
                    # 新客户端: 从【下一包】开始(不补历史, 也不重复最后一包)
                    last_seen = q[-1][0] if q else 0
                else:
                    for s_no, payload, cam_ts in q:
                        if s_no > last_seen:
                            item = (payload, cam_ts)
                            last_seen = s_no
                            break
            if item is None:
                time.sleep(0.005)
                continue
            payload, cam_ts = item
            if ts_base is None:
                ts_base = cam_ts
            new_ts = (cam_ts - ts_base) & 0xFFFFFFFF
            body = payload[RTP_HDR:]
            marker = bool(payload[1] & 0x80)
            conn.sendall(interleave(make_rtp(body, seq, new_ts, ssrc, marker)))
            seq += 1
    except OSError:
        pass


# ===========================================================================
def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--path", default=DEFAULT_PATH)
    ap.add_argument("--source", default="", help="相机的 RTSP 地址(带凭据)")
    ap.add_argument("--source-file", default="",
                    help="从文件读相机 RTSP 地址(推荐, 免得凭据进命令行/ps)")
    ap.add_argument("--bind", default="127.0.0.1",
                    help="监听地址(默认只听本机; 现场要外网访问再改)")
    args = ap.parse_args()

    source = args.source
    if args.source_file:
        with open(args.source_file, "r") as f:
            source = f.read().strip().splitlines()[0].strip()
    if not source:
        print("必须给 --source 或 --source-file", file=sys.stderr)
        return 2

    print("[fanout] 相机源: " + re.sub(r"://[^@/]*@", "://***:***@", source),
          flush=True)

    q = collections.deque()
    qlock = threading.Lock()
    state = {"conn": False, "rtp": 0, "last": 0.0, "err": "", "cam_sdp": "",
             "sps": None, "pps": None}

    puller = CameraPuller(source, q, qlock, state)
    puller.start()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.bind, args.port))
    srv.listen(16)
    print(f"[fanout] 本地分发地址: rtsp://{args.bind}:{args.port}{args.path}",
          flush=True)

    while True:
        try:
            conn, _addr = srv.accept()
        except OSError:
            break
        threading.Thread(target=handle_client, daemon=True,
                         args=(conn, args, state, q, qlock)).start()


if __name__ == "__main__":
    sys.exit(main())
