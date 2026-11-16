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
    python3 scripts/web_console.py --host 192.168.31.214 --sn rk3588-01 --http-port 8080

然后浏览器打开:  http://<PC的IP>:8080/
"""
from __future__ import annotations

import argparse
import collections
import json
import queue
import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

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
            self._send(200, PAGE, "text/html; charset=utf-8")
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
                    "history": list(HISTORY),
                }
            self._send(200, json.dumps(s, ensure_ascii=False))
        elif path == "/api/health":
            self._send(200, json.dumps({"ok": True, "sn": self.sn}))
        elif path == "/favicon.ico":
            self._send(204, b"", "image/x-icon")
        else:
            self._send(404, json.dumps({"error": "not found"}))

    # -------------------------------------------------------------- POST
    def do_POST(self):
        path = self.path.split("?", 1)[0].split("#", 1)[0]
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
  .toggle{position:relative;width:46px;height:25px;flex:0 0 auto;cursor:pointer;
          border-radius:13px;background:#30363d;transition:background .18s}
  .toggle.on{background:var(--ok)}
  .toggle.busy{background:var(--warn)}
  .toggle.disabled{opacity:.4;cursor:not-allowed}
  .toggle::after{content:"";position:absolute;top:3px;left:3px;width:19px;height:19px;
          border-radius:50%;background:#fff;transition:transform .18s}
  .toggle.on::after{transform:translateX(21px)}
  .toggle.busy::after{transform:translateX(21px);background:#ffe9a8}
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
  <div class="row ctrl">
    <div class="card">
      <h2>硬件引擎开关(点一下立刻生效)</h2>
      <div class="switch">
        <div class="info">
          <div class="nm">MPP 硬解</div>
          <div class="ds">关掉 = 改用 FFmpeg 软解, 解码耗时飙升、FPS 暴跌</div>
        </div>
        <div class="toggle" id="sw-mpp" data-key="mpp"></div>
      </div>
      <div class="switch">
        <div class="info">
          <div class="nm">RGA 硬件加速</div>
          <div class="ds">关掉 = 改用 OpenCV CPU 缩放, 预处理变慢、CPU 占用上升</div>
        </div>
        <div class="toggle" id="sw-rga" data-key="rga"></div>
      </div>
      <div class="switch">
        <div class="info">
          <div class="nm">NPU 推理</div>
          <div class="ds">关掉 = 只跑解码+预处理, 不出检测框, 负载归零</div>
        </div>
        <div class="toggle" id="sw-npu" data-key="npu"></div>
      </div>
      <div class="legendnote" id="ctlNote">开关状态以板端上报为准</div>
      <div class="stressrow">
        <button class="stressbtn" id="btn-stress">⚡ 压力测试(极限并发)</button>
        <span class="meta" id="stressNote">起一个不限速 + 三核并行的进程, FPS 会飙到 100+, 再把 MPP 关掉看差别</span>
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
let pending = {};          // 正在下发、还没被板端确认的开关
let prevEngine = null;

function toast(msg, isErr){
  const t = $("toast");
  t.textContent = msg;
  t.className = "toast show" + (isErr?" err":"");
  clearTimeout(t._tm);
  t._tm = setTimeout(()=>{ t.className="toast"; }, 2600);
}

function setSwitch(el, on, busy){
  el.classList.toggle("on", !!on && !busy);
  el.classList.toggle("busy", !!busy);
}

async function sendControl(key, value){
  const body = {mpp: lastState?lastState.engine.mpp:true,
                rga: lastState?lastState.engine.rga:true,
                npu: lastState?lastState.engine.npu:true};
  body[key] = value;
  pending[key] = value;
  for(const k of ["mpp","rga","npu"]) setSwitch($("sw-"+k), body[k], k in pending);
  try{
    const r = await fetch("/api/control", {
      method:"POST",
      headers:{"Content-Type":"application/json"},
      body:JSON.stringify(body)
    });
    const j = await r.json();
    if(!j.ok) toast("下发失败: "+(j.error||"未知错误"), true);
    else toast("已下发: " + key + " = " + (value?"开":"关"));
  }catch(e){
    toast("请求失败: "+e, true);
  }
}

for(const key of ["mpp","rga","npu"]){
  $("sw-"+key).addEventListener("click", ()=>{
    const el = $("sw-"+key);
    const on = !el.classList.contains("on");
    sendControl(key, on);
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

function render(s){
  lastState = s;
  $("sn").textContent = s.sn;
  $("broker").textContent = s.broker;
  const dot = $("dot");
  dot.className = "dot " + (s.connected ? "on" : "off");
  $("conn").textContent = s.connected ? "MQTT 已连接" : "MQTT 未连接";
  if(s.updated_at) $("upd").textContent = new Date(s.updated_at*1000).toLocaleTimeString("zh-CN");

  // ---- 开关(以板端上报为准) ----
  const eng = s.engine || {};
  for(const k of ["mpp","rga","npu"]){
    const el = $("sw-"+k);
    if(k in pending){
      setSwitch(el, pending[k], true);
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
    ap.add_argument("--host", default="192.168.31.214", help="MQTT broker 地址")
    ap.add_argument("--port", type=int, default=1883, help="MQTT 端口")
    ap.add_argument("--sn", default="rk3588-01", help="设备序列号(决定主题)")
    ap.add_argument("--http-port", type=int, default=8080, help="网页端口")
    ap.add_argument("--bind", default="0.0.0.0", help="网页监听地址")
    args = ap.parse_args()

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
