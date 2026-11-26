#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""rtsp_fanout_server.py —— 真实相机的【1 拉 N 播】本地零转码中继(服务端)

★ 为什么需要它(2026-11-26 Phase 1 实测结论, 全部有实测出处):
    rk_gate 的 4 路通道【直连远端相机】时, 4 路里只有最先启动的那一路能持续
    出帧, 其余 3 路的帧号永不推进 -> 看门狗 L2 判定卡死 -> 每 ~90 秒重启一次服务。

    逐项排除(全是实测, 不是推测):
      · 相机 RTSP 协议层无罪: 原生协议并发 4 路, 接入间隔 0/0.3/1.0s 三档,
        4 路收字节增量完全一致, 30 秒持续不断;
      · 相机不会踢旧连接: A 连 8 秒后接 B, A/B 仍同时稳定收流;
      · libav 无罪: 4 路并发 field_probe 抓帧全部成功;
      · 4xMPP 无罪: 4 路连【本地】RTSP 服务器时 4 路全部满速 deliv=20.0;
      · MP4 源无罪: 4 路 MP4 走同一二进制 4/4 正常;
      · 相机码流结构: 每个 IDR 前都带 SPS+PPS, IDR 间隔约 162 个 RTP 包(约 1 秒)。
    => 只有"FFmpeg RTSP 客户端直连远端相机"这条路有问题, 而那几个
       rtsp_transport/stimeout/max_delay/nobuffer/direct 写死在
       src/gate_main.cc:670-675, 本阶段 C++ 冻结不能改。

★ 真正的机理(实测): 【客户端拿不到能解的码流】。
    直连时 FFmpeg 反复报 "no frame!" / "non-existing PPS 0 referenced",
    帧号永不推进; 而连本地服务器时同一套 C++ 一切正常。

★ 本中继怎么解决(两招一起上):
    1) 在【每个 IDR 前面补插 SPS/PPS】—— 任何时刻接入的客户端, 只要等到
       下一个关键帧就能立刻解码, 不会因为错过带内参数集而永久卡住;
    2) 把抓到的 SPS/PPS 以 base64 写进本地 SDP 的 sprop-parameter-sets ——
       客户端一连上就知道参数集, 连等关键帧都不必。

★ 零转码: H.264 【不解码、不重编码】, 码流 payload 一个字节都不改。
    只在 RTP 层做两件事: 补插 SPS/PPS、重写 seq/ts/ssrc。

★ 队列模型(2026-11-26 踩坑后定稿):
    【每个客户端一条自己的队列】, 相机读线程只做 O(1) 的 append。
    之前是"一条共享队列 + 全局锁 + 每个客户端线性扫描找下一个包":
    4 客户端 × 226 包/秒 × 最多 1200 项扫描 ≈ 100 万次/秒持锁迭代,
    把相机读线程挤死 -> 队列不再填充 -> 4 路通道一起饿死 -> 看门狗反复重启。
    现在: 相机线程 append(4 次 O(1)), 每个客户端从自己的队列 O(1) 取。

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
QUEUE_MAX = 900           # 每个客户端的队列上限(约 4 秒 @226 包/秒)
MTU = 1400

NAL_SPS, NAL_PPS, NAL_IDR = 7, 8, 5


# ---------------------------------------------------------------------------
# 基础工具
# ---------------------------------------------------------------------------
def interleave(payload: bytes, channel: int = 0) -> bytes:
    """RTSP over TCP(interleaved) 帧头: '$' + 通道 + 2 字节长度(大端)。"""
    return b"$" + bytes([channel]) + struct.pack(">H", len(payload)) + payload


def ts_now() -> str:
    """日志时间戳(带毫秒), 排查"多久掉一次"要用。"""
    return time.strftime("%H:%M:%S") + f".{int(time.time() * 1000) % 1000:03d}"


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
# 分发侧: 每个客户端一条自己的队列
# ===========================================================================
class Client:
    """一个本地客户端。q 是【它自己的】队列, 相机线程只做 O(1) append。"""

    __slots__ = ("conn", "q", "lock", "seq", "ts_base", "sent", "dropped",
                 "alive", "cid")

    def __init__(self, conn: socket.socket, cid: int):
        self.conn = conn
        self.cid = cid
        self.q = collections.deque()
        self.lock = threading.Lock()
        self.seq = 1000
        self.ts_base = None
        self.sent = 0
        self.dropped = 0
        self.alive = True


# ===========================================================================
# 拉流侧: 从相机拉 1 路 -> 补插 SPS/PPS -> 分发给所有客户端
# ===========================================================================
class CameraPuller(threading.Thread):
    def __init__(self, source: str, state: dict):
        super().__init__(daemon=True)
        self.source = source
        self.state = state
        self.clients = []
        self.clients_lock = threading.Lock()
        self.stop = threading.Event()
        self.sps = None            # 缓存的 SPS RTP 载荷
        self.pps = None            # 缓存的 PPS RTP 载荷
        self.seq = 0
        self.ssrc = 0x5EED0001
        # ★ 本地时间戳: 由【我们自己】生成, 不透传相机的 ts。
        #   相机会话断了重连后, 相机的 ts 会从头开始 —— 直接透传的话,
        #   本地客户端会看到时间戳大跳变, 解码器失步。
        #   自己生成就能保证【跨重连连续】, 客户端看到的是一条平稳的流。
        self.local_ts = 0
        self.prev_marker = False

    def add_client(self, c: Client) -> None:
        with self.clients_lock:
            self.clients.append(c)

    def remove_client(self, c: Client) -> None:
        with self.clients_lock:
            if c in self.clients:
                self.clients.remove(c)

    def heartbeat(self) -> None:
        """每 5 秒打一行心跳: 相机进包速率 + 每个客户端的队列长度/消费速率。

        ★ 这是排查"通道为什么会停"的【第一手数据】:
          相机送 25fps 的包, 而 rk_gate 每路只分析 20fps —— 如果客户端消费
          速率低于相机进包速率, 客户端队列就会一直涨, 涨到上限就丢包,
          H.264 丢包 = 解码失败 = FFmpeg 拆连接 = 通道断流。
        """
        prev_rtp = self.state.get("rtp", 0)
        prev_t = time.time()
        while not self.stop.is_set():
            time.sleep(5)
            now = time.time()
            dt = max(now - prev_t, 1e-6)
            rtp = self.state.get("rtp", 0)
            rate = (rtp - prev_rtp) / dt
            prev_rtp, prev_t = rtp, now
            with self.clients_lock:
                targets = list(self.clients)
            parts = []
            for c in targets:
                with c.lock:
                    qlen = len(c.q)
                parts.append(f"#{c.cid}(q={qlen} sent={c.sent} drop={c.dropped})")
            print(f"[fanout] {ts_now()} 相机 {rate:.0f} 包/秒 "
                  f"({self.state.get('rtp', 0)} 总) | 客户端: "
                  f"{' '.join(parts) if parts else '无'}", flush=True)

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
                print(f"[fanout] {ts_now()} 相机会话异常: {self.state['err']}",
                      flush=True)
                time.sleep(2.0)

    def _session(self):
        cred, host, port, path = split_url(self.source)
        auth = ""
        if cred:
            auth = ("Authorization: Basic "
                    f"{base64.b64encode(cred.encode()).decode()}\r\n")
        s = socket.create_connection((host, port), timeout=8)
        s.settimeout(10)          # 握手阶段保留超时
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
        t_start = time.time()
        print(f"[fanout] {ts_now()} 已连上相机 {host}:{port}{path}  track={track}",
              flush=True)

        # ★★ keep-alive: 每 20 秒发一个 RTSP GET_PARAMETER ★★
        #   这台相机(LIVE555)实测【推满约 10000 个 RTP 包 / 55 秒后就会停推】,
        #   而 TCP 连接还挂着 —— 实测与有没有本地客户端【完全无关】(中继独跑
        #   同样在 10003 包处停), 所以不是"客户端不读", 是相机侧的会话上限。
        #   两种应对一起上:
        #     (a) keep-alive: 万一相机是因为"客户端 60 秒没发过 RTSP 请求"
        #         而停推, 定时 GET_PARAMETER 就能让它一直推;
        #     (b) 停滞检测: 一旦真停了, 立刻重连, 靠 IDR 重新同步。
        ka_stop = threading.Event()
        ka_thread = threading.Thread(target=self._keepalive,
                                      args=(s, base, sid, auth, ka_stop),
                                      daemon=True)
        ka_thread.start()
        try:
            # ★ 收流循环用【5 秒超时】做停滞检测: 超时 = 相机不推了 -> 重连。
            #   (早先用阻塞读, 相机停推后我们会一直挂着, 客户端就永远收不到
            #    下一帧, 通道随之停帧。)
            s.settimeout(5)
            while not self.stop.is_set():
                try:
                    item = read_interleaved(s, buf)
                except socket.timeout:
                    self.state["err"] = "相机 5 秒没有推流(疑似会话推流上限)"
                    break
                if item is None:
                    self.state["err"] = "相机关闭了 TCP 连接"
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
            ka_stop.set()
            self.state["conn"] = False
            try:
                s.close()
            except OSError:
                pass
            dur = time.time() - t_start
            print(f"[fanout] {ts_now()} 相机连接断开(本次会话 {dur:.1f}s, "
                  f"累计收 {self.state['rtp']} 包): {self.state['err'] or '未知'} "
                  f"-> 3 秒后重连", flush=True)
            time.sleep(3.0)

    @staticmethod
    def _keepalive(sock, base, sid, auth, stop_evt) -> None:
        """每 20 秒给相机发一个 GET_PARAMETER, 试探它是不是因为缺 keep-alive
        才停推的。发出去的响应会以文本形式混在 interleaved 流里, 而
        read_interleaved() 会把 '$' 之前的字节全部丢掉, 所以不会干扰 RTP。"""
        cseq = 100
        while not stop_evt.wait(20.0):
            try:
                cseq += 1
                sock.sendall((f"GET_PARAMETER {base} RTSP/1.0\r\n"
                              f"CSeq: {cseq}\r\n{auth}"
                              f"Session: {sid}\r\n\r\n").encode())
            except OSError:
                return

    def _on_packet(self, pl: bytes, ts: int, marker: bool) -> None:
        """处理一个 RTP 【载荷】: 缓存 SPS/PPS, 在 IDR 前补插, 然后分发给所有客户端。"""
        t = nal_type(pl)
        if t == NAL_SPS:
            self.sps = pl
            self.state["sps"] = pl
        elif t == NAL_PPS:
            self.pps = pl
            self.state["pps"] = pl

        # ★ 本地时间戳: 上一个包是上一帧的最后一个(marker=1) -> 进入新的一帧
        if self.prev_marker:
            self.local_ts += 3600          # 90kHz / 25fps
            self.prev_marker = False
        lts = self.local_ts

        out = []
        # ★ 在 IDR 之前补插 SPS/PPS(带内参数集) —— 这是中继的核心价值:
        #   相机只在关键帧里带 SPS/PPS, 客户端错过就永远解不出来
        #   ("non-existing PPS 0 referenced" / "no frame!")。
        #   ★ 补插的 SPS/PPS 与 IDR 同属一帧, 用同一个时间戳。
        if starts_idr(pl) and self.sps and self.pps:
            out.append((self.sps, lts, False))
            out.append((self.pps, lts, False))
        out.append((pl, lts, marker))
        self.prev_marker = marker

        with self.clients_lock:
            targets = list(self.clients)
        for payload, tstamp, mk in out:
            self.seq = (self.seq + 1) & 0xFFFF
            rtp = make_rtp(payload, self.seq, tstamp, self.ssrc, mk)
            for c in targets:
                with c.lock:
                    c.q.append((rtp, tstamp))
                    # 队列满 = 这个客户端消费不过来, 丢最老的
                    while len(c.q) > QUEUE_MAX:
                        c.q.popleft()
                        c.dropped += 1


# ===========================================================================
# 本地 RTSP 服务器
# ===========================================================================
def build_sdp(path: str, cam_sdp: str, sps: bytes, pps: bytes) -> bytes:
    """把相机的 SDP 裁成【只有视频轨】的本地 SDP。

    ★ 为什么要裁: 相机的 SDP 混着会话级 PCMA 音频属性和会话级 a=control:* ,
      FFmpeg 解析这种 SDP 会直接报 "Invalid data found when processing input"。
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
        if not any(x.startswith("a=fmtp:96 sprop") for x in out):
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


def handle_client(conn: socket.socket, args, state: dict, puller: CameraPuller,
                  cid: int) -> None:
    conn.settimeout(120)
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    buf = bytearray()
    client = None
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
                    # ★★★ 关键: PLAY 之后【必须改成阻塞读, 不能留超时】★★★
                    #   RTSP 客户端(FFmpeg)在 PLAY 之后【不会再发任何 RTSP 请求】,
                    #   它只是被动收 RTP。socket 上留了超时的话, _pump() 的
                    #   recv 会抛 socket.timeout -> 被 except OSError 吞掉 ->
                    #   _pump 返回 -> handle_client 返回 -> finally 里 close()
                    #   -> 我们【自己把客户端会话掐掉】, 通道随之断流。
                    #   实测: 连上后【正好 120 秒】4 路通道全部停帧,
                    #   看门狗 L2 判卡死 -> 反复重启服务(2026-11-26)。
                    conn.settimeout(None)
                    client = Client(conn, cid)
                    puller.add_client(client)
                    print(f"[fanout] {ts_now()} 客户端#{cid} 开始推流", flush=True)
                    threading.Thread(target=_send_media, daemon=True,
                                     args=(client,)).start()
                    _pump(conn)
                    return
    except Exception as e:  # noqa: BLE001
        print(f"[fanout] {ts_now()} 客户端#{cid} 连接结束: {e}", flush=True)
    finally:
        if client is not None:
            puller.remove_client(client)
            print(f"[fanout] {ts_now()} 客户端#{client.cid} 断开 "
                  f"(已发 {client.sent} 包, 丢弃 {client.dropped} 包)", flush=True)
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


def _send_media(client: Client) -> None:
    """从【本客户端自己的队列】取包转发(重写 seq/ts/ssrc)。O(1), 无全局锁。"""
    c = client
    ssrc = 0x5EED0000 | (os.getpid() & 0xFFFF)
    try:
        while c.alive:
            rtp = None
            with c.lock:
                if c.q:
                    rtp, cam_ts = c.q.popleft()
                else:
                    cam_ts = None
            if rtp is None:
                time.sleep(0.003)
                continue
            if c.ts_base is None:
                c.ts_base = cam_ts
            new_ts = (cam_ts - c.ts_base) & 0xFFFFFFFF
            body = rtp[RTP_HDR:]
            marker = bool(rtp[1] & 0x80)
            c.conn.sendall(interleave(make_rtp(body, c.seq, new_ts, ssrc, marker)))
            c.seq += 1
            c.sent += 1
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

    state = {"conn": False, "rtp": 0, "last": 0.0, "err": "", "cam_sdp": "",
             "sps": None, "pps": None}
    puller = CameraPuller(source, state)
    puller.start()
    threading.Thread(target=puller.heartbeat, daemon=True).start()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.bind, args.port))
    srv.listen(16)
    print(f"[fanout] 本地分发地址: rtsp://{args.bind}:{args.port}{args.path}",
          flush=True)

    cid = 0
    while True:
        try:
            conn, _addr = srv.accept()
        except OSError:
            break
        cid += 1
        threading.Thread(target=handle_client, daemon=True,
                         args=(conn, args, state, puller, cid)).start()


if __name__ == "__main__":
    sys.exit(main())
