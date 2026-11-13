#!/usr/bin/env python3
"""md_to_html.py —— 把交付文档打包成一个可打印/可投屏的 HTML(零依赖)

没有 markdown 库可用(板子上没装), 所以自己实现一个够用的子集:
标题 / 表格 / 代码块 / 列表 / 引用 / 粗体 / 行内代码 / 分隔线。

用法:
    python3 scripts/md_to_html.py <out.html> <md文件> [md文件...]
    chromium --headless --print-to-pdf=out.pdf out.html
"""
from __future__ import annotations

import html
import re
import sys


def inline(t: str) -> str:
    t = html.escape(t, quote=False)
    t = re.sub(r"`([^`]+)`", r"<code>\1</code>", t)
    t = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", t)
    t = re.sub(r"(?<!\*)\*([^*\n]+)\*(?!\*)", r"<em>\1</em>", t)
    # ★ 图片必须在链接之前处理: ![说明](路径)
    #   少了这一步, 图片会被下面的链接正则吃成 "! <a href=...>说明</a>",
    #   PDF 里就只剩一个坏掉的链接, 图根本不显示。
    #   (交付白皮书里的长跑监控图就是靠这一步才能进 PDF。)
    t = re.sub(r"!\[([^\]]*)\]\(([^)\s]+)\)", r'<img src="\2" alt="\1">', t)
    t = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", r'<a href="\2">\1</a>', t)
    return t


def render(md: str) -> str:
    out = []
    lines = md.split("\n")
    i = 0
    while i < len(lines):
        line = lines[i]

        # 代码块
        if line.strip().startswith("```"):
            lang = line.strip()[3:].strip()
            i += 1
            buf = []
            while i < len(lines) and not lines[i].strip().startswith("```"):
                buf.append(lines[i])
                i += 1
            i += 1
            out.append(f'<pre class="code{" lang-"+html.escape(lang) if lang else ""}">'
                       f'{html.escape(chr(10).join(buf))}</pre>')
            continue

        # 表格
        if line.strip().startswith("|") and i + 1 < len(lines) and \
                re.match(r"^\s*\|[\s:|-]+\|\s*$", lines[i + 1]):
            head = [c.strip() for c in line.strip().strip("|").split("|")]
            i += 2
            rows = []
            while i < len(lines) and lines[i].strip().startswith("|"):
                rows.append([c.strip() for c in lines[i].strip().strip("|").split("|")])
                i += 1
            out.append("<table><thead><tr>" +
                       "".join(f"<th>{inline(c)}</th>" for c in head) +
                       "</tr></thead><tbody>" +
                       "".join("<tr>" + "".join(f"<td>{inline(c)}</td>" for c in r) + "</tr>"
                               for r in rows) +
                       "</tbody></table>")
            continue

        # 标题
        m = re.match(r"^(#{1,6})\s+(.*)$", line)
        if m:
            lvl = len(m.group(1))
            out.append(f"<h{lvl}>{inline(m.group(2))}</h{lvl}>")
            i += 1
            continue

        # 分隔线
        if re.match(r"^\s*---+\s*$", line):
            out.append("<hr>")
            i += 1
            continue

        # 引用
        if line.strip().startswith(">"):
            buf = []
            while i < len(lines) and lines[i].strip().startswith(">"):
                buf.append(lines[i].strip().lstrip(">").strip())
                i += 1
            out.append("<blockquote>" + "<br>".join(inline(b) for b in buf) + "</blockquote>")
            continue

        # ★ 独占一行的图片: 排成整块, 上下留白, 免得和正文挤在一起
        #   (长跑监控图这种大图, 挤在段落里 PDF 上很难看)
        m_img = re.match(r"^\s*!\[([^\]]*)\]\(([^)\s]+)\)\s*$", line)
        if m_img:
            alt, src = html.escape(m_img.group(1)), m_img.group(2)
            out.append(f'<div class="figure"><img src="{src}" alt="{alt}">'
                       f'<div class="caption">{alt}</div></div>')
            i += 1
            continue

        # 列表
        if re.match(r"^\s*([-*+]|\d+\.)\s+", line):
            ordered = bool(re.match(r"^\s*\d+\.\s+", line))
            items = []
            while i < len(lines) and re.match(r"^\s*([-*+]|\d+\.)\s+", lines[i]):
                items.append(re.sub(r"^\s*([-*+]|\d+\.)\s+", "", lines[i]))
                i += 1
            tag = "ol" if ordered else "ul"
            out.append(f"<{tag}>" + "".join(f"<li>{inline(x)}</li>" for x in items) + f"</{tag}>")
            continue

        # 空行 / 段落
        if not line.strip():
            i += 1
            continue
        buf = []
        while i < len(lines) and lines[i].strip() and \
                not re.match(r"^(#{1,6}\s|\s*[-*+]\s|\s*\d+\.\s|>|```|\|)", lines[i]):
            buf.append(lines[i].strip())
            i += 1
        out.append("<p>" + "<br>".join(inline(b) for b in buf) + "</p>")

    return "\n".join(out)


CSS = """
:root{--bg:#ffffff;--fg:#1f2328;--line:#d0d7de;--accent:#0969da;--code:#f6f8fa}
*{box-sizing:border-box}
body{margin:0;background:#eef1f4;font:15px/1.75 -apple-system,"Noto Sans CJK SC","Microsoft YaHei",sans-serif;color:var(--fg)}
.page{max-width:1080px;margin:28px auto;background:var(--bg);padding:46px 56px;
      box-shadow:0 2px 18px rgba(0,0,0,.10);border-radius:8px}
h1{font-size:30px;border-bottom:2px solid var(--line);padding-bottom:14px;margin:0 0 22px}
h2{font-size:23px;border-bottom:1px solid var(--line);padding-bottom:8px;margin:34px 0 14px}
h3{font-size:18px;margin:26px 0 10px}
h4{font-size:15px;margin:20px 0 8px;color:#33383d}
p,li{margin:9px 0}
table{border-collapse:collapse;width:100%;margin:16px 0;font-size:13.5px}
th,td{border:1px solid var(--line);padding:7px 11px;text-align:left;vertical-align:top}
th{background:var(--code);font-weight:600;white-space:nowrap}
tr:nth-child(even) td{background:#fafbfc}
code{background:var(--code);padding:2px 6px;border-radius:4px;font-size:12.5px;
     font-family:"DejaVu Sans Mono",Consolas,monospace}
pre.code{background:var(--code);padding:14px 16px;border-radius:8px;overflow-x:auto;
         border:1px solid var(--line);font-size:12.5px;line-height:1.55}
pre.code code{background:none;padding:0}
blockquote{margin:14px 0;padding:10px 16px;border-left:4px solid var(--accent);
           background:#f2f7fd;color:#31383d}
a{color:var(--accent)}
hr{border:0;border-top:1px solid var(--line);margin:26px 0}
/* 图片: 整块排版, 不超过版心宽度, 打印时也不溢出 */
img{max-width:100%;height:auto;display:block;margin:0 auto;border:1px solid var(--line);
    border-radius:6px}
.figure{margin:20px 0;text-align:center;page-break-inside:avoid;break-inside:avoid}
.figure .caption{margin-top:8px;font-size:12.5px;color:#5a636d;text-align:center}
@media print{.figure{page-break-inside:avoid}}
.toc{background:#f6f8fa;border:1px solid var(--line);border-radius:8px;padding:16px 22px;margin-bottom:28px}
.toc h2{margin:0 0 10px;font-size:17px;border:0}
@media print{body{background:#fff}.page{box-shadow:none;margin:0;max-width:none;padding:12mm}
             h2{page-break-after:avoid}table{page-break-inside:avoid}}
"""


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    out_html = sys.argv[1]
    files = sys.argv[2:]
    parts = []
    for f in files:
        md = open(f, encoding="utf-8").read()
        parts.append(f'<section class="doc" id="{html.escape(f)}">{render(md)}</section>')
    # 目录: 多份文档合并时给出跳转链接; 只合并一份时就别画这个块了 ——
    #   单文档的正文里通常自带章节目录, 顶上再挂一个"目录: xxx.md"既多余,
    #   而且那个链接在 PDF 里指向一个 .md 文件, 点了什么也不会发生。
    if len(files) > 1:
        toc = ('<div class="toc"><h2>目录</h2><ul>' +
               "".join(f'<li><a href="#{html.escape(f)}">{html.escape(f)}</a></li>' for f in files) +
               "</ul></div>")
    else:
        toc = ""
    doc = (f'<!DOCTYPE html><html lang="zh"><head><meta charset="utf-8">'
            f"<title>RK3588 交付文档</title><style>{CSS}</style></head><body>"
            f'<div class="page">{toc}{"".join(parts)}</div></body></html>')
    open(out_html, "w", encoding="utf-8").write(doc)
    print(f"已生成: {out_html}  ({len(files)} 个文档)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
