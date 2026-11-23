#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""rtsp_replay_server.py —— 本地 RTSP 回放服务器(把本地视频当"摄像头"推出去)

★ 为什么要有它:
    现场有两件事没法靠真相机验证:
      ① 相机离线/换网段 —— 页面上的"视频源连接失败"长什么样;
      ② 相机假死(网线松了) —— TCP 还连着, 但码流断了, 这种现象怎么复现。
    而且演示/验收时, 手上未必有一台能连的相机。
    这个脚本在板子上起一个【协议完全合规】的 RTSP 服务:
        把本地 MP4 转成 H.264 Annex-B, 按标准 RTP/TCP 推出去,
        客户端(OpenCV / FFmpeg / 本仓库的 rk_zero_copy 主程序)拿到的
        跟真相机的码流【一模一样】。

★ 推流协议(标准做法, 不是自创格式):
    RTSP over TCP(interleaved): '$' + 通道号 + 2 字节长度 + RTP 包
    RTP 头(12 字节): V=2, PT=96, seq, timestamp(90kHz), ssrc; 访问单元最后一个包置 marker
    H.264: 小 NAL 单包发; 大 NAL 用 FU-A 分片(RFC 6184)
        ★ FU indicator 的低 5 位必须是 28 —— 少了它对端会把分片全丢掉
          (踩过: 写成 nal[0]&0xE0, 对端 nal_type 算成 0, 握手能成但一帧都解不出)

★ MP4 -> Annex-B 用仓库里的 tools/es_dump:
    板上【没有 H.264 编码器】(opencv 的编码器是 v4l2m2m, 这板子没接),
    所以是"把现成的码流拿来播", 不是现场编码。

用法:
    # 默认: testvideo3.mp4 推到 rtsp://127.0.0.1:8554/camera1
    python3 scripts/rtsp_replay_server.py

    # 换端口/路径/视频
    python3 scripts/rtsp_replay_server.py --port 8555 --path /cam2 \
        --video /home/seeed/电动车素材.mp4

    # 假死模式: 握手成功后一直不说话(模拟网线松了), 用来验重连
    python3 scripts/rtsp_replay_server.py --stall-only

    # 中途恢复: 先静默 N 秒, 然后开始出画
    python3 scripts/rtsp_replay_server.py --stall 10

验证(板子上没有 ffplay, 用这两个):
    # ① OpenCV 拉(能解出帧就算成功)
    python3 -c "import cv2;c=cv2.VideoCapture('rtsp://127.0.0.1:8554/camera1');\
    print(c.isOpened());ok,f=c.read();print(ok, None if f is None else f.shape)"
    # ② 【主程序】拉(证明 MPP 硬解能吃这条流)
    ./build/rk_zero_copy rtsp://127.0.0.1:8554/camera1 model/ebike_int8_rk2332.rknn \
        --labels model/labels_ebike.txt --no-show --frames 60
"""
from __future__ import annotations

import argparse
import os
import socket
import struct
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)                    # <repo>/rk3588_zero_copy
REPO = os.path.dirname(PROJ)                    # <repo>
# es_dump 在仓库根的 tools/ 下(板子上的 MP4 -> Annex-B 裸流工具)
ES_DUMP_CANDIDATES = [
    os.path.join(REPO, "tools", "es_dump"),
    os.path.join(PROJ, "tools", "es_dump"),
]
ES_DUMP = next((p for p in ES_DUMP_CANDIDATES if os.path.exists(p)),
               ES_DUMP_CANDIDATES[0])

DEFAULT_VIDEO = "/home/seeed/testvideo3.mp4"
DEFAULT_PORT = 8554
DEFAULT_PATH = "/camera1"

RTP_PT = 96
MTU = 1400
ES_CACHE = "/tmp/rtsp_replay_es.h264"


# ===========================================================================
# H.264: Annex-B 解析 / 访问单元切分 / RTP 打包
# ===========================================================================
def split_nals(data: bytes) -> list[bytes]:
    """Annex-B -> NAL 列表(同时认 3 字节和 4 字节起始码)。"""
    starts = []
    i = 0
    n = len(data)
    while i + 3 <= n:
        if data[i] == 0 and data[i + 1] == 0:
            if data[i + 2] == 1:
                starts.append((i, 3))
                i += 3
                continue
            if i + 4 <= n and data[i + 2] == 0 and data[i + 3] == 1:
                starts.append((i, 4))
                i += 4
                continue
        i += 1
    nals = []
    for k, (off, size) in enumerate(starts):
        end = starts[k + 1][0] if k + 1 < len(starts) else n
        nals.append(data[off + size:end])
    return nals


def split_access_units(nals: list[bytes]) -> list[list[bytes]]:
    """按访问单元切分(AU = 一个画面所需的全部 NAL)。

    边界规则: AUD(type 9) 开头算新的 AU; 参数集(SPS/PPS)跟在帧后面算下一个 AU 的。
    """
    aus: list[list[bytes]] = []
    cur: list[bytes] = []
    for nal in nals:
        if not nal:
            continue
        t = nal[0] & 0x1F
        if t == 9 and cur:
            aus.append(cur)
            cur = [nal]
            continue
        if t in (7, 8) and cur and any((x[0] & 0x1F) in (1, 5) for x in cur):
            aus.append(cur)
            cur = [nal]
            continue
        cur.append(nal)
    if cur:
        aus.append(cur)
    return aus


def rtp_header(seq: int, ts: int, ssrc: int, marker: bool) -> bytes:
    b1 = RTP_PT | (0x80 if marker else 0x00)
    return struct.pack("!BBHII", 0x80, b1, seq & 0xFFFF, ts & 0xFFFFFFFF, ssrc)


def packetize_au(au: list[bytes], seq: int, ts: int, ssrc: int):
    """一个访问单元 -> 若干 RTP 包(单 NAL 或 FU-A 分片)。"""
    packets = []
    last = len(au) - 1
    for idx, nal in enumerate(au):
        marker = (idx == last)
        if len(nal) <= MTU:
            packets.append(rtp_header(seq, ts, ssrc, marker) + nal)
            seq += 1
            continue
        # FU-A: 大 NAL 分片
        # ★ FU indicator 的低 5 位【必须】是 28(RFC 6184): 接收方靠这 5 位
        #   判断"这是 FU-A 分片"。少了它, 对端会把每个分片当垃圾丢掉。
        fu_ind = (nal[0] & 0xE0) | 28
        ntype = nal[0] & 0x1F
        body = nal[1:]
        step = MTU - 32
        chunks = [body[i:i + step] for i in range(0, len(body), step)]
        for i, ch in enumerate(chunks):
            start = i == 0
            end = i == len(chunks) - 1
            fu_hdr = (0x80 if start else 0) | (0x40 if end else 0) | ntype
            packets.append(rtp_header(seq, ts, ssrc, marker and end)
                           + bytes([fu_ind, fu_hdr]) + ch)
            seq += 1
    return packets, seq


def extract_es(video: str, max_frames: int) -> list[list[bytes]]:
    """MP4 -> Annex-B 裸流 -> 访问单元列表。"""
    if not os.path.exists(ES_DUMP):
        print(f"[rtsp] 找不到 {ES_DUMP}, 先编译 tools/es_dump", file=sys.stderr)
        raise SystemExit(2)
    es_path = ES_CACHE
    if os.path.exists(es_path):
        os.unlink(es_path)
    cmd = [ES_DUMP, video, es_path, str(max_frames)]
    print(f"[rtsp] 转 Annex-B: {' '.join(cmd)}", flush=True)
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    data = open(es_path, "rb").read()
    aus = split_access_units(split_nals(data))
    print(f"[rtsp] {len(aus)} 个访问单元 / {len(data) // 1024} KB 码流", flush=True)
    return aus


# ===========================================================================
# RTSP 服务
# ===========================================================================
def build_response(method: str, cseq: str, path: str) -> bytes:
    if method == "OPTIONS":
        body = "Public: DESCRIBE, SETUP, PLAY, TEARDOWN\r\n"
        return (f"RTSP/1.0 200 OK\r\nCSeq: {cseq}\r\n"
                f"Content-Length: {len(body)}\r\n\r\n{body}").encode()
    if method == "DESCRIBE":
        # ★ body 必须是 str: 如果这里先 .encode() 变成 bytes, 后面 f-string 里
        #   的 {body} 插进去的是 bytes 的【repr】(b'v=0\\r\\n...'),
        #   对端拿到的 SDP 是坏的 —— Content-Length 算的是真 SDP 的长度,
        #   实际发过去的却是 repr, 对端读完长度就卡住不动了(踩过: FFmpeg
        #   打印 "SDP:" 之后直接超时, 一帧都收不到)。
        body = ("v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=rtsp-replay\r\n"
                "c=IN IP4 127.0.0.1\r\nt=0 0\r\n"
                f"a=control:{path}\r\n"
                "m=video 0 RTP/AVP 96\r\n"
                "a=rtpmap:96 H264/90000\r\n"
                f"a=control:{path}/trackID=0\r\n")
        return (f"RTSP/1.0 200 OK\r\nCSeq: {cseq}\r\n"
                f"Content-Type: application/sdp\r\n"
                f"Content-Length: {len(body)}\r\n\r\n{body}").encode()
    if method == "SETUP":
        return (f"RTSP/1.0 200 OK\r\nCSeq: {cseq}\r\nSession: 87654321\r\n"
                f"Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
                f"Content-Length: 0\r\n\r\n").encode()
    return (f"RTSP/1.0 200 OK\r\nCSeq: {cseq}\r\n"
            f"Session: 87654321\r\nContent-Length: 0\r\n\r\n").encode()


def interleave(payload: bytes) -> bytes:
    """RTSP over TCP 的 interleaved 帧头: '$' + 通道 + 2 字节长度。"""
    return b"$" + bytes([0]) + struct.pack(">H", len(payload)) + payload


def handle(conn: socket.socket, args, aus) -> None:
    conn.settimeout(120)
    buf = b""
    started = time.time()
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
                head, _, buf = buf.partition(b"\r\n\r\n")
                text = head.decode("utf-8", "replace")
                method = text.split(" ", 1)[0]
                cseq = "1"
                for line in text.splitlines():
                    if line.lower().startswith("cseq:"):
                        cseq = line.split(":", 1)[1].strip()
                conn.sendall(build_response(method, cseq, args.path))
                if method == "PLAY":
                    # ★ PLAY 之后【不能】直接 return: finally 里会把连接关掉,
                    #   对端立刻收到 "Connection reset by peer", 一帧都收不到。
                    #   正确做法: 保持连接, 推流线程在后台发, 这里只负责收
                    #   (收 TEARDOWN / 探测断开), 直到客户端走人。
                    _start_media(conn, args, aus, started)
                    _pump(conn)
                    return
    except Exception as e:  # noqa: BLE001
        print(f"[rtsp] 连接结束: {e}", flush=True)
    finally:
        try:
            conn.close()
        except OSError:
            pass


def _pump(conn) -> None:
    """PLAY 之后: 连接保持打开, 只负责把客户端的数据读掉(等它断开)。"""
    try:
        while True:
            chunk = conn.recv(4096)
            if not chunk:
                break
    except OSError:
        pass


def _start_media(conn, args, aus, started) -> None:
    """PLAY 之后按剧本推流。"""
    interval = 1.0 / float(args.fps)
    state = {"seq": 1000, "ts": 0}
    ssrc = 0x5EED1234

    def sender():
        # 先静默(模拟网线松了/相机假死)
        while time.time() - started < args.stall:
            time.sleep(0.2)
        if not aus:
            return                      # --stall-only: 握手完就不说话了
        i = 0
        while True:
            au = aus[i % len(aus)]
            pkts, state["seq"] = packetize_au(au, state["seq"], state["ts"], ssrc)
            try:
                for p in pkts:
                    conn.sendall(interleave(p))
            except OSError:
                return
            state["ts"] += 3000          # 90kHz / 30fps
            i += 1
            time.sleep(interval)
    threading.Thread(target=sender, daemon=True).start()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=DEFAULT_PORT, help="监听端口")
    ap.add_argument("--path", default=DEFAULT_PATH, help="聚合 URL 的路径")
    ap.add_argument("--video", default=DEFAULT_VIDEO, help="要当相机播的 MP4")
    ap.add_argument("--stall", type=float, default=0.0,
                    help="PLAY 之后先静默多少秒(模拟假死)")
    ap.add_argument("--stall-only", action="store_true",
                    help="握手后一直不说话(纯假死, 不推码流)")
    ap.add_argument("--fps", type=float, default=30.0, help="推流帧率")
    ap.add_argument("--frames", type=int, default=600, help="最多用多少个访问单元")
    args = ap.parse_args()

    aus = []
    if not args.stall_only:
        if not os.path.exists(args.video):
            print(f"[rtsp] 找不到视频 {args.video}", file=sys.stderr)
            return 2
        aus = extract_es(args.video, args.frames)

    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.port))
    srv.listen(8)
    mode = "一直不说话(假死)" if args.stall_only else "推 H.264 码流"
    print(f"[rtsp] 回放服务器已启动: rtsp://127.0.0.1:{args.port}{args.path}"
          f"  ({mode}"
          f"{', 先静默 %.0f 秒' % args.stall if args.stall else ''})", flush=True)
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=handle, args=(conn, args, aus), daemon=True).start()


if __name__ == "__main__":
    raise SystemExit(main())
