#!/usr/bin/env python3
"""ansi_to_png.py —— 把终端抓屏(带 ANSI 颜色)渲染成 PNG 截图

用法:
    tmux capture-pane -t <session> -e -p > pane.ansi
    python3 scripts/ansi_to_png.py pane.ansi out.png [标题]

做法: ANSI -> HTML(等宽字体 + 深色背景) -> headless chromium 截图。
不依赖任何 python 第三方库。
"""
from __future__ import annotations

import html
import os
import re
import subprocess
import sys

# xterm 16 色
BASE = {
    30: "#2e3436", 31: "#e06c75", 32: "#98c379", 33: "#e5c07b",
    34: "#61afef", 35: "#c678dd", 36: "#56b6c2", 37: "#abb2bf",
    90: "#5c6370", 91: "#e06c75", 92: "#98c379", 93: "#e5c07b",
    94: "#61afef", 95: "#c678dd", 96: "#56b6c2", 97: "#ffffff",
}
BG_BASE = {
    40: "#1c2128", 41: "#e06c75", 42: "#98c379", 43: "#e5c07b",
    44: "#61afef", 45: "#c678dd", 46: "#56b6c2", 47: "#abb2bf",
    100: "#3e4451", 101: "#e06c75", 102: "#98c379", 103: "#e5c07b",
    104: "#61afef", 105: "#c678dd", 106: "#56b6c2", 107: "#ffffff",
}


def xterm256(n: int) -> str:
    if n < 16:
        codes = [30, 31, 32, 33, 34, 35, 36, 37, 90, 91, 92, 93, 94, 95, 96, 97]
        return BASE[codes[n]]
    if n < 232:
        n -= 16
        r, g, b = n // 36, (n // 6) % 6, n % 6
        conv = lambda v: 0 if v == 0 else 55 + 40 * v
        return f"#{conv(r):02x}{conv(g):02x}{conv(b):02x}"
    v = 8 + (n - 232) * 10
    return f"#{v:02x}{v:02x}{v:02x}"


SGR = re.compile(r"\x1b\[([0-9;]*)m")
OTHER_ESC = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b\][^\x07]*\x07|\x1b[()][B0]")


def ansi_to_html(text: str) -> str:
    out = []
    fg = None
    bg = None
    bold = False
    reverse = False
    open_span = False

    def close():
        nonlocal open_span
        if open_span:
            out.append("</span>")
            open_span = False

    def open_():
        nonlocal open_span
        if open_span:
            out.append("</span>")
        style = []
        f, b = fg, bg
        if reverse:
            f, b = (b or "#0b0e13"), (f or "#c9d1d9")
        if f:
            style.append(f"color:{f}")
        if b:
            style.append(f"background:{b}")
        if bold:
            style.append("font-weight:bold")
        out.append(f'<span style="{";".join(style)}">' if style else "<span>")
        open_span = True

    pos = 0
    for m in SGR.finditer(text):
        chunk = text[pos:m.start()]
        if chunk:
            out.append(html.escape(OTHER_ESC.sub("", chunk)))
        pos = m.end()
        params = [p for p in m.group(1).split(";") if p != ""] or ["0"]
        i = 0
        while i < len(params):
            p = int(params[i])
            if p == 0:
                fg = bg = None
                bold = reverse = False
                close()
            elif p == 1:
                bold = True
                open_()
            elif p == 7:
                reverse = True
                open_()
            elif p == 22:
                bold = False
                open_()
            elif p == 27:
                reverse = False
                open_()
            elif p in BASE:
                fg = BASE[p]
                open_()
            elif p in BG_BASE:
                bg = BG_BASE[p]
                open_()
            elif p == 39:
                fg = None
                open_()
            elif p == 49:
                bg = None
                open_()
            elif p in (38, 48):
                target = "fg" if p == 38 else "bg"
                if i + 1 < len(params) and params[i + 1] == "5":
                    c = xterm256(int(params[i + 2]))
                    i += 2
                elif i + 1 < len(params) and params[i + 1] == "2":
                    c = "#%02x%02x%02x" % (int(params[i + 2]), int(params[i + 3]), int(params[i + 4]))
                    i += 4
                else:
                    i += 1
                    continue
                if target == "fg":
                    fg = c
                else:
                    bg = c
                open_()
            i += 1
    tail = text[pos:]
    if tail:
        out.append(html.escape(OTHER_ESC.sub("", tail)))
    close()
    return "".join(out)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    ansi_path, out_png = sys.argv[1], sys.argv[2]
    title = sys.argv[3] if len(sys.argv) > 3 else "rktop — RK3588 实时硬件负载"

    raw = open(ansi_path, errors="replace").read()
    body = ansi_to_html(raw)
    # 去掉末尾空行, 去掉行尾空格
    lines = body.split("\n")
    while lines and not lines[-1].strip():
        lines.pop()
    lines = [ln.rstrip() for ln in lines]
    body = "\n".join(lines)
    # 估算字符宽度(去掉 span 标签后)
    plain = re.sub(r"<[^>]+>", "", body)
    width = max((len(re.sub(r"<[^>]+>", "", ln)) for ln in lines), default=120)
    width = max(width, 120)

    html_doc = f"""<!DOCTYPE html><html><head><meta charset="utf-8">
<style>
 body {{ margin:0; background:#0b0e13; }}
 .wrap {{ display:inline-block; background:#0b0e13; padding:14px 18px 18px 18px; }}
 h1 {{ color:#e6edf3; font:600 15px/1.4 "DejaVu Sans",sans-serif; margin:0 0 10px 0; }}
 pre {{ margin:0; color:#c9d1d9; background:#0b0e13;
       font:13px/1.18 "DejaVu Sans Mono","Noto Sans Mono",monospace;
       white-space:pre; letter-spacing:0; }}
</style></head><body><div class="wrap">
<h1>{html.escape(title)}</h1><pre>{body}</pre>
</div></body></html>"""

    tmp_html = "/tmp/_ansi_page.html"
    with open(tmp_html, "w") as f:
        f.write(html_doc)
    px_w = int(width * 8.0) + 60
    px_h = int(len(lines) * 16.0) + 80
    cmd = ["chromium", "--headless", "--no-sandbox", "--disable-gpu",
           "--hide-scrollbars", f"--screenshot={out_png}",
           f"--window-size={px_w},{px_h}", f"file://{tmp_html}"]
    subprocess.run(cmd, capture_output=True, timeout=120)
    if os.path.exists(out_png):
        print(f"已生成: {out_png}  ({len(lines)} 行 x {width} 列)")
        return 0
    print("渲染失败")
    return 1


if __name__ == "__main__":
    sys.exit(main())
