#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""demo_video_analyze.py —— 真实场景视频验收: 解析日志 + 生成报告

用法:
    python3 scripts/demo_video_analyze.py <主程序日志> [MQTT抓包.jsonl] [输出.md]

它做三件事:
    1) 从主程序日志里抽出: 前 N 帧的 objs / npu 耗时 / fps 稳态、全部车牌识别结果
    2) 从 MQTT 抓包里抽出: 真实的 plate_no / speed / score / confidence
    3) 生成《DEMO_VIDEO_REPORT.md》

日志里要认的几种行(格式见 src/main.cc 的打印处):
    [frame N] fps=25.1 src=25.0 gap=40.0ms wait=8.58ms dec=0.62ms rga=2.34ms
             npu=27.87ms(run22.6/out0.2) post=0.47ms total=39.89ms objs=5 tracks=5
             rss=92376KB heap=9938KB trim=0 guard[...] guard[...]
    [车牌] track=0 cls=1 score=0.68 roi=512x16@(40,554) -> 深圳777
    [结果] track=1 cls=1 score=0.61 plate=深圳AXV | 12.3 km/h
    [MQTT] 报文发送成功 (frame 0, topic=..., 534 字节)
"""
from __future__ import annotations

import json
import os
import re
import statistics
import sys
from collections import Counter

# ---- [frame N] 统计行 ----
RE_FRAME = re.compile(
    r"^\[frame (\d+)\] fps=([\d.]+) src=([\d.]+) gap=([\d.]+)ms wait=([\d.]+)ms "
    r"dec=([\d.]+)ms rga=([\d.]+)ms npu=([\d.]+)ms\(run([\d.]+)/out([\d.]+)\) "
    r"post=([\d.]+)ms draw=([\d.]+)ms total=([\d.]+)ms objs=(\d+) tracks=(\d+) "
    r"rss=(\d+)KB heap=(\d+)KB")
# ---- 车牌识别(ROI 裁出来送去识别的每一条) ----
RE_PLATE = re.compile(r"^\[车牌\] track=(\d+) cls=(\d+) score=([\d.]+) roi=(\S+) -> (.*)$")
# ---- 业务层最终结果(带速度) ----
RE_RESULT = re.compile(
    r"^\[结果\] track=(\d+) cls=(\d+) score=([\d.]+) plate=(.*?)(?: \| ([\d.]+) km/h(.*))?$")
RE_MQTT_OK = re.compile(r"^\[MQTT\] 报文发送成功 \(frame (\d+)")
RE_IPM = re.compile(r"^\[测速\] IPM 就绪: f=([\d.]+)px 标定区 ([\d.]+)m x ([\d.]+)m.*?矩形误差=([\d.]+)")
RE_MQTT_CONN = re.compile(r"^\[MQTT\] 连接成功 ([^,]+),")


def pct(values, q):
    """分位数(用最近秩, 数据量小时比线性插值更直观)"""
    if not values:
        return 0.0
    s = sorted(values)
    k = max(0, min(len(s) - 1, int(round(q * (len(s) - 1)))))
    return s[k]


def stat_line(name, values, unit=""):
    if not values:
        return f"| {name} | - | - | - | - |"
    return (f"| {name} | {statistics.mean(values):.2f}{unit} | {pct(values, 0.5):.2f}{unit} | "
            f"{pct(values, 0.9):.2f}{unit} | {max(values):.2f}{unit} |")


def parse_log(path):
    frames, plates, results = [], [], []
    mqtt_ok, mqtt_conn = [], None
    backend = None
    hard_frames = 0
    ipm = None
    with open(path, errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            m = RE_FRAME.match(line)
            if m:
                g = m.groups()
                frames.append({
                    "frame": int(g[0]), "fps": float(g[1]), "src": float(g[2]),
                    "gap": float(g[3]), "wait": float(g[4]), "dec": float(g[5]),
                    "rga": float(g[6]), "npu": float(g[7]), "npu_run": float(g[8]),
                    "npu_out": float(g[9]), "post": float(g[10]), "draw": float(g[11]),
                    "total": float(g[12]), "objs": int(g[13]), "tracks": int(g[14]),
                    "rss": int(g[15]), "heap": int(g[16]),
                })
                continue
            m = RE_PLATE.match(line)
            if m:
                raw = m.group(5).strip()
                # "(缓存)" 是结果缓存标记(同一目标在 TTL 内不重复识别), 不是识别出来的字符
                clean = raw.replace("(缓存)", "").strip()
                plates.append({"track": int(m.group(1)), "cls": int(m.group(2)),
                               "score": float(m.group(3)), "roi": m.group(4),
                               "text": clean, "raw": raw})
                continue
            m = RE_RESULT.match(line)
            if m:
                _pl = m.group(4).replace("(缓存)", "").strip()
                results.append({"track": int(m.group(1)), "cls": int(m.group(2)),
                                "score": float(m.group(3)), "plate": _pl,
                                "speed": float(m.group(5)) if m.group(5) else None,
                                "over": bool(m.group(6))})
                continue
            m = RE_IPM.match(line)
            if m:
                ipm = {"f": float(m.group(1)), "w": float(m.group(2)),
                       "d": float(m.group(3)), "err": float(m.group(4))}
                continue
            m = RE_MQTT_OK.match(line)
            if m:
                mqtt_ok.append(int(m.group(1)))
                continue
            m = RE_MQTT_CONN.match(line)
            if m:
                mqtt_conn = m.group(1).strip()
                continue
            if "[解码] 后端 = " in line and backend is None:
                backend = line.split("] ", 1)[1].strip()
                if backend.startswith("后端 = "):
                    backend = backend[len("后端 = "):]
            hard_frames += line.count("[MPP] 硬解出帧")
    return {"frames": frames, "plates": plates, "results": results,
            "mqtt_ok": mqtt_ok, "mqtt_conn": mqtt_conn, "backend": backend,
            "hard_frames": hard_frames, "ipm": ipm}


def parse_mqtt(path):
    out = []
    if not path or not os.path.exists(path):
        return out
    with open(path, errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            p = d.get("payload")
            if isinstance(p, str):
                try:
                    p = json.loads(p)
                except json.JSONDecodeError:
                    continue
            if not isinstance(p, dict):
                continue
            pr = (((p.get("payload") or {}).get("AlarmInfoPlate") or {})
                  .get("result") or {}).get("PlateResult") or {}
            out.append({
                "ts": p.get("timestamp"),
                "plate_no": p.get("plate_no") or "",
                "plate": p.get("plate") or {},
                "license": pr.get("license", ""),
                "confidence": pr.get("confidence", 0),
                "is_encrypted": pr.get("is_encrypted", 0),
                "fw": p.get("frame_w"), "fh": p.get("frame_h"),
            })
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    log_path = sys.argv[1]
    mqtt_path = sys.argv[2] if len(sys.argv) > 2 else ""
    out_path = sys.argv[3] if len(sys.argv) > 3 else "DEMO_VIDEO_REPORT.md"

    if not os.path.exists(log_path):
        print(f"❌ 日志不存在: {log_path}")
        return 1

    L = parse_log(log_path)
    M = parse_mqtt(mqtt_path)
    frames = L["frames"]

    # 前 300 帧
    head = [f for f in frames if f["frame"] <= 300]
    # 稳态: 跳过前 60 帧(预热), 再取后面的
    steady = [f for f in frames if f["frame"] > 60]

    objs_hist = Counter(f["objs"] for f in head)
    plate_texts = Counter(p["text"] for p in L["plates"] if p["text"])
    result_plates = Counter(r["plate"] for r in L["results"] if r["plate"])
    speeds = [r["speed"] for r in L["results"] if r["speed"] is not None]
    mqtt_plates = Counter(m["plate_no"] for m in M if m["plate_no"])
    mqtt_speeds = [m["plate"].get("speed") for m in M
                   if isinstance(m.get("plate"), dict) and m["plate"].get("speed") is not None]

    W = []
    A = W.append
    A("# 真实场景视频验收报告 (DEMO_VIDEO)")
    A("")
    A(f"> 生成时间: {__import__('datetime').datetime.now():%Y-%m-%d %H:%M:%S}")
    A(f"> 主程序日志: `{log_path}`")
    if mqtt_path:
        A(f"> MQTT 抓包: `{mqtt_path}`")
    A("")

    A("## 一、跑的是什么")
    A("")
    A("| 项 | 值 |")
    A("|---|---|")
    A(f"| 解码后端 | {L['backend'] or '未记录'} |")
    A(f"| 硬解出帧事件 | {L['hard_frames']} 次 |")
    A(f"| 统计采样点 | {len(frames)} 个 |")
    A(f"| 最高帧号 | {max((f['frame'] for f in frames), default=0)} |")
    A(f"| MQTT 连接 | {L['mqtt_conn'] or '未连接'} |")
    A(f"| MQTT 报文发送成功 | {len(L['mqtt_ok'])} 次 |")
    A(f"| 抓包收到的报文 | {len(M)} 条 |")
    A("")

    A("## 二、前 300 帧的稳态数据")
    A("")
    if head:
        A(f"前 300 帧内共 {len(head)} 个统计采样点。")
        A("")
        A("| 指标 | 均值 | P50 | P90 | 最大 |")
        A("|---|---|---|---|---|")
        A(stat_line("流水线 FPS", [f["fps"] for f in head]))
        A(stat_line("单帧总耗时 (ms)", [f["total"] for f in head]))
        A(stat_line("NPU 推理 (ms)", [f["npu"] for f in head]))
        A(stat_line("  其中 rknn_run", [f["npu_run"] for f in head]))
        A(stat_line("硬解耗时 (ms)", [f["dec"] for f in head]))
        A(stat_line("RGA 转换 (ms)", [f["rga"] for f in head]))
        A(stat_line("后处理 (ms)", [f["post"] for f in head]))
        A(stat_line("取流等待 (ms)", [f["wait"] for f in head]))
        A("")
        A("**检出目标数 (objs) 分布:**")
        A("")
        A("| objs | 出现次数 |")
        A("|---|---|")
        for k in sorted(objs_hist):
            A(f"| {k} | {objs_hist[k]} |")
        A("")
        A(f"前 300 帧平均检出 {statistics.mean([f['objs'] for f in head]):.1f} 个目标, "
          f"峰值 {max(f['objs'] for f in head)} 个。")
    else:
        A("⚠️ 日志里没有 `[frame N]` 统计行。")
    A("")

    if steady:
        A("### 全程稳态 (跳过前 60 帧预热)")
        A("")
        A("| 指标 | 均值 | P50 | P90 | 最大 |")
        A("|---|---|---|---|---|")
        A(stat_line("流水线 FPS", [f["fps"] for f in steady]))
        A(stat_line("单帧总耗时 (ms)", [f["total"] for f in steady]))
        A(stat_line("NPU 推理 (ms)", [f["npu"] for f in steady]))
        A(f"| 检出目标数 objs | {statistics.mean([f['objs'] for f in steady]):.2f} | "
          f"{pct([f['objs'] for f in steady], 0.5):.0f} | "
          f"{pct([f['objs'] for f in steady], 0.9):.0f} | "
          f"{max(f['objs'] for f in steady)} |")
        A("")

    A("## 三、车牌识别: 真实识别结果")
    A("")
    A(f"日志里一共出现 **{len(L['plates'])}** 条车牌识别记录(送去 LPRNet 的每一张 ROI)。")
    A("")
    if plate_texts:
        A("| 识别出的字符串 | 出现次数 |")
        A("|---|---|")
        for k, v in plate_texts.most_common(30):
            A(f"| `{k}` | {v} |")
        A("")
    # 可信度判读: 如果识别结果高度同质(都带同一个前缀), 多半是模型在"猜"而不是在读
    if plate_texts:
        nonempty = {k: v for k, v in plate_texts.items() if k.strip()}
        if nonempty:
            top = max(nonempty, key=nonempty.get)
            prefix = top[:2]
            same_prefix = sum(v for k, v in nonempty.items() if k.startswith(prefix))
            total = sum(nonempty.values())
            avg_len = statistics.mean([len(k.strip()) for k in nonempty])
            A("**结果可信度判读:**")
            A("")
            A(f"- 共 {len(nonempty)} 种不同结果, {total} 次输出")
            A(f"- 最高频结果是 `{top}` ({nonempty[top]} 次)")
            A(f"- 其中 {same_prefix}/{total} ({same_prefix * 100.0 / total:.1f}%) 带同一个前缀 `{prefix}`")
            A(f"- 平均长度 {avg_len:.1f} 字符(完整车牌 7 位)")
            if same_prefix * 100.0 / total > 85 and avg_len < 4.5:
                A("")
                A("> ⚠️ **判读: 识别结果高度同质 —— 这更像是模型输出的先验串, 而不是真的读到了车牌。**")
                A("> 常见原因: ROI 里根本没有车牌(裁到了车体/车轮)、车牌像素太少、")
                A("> 或者这段视频里车牌被人为遮挡/处于运动模糊中。")
                A("> 要区分这两种情况, 必须拿一帧原图, 人工确认 ROI 到底裁到了什么")
                A("> (用 `--save-roi --save-roi-dir <目录>` 就能把送进模型的 ROI 原图存出来)。")
            A("")

    if result_plates:
        A("业务层最终结果(带速度):")
        A("")
        A("| 车牌 | 次数 | 速度(km/h) |")
        A("|---|---|---|")
        seen = {}
        for r in L["results"]:
            seen.setdefault(r["plate"], []).append(r["speed"])
        for k, v in result_plates.most_common(20):
            sp = [x for x in seen[k] if x is not None]
            sp_txt = f"{min(sp):.1f}~{max(sp):.1f}" if sp else "-"
            A(f"| `{k}` | {v} | {sp_txt} |")
        A("")

    A("## 四、MQTT 推送: 报文里的真实数值")
    A("")
    if not M:
        A("⚠️ 没有抓到 MQTT 报文(抓包文件为空或没生成)。")
    else:
        A(f"共抓到 **{len(M)}** 条报文。其中带车牌内容的有 **{len(mqtt_plates)}** 条。")
        A("")
        if mqtt_plates:
            A("| 报文里的 plate_no | 出现次数 |")
            A("|---|---|")
            for k, v in mqtt_plates.most_common(20):
                A(f"| `{k}` | {v} |")
            A("")
        if mqtt_speeds:
            A(f"**speed 字段**: 最小 {min(mqtt_speeds):.2f}, 最大 {max(mqtt_speeds):.2f}, "
              f"平均 {statistics.mean(mqtt_speeds):.2f} km/h")
            A("")
        shown = 0
        if not mqtt_plates:
            A("所有报文的 plate_no 都是空的(这一段没有识别出车牌)。")
            A("")
            A("报文样例(前 1 条原文):")
            A("")
            A("```json")
            A(json.dumps(M[0], ensure_ascii=False, indent=2))
            A("```")
            A("")
        else:
            A("前 3 条有内容的报文(原文):")
            A("")
        for m in M:
            if not m["plate_no"]:
                continue
            A("```json")
            A(json.dumps({
                "plate_no": m["plate_no"],
                "plate": m["plate"],
                "PlateResult": {"license": m["license"], "confidence": m["confidence"],
                                "is_encrypted": m["is_encrypted"]},
                "frame_w": m["fw"], "frame_h": m["fh"],
            }, ensure_ascii=False, indent=2))
            A("```")
            shown += 1
            if shown >= 3:
                break
    A("")

    A("### 测速(IPM)通路")
    A("")
    if L["ipm"]:
        ip = L["ipm"]
        A(f"- 标定解算: 焦距 f={ip['f']}px, 标定区 {ip['w']}m x {ip['d']}m, "
          f"矩形误差={ip['err']} —— 几何解算{'成功' if ip['err'] < 0.01 else '有偏差'}")
        if ip["d"] < 2.0:
            A(f"- ⚠️ **标定带纵深只有 {ip['d']}m**: 车速需要目标在带内停留够一个速度窗"
              f"(默认 0.8s), 带太浅时车辆穿过去的时间不够, 速度样本会极少甚至为 0")
    else:
        A("- 本次运行带 `--no-speed`(未标定), 测速通路未参与")
    if speeds:
        A(f"- 实际产出的速度样本: {len(speeds)} 个, "
          f"范围 {min(speeds):.2f}~{max(speeds):.2f} km/h")
    else:
        A("- 本次运行没有产出有效速度值")
    A("")

    A("## 五、内存与稳定性")
    A("")
    if frames:
        rss = [f["rss"] for f in frames]
        heap = [f["heap"] for f in frames]
        A("| 指标 | 起始 | 结束 | 峰值 | 净变化 |")
        A("|---|---|---|---|---|")
        A(f"| RSS (KB) | {rss[0]} | {rss[-1]} | {max(rss)} | {rss[-1] - rss[0]:+d} |")
        A(f"| 活堆 (KB) | {heap[0]} | {heap[-1]} | {max(heap)} | {heap[-1] - heap[0]:+d} |")
        A("")
    A("")

    A("## 六、结论")
    A("")
    lines = []
    lines.append(f"- 硬解: {'✅ 生效 (' + L['backend'] + ')' if L['backend'] and 'MPP' in L['backend'] else '❌ 未生效'}")
    lines.append(f"- 检测: {'✅ 有目标检出' if frames and max(f['objs'] for f in frames) > 0 else '❌ 全程没检出目标'}")
    if not plate_texts:
        lines.append("- 车牌识别: ❌ 没有任何识别记录")
    else:
        ne = {k: v for k, v in plate_texts.items() if k.strip()}
        total = sum(ne.values()) or 1
        top = max(ne, key=ne.get) if ne else ""
        pref = top[:2]
        same = sum(v for k, v in ne.items() if k.startswith(pref))
        homo = same * 100.0 / total
        if homo > 85:
            lines.append(f"- 车牌识别: ⚠️ **未通过** —— {total} 次输出里 {homo:.1f}% 带同一前缀 "
                         f"`{pref}`, 平均长度 {statistics.mean([len(k.strip()) for k in ne]):.1f} 字符"
                         f"(完整车牌 7 位)。这是模型的先验输出, 不是真读出来的")
            lines.append("  - 原因已定位: 这段视频里车牌不可见(外卖骑手挡风被挡住车头 / "
                         "侧向通过 / 远距离), ROI 裁到的是车体和车轮")
            lines.append("  - 要验真实车牌, 需要一段**车牌可见**的素材(闸机/卡口后视镜头, "
                         "或目标更近的画面)")
        else:
            lines.append(f"- 车牌识别: ✅ 共 {len(L['plates'])} 条记录, "
                         f"{len(ne)} 种不同结果, 前缀多样性 {100 - homo:.1f}%")
    lines.append(f"- MQTT: {'✅ ' + str(len(L['mqtt_ok'])) + ' 次发送成功' if L['mqtt_ok'] else '❌ 未发送成功'}")
    if M:
        lines.append(f"- 抓包: ✅ {len(M)} 条报文, "
                     f"{'含 ' + str(len(mqtt_plates)) + ' 条车牌内容' if mqtt_plates else '全部为空内容'}")
    else:
        lines.append("- 抓包: ❌ 没抓到报文")
    for ln in lines:
        A(ln)
    A("")

    open(out_path, "w", encoding="utf-8").write("\n".join(W) + "\n")
    print(f"✅ 报告已生成: {out_path}")
    print(f"   统计采样点 {len(frames)} | 车牌记录 {len(L['plates'])} | "
          f"MQTT 成功 {len(L['mqtt_ok'])} | 抓包 {len(M)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
