#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""plot_fair_ab.py —— 把公平 A/B/C 对比数据画成 PPT 能直接用的图

★ 为什么不用 cv2.putText 画
   OpenCV 的 Hershey 字体不支持中文(会画成乱码方块), 板子上也没装 matplotlib。
   所以这里的做法是: 生成一个 HTML/CSS 图表 -> 用 chromium(CDP)截成 PNG。
   板子上装了 Noto Sans CJK, 中文直接可用。

用法:
    python3 scripts/plot_fair_ab.py --data /tmp/fair_ab/cli_ttl5000.json \
        --out /home/seeed/rk_deploy/docs/assets/fair_ab.png
"""
from __future__ import annotations

import argparse
import html
import json
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from console_shot import WS, find_page_target          # noqa: E402  (复用 CDP 客户端)

GROUP_LABEL = {
    "A": "A 全硬件加速<br><small>MPP硬解 + RGA + NPU</small>",
    "B": "B 关硬解<br><small>只关 MPP(软解)</small>",
    "C": "C 关 RGA<br><small>只关 RGA(CPU)</small>",
}
COLORS = {"A": "#3fb950", "B": "#d29922", "C": "#58a6ff"}


def bar_rows(title: str, unit: str, series: list[tuple[str, str, float]], maxv: float | None = None) -> str:
    """series: [(指标名, 组, 值)]"""
    top = maxv or max(v for _, _, v in series if v is not None) or 1.0
    out = [f'<div class="panel"><div class="ptitle">{html.escape(title)}'
           f'<span class="unit">({html.escape(unit)})</span></div><div class="bars">']
    for name, g, v in series:
        if v is None:
            v = 0.0
        pct = max(1.5, 100.0 * v / top)
        out.append(
            f'<div class="brow"><div class="blabel">{html.escape(name)}</div>'
            f'<div class="btrack"><div class="bfill" style="width:{pct:.1f}%;'
            f'background:{COLORS.get(g, "#888")}"></div></div>'
            f'<div class="bval">{v:.2f}</div></div>')
    out.append("</div></div>")
    return "".join(out)


def build_html(res: list[dict], subtitle: str, note: str) -> str:
    groups = [r["group"] for r in res]
    rows = []
    # ---- 图1: FPS 与 CPU ----
    fps = [(g, r["group"], r["fps"]) for g, r in zip(groups, res)]
    cpu = [(g, r["group"], r["cpu_pct"]) for g, r in zip(groups, res)]
    # ---- 图2: 各阶段耗时 ----
    dec = [(g, r["group"], r["dec_ms"]) for g, r in zip(groups, res)]
    rga = [(g, r["group"], r["rga_ms"]) for g, r in zip(groups, res)]
    npu = [(g, r["group"], r["npu_ms"]) for g, r in zip(groups, res)]
    tot = [(g, r["group"], r["total_ms"]) for g, r in zip(groups, res)]

    body = []
    body.append('<div class="cols">')
    body.append(bar_rows("流水线 FPS", "fps", fps))
    body.append(bar_rows("进程 CPU 占用", "%", cpu, maxv=120))
    body.append("</div>")
    body.append('<div class="cols">')
    body.append(bar_rows("解码耗时", "ms", dec, maxv=max(v for _, _, v in dec) * 1.15))
    body.append(bar_rows("RGA 预处理耗时", "ms", rga, maxv=max(v for _, _, v in rga) * 1.15))
    body.append("</div>")
    body.append('<div class="cols">')
    body.append(bar_rows("NPU 推理耗时(三组应当一致)", "ms", npu, maxv=max(v for _, _, v in npu) * 1.15))
    body.append(bar_rows("单帧总耗时", "ms", tot, maxv=max(v for _, _, v in tot) * 1.15))
    body.append("</div>")

    # ---- 数据表 ----
    tr = []
    for r in res:
        tr.append(
            f"<tr><td class='g' style='color:{COLORS.get(r['group'], '#fff')}'>{r['group']}</td>"
            f"<td>{html.escape(r['desc'])}</td>"
            f"<td class='num'>{r['fps']:.2f}</td>"
            f"<td class='num'>{r['dec_ms']:.2f}</td>"
            f"<td class='num'>{r['rga_ms']:.2f}</td>"
            f"<td class='num'>{r['npu_ms']:.2f}</td>"
            f"<td class='num'>{r['total_ms']:.2f}</td>"
            f"<td class='num'>{r['cpu_pct']:.1f}%</td></tr>")
    base = next((r for r in res if r["group"] == "A"), None)
    for r in res:
        if base and r["group"] != "A" and base.get("fps"):
            tr.append(
                f"<tr class='delta'><td class='g' style='color:{COLORS.get(r['group'], '#fff')}'>"
                f"{r['group']}</td><td>相对 A</td>"
                f"<td class='num'>{r['fps']/base['fps']*100:.0f}%</td>"
                f"<td class='num'>{r['dec_ms']/max(base['dec_ms'],1e-9):.1f}×</td>"
                f"<td class='num'>{r['rga_ms']/max(base['rga_ms'],1e-9):.1f}×</td>"
                f"<td class='num'>{r['npu_ms']/base['npu_ms']:.2f}×</td>"
                f"<td class='num'>{r['total_ms']/base['total_ms']:.1f}×</td>"
                f"<td class='num'>{r['cpu_pct']/max(base['cpu_pct'],1e-9):.1f}×</td></tr>")

    return f"""<!DOCTYPE html><html><head><meta charset="utf-8"><style>
  *{{box-sizing:border-box}}
  body{{margin:0;background:#0b0e13;color:#e6edf3;font:14px/1.55 "Noto Sans CJK SC","Microsoft YaHei",sans-serif;padding:22px 26px}}
  h1{{font-size:24px;margin:0 0 4px}}
  .sub{{color:#8b949e;font-size:13.5px;margin-bottom:14px}}
  .warn{{background:#1c2128;border-left:3px solid #d29922;padding:9px 13px;border-radius:5px;
        font-size:13px;margin-bottom:16px;color:#e6edf3}}
  .cols{{display:grid;grid-template-columns:1fr 1fr;gap:14px;margin-bottom:14px}}
  .panel{{background:#141922;border:1px solid #232a35;border-radius:9px;padding:12px 14px}}
  .ptitle{{font-size:13px;color:#8b949e;margin-bottom:9px;font-weight:600}}
  .unit{{font-weight:400;color:#6e7681}}
  .brow{{display:flex;align-items:center;gap:9px;margin-bottom:7px}}
  .blabel{{width:64px;font-size:12.5px;color:#c9d1d9;text-align:right}}
  .btrack{{flex:1;background:#0f141b;border:1px solid #232a35;border-radius:5px;height:20px;overflow:hidden}}
  .bfill{{height:100%;border-radius:4px}}
  .bval{{width:66px;text-align:right;font-family:"DejaVu Sans Mono",monospace;font-size:12.5px;color:#e6edf3}}
  table{{width:100%;border-collapse:collapse;background:#141922;border:1px solid #232a35;border-radius:9px;
         overflow:hidden;margin-top:2px}}
  th,td{{padding:7px 11px;border-bottom:1px solid #1d232c;font-size:13px;text-align:left}}
  th{{background:#182029;color:#8b949e;font-weight:600;font-size:12px}}
  td.num{{text-align:right;font-family:"DejaVu Sans Mono",monospace}}
  td.g{{font-weight:700;width:34px}}
  tr.delta td{{color:#8b949e;font-style:italic}}
  .foot{{color:#6e7681;font-size:11.5px;margin-top:12px;line-height:1.7}}
  .foot b{{color:#8b949e}}
</style></head><body>
  <h1>硬件加速公平对比: 满负荷业务下单独关闭某一个硬件单元</h1>
  <div class="sub">{html.escape(subtitle)}</div>
  <div class="warn">★ 三组都保持【检测 + PP-OCR 车牌识别】全开(真实业务), 每组只改一个硬件单元;
    NPU 推理耗时三组一致 —— 说明变量隔离干净, 差异确实来自被关掉的那个硬件单元。</div>
  {''.join(body)}
  <table>
    <tr><th>组</th><th>配置</th><th style="text-align:right">FPS</th><th style="text-align:right">解码 ms</th>
        <th style="text-align:right">预处理 ms</th><th style="text-align:right">NPU ms</th>
        <th style="text-align:right">总耗时 ms</th><th style="text-align:right">CPU</th></tr>
    {''.join(tr)}
  </table>
  <div class="foot">{note}</div>
</body></html>"""


def shoot_html(html_text: str, out_png: str, width: int, height: int) -> None:
    chrome = None
    for c in ("chromium", "chromium-browser", "google-chrome"):
        import shutil
        chrome = shutil.which(c)
        if chrome:
            break
    if not chrome:
        raise RuntimeError("找不到 chromium")
    fd, html_path = tempfile.mkstemp(suffix=".html")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write(html_text)
    port = 9334
    profile = tempfile.mkdtemp(prefix="plot-")
    proc = subprocess.Popen(
        [chrome, "--headless", "--disable-gpu", "--no-sandbox", "--hide-scrollbars",
         f"--remote-debugging-port={port}", f"--user-data-dir={profile}",
         f"--window-size={width},{height}", "about:blank"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ws = None
    try:
        ws = WS(find_page_target(port))
        ws.call("Page.enable")
        ws.call("Runtime.enable")
        ws.call("Page.navigate", {"url": "file://" + html_path})
        time.sleep(2.5)
        res = ws.call("Page.captureScreenshot", {"format": "png", "captureBeyondViewport": True})
        import base64
        with open(out_png, "wb") as f:
            f.write(base64.b64decode(res["data"]))
        print(f"已生成 {out_png} ({os.path.getsize(out_png)//1024} KB)", flush=True)
    finally:
        if ws:
            ws.close()
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        os.unlink(html_path)
        import shutil
        shutil.rmtree(profile, ignore_errors=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default="/tmp/fair_ab/cli_ttl5000.json")
    ap.add_argument("--out", default="/home/seeed/rk_deploy/docs/assets/fair_ab.png")
    ap.add_argument("--title", default="")
    ap.add_argument("--note", default="")
    ap.add_argument("--width", type=int, default=1600)
    ap.add_argument("--height", type=int, default=1180)
    args = ap.parse_args()

    with open(args.data, encoding="utf-8") as f:
        d = json.load(f)
    res = d["results"]
    ttl = d.get("ppocr_ttl")
    subtitle = args.title or (
        f"测试视频 {d.get('video', 'testvideo3.mp4')}(真实骑行路口) · 每组 {d.get('frames', '?')} 帧(同一段画面) · "
        f"单实例(npu-pipe={d.get('pipe', 1) or 1}) · 检测+PP-OCR 识别全程开启"
        + (f" · 车牌缓存 TTL={ttl}ms" if ttl else ""))
    note_html = html.escape(args.note or (
        "测试方法: 三组跑【同一条命令】, 只差一个参数(--decoder mpp / --decoder ffmpeg / --rga 0), "
        "且跑的是同样的帧(同一段画面), 因此逐项可比; CPU 为该进程自己的 utime+stime 占墙钟比例。<br>"
        "数据来源: RK3588 板端实测, 原始数据 /tmp/fair_ab/*.json, 复现脚本 rk3588_zero_copy/scripts/fair_ab_test.py"))
    note = note_html.replace("&lt;br&gt;", "<br>")
    page = build_html(res, subtitle, note)
    shoot_html(page, args.out, args.width, args.height)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
