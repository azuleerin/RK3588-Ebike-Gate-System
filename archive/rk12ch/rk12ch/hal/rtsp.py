"""极简 RTSP 客户端: RTSP over TCP(interleaved) -> AnnexB H.264 访问单元

★ 为什么自己写而不是用 ffmpeg/PyAV/GStreamer:
    板子上常常【一个都没有】(本板实测: 没有 ffmpeg CLI, 没有 PyAV,
    GStreamer 的 python 绑定也没装), 只有 libav*.so。
    自己实现 RTSP 文本协议 + RTP 解包只有 200 行, 却换来:
      ① 零外部依赖
      ② 少一次用户态拷贝(ffmpeg CLI 那条路是 socket -> pipe -> MPP, 两次拷贝)
      ③ 能拿到 SDP 里的 sprop-parameter-sets, 直接拼成 AnnexB 的 SPS/PPS

★ 混流解析(最容易写错的地方):
    RTSP over TCP 时, 媒体数据和 RTSP 响应文本在【同一条 TCP 流】里,
    而且媒体数据经常排在响应【之前】到达。
    所以必须【顺序解析】:
        缓冲区开头是 "RTSP/1.0" -> 解析一条响应
        缓冲区开头是 '$'(0x24) -> 解析一个 interleaved 帧
        其它 -> 跳过一个字节重新同步
    用"找 \r\n\r\n"或者"找 '$'"的方式都会互相吃掉对方的数据。

协议要点:
    OPTIONS -> DESCRIBE -> SETUP(TCP interleaved) -> PLAY
    之后在同一条 TCP 连接上收 '$'(0x24) + channel(1) + len16 + RTP 包
    RTP/H.264:
        1~23 单NAL包, 24 STAP-A 聚合, 28 FU-A 分片
"""

from __future__ import annotations

import base64
import logging
import re
import socket
import struct
import time
from typing import Callable

log = logging.getLogger(__name__)

NAL_START = b"\x00\x00\x00\x01"
RTSP_MAGIC = b"RTSP/1.0"


class RtspError(RuntimeError):
    pass


def parse_rtsp_url(url: str):
    """rtsp://host:port/path?query -> (host, port, path)"""
    if not url.startswith("rtsp://"):
        raise RtspError(f"不是 rtsp:// 地址: {url}")
    rest = url[len("rtsp://"):]
    if "/" in rest:
        hostport, path = rest.split("/", 1)
        path = "/" + path
    else:
        hostport, path = rest, "/"
    if ":" in hostport:
        host, port = hostport.split(":", 1)
        port = int(port)
    else:
        host, port = hostport, 554
    return host, port, path


class RtspH264Client:
    """RTSP over TCP。on_access_unit(NAL bytes) 回调"""

    def __init__(self, url: str, timeout: float = 5.0,
                 on_access_unit: Callable[[bytes], None] | None = None):
        self.url = url
        self.host, self.port, self.path = parse_rtsp_url(url)
        self.timeout = timeout
        self.on_access_unit = on_access_unit
        self.sock: socket.socket | None = None
        self.session_id = ""
        self._buf = b""
        self._responses: list[str] = []
        self._au = bytearray()      # 正在攒的访问单元
        self._pending = b""          # 已经攒完、等待被取走的访问单元
        self._fu: bytearray | None = None
        self._sps_pps = b""
        self._control = ""
        self._cseq = 1

    # ------------------------------------------------------------------
    def connect(self) -> None:
        s = socket.create_connection((self.host, self.port), timeout=self.timeout)
        s.settimeout(self.timeout)
        self.sock = s
        self._request("OPTIONS", self.url)
        sdp = self._request("DESCRIBE", self.url,
                            extra={"Accept": "application/sdp"})
        self._parse_sdp(sdp)
        transport = "RTP/AVP/TCP;unicast;interleaved=0-1"
        resp = self._request("SETUP", self._setup_url(), extra={"Transport": transport})
        if " 200 " not in resp:
            for alt in ("trackID=0", "video", "Video", "h264", "streamid=0"):
                self._control = alt
                resp = self._request("SETUP", self._setup_url(),
                                     extra={"Transport": transport})
                if " 200 " in resp:
                    log.info("[rtsp] SETUP 用 control=%s 成功", alt)
                    break
        if " 200 " not in resp:
            raise RtspError(f"SETUP 失败: {resp.splitlines()[0] if resp else '无响应'}")
        m = re.search(r"Session:\s*(\S+)", resp)
        if m:
            self.session_id = m.group(1).split(";")[0]
        self._request("PLAY", self.url,
                      extra={"Session": self.session_id, "Range": "npt=0.000-"})
        log.info("[rtsp] %s 已 PLAY (control=%s, sps/pps %d 字节)",
                 self.url, self._control, len(self._sps_pps))

    # ------------------------------------------------------------------
    def _setup_url(self) -> str:
        """SETUP 的 URL: base + '/' + SDP 里 a=control 给的地址

        ★ 必须用 SDP 里的 control: 不同相机的 control 地址五花八门
          (track1 / trackID=0 / videoTrack / ...), 写死会 404 Stream Not Found。
        """
        # ★ base 是【聚合 URL 本身】(rtsp://host:port/path), 不是去掉最后一段。
        #   SDP 里的 a=control 是相对于聚合 URL 的, 例如
        #       聚合: rtsp://192.168.8.200:8557/h264
        #       control: track1  ->  SETUP: rtsp://192.168.8.200:8557/h264/track1
        #   去掉最后一段会拼成 .../track1 -> 建到别的流上 -> PLAY 404。
        base = self.url.rstrip("/")
        ctrl = self._control or "trackID=0"
        if ctrl.startswith("rtsp://"):
            return ctrl
        return f"{base}/{ctrl.lstrip('/')}"

    # ------------------------------------------------------------------
    def _request(self, method: str, url: str, extra: dict | None = None) -> str:
        assert self.sock is not None
        headers = {"CSeq": str(self._cseq), "User-Agent": "rk12ch/1.0"}
        self._cseq += 1
        if extra:
            headers.update(extra)
        if self.session_id and method not in ("SETUP",):
            headers["Session"] = self.session_id
        req = f"{method} {url} RTSP/1.0\r\n" + \
              "".join(f"{k}: {v}\r\n" for k, v in headers.items()) + "\r\n"
        self.sock.sendall(req.encode())
        return self._wait_response()

    # ------------------------------------------------------------------
    def _wait_response(self) -> str:
        deadline = time.monotonic() + self.timeout
        while not self._responses:
            if time.monotonic() > deadline:
                raise RtspError("RTSP 响应超时")
            self._pump()
        return self._responses.pop(0)

    # ------------------------------------------------------------------
    def _pump(self) -> None:
        """读 socket + 顺序解析缓冲区"""
        assert self.sock is not None
        try:
            chunk = self.sock.recv(65536)
        except socket.timeout:
            return
        except OSError:
            raise RtspError("RTSP 连接断开")
        if not chunk:
            raise RtspError("RTSP 连接断开")
        self._buf += chunk
        self._parse_buffer()

    # ------------------------------------------------------------------
    def _parse_buffer(self) -> None:
        """顺序解析: RTSP 响应 / interleaved 帧 / 噪声重新同步"""
        while self._buf:
            if self._buf.startswith(RTSP_MAGIC):
                if not self._take_response():
                    return          # 数据不够, 等下次
                continue
            if self._buf[0] == 0x24:      # '$'
                if not self._take_interleaved():
                    return
                continue
            # 噪声(可能是上一次响应残留), 跳一个字节重新同步
            self._buf = self._buf[1:]

    # ------------------------------------------------------------------
    def _take_response(self) -> bool:
        """从缓冲区头取一条 RTSP 响应。数据不够返回 False"""
        idx = self._buf.find(b"\r\n\r\n")
        if idx < 0:
            return False
        header = self._buf[:idx].decode(errors="ignore")
        rest = self._buf[idx + 4:]
        m = re.search(r"Content-Length:\s*(\d+)", header, re.I)
        n = int(m.group(1)) if m else 0
        if len(rest) < n:
            return False
        body = rest[:n].decode(errors="ignore")
        self._buf = rest[n:]
        self._responses.append(header + ("\n" + body if body else ""))
        return True

    # ------------------------------------------------------------------
    def _take_interleaved(self) -> bool:
        """从缓冲区头取一个 '$' 帧。数据不够返回 False"""
        if len(self._buf) < 4:
            return False
        chan = self._buf[1]
        length = struct.unpack(">H", self._buf[2:4])[0]
        if len(self._buf) < 4 + length:
            return False
        payload = self._buf[4:4 + length]
        self._buf = self._buf[4 + length:]
        if chan == 0:                     # chan 1 是 RTCP
            self._on_rtp(payload)
        return True

    # ------------------------------------------------------------------
    def _parse_sdp(self, sdp: str) -> None:
        """从 SDP 里取: ① 视频轨的 a=control ② H.264 的 sprop-parameter-sets"""
        video_pt = None
        control = None
        in_video = False
        for line in sdp.splitlines():
            line = line.strip()
            if line.startswith("m="):
                in_video = line.startswith("m=video")
                if in_video:
                    parts = line.split()
                    if len(parts) >= 4:
                        video_pt = parts[3].split("/")[0]
                continue
            if not in_video:
                continue
            if line.startswith("a=rtpmap:") and "H264" in line.upper():
                video_pt = line.split(":")[1].split("/")[0]
            elif line.startswith("a=control:"):
                control = line.split(":", 1)[1].strip()
        if control:
            self._control = control
        log.info("[rtsp] 视频轨 pt=%s control=%s", video_pt, self._control)

        m = re.search(r"sprop-parameter-sets=([^\s;]+)", sdp)
        if not m:
            log.info("[rtsp] SDP 无 sprop-parameter-sets, 用流内 in-band SPS/PPS")
            return
        try:
            raw = base64.b64decode(m.group(1))
        except Exception:
            log.warning("[rtsp] sprop-parameter-sets 解码失败")
            return
        i = 0
        out = bytearray()
        while i + 2 <= len(raw):
            n = struct.unpack(">H", raw[i:i + 2])[0]
            i += 2
            if i + n > len(raw):
                break
            out += NAL_START + raw[i:i + n]
            i += n
        self._sps_pps = bytes(out)
        log.info("[rtsp] sprop-parameter-sets %d 字节", len(out))

    # ------------------------------------------------------------------
    def read_frame(self) -> bytes | None:
        """读一个完整的 H.264 访问单元(AnnexB)。超时返回 None"""
        if self._pending:
            au, self._pending = self._pending, b""
            return au
        if self.sock is None:
            return None
        deadline = time.monotonic() + self.timeout
        while True:
            if self._pending:
                au, self._pending = self._pending, b""
                return au
            if time.monotonic() > deadline:
                return None
            self._pump()

    # ------------------------------------------------------------------
    def _on_rtp(self, rtp: bytes) -> None:
        if len(rtp) < 12:
            return
        b0, b1 = rtp[0], rtp[1]
        cc = b0 & 0x0F
        offset = 12 + cc * 4
        if b0 & 0x10:                     # 扩展头
            if len(rtp) < offset + 4:
                return
            ext_len = struct.unpack(">H", rtp[offset + 2:offset + 4])[0]
            offset += 4 + ext_len * 4
        payload = rtp[offset:]
        marker = bool(b1 & 0x80)
        self._depacketize(payload, marker)

    # ------------------------------------------------------------------
    def _depacketize(self, payload: bytes, marker: bool) -> None:
        if not payload:
            return
        nal_type = payload[0] & 0x1F
        if not self._au and self._sps_pps:
            # ★ 每个访问单元前面带一次 SPS/PPS: MPP 的解析器需要 SPS/PPS 才能开帧
            self._au += self._sps_pps
        if 1 <= nal_type <= 23:                       # 单 NAL
            self._au += NAL_START + payload
        elif nal_type == 24:                          # STAP-A
            i = 1
            while i + 2 <= len(payload):
                n = struct.unpack(">H", payload[i:i + 2])[0]
                i += 2
                if i + n > len(payload):
                    break
                self._au += NAL_START + payload[i:i + n]
                i += n
        elif nal_type == 28:                          # FU-A
            fu_ind = payload[0]
            fu_hdr = payload[1]
            start = bool(fu_hdr & 0x80)
            end = bool(fu_hdr & 0x04)
            if start:
                self._fu = bytearray([(fu_ind & 0xE0) | (fu_hdr & 0x1F)])
            if self._fu is None:
                return
            self._fu += payload[2:]
            if end:
                self._au += NAL_START + bytes(self._fu)
                self._fu = None
        if marker:
            if self._au:
                self._pending = bytes(self._au)
                self._au.clear()
                if self.on_access_unit:
                    self.on_access_unit(self._pending)

    # ------------------------------------------------------------------
    def close(self) -> None:
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None
