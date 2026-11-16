#!/usr/bin/env python3
"""web_console.py —— RK3588 流水线实时控制台(网页)

在 PC 上跑一个网页服务, 浏览器打开一个地址就能:
  · 看到 MPP / RGA / NPU 的实时负载百分比和流水线 FPS
  · 点开关, 【立刻】动态切换板端的 MPP 硬解 / RGA 加速 / NPU 推理
  · 看 FPS 与各阶段耗时的实时曲线

数据通路(MQTT 双向):
    网页  --POST /api/control-->  后端 --MQTT-->  device/<sn>/message/down/control
                                                                  |
                                                              板端主程序(订阅)
                                                                  |
    网页  <--GET /api/state---   后端 <--MQTT--  device/<sn>/message/up/status
                                                     device/<sn>/message/up/load

★ 为什么用标准库而不是 Flask/FastAPI:
    现场 PC 上不一定装了 flask/paho(这个仓库其它脚本也是零依赖的),
    离线环境装依赖很痛苦。这里只用 http.server + socket, 拷过去就能跑。

用法(PC 上):
    python3 scripts/web_console.py --host 192.168.8.21 --sn rk3588-01 --http-port 8080

然后浏览器打开:  http://<PC的IP>:8080/
"""
from __future__ import annotations

import argparse
import collections
import json
import os
import queue
import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ★ OpenCV 只用来"出画面", 装不上也不影响控制台其它功能:
#   连通性测试/假死检测那条路是纯标准库实现的(见 rtsp_liveness)。
try:                                    # pragma: no cover - 环境相关
    import cv2
    HAVE_CV2 = True
except Exception:                       # noqa: BLE001
    cv2 = None
    HAVE_CV2 = False

# ===========================================================================
# MQTT 最小实现(订阅 + 发布, 零依赖)
# ===========================================================================


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


class MqttClient:
    """能订阅也能发布的极简 MQTT 3.1.1 客户端(自动重连)。"""

    def __init__(self, host: str, port: int, client_id: str):
        self.host = host
        self.port = port
        self.client_id = client_id
        self.sock: socket.socket | None = None
        self.pid = 0
        self.connected = False
        self.last_error = ""
        self.subscribed: list[str] = []

    # ---------------------------------------------------------------- 底层
    def _connect_socket(self) -> bool:
        try:
            s = socket.create_connection((self.host, self.port), timeout=10)
            s.settimeout(2.0)
            var = (enc_str("MQTT") + bytes([4, 0x02])
                   + struct.pack("!H", 60) + enc_str(self.client_id))
            s.sendall(bytes([0x10]) + enc_len(len(var)) + var)
            hdr = s.recv(1)
            if not hdr:
                raise ConnectionError("连接被关闭")
            rl = self._read_len(s)
            body = self._read_exact(s, rl)
            if len(body) < 2 or body[1] != 0:
                raise ConnectionError(f"CONNACK 返回码 {body[1] if len(body) > 1 else '?'}")
            self.sock = s
            self.connected = True
            self.last_error = ""
            return True
        except Exception as e:  # noqa: BLE001
            self.last_error = str(e)
            self.sock = None
            self.connected = False
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

    def _subscribe(self, topic: str) -> None:
        assert self.sock is not None
        self.pid += 1
        var = struct.pack("!H", self.pid) + enc_str(topic) + bytes([0])
        self.sock.sendall(bytes([0x82]) + enc_len(len(var)) + var)
        # SUBACK: 0x90 <len> <pid 2字节> <rc>
        hdr = self.sock.recv(1)
        if not hdr:
            raise ConnectionError("连接断开")
        rl = self._read_len(self.sock)
        body = self._read_exact(self.sock, rl)
        if len(body) < 3 or body[2] == 0x80:
            raise ConnectionError("订阅被拒绝")
        self.subscribed.append(topic)

    def publish(self, topic: str, payload: str, qos: int = 0) -> bool:
        if self.sock is None:
            return False
        try:
            var = enc_str(topic) + payload.encode("utf-8")
            self.sock.sendall(bytes([0x30 | (qos << 1)]) + enc_len(len(var)) + var)
            return True
        except Exception as e:  # noqa: BLE001
            self.last_error = str(e)
            self.connected = False
            return False

    def close(self) -> None:
        if self.sock is not None:
            try:
                self.sock.sendall(bytes([0xE0, 0x00]))
                self.sock.close()
            except Exception:  # noqa: BLE001
                pass
        self.sock = None
        self.connected = False


# ===========================================================================
# 全局状态
# ===========================================================================

MAX_HISTORY = 180          # 保留 180 个采样点(约 3 分钟)

STRESS_SN = ""          # 压测实例的 sn, 由 main() 设定

STATE = {
    "broker": "",
    "sn": "rk3588-01",
    "connected": False,
    "status_count": 0,
    "load_count": 0,
    "last_status": None,
    "last_load": None,
    "last_error": "",
    "updated_at": 0.0,
    "engine": {"mpp": True, "rga": True, "npu": True},
    "stress_on": False,
    "stress_time": 0.0,
    # ★ 这两个键必须一开始就存在: 压测实例的状态报文到达时会直接 += 1 / 赋值,
    #   缺键会抛 KeyError, 被外层的 except 吃掉 -> 表现是"MQTT 连接断开"反复重连,
    #   而且压测实例的状态永远合不进历史曲线。
    "stress_status_count": 0,
    "last_stress_status": None,
    "decoder": "",
    "rga_mode": "",
    "control_sent": None,      # 最近一次下发的指令(给页面回显)
    "control_time": 0.0,
}
HISTORY: "collections.deque" = collections.deque(maxlen=MAX_HISTORY)
# ★ 必须是 RLock(可重入): mqtt_thread 持锁时会再调用 merge_history(),
#   而 merge_history 内部也要加锁 —— 用普通 Lock 会自己把自己锁死
#   (踩过: 第一条 status 报文进来就死锁, 页面一直转圈)。
LOCK = threading.RLock()

# ===========================================================================
# 视频源: RTSP 地址 / 连通性探测 / 拉流(带假死检测 + 自动重连)
# ===========================================================================
#
# ★ 为什么要有这一整段:
#   现场最常见的故障不是程序挂了, 而是【网段变了】—— 路由器从 192.168.31.x
#   换成 192.168.8.x, 相机和板子的 IP 全变了, 代码里写死的 RTSP 地址就废了,
#   页面只显示一句"视频源连接失败", 现场根本不知道是地址错了还是相机没开机。
#   所以这个地址必须能【现场手填 + 立刻测通不通 + 自动重连】, 而不是改代码重编译。

DEFAULT_RTSP = "rtsp://192.168.8.200:8557/h264"   # 相机默认地址(网段已变更为 192.168.8.x)
PROBE_TIMEOUT = 1.5      # 连通性测试的 TCP 握手超时(秒)—— 要"立刻"出红字, 不能等几十秒
HANG_TIMEOUT_S = 5.0      # 连续多久没收到画面就判定相机假死 -> 主动重连
# 重连退避(秒):
#   ① 【之前连上过, 突然断了】—— 多半是网线松了/相机重启, 快点重试
#   ② 【一直没连上过】—— 多半是地址错了/相机没开机, 别每秒去敲一次门:
#      每秒重试一次会刷爆内核日志(ARP 失败会刷 "detected local route"),
#      也会占着 CPU。慢慢退避到 30 秒一次, 现场改完地址点【应用】立刻生效。
RECONNECT_BACKOFF = (1.0, 2.0, 4.0, 8.0)           # 连上过又断了
RECONNECT_BACKOFF_COLD = (5.0, 10.0, 20.0, 30.0)   # 一直没连上过
RTSP_CONFIG_FILE = ""     # 由 main() 设定: 保存用户填的地址, 重启控制台还在

# 1x1 灰底 JPEG —— MJPEG 流在第一帧到达前先发它, 让浏览器别显示"坏图"图标
PLACEHOLDER_JPEG = bytes.fromhex(
    "ffd8ffe000104a46494600010100000100010000ffdb004300"
    "08060607060508070707090909080a0c140d0c0b0b0c1912130f"
    "141d1a1f1e1d1a1c1c20242e2720222c231c1c2837292c303134"
    "34341f27393d38323c2e333432ffc0000b080001000101011100"
    "ffc40014000100000000000000000000000000000009ffc40014"
    "01000000000000000000000000000000000000ffda0008010100003f"
    "00d2cf20ffd9")

if HAVE_CV2:                              # 有 OpenCV 就画一张 16:9 的占位图, 比 1x1 体面
    try:
        import numpy as np
        _ph = np.zeros((360, 640, 3), np.uint8)
        _ph[:] = (16, 20, 26)
        cv2.putText(_ph, "waiting for video source", (170, 185),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.7, (110, 125, 140), 1)
        PLACEHOLDER_JPEG = cv2.imencode(".jpg", _ph,
                                        [int(cv2.IMWRITE_JPEG_QUALITY), 80])[1].tobytes()
    except Exception:  # noqa: BLE001
        pass


def parse_rtsp_url(url: str) -> tuple[str, int, str]:
    """rtsp://host:port/path -> (host, port, path)。也认不带 scheme 的写法。"""
    u = (url or "").strip()
    if not u:
        raise ValueError("RTSP 地址为空")
    if "://" in u:
        scheme, rest = u.split("://", 1)
        if scheme.lower() not in ("rtsp", "rtsps"):
            raise ValueError(f"只支持 rtsp:// 地址(现在填的是 {scheme}://)")
    else:
        rest = u
    if "/" in rest:
        hostport, path = rest.split("/", 1)
        path = "/" + path
    else:
        hostport, path = rest, "/"
    if "@" in hostport:                      # rtsp://user:pass@host:port
        hostport = hostport.rsplit("@", 1)[1]
    if ":" in hostport:
        host, _, pstr = hostport.rpartition(":")
        try:
            port = int(pstr)
        except ValueError:
            raise ValueError(f"端口号不合法: {pstr}") from None
    else:
        host, port = hostport, 554
    if not host:
        raise ValueError("地址里没有 IP 或域名")
    if not (0 < port < 65536):
        raise ValueError(f"端口号超出范围: {port}")
    return host, port, path


def probe_rtsp(url: str, timeout: float = PROBE_TIMEOUT) -> dict:
    """连通性测试: TCP 握手 + 一句 RTSP OPTIONS。

    分两级是为了给现场【可区分】的结论:
      · TCP 都握不上 -> "IP 或端口不可达"(网段变了/相机没开机/防火墙)
      · TCP 握上了但 RTSP 不回包 -> "端口通但不是 RTSP 服务"(地址填错了端口)
    """
    res = {"url": url, "ok": False, "tcp_ok": False, "rtsp_ok": False,
           "latency_ms": None, "host": "", "port": 0, "error": "", "detail": ""}
    try:
        host, port, _ = parse_rtsp_url(url)
    except ValueError as e:
        res["error"] = "地址格式不对"
        res["detail"] = str(e)
        return res
    res["host"], res["port"] = host, port

    t0 = time.monotonic()
    try:
        sock = socket.create_connection((host, port), timeout=timeout)
    except Exception as e:  # noqa: BLE001
        res["error"] = "IP 或端口不可达"
        res["detail"] = f"{type(e).__name__}: {e}"
        return res
    try:
        res["latency_ms"] = int((time.monotonic() - t0) * 1000)
        res["tcp_ok"] = True
        sock.settimeout(timeout)
        sock.sendall((f"OPTIONS {url} RTSP/1.0\r\nCSeq: 1\r\n"
                      f"User-Agent: rk3588-console/1.0\r\n\r\n").encode())
        try:
            resp = sock.recv(256)
        except socket.timeout:
            resp = b""
        if resp.startswith(b"RTSP/"):
            res["rtsp_ok"] = True
            res["ok"] = True
            res["error"] = ""
            res["detail"] = resp.split(b"\r\n")[0].decode("utf-8", "replace")
        elif not resp:
            res["error"] = "端口通，但 RTSP 无响应"
            res["detail"] = ("TCP 握手成功, 但 OPTIONS 没回包 —— "
                             "这个端口可能不是 RTSP 服务, 或者被防火墙拦了")
        else:
            res["error"] = "端口通，但不是 RTSP 服务"
            res["detail"] = resp[:120].decode("utf-8", "replace")
    finally:
        try:
            sock.close()
        except OSError:
            pass
    return res


# ---------------------------------------------------------------------------
# 拉流: 两条路
#   ① 有 OpenCV -> 直接出画面(顺便用"多久没出帧"判断假死)
#   ② 没 OpenCV -> 纯标准库 RTSP 握手, 用"多久没收到 interleaved 数据"判断假死
#      (现场 PC 上经常没装 opencv, 但"假死检测"这种能力不能因此就没有)
# ---------------------------------------------------------------------------
class _Cv2Pull:
    """OpenCV 拉流: read() 返回一帧(BGR numpy)或者 None(这一轮没拿到)。"""

    def __init__(self, url: str):
        params = []
        if hasattr(cv2, "CAP_PROP_OPEN_TIMEOUT_MSEC"):
            params.append(cv2.CAP_PROP_OPEN_TIMEOUT_MSEC)
            params.append(3000)
        if hasattr(cv2, "CAP_PROP_READ_TIMEOUT_MSEC"):
            params.append(cv2.CAP_PROP_READ_TIMEOUT_MSEC)
            params.append(2000)
        cap = cv2.VideoCapture(url, cv2.CAP_FFMPEG, params)
        if not cap.isOpened():
            raise ConnectionError(f"OpenCV 打不开 {url}(相机没开机 / 网段不对 / 端口没监听)")
        self.cap = cap

    def read(self):
        ok, frame = self.cap.read()
        return frame if ok else None

    def close(self):
        try:
            self.cap.release()
        except Exception:  # noqa: BLE001
            pass


class _StdlibPull:
    """纯标准库 RTSP 客户端: 只关心"码流还在不在动", 不解 H.264。

    握手顺序 OPTIONS -> DESCRIBE -> SETUP(TCP interleaved) -> PLAY,
    之后 socket 上收到任何 interleaved 数据就说明相机还在出码流。
    """

    def __init__(self, url: str, timeout: float = 3.0):
        self.url = url
        self.host, self.port, self.path = parse_rtsp_url(url)
        self.sock = socket.create_connection((self.host, self.port), timeout=timeout)
        self.sock.settimeout(1.0)
        self._buf = b""
        self._cseq = 1
        self._session = ""
        self._request("OPTIONS", url)
        sdp = self._request("DESCRIBE", url, {"Accept": "application/sdp"})
        control = self._control_from_sdp(sdp)
        base = url.rstrip("/")
        setup_url = f"{base}/{control.lstrip('/')}" if control else f"{base}/trackID=0"
        self._request("SETUP", setup_url,
                      {"Transport": "RTP/AVP/TCP;unicast;interleaved=0-1"})
        self._request("PLAY", url, {"Range": "npt=0.000-"})

    # ------------------------------------------------------------------
    @staticmethod
    def _control_from_sdp(sdp: str) -> str:
        for line in sdp.splitlines():
            line = line.strip()
            if line.startswith("a=control:"):
                return line.split(":", 1)[1].strip()
        return ""

    def _request(self, method: str, url: str, extra: dict | None = None) -> str:
        headers = {"CSeq": str(self._cseq), "User-Agent": "rk3588-console/1.0"}
        self._cseq += 1
        if extra:
            headers.update(extra)
        if self._session and method != "SETUP":
            headers["Session"] = self._session
        req = f"{method} {url} RTSP/1.0\r\n" + \
              "".join(f"{k}: {v}\r\n" for k, v in headers.items()) + "\r\n"
        self.sock.sendall(req.encode())
        return self._wait_response()

    def _wait_response(self) -> str:
        deadline = time.monotonic() + 3.0
        while True:
            while b"\r\n\r\n" not in self._buf:
                if time.monotonic() > deadline:
                    raise ConnectionError("RTSP 响应超时")
                chunk = self.sock.recv(4096)
                if not chunk:
                    raise ConnectionError("RTSP 连接被关闭")
                self._buf += chunk
            head, _, rest = self._buf.partition(b"\r\n\r\n")
            header = head.decode("utf-8", "replace")
            n = 0
            for line in header.splitlines():
                if line.lower().startswith("content-length:"):
                    try:
                        n = int(line.split(":", 1)[1].strip())
                    except ValueError:
                        n = 0
            if len(rest) < n:
                chunk = self.sock.recv(4096)
                if not chunk:
                    raise ConnectionError("RTSP 连接被关闭")
                self._buf += chunk
                continue
            self._buf = rest[n:]
            if " 200 " not in header.split("\r\n")[0]:
                raise ConnectionError(f"RTSP {method} 被拒绝: "
                                      f"{header.splitlines()[0] if header else '无响应'}")
            for line in header.splitlines():
                if line.lower().startswith("session:"):
                    self._session = line.split(":", 1)[1].strip().split(";")[0]
            return header

    def read(self):
        """有数据返回 True, 1 秒内没数据返回 None。"""
        try:
            chunk = self.sock.recv(65536)
        except socket.timeout:
            return False
        if not chunk:
            raise ConnectionError("RTSP 连接被关闭")
        self._buf += chunk
        # 只要收到字节就说明码流在动(内容不用解)
        return True

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class RtspSource:
    """拉 RTSP 的后台线程: 出画面 + 假死检测 + 自动重连。

    状态机: idle -> connecting -> streaming -> (reconnecting -> connecting)*
      · connecting  : 正在打开流
      · streaming   : 正在出画面(帧在更新)
      · reconnecting: 流断了/假死了, 正在重连(退避中)
      · failed      : 地址不合法之类, 重试也没用
    """

    def __init__(self, hang_timeout: float = HANG_TIMEOUT_S):
        self.hang_timeout = hang_timeout
        self._lock = threading.Lock()
        self._url = ""
        self._seq = 0
        self._jpeg = None
        self._frame_seq = 0
        self._stop = threading.Event()
        self._changed = threading.Event()
        self.state = "idle"
        self.last_error = ""
        self.reconnects = 0
        self.frames = 0
        self.last_frame_ts = 0.0
        self.started_at = 0.0
        self._ever_connected = False      # 之前有没有成功连上过(决定退避快慢)
        self._thread = threading.Thread(target=self._run, daemon=True)

    # ------------------------------------------------------------------
    def start(self) -> None:
        if not self._thread.is_alive():
            self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._changed.set()

    def set_url(self, url: str) -> None:
        new = (url or "").strip()
        with self._lock:
            changed = (new != self._url)
            self._url = new
            self._seq += 1
            if changed:
                # 换了地址就是从头开始: 状态回到连接中, 重连计数清零, 退避也从头算
                self.state = "connecting" if new else "idle"
                self.last_error = ""
                self.reconnects = 0
                self._ever_connected = False
        self._changed.set()

    def get_url(self) -> str:
        with self._lock:
            return self._url

    def snapshot(self) -> dict:
        with self._lock:
            return {
                "url": self._url,
                "state": self.state,
                "last_error": self.last_error,
                "reconnects": self.reconnects,
                "frames": self.frames,
                "last_frame_ts": self.last_frame_ts,
                "has_frame": self._jpeg is not None,
                "hang_timeout": self.hang_timeout,
            }

    def get_frame(self):
        """返回 (jpeg 字节, 帧序号); 没有帧返回 (None, 序号)。"""
        with self._lock:
            return self._jpeg, self._frame_seq

    # ------------------------------------------------------------------
    def _set_state(self, state: str, err: str = "", clear_err: bool = False) -> None:
        """切状态。

        ★ err 的处理有讲究: 失败原因要【一直留在页面上】,
          直到码流真的恢复 —— 早先的实现每次重试前把状态写成 "connecting"
          时顺手把错误信息清空了, 于是页面只显示"正在重连…"却说不出为什么。
        """
        with self._lock:
            self.state = state
            if clear_err:
                self.last_error = ""
            elif err:
                self.last_error = err

    def _mark_streaming(self) -> None:
        """收到帧了 -> 确认状态是"已连接"(只在需要时才动锁里的东西)。"""
        with self._lock:
            if self.state == "streaming" and not self.last_error:
                return
            self.state = "streaming"
            self.last_error = ""

    def _publish(self, frame) -> None:
        """frame 是 BGR numpy(有 cv2 时)。编码成 JPEG 给浏览器。"""
        if cv2 is None:
            return
        try:
            h, w = frame.shape[:2]
            if w > 960:                       # 预览不用全分辨率, 省带宽
                scale = 960.0 / w
                frame = cv2.resize(frame, (960, int(h * scale)),
                                   interpolation=cv2.INTER_AREA)
            ok, buf = cv2.imencode(".jpg", frame,
                                   [int(cv2.IMWRITE_JPEG_QUALITY), 70])
            if not ok:
                return
            data = buf.tobytes()
        except Exception:  # noqa: BLE001
            return
        with self._lock:
            self._jpeg = data
            self._frame_seq += 1
            self.frames += 1
            self.last_frame_ts = time.time()

    def _run(self) -> None:
        backoff_i = 0
        while not self._stop.is_set():
            url = self.get_url()
            if not url:
                self._set_state("idle")
                self._changed.wait(0.5)
                self._changed.clear()
                continue
            pull = None
            try:
                # ★ 状态别来回闪: 失败过一次之后, 徽章一直显示"正在重连",
                #   不要在"连接中"和"正在重连"之间每秒切一次(看着像抽风)。
                self._set_state("connecting" if self.reconnects == 0 else "reconnecting")
                self.started_at = time.time()
                pull = (_Cv2Pull(url) if HAVE_CV2 else _StdlibPull(url))
                self._set_state("streaming", clear_err=True)
                self._ever_connected = True
                backoff_i = 0
                last_frame = time.monotonic()
                while not self._stop.is_set():
                    if url != self.get_url():     # 用户换了地址 -> 立刻换过去
                        break
                    data = pull.read()
                    if data is None or data is False:
                        # 这一轮没拿到帧: 看看是不是假死
                        idle = time.monotonic() - last_frame
                        if idle > self.hang_timeout:
                            raise TimeoutError(
                                f"{self.hang_timeout:.0f} 秒没收到画面"
                                f"(相机假死 / 网线松了 / 相机重启中)")
                        continue
                    last_frame = time.monotonic()
                    if HAVE_CV2:
                        self._publish(data)
                    else:
                        with self._lock:
                            self.frames += 1
                            self.last_frame_ts = time.time()
                    # 码流在动 -> 状态就是"已连接"。
                    # ★ 这里要重新确认一次: 用户点了【应用】但地址没变时,
                    #   连接其实一直是好的, 状态不能停在"连接中…"(踩过:
                    #   徽章一直转, 画面却是好的)。
                    self._mark_streaming()
                try:
                    pull.close()
                except Exception:  # noqa: BLE001
                    pass
                continue
            except Exception as e:  # noqa: BLE001
                msg = str(e)
                # ★ 出错时一定要把 pull 关掉 —— read() 抛异常(比如对端把连接断了)
                #   的话, socket / VideoCapture 就没人管了, 反复重连会漏一堆 fd。
                if pull is not None:
                    try:
                        pull.close()
                    except Exception:  # noqa: BLE001
                        pass
                with self._lock:
                    self.reconnects += 1
                    was_connected = self._ever_connected
                self._set_state("reconnecting", msg)
                # 连上过又断了 -> 快速重试; 一直没连上 -> 慢速, 别刷爆日志
                seq = RECONNECT_BACKOFF if was_connected else RECONNECT_BACKOFF_COLD
                wait = seq[min(backoff_i, len(seq) - 1)]
                print(f"[web] 视频源 {url} 断开: {msg}; {wait:.0f} 秒后重连", flush=True)
                backoff_i += 1
                self._changed.wait(wait)
                self._changed.clear()


SOURCE = RtspSource()


def load_rtsp_config(path: str) -> str:
    """读上次保存的 RTSP 地址(读不到就返回空)。"""
    if not path:
        return ""
    try:
        with open(path, "r", encoding="utf-8") as f:
            obj = json.load(f)
        url = obj.get("url")
        if isinstance(url, str) and url.strip():
            return url.strip()
    except Exception:  # noqa: BLE001
        pass
    return ""


def save_rtsp_config(path: str, url: str) -> bool:
    if not path:
        return False
    try:
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump({"url": url, "updated_at": time.time()},
                      f, ensure_ascii=False, indent=2)
        os.replace(tmp, path)
        return True
    except Exception as e:  # noqa: BLE001
        print(f"[web] 保存 RTSP 配置失败: {e}", flush=True)
        return False


def merge_history(status: dict | None, load: dict | None) -> None:
    """把 status / load 两路报文合并成一条时间序列。(调用方已持锁)"""
    with LOCK:
        if status is not None:
            entry = {
                "t": status.get("ts", time.time()),
                "fps": status.get("fps", 0),
                "src_fps": status.get("src_fps", 0),
                "dec": status.get("dec_ms", 0),
                "rga": status.get("rga_ms", 0),
                "npu": status.get("npu_ms", 0),
                "post": status.get("post_ms", 0),
                "draw": status.get("draw_ms", 0),
                "wait": status.get("wait_ms", 0),
                "total": status.get("total_ms", 0),
                "objs": status.get("n_objs", 0),
                "frames": status.get("frames", 0),
                "decoder": status.get("decoder", ""),
                "rga_mode": status.get("rga_mode", ""),
            }
            # 报文自带的负载(主程序读的那几个接口)
            ld = status.get("load") or {}
            for k in ("npu", "npu_core", "npu_c0", "npu_c1", "npu_c2",
                      "rga", "vpu", "cpu", "dmc", "gpu"):
                entry["load_" + k] = ld.get(k, -1)
            # ★ 状态报文和负载报文是两个独立的 1Hz 流, 到达顺序不固定:
            #   状态先到 -> 新条目里分核是 -1, 要等下一次负载报文才有值;
            #   负载先到 -> 它更新的是【上一条】, 新条目又是 -1。
            #   于是网页上分核柱子会一秒一变(-1 <-> 真实值), 看起来在闪。
            #   办法: 把上一条【已经知道的】分核值带过来, 等新的负载报文覆盖。
            if HISTORY:
                prev = HISTORY[-1]
                for k in ("npu_c0", "npu_c1", "npu_c2"):
                    if entry.get("load_" + k, -1) < 0 and prev.get("load_" + k, -1) >= 0:
                        entry["load_" + k] = prev["load_" + k]
                if entry.get("npu_src", "none") == "devfreq" and prev.get("npu_src") == "rknpu":
                    entry["npu_src"] = "rknpu"
                if entry.get("load_rga", -1) < 0 and prev.get("load_rga", -1) >= 0:
                    entry["load_rga"] = prev["load_rga"]
                    entry["rga_src"] = prev.get("rga_src", "none")
            entry["npu_src"] = ld.get("npu_src", "none")
            entry["rga_src"] = ld.get("rga_src", "none")
            HISTORY.append(entry)
        elif load is not None:
            # 独立的负载报文(root 采样器发的): 更新最近一条的负载字段
            if HISTORY:
                e = HISTORY[-1]
                for k in ("npu", "npu_core", "npu_c0", "npu_c1", "npu_c2",
                          "rga", "vpu", "cpu", "dmc", "gpu"):
                    if load.get(k, -1) >= 0:
                        e["load_" + k] = load[k]
                if load.get("npu_src"):
                    e["npu_src"] = load["npu_src"]
                if load.get("rga_src"):
                    e["rga_src"] = load["rga_src"]


# ===========================================================================
# 后台线程: MQTT 订阅
# ===========================================================================

def mqtt_thread(client: MqttClient, topics: list[str]) -> None:
    while True:
        if not client.connected:
            if not client._connect_socket():
                time.sleep(2)
                continue
            try:
                for t in topics:
                    client._subscribe(t)
                print(f"[web] 已连接 {client.host}:{client.port}, 订阅 {topics}", flush=True)
            except Exception as e:  # noqa: BLE001
                client.last_error = str(e)
                client.connected = False
                time.sleep(2)
                continue

        try:
            hdr = client.sock.recv(1)  # type: ignore[union-attr]
            if not hdr:
                raise ConnectionError("连接被对端关闭")
            rl = client._read_len(client.sock)  # type: ignore[arg-type]
            body = client._read_exact(client.sock, rl) if rl else b""
            if (hdr[0] & 0xF0) != 0x30:
                continue
            tlen = struct.unpack("!H", body[:2])[0]
            topic = body[2:2 + tlen].decode("utf-8", "replace")
            off = 2 + tlen
            if ((hdr[0] & 0x06) >> 1) > 0:
                off += 2
            data = body[off:].decode("utf-8", "replace")
            try:
                obj = json.loads(data)
            except Exception:  # noqa: BLE001
                continue

            with LOCK:
                is_stress = STRESS_SN and ("/" + STRESS_SN + "/") in topic
                if topic.endswith("/status"):
                    if is_stress:
                        STATE["last_stress_status"] = obj
                        STATE["stress_status_count"] += 1
                        # ★ 压测开着时, 页面上的开关/后端要显示【压测实例】的,
                        #   否则点一下 MPP 看的是生产实例的开关, 演示时对不上。
                        if STATE.get("stress_on"):
                            STATE["engine"] = obj.get("engine", STATE["engine"])
                            STATE["decoder"] = obj.get("decoder", "")
                            STATE["rga_mode"] = obj.get("rga_mode", "")
                        merge_history(obj, None)
                    else:
                        STATE["last_status"] = obj
                        STATE["status_count"] += 1
                        # ★ 压测开着的时候, 生产实例【不能】再改开关状态:
                        #   两个实例每秒各报一次, 谁后到谁说了算 ->
                        #   页面上的开关会在"生产实例的 mpp=on"和"压测实例的 mpp=off"
                        #   之间来回跳, 演示时看到的开关跟实际生效的对不上(踩过)。
                        if not (STATE.get("stress_on") and STATE.get("last_stress_status")):
                            STATE["engine"] = obj.get("engine", STATE["engine"])
                            STATE["decoder"] = obj.get("decoder", "")
                            STATE["rga_mode"] = obj.get("rga_mode", "")
                        STATE["updated_at"] = time.time()
                        # ★ 压测开着的时候, 生产实例的报文【不进历史曲线】:
                        #   两个实例每秒各报一次, 混在一起画出来是两条锯齿线(16 / 96 交替),
                        #   看不清压测效果。压测时只留压测实例的。
                        if not (STATE.get("stress_on") and STATE.get("last_stress_status")):
                            merge_history(obj, None)
                elif topic.endswith("/load"):
                    STATE["load_count"] += 1
                    merge_history(None, obj)
        except socket.timeout:
            continue
        except Exception as e:  # noqa: BLE001
            client.last_error = str(e)
            client.connected = False
            print(f"[web] MQTT 连接断开: {e}, 2 秒后重连", flush=True)
            time.sleep(2)


# ===========================================================================
# HTTP 服务
# ===========================================================================

class Handler(BaseHTTPRequestHandler):
    server_version = "RKConsole/1.0"
    client: MqttClient = None       # type: ignore[assignment]
    ctl_topic: str = ""
    stress_ctl_topic: str = ""
    sn: str = ""

    def log_message(self, *a):
        pass

    def _send(self, code: int, body, ctype="application/json; charset=utf-8"):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    # -------------------------------------------------------------- GET
    def do_GET(self):
        # ★ 路由只看【路径部分】, 查询串要剥掉。
        #   以前直接拿 self.path 比对, 于是 "http://ip:8080/?x=1" 这种带参数的
        #   地址会返回 404(浏览器收藏带参数 / 调试时很容易踩到)。
        path = self.path.split("?", 1)[0].split("#", 1)[0]
        if path == "/" or path == "/index.html":
            # 页面里的默认地址由后端注入 —— 换网段只改后端一个常量
            self._send(200, PAGE.replace("__DEFAULT_RTSP__", DEFAULT_RTSP),
                       "text/html; charset=utf-8")
        elif path == "/api/state":
            with LOCK:
                s = {
                    "broker": STATE["broker"],
                    "sn": STATE["sn"],
                    "connected": STATE["connected"],
                    "status_count": STATE["status_count"],
                    "stress_on": STATE.get("stress_on", False),
                    "stress_sn": STATE.get("stress_sn", ""),
                    "stress_status_count": STATE.get("stress_status_count", 0),
                    "load_count": STATE["load_count"],
                    # 压测开着 -> 显示压测实例的状态(FPS 100+ 那份)
                    "status": (STATE.get("last_stress_status")
                               if STATE.get("stress_on") and STATE.get("last_stress_status")
                               else STATE["last_status"]),
                    "load": STATE["last_load"],
                    "engine": STATE["engine"],
                    "decoder": STATE["decoder"],
                    "rga_mode": STATE["rga_mode"],
                    "control_sent": STATE["control_sent"],
                    "control_time": STATE["control_time"],
                    "updated_at": STATE["updated_at"],
                    # ★ 视频源状态: 前端靠它显示"正在重连"/"不可达"
                    "rtsp": SOURCE.snapshot(),
                    "rtsp_default": DEFAULT_RTSP,
                    "has_cv2": HAVE_CV2,
                    "history": list(HISTORY),
                }
            self._send(200, json.dumps(s, ensure_ascii=False))
        elif path == "/api/rtsp":
            # 当前视频源配置 + 实时状态(页面首次加载时拉一次)
            self._send(200, json.dumps({
                "ok": True,
                "url": SOURCE.get_url(),
                "default": DEFAULT_RTSP,
                "has_cv2": HAVE_CV2,
                "config_file": RTSP_CONFIG_FILE,
                "state": SOURCE.snapshot(),
            }, ensure_ascii=False))
        elif path == "/api/video.mjpeg":
            self._stream_mjpeg()
        elif path == "/api/video/snapshot.jpg":
            jpeg, _ = SOURCE.get_frame()
            if jpeg is None:
                self._send(404, json.dumps({"error": "还没有画面"}), "text/plain")
            else:
                self._send(200, jpeg, "image/jpeg")
        elif path == "/api/health":
            self._send(200, json.dumps({"ok": True, "sn": self.sn}))
        elif path == "/favicon.ico":
            self._send(204, b"", "image/x-icon")
        else:
            self._send(404, json.dumps({"error": "not found"}))

    # -------------------------------------------------------------- 视频源
    def _handle_rtsp(self, path: str):
        """``/api/rtsp`` 换源 / ``/api/rtsp/test`` 连通性测试。

        连通性测试是【同步】做的: 1.5 秒内 TCP 握不上就立刻回红字,
        不让页面"转圈半天才报超时"。
        """
        try:
            n = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(n) if n else b""
            obj = json.loads(raw.decode("utf-8") or "{}")
        except Exception as e:  # noqa: BLE001
            self._send(400, json.dumps({"ok": False, "error": str(e)},
                                       ensure_ascii=False))
            return
        url = (obj.get("url") or "").strip()

        if path == "/api/rtsp/test":
            res = probe_rtsp(url)
            self._send(200, json.dumps(res, ensure_ascii=False))
            return

        # /api/rtsp —— 应用新的地址
        try:
            host, port, _ = parse_rtsp_url(url)
        except ValueError as e:
            self._send(400, json.dumps({"ok": False, "error": str(e)},
                                       ensure_ascii=False))
            return
        SOURCE.set_url(url)
        saved = save_rtsp_config(RTSP_CONFIG_FILE, url)
        print(f"[web] 视频源切换为 {url} (host={host} port={port}, "
              f"已保存={'是' if saved else '否'})", flush=True)
        self._send(200, json.dumps({"ok": True, "url": url, "host": host,
                                    "port": port, "saved": saved,
                                    "state": SOURCE.snapshot()},
                                   ensure_ascii=False))

    # -------------------------------------------------------------- MJPEG
    def _stream_mjpeg(self):
        """multipart/x-mixed-replace 推 JPEG 帧(浏览器 <img> 直接吃)。

        ★ 帧是后台线程解码好的, 这里只负责"取最新的那一张"往外发 ——
          所以拉流慢不会拖住 HTTP 响应, 也不会让页面卡住。
        """
        self.send_response(200)
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
        self.send_header("Pragma", "no-cache")
        self.end_headers()
        last_seq = -1
        try:
            while True:
                jpeg, seq = SOURCE.get_frame()
                if jpeg is None:
                    # 还没出画面: 先发一张占位图, 免得浏览器显示"坏图"
                    if last_seq != -2:
                        self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\n"
                                         b"Content-Length: %d\r\n\r\n" % len(PLACEHOLDER_JPEG)
                                         + PLACEHOLDER_JPEG + b"\r\n")
                        last_seq = -2
                    time.sleep(0.25)
                    continue
                if seq == last_seq:
                    time.sleep(0.05)
                    continue
                last_seq = seq
                self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\n"
                                 b"Content-Length: %d\r\n\r\n" % len(jpeg)
                                 + jpeg + b"\r\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass        # 浏览器关掉了页面, 正常

    # -------------------------------------------------------------- POST
    def do_POST(self):
        path = self.path.split("?", 1)[0].split("#", 1)[0]
        if path in ("/api/rtsp", "/api/rtsp/test"):
            self._handle_rtsp(path)
            return
        if path == "/api/stress":
            # 【压力测试】按钮: 让板端代理起一个极限并发的进程。
            # 走 MQTT 下行, 所以控制台跑在 PC 上也能用。
            try:
                n = int(self.headers.get("Content-Length", "0"))
                raw = self.rfile.read(n) if n else b""
                obj = json.loads(raw.decode("utf-8") or "{}")
            except Exception as e:  # noqa: BLE001
                self._send(400, json.dumps({"ok": False, "error": str(e)}))
                return
            on = obj.get("on", True)
            on = True if isinstance(on, str) else bool(on)
            payload = json.dumps({"cmd": "stress", "on": on})
            # ★ 压测指令要发给【压测实例】的控制主题(它订阅的是 <sn>-stress 那个);
            #   发到生产实例的主题上, 压测代理收不到。
            ok = self.client.publish(self.stress_ctl_topic, payload)
            with LOCK:
                STATE["stress_on"] = on
                STATE["stress_time"] = time.time()
            self._send(200 if ok else 503,
                       json.dumps({"ok": ok, "stress": on, "topic": self.stress_ctl_topic,
                                   "error": "" if ok else self.client.last_error},
                                  ensure_ascii=False))
            return
        if path != "/api/control":
            self._send(404, json.dumps({"error": "not found"}))
            return
        try:
            n = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(n) if n else b""
            obj = json.loads(raw.decode("utf-8") or "{}")
        except Exception as e:  # noqa: BLE001
            self._send(400, json.dumps({"ok": False, "error": str(e)}))
            return

        # 只认三个开关字段; 缺省就沿用当前状态
        with LOCK:
            cur = dict(STATE["engine"])
        want = {}
        for k in ("mpp", "rga", "npu"):
            v = obj.get(k)
            if isinstance(v, bool):
                want[k] = v
            elif isinstance(v, (int, float)) and v in (0, 1):
                want[k] = bool(v)
            elif isinstance(v, str) and v.lower() in ("true", "false", "on", "off", "1", "0"):
                want[k] = v.lower() in ("true", "on", "1")
            else:
                want[k] = cur.get(k, True)
        if not want:
            self._send(400, json.dumps({"ok": False, "error": "没有可识别的开关字段"}))
            return

        payload = json.dumps({"cmd": "set_engine",
                              "mpp": want["mpp"], "rga": want["rga"], "npu": want["npu"]})
        # ★ 压测开着的时候, 开关必须下发给压测实例 —— 这正是演示要的:
        #   在网页上关掉 MPP, 压测实例立刻从 100fps 掉下来、CPU 飙上去。
        with LOCK:
            stress_on = bool(STATE.get("stress_on"))
        topic = self.stress_ctl_topic if stress_on else self.ctl_topic
        ok = self.client.publish(topic, payload)
        with LOCK:
            STATE["control_sent"] = want
            STATE["control_time"] = time.time()
        self._send(200 if ok else 503,
                   json.dumps({"ok": ok, "sent": want,
                               "error": "" if ok else self.client.last_error},
                              ensure_ascii=False))


# ===========================================================================
# 前端页面
# ===========================================================================

PAGE = r"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>RK3588 流水线控制台</title>
<style>
  :root{
    --bg:#0b0e13; --card:#141922; --line:#232a35; --fg:#e6edf3; --dim:#8b949e;
    --ok:#3fb950; --warn:#d29922; --bad:#f85149; --accent:#58a6ff; --violet:#bc8cff;
  }
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--fg);
       font:14px/1.6 -apple-system,"Noto Sans CJK SC","Microsoft YaHei",sans-serif}
  header{padding:12px 22px;border-bottom:1px solid var(--line);background:#0e1218;
         display:flex;align-items:center;gap:16px;flex-wrap:wrap}
  h1{font-size:15px;margin:0;font-weight:600;letter-spacing:.02em}
  .dot{width:9px;height:9px;border-radius:50%;background:#555;display:inline-block}
  .dot.on{background:var(--ok);box-shadow:0 0 8px var(--ok)}
  .dot.off{background:var(--bad)}
  .meta{color:var(--dim);font-size:12px}
  .meta b{color:var(--fg);font-weight:600}
  main{padding:16px 22px;display:flex;flex-direction:column;gap:14px}
  .row{display:grid;gap:14px}
  .row.ctrl{grid-template-columns:minmax(430px,1fr) minmax(430px,1.2fr)}
  .row.charts{grid-template-columns:1fr 1fr}
  @media(max-width:1100px){.row.ctrl,.row.charts{grid-template-columns:1fr}}
  .card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px 16px}
  .card h2{font-size:12px;margin:0 0 12px;color:var(--dim);font-weight:600;
           text-transform:uppercase;letter-spacing:.08em}
  /* ---- 开关 ---- */
  .switch{display:flex;align-items:center;gap:12px;padding:9px 0;
          border-bottom:1px solid #1d232c}
  .switch:last-child{border-bottom:none}
  .switch .info{flex:1;min-width:0}
  .switch .nm{font-weight:600;font-size:14px}
  .switch .ds{color:var(--dim);font-size:12px;margin-top:2px}
  /* ---- 开关 ----
     ★ 视觉必须【跟着"当前生效值"走】, 不能被"下发中"这个临时状态覆盖:
       以前 .toggle.busy 直接把背景刷成黄色、滑块硬推到右边 ——
       于是"开"和"关"两个状态在页面上长得一模一样(都是黄+滑块在右),
       用户点完看不出自己到底开了还是关了。
       现在: 颜色和滑块位置只由 on/off 决定; "下发中"只是外面一圈呼吸光晕,
       不改变开关本身的读数。 */
  .toggle{position:relative;width:46px;height:25px;flex:0 0 auto;cursor:pointer;
          border-radius:13px;background:#30363d;transition:background .18s, box-shadow .18s}
  .toggle.on{background:var(--ok)}
  .toggle.off{background:#39414d}
  .toggle.disabled{opacity:.4;cursor:not-allowed}
  @keyframes tg-pulse{0%{box-shadow:0 0 0 0 rgba(210,153,34,.55)}
                       100%{box-shadow:0 0 0 7px rgba(210,153,34,0)}}
  .toggle.busy{animation:tg-pulse 1.1s ease-out infinite}
  .toggle::after{content:"";position:absolute;top:3px;left:3px;width:19px;height:19px;
          border-radius:50%;background:#fff;transition:transform .18s, background .18s}
  .toggle.on::after{transform:translateX(21px)}
  .toggle.off::after{transform:translateX(0)}
  /* 开关旁边的文字: 开/关/待确认, 颜色跟着开关走 —— 颜色盲也能读 */
  .tstate{font-size:12px;font-weight:600;min-width:56px;text-align:right;
          font-family:"DejaVu Sans Mono",monospace}
  .tstate.on{color:var(--ok)}
  .tstate.off{color:var(--dim)}
  .tstate.pending{color:var(--warn)}
  /* 页面底部: 数据来源标注(打消"数据是不是真的"的质疑) */
  .srcline{margin-top:4px;color:var(--dim);font-size:11.5px;line-height:1.7;
           border-top:1px solid var(--line);padding-top:12px}
  .srcline code{background:#0f141b;border:1px solid var(--line);border-radius:4px;
                padding:1px 5px;color:#c9d1d9;font-size:11px}
  /* ---- KPI ---- */
  .kpis{display:grid;grid-template-columns:repeat(4,1fr);gap:10px}
  @media(max-width:700px){.kpis{grid-template-columns:repeat(2,1fr)}}
  .kpi{background:#0f141b;border:1px solid var(--line);border-radius:8px;padding:9px 11px}
  .kpi .k{color:var(--dim);font-size:11px;letter-spacing:.04em}
  .kpi .v{font-size:21px;font-weight:700;font-family:"DejaVu Sans Mono",monospace;
          margin-top:1px;line-height:1.25}
  .kpi .u{font-size:11px;color:var(--dim);font-weight:400;margin-left:3px}
  .kpi.hl .v{color:var(--accent)}
  /* ---- 图表 ---- */
  .chartbox{position:relative;height:210px}
  canvas{width:100%;height:100%;display:block}
  pre{white-space:pre-wrap;word-break:break-all;font:11.5px/1.5 "DejaVu Sans Mono",monospace;
      color:#c9d1d9;background:#0d1117;border:1px solid var(--line);border-radius:6px;
      padding:10px;margin:0;max-height:230px;overflow:auto}
  .toast{position:fixed;right:18px;bottom:18px;background:#1c2128;border:1px solid var(--line);
         border-left:3px solid var(--accent);padding:9px 14px;border-radius:6px;
         font-size:13px;opacity:0;transition:opacity .25s;pointer-events:none;max-width:420px}
  .toast.show{opacity:1}
  .toast.err{border-left-color:var(--bad)}
  .toast.warn{border-left-color:var(--warn)}
  .legendnote{color:var(--dim);font-size:11px;margin-top:6px}
  /* ---- NPU 分核仪表盘 ---- */
  .cores{display:flex;flex-direction:column;gap:9px}
  .core{display:flex;align-items:center;gap:10px}
  .clabel{width:52px;color:var(--dim);font-size:11.5px;font-family:"DejaVu Sans Mono",monospace}
  .bar{flex:1;height:16px;background:#0f141b;border:1px solid var(--line);border-radius:8px;overflow:hidden}
  .bar i{display:block;height:100%;width:0;background:linear-gradient(90deg,#1f6feb,#58a6ff);
         border-radius:8px;transition:width .35s ease}
  .bar i.hot{background:linear-gradient(90deg,#bc4c00,#f78166)}
  .bar i.warm{background:linear-gradient(90deg,#9e6a03,#d29922)}
  .cval{width:52px;text-align:right;font-family:"DejaVu Sans Mono",monospace;font-size:12.5px}
  /* ---- 流水线漏斗 ---- */
  .funnel{display:flex;flex-direction:column;gap:8px}
  .fn{display:flex;align-items:center;gap:10px}
  .fnl{width:52px;color:var(--dim);font-size:11.5px}
  .fnbar{flex:1;height:18px;background:#0f141b;border:1px solid var(--line);border-radius:4px;overflow:hidden}
  .fnbar i{display:block;height:100%;width:0;background:#2f6fb5;border-radius:4px;transition:width .35s ease}
  .fnv{width:74px;text-align:right;font-family:"DejaVu Sans Mono",monospace;font-size:12.5px}
  /* ---- 压力测试按钮 ---- */
  .stressrow{display:flex;align-items:center;gap:12px;margin-top:12px;
              padding-top:12px;border-top:1px dashed var(--line)}
  .stressbtn{background:#1f6feb;color:#fff;border:1px solid #2f81f7;border-radius:7px;
             padding:8px 15px;font-size:13px;font-weight:600;cursor:pointer;
             font-family:inherit;transition:background .18s}
  .stressbtn:hover{background:#2f81f7}
  .stressbtn.on{background:#bc4c00;border-color:#f78166}
  .stressbtn.busy{opacity:.6;cursor:progress}
  /* ---- 视频源(现场手填 RTSP 地址) ---- */
  .srcgrid{display:grid;grid-template-columns:minmax(380px,1fr) 420px;
           gap:18px;align-items:start}
  @media(max-width:1100px){.srcgrid{grid-template-columns:1fr}}
  .srcrow{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
  .srcurl{flex:1;min-width:260px;background:#0f141b;border:1px solid var(--line);
          border-radius:7px;color:var(--fg);padding:9px 11px;
          font:13px "DejaVu Sans Mono",monospace}
  .srcurl:focus{outline:none;border-color:var(--accent);box-shadow:0 0 0 2px rgba(88,166,255,.15)}
  .srcbtn{background:#21262d;color:var(--fg);border:1px solid var(--line);border-radius:7px;
          padding:9px 14px;font-size:13px;font-weight:600;cursor:pointer;font-family:inherit;
          white-space:nowrap}
  .srcbtn:hover{background:#2a313a}
  .srcbtn.primary{background:#1f6feb;border-color:#2f81f7;color:#fff}
  .srcbtn.primary:hover{background:#2f81f7}
  .srcbtn.busy{opacity:.6;cursor:progress}
  .srchint{color:var(--dim);font-size:12px;margin-top:10px;line-height:1.65}
  .srcmsg{margin-top:10px;font-size:12.5px;font-weight:600;min-height:19px;line-height:1.5}
  .srcmsg.bad{color:var(--bad)}
  .srcmsg.good{color:var(--ok)}
  .srcmsg.warn{color:var(--warn)}
  .srcstate{display:inline-block;font-size:12px;font-weight:600;padding:3px 10px;
            border-radius:20px;border:1px solid var(--line);color:var(--dim)}
  .srcstate.on{color:var(--ok);border-color:var(--ok)}
  .srcstate.warn{color:var(--warn);border-color:var(--warn)}
  .srcstate.bad{color:var(--bad);border-color:var(--bad)}
  .vidbox{position:relative;background:#05080c;border:1px solid var(--line);border-radius:8px;
          aspect-ratio:16/9;overflow:hidden;display:flex;align-items:center;justify-content:center}
  .vidbox img{width:100%;height:100%;object-fit:contain;display:block}
  .vidoverlay{position:absolute;inset:0;display:flex;flex-direction:column;gap:6px;
              align-items:center;justify-content:center;
              color:var(--dim);font-size:13px;background:rgba(5,8,12,.86);text-align:center;
              padding:16px;line-height:1.7}
  .vidoverlay small{display:block;color:#6d7f96;font-size:11.5px;
                    font-family:"DejaVu Sans Mono",monospace}
  .vidoverlay.hide{display:none}
</style>
</head>
<body>
<header>
  <h1>RK3588 流水线控制台</h1>
  <span class="dot" id="dot"></span>
  <span class="meta" id="conn">连接中…</span>
  <span class="meta">设备 <b id="sn">-</b></span>
  <span class="meta">Broker <b id="broker">-</b></span>
  <span class="meta">更新 <b id="upd">-</b></span>
</header>

<main>
  <div class="card" id="card-rtsp">
    <h2>视频源(现场可换地址, 不用改代码重编译)</h2>
    <div class="srcgrid">
      <div>
        <div class="srcrow">
          <input id="rtsp-url" class="srcurl" type="text" spellcheck="false"
                 autocomplete="off" placeholder="__DEFAULT_RTSP__">
          <button class="srcbtn" id="btn-rtsp-test">测试连接</button>
          <button class="srcbtn primary" id="btn-rtsp-apply">应用</button>
        </div>
        <div class="srchint">
          如果路由器网段变更, <b>请在此处填入摄像头的最新 RTSP 地址</b> ——
          填完先点【测试连接】, 1.5 秒内就知道通不通(不通会直接红字提示"IP 或端口不可达"),
          通了再点【应用】。地址会存在浏览器和后端, 下次打开还是它。
        </div>
        <div class="srcmsg" id="rtsp-msg"></div>
        <div class="srcrow" style="margin-top:10px">
          <span class="srcstate" id="rtsp-state">-</span>
          <span class="meta" id="rtsp-detail"></span>
        </div>
      </div>
      <div class="vidbox">
        <img id="vid" src="/api/video.mjpeg" alt="视频预览">
        <div class="vidoverlay" id="vid-overlay">未连接视频源</div>
      </div>
    </div>
  </div>

  <div class="row ctrl">
    <div class="card">
      <h2>硬件引擎开关(点一下立刻生效)</h2>
      <div class="switch">
        <div class="info">
          <div class="nm">MPP 硬解</div>
          <div class="ds">关掉 = 改用 FFmpeg 软解, 解码耗时飙升、FPS 暴跌</div>
        </div>
        <span class="tstate" id="st-mpp">-</span>
        <div class="toggle" id="sw-mpp" data-key="mpp"></div>
      </div>
      <div class="switch">
        <div class="info">
          <div class="nm">RGA 硬件加速</div>
          <div class="ds">关掉 = 改用 OpenCV CPU 缩放, 预处理变慢、CPU 占用上升</div>
        </div>
        <span class="tstate" id="st-rga">-</span>
        <div class="toggle" id="sw-rga" data-key="rga"></div>
      </div>
      <div class="switch">
        <div class="info">
          <div class="nm">NPU 推理</div>
          <div class="ds">关掉 = 只跑解码+预处理, 不出检测框, 负载归零。<b>仅用于看视频转发能力, 不参与性能对比</b>(对比时必须保持检测+识别在跑)</div>
        </div>
        <span class="tstate" id="st-npu">-</span>
        <div class="toggle" id="sw-npu" data-key="npu"></div>
      </div>
      <div class="legendnote" id="ctlNote">开关状态以板端上报为准(点一下立刻生效, 页面先乐观更新, 板端确认或报错后回滚)</div>
      <div class="stressrow">
        <button class="stressbtn" id="btn-stress">⚡ 压力测试(极限并发)</button>
        <span class="meta" id="stressNote">一键启动多路并发极限压测(不限速 + 三核并行 + <b>检测与车牌识别全开</b>), 用于展示算力瓶颈; 可在同一页面逐个关闭 MPP 硬解 / RGA 预处理, 观察 FPS 与 CPU 的真实变化</span>
      </div>
    </div>

    <div class="card">
      <h2>实时指标</h2>
      <div class="kpis">
        <div class="kpi hl"><div class="k">流水线 FPS</div><div class="v" id="k-fps">-<span class="u">fps</span></div></div>
        <div class="kpi"><div class="k">源推送 FPS</div><div class="v" id="k-srcfps">-<span class="u">fps</span></div></div>
        <div class="kpi"><div class="k">解码</div><div class="v" id="k-dec">-<span class="u">ms</span></div></div>
        <div class="kpi"><div class="k">RGA 预处理</div><div class="v" id="k-rga">-<span class="u">ms</span></div></div>
        <div class="kpi"><div class="k">NPU 推理</div><div class="v" id="k-npu">-<span class="u">ms</span></div></div>
        <div class="kpi"><div class="k">单帧总耗时</div><div class="v" id="k-total">-<span class="u">ms</span></div></div>
        <div class="kpi"><div class="k">目标数</div><div class="v" id="k-objs">-</div></div>
        <div class="kpi"><div class="k">解码后端 / RGA</div><div class="v" id="k-mode" style="font-size:15px">-</div></div>
      </div>
    </div>
  </div>

  <div class="row charts">
    <div class="card">
      <h2>NPU 分核负载(内核 rknpu/load 真值)</h2>
      <div class="cores">
        <div class="core"><div class="clabel">Core0</div><div class="bar"><i id="bar-c0"></i></div><div class="cval" id="val-c0">-</div></div>
        <div class="core"><div class="clabel">Core1</div><div class="bar"><i id="bar-c1"></i></div><div class="cval" id="val-c1">-</div></div>
        <div class="core"><div class="clabel">Core2</div><div class="bar"><i id="bar-c2"></i></div><div class="cval" id="val-c2">-</div></div>
      </div>
      <div class="legendnote" id="coreSrc">-</div>
    </div>
    <div class="card">
      <h2>流水线耗时漏斗(每帧 ms)</h2>
      <div class="funnel" id="funnel">
        <div class="fn"><div class="fnl">解码</div><div class="fnbar"><i id="fn-dec"></i></div><div class="fnv" id="v-dec">-</div></div>
        <div class="fn"><div class="fnl">预处理</div><div class="fnbar"><i id="fn-rga"></i></div><div class="fnv" id="v-rga">-</div></div>
        <div class="fn"><div class="fnl">推理</div><div class="fnbar"><i id="fn-npu"></i></div><div class="fnv" id="v-npu">-</div></div>
        <div class="fn"><div class="fnl">后处理</div><div class="fnbar"><i id="fn-post"></i></div><div class="fnv" id="v-post">-</div></div>
      </div>
      <div class="legendnote" id="fnNote">-</div>
    </div>
  </div>

  <div class="card">
    <h2>FPS 与单帧总耗时</h2>
    <div class="chartbox"><canvas id="c1"></canvas></div>
  </div>

  <div class="row charts">
    <div class="card">
      <h2>各阶段耗时(ms)</h2>
      <div class="chartbox"><canvas id="c2"></canvas></div>
    </div>
    <div class="card">
      <h2>硬件负载(%)</h2>
      <div class="chartbox"><canvas id="c3"></canvas></div>
      <div class="legendnote" id="loadSrc">-</div>
    </div>
  </div>

  <div class="card">
    <h2>板端原始状态报文</h2>
    <pre id="raw">等待数据…</pre>
  </div>

  <div class="card">
    <div class="srcline">
      * <b>数据来源</b>：底层 C++ 程序直连 RK3588 内核节点
      <code>/sys/kernel/debug/rknpu/load</code>（NPU 三核真值）、
      <code>/sys/kernel/debug/rkga/load</code>（RGA 真值）、
      <code>/sys/class/devfreq/*</code>（DDR / VPU / DMC / GPU）与
      <code>/proc/stat</code>（CPU），经 MQTT 上报
      <code>device/&lt;sn&gt;/message/up/status</code>，页面只做展示，不做任何二次加工。<br>
      * <b>控制链路</b>：网页点击开关 → <code>POST /api/control</code> → MQTT 下行
      <code>device/&lt;sn&gt;/message/down/control</code> → 板端 C++ 原子开关 → <b>下一帧即生效</b>
      （不是重启进程，也不是模拟数值）。页面采用乐观更新：先按目标状态画，等板端上报确认后固化，
      报错或 5 秒未确认则自动回滚。<br>
      * <b>压力测试</b>：一键启动多路并发极限压测（不限速 + 三核并行 + <b>检测与车牌识别全开</b>），
      用于展示算力瓶颈。性能对比只做「满负荷业务下单独关闭某一个硬件单元」，
      <b>不会用「关掉 NPU」这种没有业务含义的组参与对比</b>。<br>
      * <b>视频源</b>：RTSP 地址<b>可以现场手填</b>（存浏览器 + 后端，重启还在），
      不用改代码重编译 —— 路由器换网段时这是最省事的一条路。
      【测试连接】只做 TCP 握手，<b>1.5 秒</b>内给结论：不通直接红字
      「IP 或端口不可达」，不再让人对着"加载半天"的转圈圈猜。
      拉流线程带<b>假死检测</b>：<b>5 秒</b>没收到画面就判定相机/网线出问题，
      自动重连（退避 1→2→4→5 秒），页面同时给「正在重连」提示。
    </div>
  </div>
</main>

<div class="toast" id="toast"></div>

<script>
"use strict";
// =====================================================================
// 迷你图表(纯 canvas, 不引任何 CDN —— 现场 PC 经常没有外网)
// =====================================================================
function niceMax(v){
  if(!(v>0)) return 1;
  const e = Math.pow(10, Math.floor(Math.log10(v)));
  const f = v/e;
  let n;
  if(f<=1) n=1; else if(f<=2) n=2; else if(f<=5) n=5; else n=10;
  return n*e;
}
function fmt(v){
  if(v==null) return "-";
  if(Math.abs(v)>=100) return v.toFixed(0);
  if(Math.abs(v)>=10) return v.toFixed(1);
  return v.toFixed(2);
}
function drawChart(canvas, cfg){
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth, h = canvas.clientHeight;
  if(w<=0||h<=0) return;
  canvas.width = w*dpr; canvas.height = h*dpr;
  const g = canvas.getContext("2d");
  g.setTransform(dpr,0,0,dpr,0,0);
  g.clearRect(0,0,w,h);

  const hasRight = cfg.series.some(s=>s.axis==="right");
  const pad = {l:48, r:hasRight?48:14, t:16, b:22};
  const pw = w-pad.l-pad.r, ph = h-pad.t-pad.b;
  if(pw<=10||ph<=10) return;

  // y 轴范围
  let lmax=0, rmax=0;
  for(const s of cfg.series){
    for(const v of s.data){
      if(v==null||!isFinite(v)) continue;
      if(s.axis==="right"){ if(v>rmax) rmax=v; }
      else { if(v> lmax) lmax=v; }
    }
  }
  lmax = cfg.leftMax!=null ? cfg.leftMax : niceMax(lmax*1.15);
  rmax = cfg.rightMax!=null ? cfg.rightMax : niceMax(rmax*1.15);
  if(lmax<=0) lmax=1;
  if(rmax<=0) rmax=1;

  const n = Math.max(...cfg.series.map(s=>s.data.length));
  const x = i => pad.l + (n<=1?0:(pw*i/(n-1)));
  const yL = v => pad.t + ph - ph*(v/lmax);
  const yR = v => pad.t + ph - ph*(v/rmax);

  // 网格 + 左右轴刻度
  g.font = "10px " + getComputedStyle(document.body).fontFamily;
  g.strokeStyle = "#1d232c"; g.fillStyle="#6e7681"; g.lineWidth=1;
  const ticks = 5;
  for(let i=0;i<=ticks;i++){
    const yy = Math.round(pad.t + ph*i/ticks) + .5;
    g.beginPath(); g.moveTo(pad.l,yy); g.lineTo(pad.l+pw,yy); g.stroke();
    g.textAlign="right"; g.textBaseline="middle";
    g.fillText(fmt(lmax*(1-i/ticks)), pad.l-6, yy);
    if(hasRight){
      g.textAlign="left";
      g.fillText(fmt(rmax*(1-i/ticks)), pad.l+pw+6, yy);
    }
  }
  g.strokeStyle="#30363d";
  g.beginPath(); g.moveTo(pad.l+.5,pad.t); g.lineTo(pad.l+.5,pad.t+ph); g.stroke();

  // 曲线
  for(const s of cfg.series){
    const yy = s.axis==="right" ? yR : yL;
    g.strokeStyle=s.color; g.lineWidth=s.width||1.8;
    g.lineJoin="round"; g.lineCap="round";
    g.beginPath();
    let started=false, lastX=0, lastY=0;
    for(let i=0;i<s.data.length;i++){
      const v=s.data[i];
      if(v==null||!isFinite(v)){ started=false; continue; }
      const px=x(i), py=yy(v);
      if(!started){ g.moveTo(px,py); started=true; }
      else g.lineTo(px,py);
      lastX=px; lastY=py;
    }
    g.stroke();
    if(s.dot && started){
      g.fillStyle=s.color;
      g.beginPath(); g.arc(lastX,lastY,2.6,0,Math.PI*2); g.fill();
    }
  }

  // 图例
  g.font="11px " + getComputedStyle(document.body).fontFamily;
  g.textBaseline="middle"; g.textAlign="left";
  let lx = pad.l+2, ly = pad.t+8;
  for(const s of cfg.series){
    const label = s.name + (s.axis==="right" ? " (右)" : "");
    const wpx = g.measureText(label).width + 18;
    if(lx + wpx > pad.l+pw){ lx = pad.l+2; ly += 15; }
    g.fillStyle=s.color; g.fillRect(lx, ly-1.5, 10, 3);
    g.fillStyle="#c9d1d9"; g.fillText(label, lx+14, ly);
    lx += wpx;
  }

  // x 轴: 左右各标一个时间
  if(cfg.xLabels && cfg.xLabels.length===2){
    g.fillStyle="#6e7681"; g.textBaseline="top";
    g.textAlign="left";  g.fillText(cfg.xLabels[0], pad.l, pad.t+ph+5);
    g.textAlign="right"; g.fillText(cfg.xLabels[1], pad.l+pw, pad.t+ph+5);
  }
}

// =====================================================================
// 页面逻辑
// =====================================================================
const $ = id => document.getElementById(id);
let lastState = null;
// pending = 【期望状态】(乐观更新): 点下去就写这里, 页面立刻按它画,
//          等板端上报确认后再删掉; 报错或超时则回滚。
let pending = {};
let pendingSince = {};
let prevEngine = null;
const PENDING_TIMEOUT_MS = 5000;   // 板端 5 秒没确认 -> 判定没生效, 回滚

function toast(msg, isErr, kind){
  const t = $("toast");
  t.textContent = msg;
  t.className = "toast show" + (kind ? (" "+kind) : (isErr ? " err" : ""));
  clearTimeout(t._tm);
  t._tm = setTimeout(()=>{ t.className="toast"; }, kind==="warn" ? 4200 : 2600);
}

// ★ 颜色 + 滑块位置只由 on/off 决定; busy 只是外面一圈"待确认"光晕。
//   (以前 busy 把背景刷成黄色、滑块硬推到右边, 开和关长得一模一样。)
function setSwitch(el, on, busy){
  on = !!on;
  el.classList.toggle("on", on);
  el.classList.toggle("off", !on);
  el.classList.toggle("busy", !!busy);
  const lab = $("st-" + el.dataset.key);
  if(lab){
    lab.textContent = busy ? (on ? "开·待确认" : "关·待确认") : (on ? "开" : "关");
    lab.className = "tstate " + (on ? "on" : "off") + (busy ? " pending" : "");
  }
}

// 当前"生效或期望"的状态 —— 点开关时按它取反, 不要按 class 取反
//   (class 在下发中会带 busy, 用 class 取反会导致连点两次发出同样的值)
function desired(k){
  if(k in pending) return pending[k];
  return (lastState && lastState.engine) ? !!lastState.engine[k] : true;
}

function paintSwitches(){
  for(const k of ["mpp","rga","npu"]){
    setSwitch($("sw-"+k), desired(k), k in pending);
  }
}

function rollback(k, why){
  delete pending[k];
  delete pendingSince[k];
  paintSwitches();
  toast(why, true);
}

async function sendControl(key, value){
  const body = {mpp:desired("mpp"), rga:desired("rga"), npu:desired("npu")};
  body[key] = value;
  pending[key] = value;
  pendingSince[key] = Date.now();
  paintSwitches();                     // ★ 乐观更新: 立刻按目标状态画, 不等 MQTT 回传
  try{
    const r = await fetch("/api/control", {
      method:"POST",
      headers:{"Content-Type":"application/json"},
      body:JSON.stringify(body)
    });
    const j = await r.json();
    if(!j.ok){
      rollback(key, "下发失败, 已回滚: " + (j.error||"未知错误"));
      return;
    }
    toast("已下发: " + key + " = " + (value?"开":"关") + " · 等板端确认");
  }catch(e){
    rollback(key, "请求失败, 已回滚: " + e);
  }
}

for(const key of ["mpp","rga","npu"]){
  $("sw-"+key).addEventListener("click", ()=>{
    sendControl(key, !desired(key));
  });
}

// ---- 【压力测试】按钮 ----
// 点一下: 板端代理起一个不限速 + 三核并行的进程, FPS 立刻飙到 100+;
// 再把 MPP 关掉, 就能当场看到 FPS 暴跌、CPU 狂飙 —— 硬件加速的价值一眼可见。
let stressOn = false;
$("btn-stress").addEventListener("click", async ()=>{
  const btn = $("btn-stress");
  btn.classList.add("busy");
  try{
    const r = await fetch("/api/stress", {
      method:"POST",
      headers:{"Content-Type":"application/json"},
      body:JSON.stringify({on: !stressOn})
    });
    const j = await r.json();
    if(!j.ok){
      toast("压测指令下发失败: "+(j.error||"未知错误"), true);
    }else{
      stressOn = !!j.stress;
      btn.classList.toggle("on", stressOn);
      btn.textContent = stressOn ? "⏹ 停止压力测试" : "⚡ 压力测试(极限并发)";
      $("stressNote").textContent = stressOn
        ? "压测进程已起 — 现在把 MPP 关掉, 看 FPS 和 CPU 的变化"
        : "起一个不限速 + 三核并行的进程, FPS 会飙到 100+, 再把 MPP 关掉看差别";
      toast(stressOn ? "压力测试已启动" : "压力测试已停止");
    }
  }catch(e){
    toast("请求失败: "+e, true);
  }finally{
    btn.classList.remove("busy");
  }
});

function num(v, digits){ return (v==null||!isFinite(v)) ? "-" : Number(v).toFixed(digits); }

// =====================================================================
// 视频源: 手填 RTSP 地址 + 连通性测试 + 应用 + 状态/重连提示
// =====================================================================
const RTSP_KEY = "rk3588.console.rtsp";     // 浏览器 LocalStorage: 下次打开还是这个地址
let lastRtspState = "";

function rtspUrl(){ return $("rtsp-url").value.trim(); }

function setSrcMsg(text, kind){
  const m = $("rtsp-msg");
  m.textContent = text;
  m.className = "srcmsg" + (kind ? (" "+kind) : "");
}

// 首次加载: 后端保存的地址优先, 其次浏览器本地的, 最后用页面里的默认值
async function loadRtspCfg(){
  let url = "";
  try{
    const r = await fetch("/api/rtsp", {cache:"no-store"});
    const j = await r.json();
    url = j.url || "";
  }catch(e){ /* 后端还没起来, 忽略 */ }
  let local = "";
  try{ local = localStorage.getItem(RTSP_KEY) || ""; }catch(e){}
  $("rtsp-url").value = url || local || $("rtsp-url").placeholder;
}

// 【测试连接】: 后端做 TCP 握手(1.5 秒超时), 通/不通立刻回话 —— 不让页面干等
$("btn-rtsp-test").addEventListener("click", async ()=>{
  const btn = $("btn-rtsp-test");
  const url = rtspUrl();
  if(!url){ setSrcMsg("请先填摄像头的 RTSP 地址", "bad"); return; }
  btn.classList.add("busy");
  setSrcMsg("正在测试 " + url + " …");
  try{
    const r = await fetch("/api/rtsp/test", {
      method:"POST",
      headers:{"Content-Type":"application/json"},
      body:JSON.stringify({url:url})
    });
    const j = await r.json();
    if(j.ok){
      setSrcMsg("✅ 连通 — " + j.host + ":" + j.port +
                (j.latency_ms!=null ? (" · 握手 " + j.latency_ms + " ms") : "") +
                " · RTSP 服务正常", "good");
      toast("视频源连通: " + j.host + ":" + j.port);
    }else{
      setSrcMsg("❌ " + (j.error||"连接失败") + " — " + (j.detail||""), "bad");
      toast(j.error||"连接失败", true);
    }
  }catch(e){
    setSrcMsg("❌ 测试请求失败: " + e, "bad");
    toast("测试请求失败: " + e, true);
  }finally{
    btn.classList.remove("busy");
  }
});

// 【应用】: 换源 + 存到浏览器和后端, 预览重新拉一次
$("btn-rtsp-apply").addEventListener("click", async ()=>{
  const btn = $("btn-rtsp-apply");
  const url = rtspUrl();
  if(!url){ setSrcMsg("请先填摄像头的 RTSP 地址", "bad"); return; }
  btn.classList.add("busy");
  try{
    const r = await fetch("/api/rtsp", {
      method:"POST",
      headers:{"Content-Type":"application/json"},
      body:JSON.stringify({url:url})
    });
    const j = await r.json();
    if(!j.ok){
      setSrcMsg("❌ " + (j.error||"地址不合法"), "bad");
      toast(j.error||"地址不合法", true);
      return;
    }
    try{ localStorage.setItem(RTSP_KEY, url); }catch(e){}
    setSrcMsg("已应用: " + url + " · 已保存(控制台重启后还在), 正在连接…", "good");
    toast("视频源已切换: " + url);
    // MJPEG 是长连接, 换地址后要重连一次才能看到新画面
    $("vid").src = "/api/video.mjpeg?t=" + Date.now();
    lastRtspState = "";
  }catch(e){
    setSrcMsg("❌ 请求失败: " + e, "bad");
  }finally{
    btn.classList.remove("busy");
  }
});

// 状态徽章 + 遮罩 + "正在重连" Toast
function renderRtsp(rs){
  if(!rs) return;
  const st = $("rtsp-state"), ov = $("vid-overlay");
  let txt = "未启用", cls = "", ovTxt = "未连接视频源", ovSub = "";
  if(rs.state === "streaming"){
    txt = "已连接"; cls = "on"; ovTxt = "";
  }else if(rs.state === "connecting"){
    txt = "连接中…"; cls = "warn"; ovTxt = "正在连接…"; ovSub = rs.url || "";
  }else if(rs.state === "reconnecting"){
    txt = "正在重连…"; cls = "warn"; ovTxt = "正在重连…";
    ovSub = rs.url || "";          // 详细原因在下面那行"已重连 N 次"里, 这里只放地址
  }else if(rs.state === "failed"){
    txt = "不可达"; cls = "bad"; ovTxt = "视频源不可达"; ovSub = rs.url || "";
  }
  st.textContent = txt;
  st.className = "srcstate " + cls;
  $("rtsp-detail").textContent = "已重连 " + (rs.reconnects||0) + " 次" +
      (rs.last_error ? (" · " + rs.last_error) : "");

  if(rs.state === "streaming"){
    ov.classList.add("hide");
  }else{
    ov.classList.remove("hide");
    ov.innerHTML = "<span>" + ovTxt + "</span>" +
        (ovSub ? ("<small>" + ovSub + "</small>") : "");
  }

  // ★ 状态跳变时给提示: 断了要告诉用户"正在重连", 恢复了也要说一声
  if(lastRtspState && lastRtspState !== rs.state){
    if(rs.state === "reconnecting"){
      toast("视频源断开, 正在重连…(" + (rs.last_error||"") + ")", false, "warn");
    }else if(rs.state === "streaming" && lastRtspState === "reconnecting"){
      toast("视频源已恢复");
    }
  }
  lastRtspState = rs.state;
}

function render(s){
  lastState = s;
  $("sn").textContent = s.sn;
  $("broker").textContent = s.broker;
  renderRtsp(s.rtsp);
  const dot = $("dot");
  dot.className = "dot " + (s.connected ? "on" : "off");
  $("conn").textContent = s.connected ? "MQTT 已连接" : "MQTT 未连接";
  if(s.updated_at) $("upd").textContent = new Date(s.updated_at*1000).toLocaleTimeString("zh-CN");

  // ---- 开关: 板端上报为准, 但下发中先按"期望值"画 ----
  const eng = s.engine || {};
  const now = Date.now();
  for(const k of ["mpp","rga","npu"]){
    const el = $("sw-"+k);
    if(k in pending){
      if(!!eng[k] === pending[k]){
        // 板端确认了 -> 落地成"生效值"
        delete pending[k]; delete pendingSince[k];
        setSwitch(el, !!eng[k], false);
      }else if(now - (pendingSince[k]||0) > PENDING_TIMEOUT_MS){
        // 板端 5 秒没认领 -> 没生效, 回滚到板端真实状态
        rollback(k, "板端未确认(" + k + "=" + (eng[k]?"开":"关") + "), 已回滚");
      }else{
        setSwitch(el, pending[k], true);   // 还在等确认: 保持乐观状态
      }
    }else{
      setSwitch(el, !!eng[k], false);
    }
  }
  if(prevEngine && JSON.stringify(prevEngine)!==JSON.stringify(eng)){
    const changed = ["mpp","rga","npu"].filter(k=>prevEngine[k]!==eng[k]);
    $("ctlNote").textContent = "板端已生效: " + changed.join(", ") +
        " -> " + changed.map(k=>k+"="+(eng[k]?"开":"关")).join(", ");
  }
  prevEngine = Object.assign({}, eng);

  // ---- KPI ----
  const st = s.status;
  if(st){
    $("k-fps").innerHTML = num(st.fps,2) + '<span class="u">fps</span>';
    $("k-srcfps").innerHTML = num(st.src_fps,2) + '<span class="u">fps</span>';
    $("k-dec").innerHTML = num(st.dec_ms,2) + '<span class="u">ms</span>';
    $("k-rga").innerHTML = num(st.rga_ms,2) + '<span class="u">ms</span>';
    $("k-npu").innerHTML = num(st.npu_ms,2) + '<span class="u">ms</span>';
    $("k-total").innerHTML = num(st.total_ms,2) + '<span class="u">ms</span>';
    $("k-objs").textContent = st.n_objs!=null ? st.n_objs : "-";
    $("k-mode").textContent = (st.decoder||"-").toUpperCase() + " / " +
        (st.rga_mode==="hw" ? "RGA硬件" : st.rga_mode==="cpu" ? "CPU" : "-");
    $("raw").textContent = JSON.stringify(st, null, 2);
  }

  // ---- NPU 分核仪表盘(内核 rknpu/load 的三个核) ----
  const hcur = (s.history && s.history.length) ? s.history[s.history.length-1] : {};
  ["c0","c1","c2"].forEach((k, i) => {
    const bar = $("bar-"+k), val = $("val-"+k);
    const v = hcur["load_npu_"+k];
    if (v === undefined || v === null || v < 0) {
      bar.style.width = "0%"; bar.className = ""; val.textContent = "-";
      return;
    }
    bar.style.width = Math.min(100, v) + "%";
    bar.className = v >= 85 ? "hot" : (v >= 60 ? "warm" : "");
    val.textContent = v.toFixed(0) + "%";
  });
  $("coreSrc").textContent = hcur.npu_src === "rknpu"
    ? "数据源: /sys/kernel/debug/rknpu/load (内核真值)"
    : "数据源: " + (hcur.npu_src || "none") + " — 拿不到分核真值, 板端需以 root 跑 scripts/hw_load_mqtt.py";

  // ---- 流水线耗时漏斗 ----
  if (st) {
    const seg = [
      {id:"dec",  v: st.dec_ms  || 0, lab:"解码"},
      {id:"rga",  v: st.rga_ms  || 0, lab:"预处理"},
      {id:"npu",  v: st.npu_ms  || 0, lab:"推理"},
      {id:"post", v: st.post_ms || 0, lab:"后处理"}
    ];
    const tot = seg.reduce((a,b) => a+b.v, 0) || 1;
    const full = st.total_ms || tot;
    seg.forEach(sg => {
      const w = Math.max(1, (sg.v / full) * 100);
      $("fn-"+sg.id).style.width = w + "%";
      $("v-"+sg.id).textContent = sg.v.toFixed(2) + " ms";
    });
    const pct = (v) => (v / full * 100).toFixed(1) + "%";
    $("fnNote").textContent =
      "单帧总耗时 " + full.toFixed(2) + " ms   |   解码 " + pct(st.dec_ms||0) +
      " · 预处理 " + pct(st.rga_ms||0) + " · 推理 " + pct(st.npu_ms||0) +
      " · 后处理 " + pct(st.post_ms||0) +
      "   |   流水线 " + (full>0 ? (1000/full).toFixed(1) : "-") + " FPS";
  }

  // ---- 曲线 ----
  const h = s.history || [];
  const clock = t => {
    const d = new Date(t*1000);
    const p = n => String(n).padStart(2,"0");
    return p(d.getHours())+":"+p(d.getMinutes())+":"+p(d.getSeconds());
  };
  const xl = h.length ? [clock(h[0].t), clock(h[h.length-1].t)] : null;
  const pick = k => h.map(e => (e[k]==null ? null : Number(e[k])));
  // 负载类字段: 负数 = "读不到", 不能画成一条贴底的线
  const pickLoad = k => h.map(e => {
    const v = e[k]==null ? null : Number(e[k]);
    return (v==null || v<0) ? null : v;
  });
  // NPU 的 devfreq 节点在这块板子上恒等于 100(空闲也是), 画出来是误导,
  // 只有拿到 rknpu 的真值(要 root)才画。
  const npuReal = h.some(e => e.npu_src === "rknpu");
  const loadSeries = [
    {name:"NPU", color:"#f78166", data: npuReal ? pickLoad("load_npu") : []},
    {name:"VPU解码", color:"#3fb950", data:pickLoad("load_vpu")},
    {name:"CPU", color:"#58a6ff", data:pickLoad("load_cpu")},
    {name:"DMC", color:"#d29922", data:pickLoad("load_dmc")},
    {name:"GPU", color:"#bc8cff", data:pickLoad("load_gpu")},
    {name:"RGA", color:"#79c0ff", data:pickLoad("load_rga")}
  ].filter(s => s.data.some(v => v!=null));

  drawChart($("c1"), {
    series:[
      {name:"FPS", color:"#58a6ff", data:pick("fps"), axis:"left", dot:true},
      {name:"单帧总耗时 ms", color:"#d29922", data:pick("total"), axis:"right"}
    ],
    xLabels: xl
  });

  drawChart($("c2"), {
    series:[
      {name:"解码", color:"#3fb950", data:pick("dec")},
      {name:"RGA", color:"#bc8cff", data:pick("rga")},
      {name:"NPU", color:"#f78166", data:pick("npu")},
      {name:"后处理", color:"#79c0ff", data:pick("post")}
    ],
    xLabels: xl
  });

  drawChart($("c3"), {
    series: loadSeries,
    leftMax:100, xLabels: xl
  });

  const last = h.length ? h[h.length-1] : null;
  $("loadSrc").textContent = last
    ? ("数据来源: NPU=" + (last.npu_src||"-") + ", RGA=" + (last.rga_src||"-") +
       "  ·  NPU/RGA 的内核真值要 root(见 hw_load_mqtt.py); devfreq 的 NPU 节点恒为 100, 不画")
    : "-";
}

async function tick(){
  try{
    const r = await fetch("/api/state", {cache:"no-store"});
    const s = await r.json();
    render(s);
  }catch(e){ /* 后端还没起来, 忽略 */ }
  setTimeout(tick, 1000);
}
loadRtspCfg();
tick();
</script>
</body>
</html>
"""


# ===========================================================================
# 入口
# ===========================================================================

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.8.21", help="MQTT broker 地址")
    ap.add_argument("--port", type=int, default=1883, help="MQTT 端口")
    ap.add_argument("--sn", default="rk3588-01", help="设备序列号(决定主题)")
    ap.add_argument("--http-port", type=int, default=8080, help="网页端口")
    ap.add_argument("--bind", default="0.0.0.0", help="网页监听地址")
    ap.add_argument("--rtsp", default="", help="视频源 RTSP 地址(不给就用上次保存的/默认的)")
    ap.add_argument("--rtsp-config", default="", help="视频源地址的保存文件(默认 logs/console_stack/rtsp.json)")
    args = ap.parse_args()

    global RTSP_CONFIG_FILE
    if not args.rtsp_config:
        here = os.path.dirname(os.path.abspath(__file__))
        repo = os.path.dirname(os.path.dirname(here))       # <repo>/rk3588_zero_copy/scripts -> <repo>
        RTSP_CONFIG_FILE = os.path.join(repo, "logs", "console_stack", "rtsp.json")
    else:
        RTSP_CONFIG_FILE = os.path.abspath(args.rtsp_config)

    status_topic = f"device/{args.sn}/message/up/status"
    load_topic = f"device/{args.sn}/message/up/load"
    ctl_topic = f"device/{args.sn}/message/down/control"

    # ★ 压测实例用【独立的 sn】(默认 <sn>-stress):
    #   否则压测进程和生产实例把状态报到同一个主题, 两边每秒各发一包,
    #   网页上看到的是"最后到达的那一包" —— 生产实例 25fps 会把压测的 100fps 顶掉,
    #   演示时反而看不出压测效果。分开之后网页优先显示压测实例的数据。
    stress_sn = f"{args.sn}-stress"
    stress_status_topic = f"device/{stress_sn}/message/up/status"
    stress_load_topic = f"device/{stress_sn}/message/up/load"
    stress_ctl_topic = f"device/{stress_sn}/message/down/control"

    global STRESS_SN
    STRESS_SN = stress_sn
    STATE["broker"] = f"{args.host}:{args.port}"
    STATE["sn"] = args.sn
    STATE["stress_sn"] = stress_sn

    # ---- 视频源: 命令行 > 上次保存的 > 默认地址 ----
    rtsp_url = args.rtsp.strip() or load_rtsp_config(RTSP_CONFIG_FILE) or DEFAULT_RTSP
    SOURCE.start()
    SOURCE.set_url(rtsp_url)
    if not args.rtsp and not load_rtsp_config(RTSP_CONFIG_FILE):
        save_rtsp_config(RTSP_CONFIG_FILE, rtsp_url)
    print(f"[web] 视频源: {rtsp_url}  (OpenCV 预览: {'有' if HAVE_CV2 else '无(只能测连通性)'})",
          flush=True)

    client = MqttClient(args.host, args.port, f"{args.sn}-web")
    Handler.client = client
    Handler.ctl_topic = ctl_topic
    Handler.stress_ctl_topic = stress_ctl_topic
    Handler.sn = args.sn

    # 订阅线程(生产实例 + 压测实例)
    threading.Thread(target=mqtt_thread,
                     args=(client, [status_topic, load_topic,
                                    stress_status_topic, stress_load_topic]),
                     daemon=True).start()

    # 监控连接状态(给页面显示小圆点)
    def watch():
        while True:
            time.sleep(1)
            with LOCK:
                STATE["connected"] = client.connected
                if client.last_error:
                    STATE["last_error"] = client.last_error
    threading.Thread(target=watch, daemon=True).start()

    srv = ThreadingHTTPServer((args.bind, args.http_port), Handler)
    print(f"[web] 控制台已启动: http://<本机IP>:{args.http_port}/", flush=True)
    print(f"[web] MQTT {args.host}:{args.port}  订阅 {status_topic}, {load_topic}", flush=True)
    print(f"[web] 控制指令发到 {ctl_topic}", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n[web] 退出", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
