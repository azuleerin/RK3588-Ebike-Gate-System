#!/usr/bin/env python3
"""mqtt_web.py —— MQTT -> 网页实时看板(零依赖)

订阅 broker 上的报文, 通过 SSE 推给浏览器, 页面实时显示收到的 ivs_result JSON。
用来验证"网页端能不能收到板子推上来的真实数据"。

用法:
    python3 scripts/mqtt_web.py --host 192.168.8.21 --port 1883 \
        --topic 'device/#' --http-port 8090

然后浏览器打开 http://<板子IP>:8090/
"""
from __future__ import annotations

import argparse
import base64
import json
import queue
import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

STATE = {
    "connected": False,
    "subscribed": [],
    "count": 0,
    "last": None,
    "last_plate": None,
    "history": [],
    "started": time.time(),
}
Q: "queue.Queue" = queue.Queue(maxsize=200)


# ---------------------------------------------------------------- MQTT 客户端
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


def read_len(sock):
    mult, value = 1, 0
    while True:
        b = sock.recv(1)
        if not b:
            raise ConnectionError("连接断开")
        value += (b[0] & 0x7F) * mult
        mult *= 128
        if not (b[0] & 0x80):
            return value


def read_exact(sock, n):
    buf = b""
    while len(buf) < n:
        c = sock.recv(n - len(buf))
        if not c:
            raise ConnectionError("连接断开")
        buf += c
    return buf


def mqtt_thread(host, port, topic):
    while True:
        try:
            sock = socket.create_connection((host, port), timeout=10)
            sock.settimeout(2.0)
            var = enc_str("MQTT") + bytes([4, 0x02]) + struct.pack("!H", 30) + enc_str("web_viewer")
            sock.sendall(bytes([0x10]) + enc_len(len(var)) + var)
            hdr = sock.recv(1)
            rl = read_len(sock)
            read_exact(sock, rl)
            sub = struct.pack("!H", 1) + enc_str(topic) + bytes([0])
            sock.sendall(bytes([0x82]) + enc_len(len(sub)) + sub)
            STATE["connected"] = True
            STATE["subscribed"] = [topic]
            while True:
                try:
                    hdr = sock.recv(1)
                except socket.timeout:
                    continue
                if not hdr:
                    break
                rl = read_len(sock)
                body = read_exact(sock, rl) if rl else b""
                if (hdr[0] & 0xF0) != 0x30:
                    continue
                tlen = struct.unpack("!H", body[:2])[0]
                tp = body[2:2 + tlen].decode("utf-8", "replace")
                off = 2 + tlen
                if ((hdr[0] & 0x06) >> 1) > 0:
                    off += 2
                data = body[off:].decode("utf-8", "replace")
                rec = {"topic": tp, "raw": data, "ts": time.time()}
                try:
                    obj = json.loads(data)
                    rec["obj"] = obj
                    lic = obj.get("payload", {}).get("AlarmInfoPlate", {}) \
                              .get("result", {}).get("PlateResult", {}).get("license", "")
                    if lic:
                        try:
                            rec["plate"] = base64.b64decode(lic).decode("utf-8")
                        except Exception:
                            rec["plate"] = lic
                    rec["plate_no"] = obj.get("plate_no", "")
                    rec["frame"] = f"{obj.get('frame_w',0)}x{obj.get('frame_h',0)}"
                    rec["sn"] = obj.get("sn", "")
                except Exception:
                    pass
                STATE["count"] += 1
                STATE["last"] = rec
                if rec.get("plate"):
                    STATE["last_plate"] = rec     # 最近一次【带车牌】的报文
                STATE["history"].insert(0, rec)
                del STATE["history"][50:]
                # ⚠️ 这个队列没有消费者, 满了【会把这个线程永久阻塞住】
                #   (踩过: 收到第 201 条就卡死, 后面全丢)。非阻塞投放。
                try:
                    Q.put_nowait(rec)
                except queue.Full:
                    pass
        except Exception as e:
            STATE["connected"] = False
            time.sleep(2)


PAGE = """<!DOCTYPE html><html lang="zh"><head><meta charset="utf-8">
<title>RK3588 边缘AI 实时上报看板</title>
<style>
 :root{--bg:#0b0e13;--card:#141922;--line:#232a35;--fg:#e6edf3;--dim:#8b949e;
       --ok:#3fb950;--warn:#d29922;--accent:#58a6ff}
 body{margin:0;background:var(--bg);color:var(--fg);
      font:14px/1.6 -apple-system,"Noto Sans CJK SC","Microsoft YaHei",sans-serif}
 header{padding:16px 24px;border-bottom:1px solid var(--line);display:flex;
        align-items:center;gap:18px;background:#0e1218}
 h1{font-size:16px;margin:0;font-weight:600}
 .dot{width:9px;height:9px;border-radius:50%;background:#555;display:inline-block}
 .dot.on{background:var(--ok);box-shadow:0 0 8px var(--ok)}
 .stat{color:var(--dim);font-size:13px}
 .stat b{color:var(--fg);font-weight:600}
 main{padding:18px 24px;display:grid;grid-template-columns:380px 1fr;gap:18px}
 .card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:14px 16px}
 .card h2{font-size:13px;margin:0 0 10px;color:var(--dim);font-weight:600;
          text-transform:uppercase;letter-spacing:.06em}
 .plate{font-size:30px;font-weight:700;color:var(--accent);letter-spacing:.06em;
        font-family:"DejaVu Sans Mono",monospace}
 pre{white-space:pre-wrap;word-break:break-all;font:12px/1.55 "DejaVu Sans Mono",monospace;
     color:#c9d1d9;background:#0d1117;border:1px solid var(--line);border-radius:6px;
     padding:10px;margin:0;max-height:460px;overflow:auto}
 .row{display:flex;justify-content:space-between;border-bottom:1px dotted #2a313d;padding:5px 0}
 .row span:last-child{color:var(--fg)}
 .k{color:var(--dim)}
 .feed div{border-bottom:1px solid #1d232c;padding:7px 0;font-size:12px;
          display:flex;justify-content:space-between;gap:10px}
</style></head><body>
<header>
  <h1>RK3588 边缘AI 实时上报看板</h1>
  <span class="dot" id="dot"></span>
  <span class="stat" id="conn">连接中…</span>
  <span class="stat">收到报文 <b id="cnt">0</b></span>
  <span class="stat">主题 <b id="topic">-</b></span>
</header>
<main>
  <div>
    <div class="card">
      <h2>最新车牌</h2>
      <div class="plate" id="plate">-</div>
      <div style="margin-top:10px">
        <div class="row"><span class="k">设备 SN</span><span id="sn">-</span></div>
        <div class="row"><span class="k">画面尺寸</span><span id="frame">-</span></div>
        <div class="row"><span class="k">时间戳</span><span id="ts">-</span></div>
      </div>
    </div>
    <div class="card" style="margin-top:18px">
      <h2>最近报文</h2>
      <div class="feed" id="feed"></div>
    </div>
  </div>
  <div class="card">
    <h2>报文 JSON(ivs_result)</h2>
    <pre id="json">等待数据…</pre>
  </div>
</main>
<script>
let lastCnt = -1;
function fmt(t){return new Date(t*1000).toLocaleTimeString('zh-CN');}
async function tick(){
  try{
    const r = await fetch('/api/state');
    const s = await r.json();
    document.getElementById('dot').className = 'dot' + (s.connected?' on':'');
    document.getElementById('conn').textContent = s.connected?'已连接 '+s.host:('未连接 ('+s.host+')');
    document.getElementById('cnt').textContent = s.count;
    document.getElementById('topic').textContent = (s.subscribed||[]).join(', ') || '-';
    const L = s.last;
    if(L){
      // 顶部"最新车牌"显示最近一次【带车牌】的报文(车牌是低频事件,
      // 只看最后一条的话几乎总是空的)
      const P = s.last_plate || L;
      document.getElementById('plate').textContent = P.plate || P.plate_no || '无车牌';
      document.getElementById('sn').textContent = (P.sn || L.sn) || '-';
      document.getElementById('frame').textContent = (P.frame !== '0x0' ? P.frame : L.frame) || '-';
      document.getElementById('ts').textContent = P.ts ? fmt(P.ts) : '-';
      document.getElementById('json').textContent =
        JSON.stringify((P.obj||P.raw), null, 2);
    }
    if(s.count !== lastCnt){
      lastCnt = s.count;
      document.getElementById('feed').innerHTML = (s.history||[]).slice(0,12).map(h =>
        '<div><span>'+(h.plate||h.plate_no||'(无车牌)')+'</span><span style="color:#8b949e">'+fmt(h.ts)+
        ' · '+h.topic.split('/').pop()+'</span></div>').join('');
    }
  }catch(e){}
  setTimeout(tick, 500);
}
tick();
</script></body></html>"""


class Handler(BaseHTTPRequestHandler):
    host_info = ""

    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="application/json; charset=utf-8"):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/":
            self._send(200, PAGE, "text/html; charset=utf-8")
        elif self.path == "/api/state":
            s = dict(STATE)
            s["host"] = self.host_info
            self._send(200, json.dumps(s, ensure_ascii=False))
        else:
            self._send(404, "{}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.8.21")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--topic", default="device/#")
    ap.add_argument("--http-port", type=int, default=8090)
    args = ap.parse_args()

    Handler.host_info = f"{args.host}:{args.port}"
    threading.Thread(target=mqtt_thread,
                     args=(args.host, args.port, args.topic), daemon=True).start()
    srv = HTTPServer(("0.0.0.0", args.http_port), Handler)
    print(f"[web] 看板已启动: http://<本机IP>:{args.http_port}/  (MQTT {args.host}:{args.port} {args.topic})",
          flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
