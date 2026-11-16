#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""console_shot.py —— 网页控制台截图工具(可以【真的点】开关再拍)

★ 为什么不用现成的截图脚本:
   PPT 要的素材是"开关点了之后界面【立刻跳变】"这件事, 光截静态页面证明不了。
   所以这里用 CDP(Chrome DevTools Protocol)真的去点那个开关, 连拍三张:
     ① 点之前(全开)
     ② 点完【立刻】(乐观更新: 颜色/滑块已经变了, 文字是"待确认")
     ③ 板端确认之后(落到生效值)
   板子上没有 playwright/selenium, 也不装 —— 这里用纯 Python 写了个最小 WebSocket
   客户端(就为了跟 CDP 说几句话), 零依赖。

用法:
    python3 scripts/console_shot.py --url http://127.0.0.1:8080/ \
        --click mpp --out-prefix /tmp/console_mpp
    python3 scripts/console_shot.py --url http://127.0.0.1:8080/ --out docs/assets/console_normal.png
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import urllib.request


# ---------------------------------------------------------------- 最小 WebSocket 客户端
class WS:
    def __init__(self, url: str):
        # ws://host:port/path
        assert url.startswith("ws://")
        rest = url[5:]
        hostport, _, path = rest.partition("/")
        host, _, port = hostport.partition(":")
        self.sock = socket.create_connection((host, int(port or 80)), timeout=15)
        self.sock.settimeout(20)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (f"GET /{path} HTTP/1.1\r\nHost: {hostport}\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
               f"Sec-WebSocket-Version: 13\r\n\r\n")
        self.sock.sendall(req.encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            buf += self.sock.recv(4096)
        if b"101" not in buf.split(b"\r\n")[0]:
            raise RuntimeError("WebSocket 握手失败: " + buf.split(b"\r\n")[0].decode(errors="replace"))
        self.buf = buf.split(b"\r\n\r\n", 1)[1]
        self.next_id = 0

    def _recv_exact(self, n: int) -> bytes:
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("连接断开")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def send(self, text: str) -> None:
        payload = text.encode()
        header = bytearray([0x81])                      # FIN + text
        n = len(payload)
        mask = os.urandom(4)
        if n < 126:
            header.append(0x80 | n)
        elif n < 65536:
            header.append(0x80 | 126)
            header += struct.pack(">H", n)
        else:
            header.append(0x80 | 127)
            header += struct.pack(">Q", n)
        header += mask
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(bytes(header) + masked)

    def recv(self) -> str:
        while True:
            b0, b1 = self._recv_exact(2)
            opcode = b0 & 0x0F
            length = b1 & 0x7F
            if length == 126:
                length = struct.unpack(">H", self._recv_exact(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", self._recv_exact(8))[0]
            data = self._recv_exact(length) if length else b""
            if opcode == 0x9:                            # ping -> pong
                self.sock.sendall(bytes([0x8A, 0x80]) + os.urandom(4))
                continue
            if opcode == 0x8:
                raise ConnectionError("服务端关闭了连接")
            if opcode in (0x1, 0x2):
                return data.decode("utf-8", "replace")

    def call(self, method: str, params: dict | None = None, timeout: float = 20.0) -> dict:
        self.next_id += 1
        mid = self.next_id
        self.send(json.dumps({"id": mid, "method": method, "params": params or {}}))
        deadline = time.time() + timeout
        while time.time() < deadline:
            msg = json.loads(self.recv())
            if msg.get("id") == mid:
                return msg.get("result", {})
        raise TimeoutError(method + " 超时")

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


# ---------------------------------------------------------------- CDP 驱动
def find_page_target(port: int) -> str:
    for _ in range(50):
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{port}/json/list", timeout=3) as r:
                targets = json.load(r)
            for t in targets:
                if t.get("type") == "page" and t.get("webSocketDebuggerUrl"):
                    return t["webSocketDebuggerUrl"]
        except Exception:      # noqa: BLE001
            pass
        time.sleep(0.2)
    raise RuntimeError("找不到 CDP 的页面目标")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:8080/")
    ap.add_argument("--out", default="/tmp/console.png")
    ap.add_argument("--out-prefix", default="", help="点开关时连拍三张: <prefix>_before/_after/_confirmed")
    ap.add_argument("--click", default="", help="要点的开关: mpp / rga / npu")
    ap.add_argument("--width", type=int, default=1400)
    ap.add_argument("--height", type=int, default=1500)
    ap.add_argument("--wait", type=float, default=3.0, help="页面加载后等多久再拍")
    ap.add_argument("--keep", action="store_true", help="保留 chromium 进程")
    args = ap.parse_args()

    chrome = shutil.which("chromium") or shutil.which("chromium-browser") or shutil.which("google-chrome")
    if not chrome:
        print("找不到 chromium", flush=True)
        return 2
    port = 9333
    profile = tempfile.mkdtemp(prefix="console-shot-")
    proc = subprocess.Popen(
        [chrome, "--headless", "--disable-gpu", "--no-sandbox", "--hide-scrollbars",
         f"--remote-debugging-port={port}", f"--user-data-dir={profile}",
         f"--window-size={args.width},{args.height}", "about:blank"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ws = None
    try:
        ws_url = find_page_target(port)
        ws = WS(ws_url)
        ws.call("Page.enable")
        ws.call("Runtime.enable")
        ws.call("Page.navigate", {"url": args.url})
        time.sleep(args.wait)

        def shoot(path: str) -> None:
            res = ws.call("Page.captureScreenshot", {"format": "png", "captureBeyondViewport": True})
            data = base64.b64decode(res["data"])
            with open(path, "wb") as f:
                f.write(data)
            print(f"  已保存 {path} ({len(data)//1024} KB)", flush=True)

        if args.click:
            prefix = args.out_prefix or (args.out[:-4] if args.out.endswith(".png") else args.out)
            shoot(f"{prefix}_before.png")
            # ★ 真的点一下那个开关
            js = f"(() => {{const el=document.getElementById('sw-{args.click}');"
            js += "if(!el) return 'no-element'; el.click(); return 'clicked';})()"
            r = ws.call("Runtime.evaluate", {"expression": js, "returnByValue": True})
            print("  点击:", r.get("result", {}).get("value"), flush=True)
            time.sleep(0.25)                       # 乐观更新已经画完了, 但板端还没回话
            shoot(f"{prefix}_after.png")
            time.sleep(3.0)                       # 等板端确认(状态报文 1 秒一条)
            shoot(f"{prefix}_confirmed.png")
        else:
            shoot(args.out)
    finally:
        if ws:
            ws.close()
        if not args.keep:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(profile, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
