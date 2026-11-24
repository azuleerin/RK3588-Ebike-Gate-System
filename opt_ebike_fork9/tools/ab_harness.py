#!/usr/bin/env python3
"""ab_harness.py —— rk_gate 板端 A/B 测量台(帧计数器差分口径)

口径(按任务书要求):
  · 吞吐 = (frames_end - frames_start) / (t_end - t_start)
    —— 【不用】rk_gate 自报的 deliv(累计平均, 会虚高 2~4%)
  · 统计窗口: 进程启动 +15s 之后到结束前 5s(避开起播爬坡)
  · 每核占用: /sys/kernel/debug/rknpu/load 逐核采样(不是恒显 100% 的 devfreq load)
  · 内存: 日志里的 rss= / heap=(mallinfo2().uordblks), 取首尾做斜率

用法:
  ab_harness.py --bin <rk_gate> --label base --duration 100 --out result_base.json
"""
import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import threading
import time

# ---- 生产 4 路配置(与 config/gate.env 一致) ----
MODEL = os.environ.get("AB_MODEL", "/home/seeed/rk_deploy/models/ebike_int8_rk2332.rknn")
LABELS = os.environ.get("AB_LABELS", "/home/seeed/rk_deploy/models/labels_ebike.txt")
OCR = os.environ.get("AB_OCR", "/home/seeed/rk_deploy/models/ppocrv4_rec_fp16.rknn")
DICT = "/home/seeed/rk_deploy/models/ppocr_keys_v1.txt"
CHANNELS = [
    "ch1:/home/seeed/testvideo3.mp4:near",
    "ch2:/home/seeed/testvideo2.mp4:high:200,415;1000,429;967,585;367,546",
    "ch3:/home/seeed/Test2_fixed.mp4:near",
    "ch4:/home/seeed/电动车素材.mp4:high:600,1244;3000,1286;2900,1754;1100,1638",
]
LD_PATH = ("/home/seeed/rk_deploy/rk3588_zero_copy/third_party/lib:"
           "/home/seeed/rk_deploy/rk3588_zero_copy/lib:/usr/lib")

# [ch1][frame 1000] fps=.. deliv=.. src=.. rga=..ms npu=..ms detwait=..ms
#                ocrwait=..ms objs=.. tracks=.. spd=.. sv=.. rss=..KB heap=..KB
STAT_RE = re.compile(
    r"\[(?P<ch>ch\d+)\]\[frame (?P<frame>\d+)\]\s+"
    r"fps=(?P<fps>[\d.]+)\s+deliv=(?P<deliv>[\d.]+)\s+src=(?P<src>[\d.]+)\s+"
    r"rga=(?P<rga>[\d.]+)ms\s+npu=(?P<npu>[\d.]+)ms\s+"
    r"detwait=(?P<detwait>[\d.]+)ms\s+ocrwait=(?P<ocrwait>[\d.]+)ms\s+"
    r"objs=(?P<objs>\d+)\s+tracks=(?P<tracks>\d+)\s+"
    r"spd=(?P<spd>[\d.]+)\s+sv=(?P<sv>\d+)\s+"
    r"rss=(?P<rss>\d+)KB\s+heap=(?P<heap>\d+)KB")

RKNPU_LOAD = "/sys/kernel/debug/rknpu/load"
SUDO = ["sudo", "-n"]


def read_npu_load():
    """读 rknpu/load 的三核占用。需要 root。"""
    try:
        p = subprocess.run(SUDO + ["cat", RKNPU_LOAD],
                           capture_output=True, timeout=5)
        txt = p.stdout.decode(errors="ignore")
        pairs = re.findall(r"Core(\d):\s*(\d+)%", txt)
        return [int(v) for _, v in sorted(pairs, key=lambda x: int(x[0]))]
    except Exception:
        return None


class Run:
    def __init__(self, binary, duration, warmup=15.0, tail=5.0, max_fps=20):
        self.binary = binary
        self.duration = duration
        self.max_fps = max_fps
        self.warmup = warmup
        self.tail = tail
        self.lines = []          # (t, text)
        self.npu_samples = []    # (t, [c0,c1,c2])
        self.proc = None

    def _reader(self):
        for raw in self.proc.stderr:
            t = time.monotonic()
            self.lines.append((t, raw.decode(errors="ignore").rstrip("\n")))

    def _sampler(self):
        while not self._stop.is_set():
            c = read_npu_load()
            if c:
                self.npu_samples.append((time.monotonic(), c))
            self._stop.wait(2.0)

    def go(self):
        env = dict(os.environ)
        env["LD_LIBRARY_PATH"] = LD_PATH
        env["RK_LOG_LEVEL"] = "warn"
        cmd = [self.binary,
               "--model", MODEL, "--labels", LABELS,
               "--ppocr", OCR, "--ppocr-dict", DICT,
               "--det-core", "0,1,2", "--ocr-core", "0,1,2",
               "--det-workers", "3", "--ocr-workers", "3", "--det-slots", "2",
               "--conf", "0.15", "--nms", "0.45", "--max-fps", str(self.max_fps),
               "--stats-interval", "100",
               "--roi-ratio", "0.164", "--roi-y", "1.0", "--roi-height", "0.40",
               "--roi-shift", "0.126", "--roi-below", "0", "--ipm-width-m", "3.5",
               "--sn", "RK3588-GATE-01", "--device-name", "0001",
               # ★ 不写死 IP: 由本机自动取(与生产 gate.env 的 GATE_IPADDR=auto 同口径)
               "--ipaddr", os.environ.get("GATE_IPADDR", "auto"),
               "--duration", str(self.duration)]
        for ch in CHANNELS:
            cmd += ["--ch", ch]
        t0 = time.monotonic()
        self.proc = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.PIPE, bufsize=1)
        th = threading.Thread(target=self._reader, daemon=True)
        th.start()
        self._stop = threading.Event()
        ts = threading.Thread(target=self._sampler, daemon=True)
        ts.start()
        self.proc.wait()
        self._stop.set()
        th.join(timeout=3)
        self.t0 = t0
        self.t_end = time.monotonic()

    def analyze(self):
        lo = self.t0 + self.warmup
        hi = self.t_end - self.tail
        per_ch = {}
        for t, line in self.lines:
            m = STAT_RE.search(line)
            if not m:
                continue
            if t < lo or t > hi:
                continue
            ch = m.group("ch")
            per_ch.setdefault(ch, []).append((t, int(m.group("frame")), m.groupdict()))
        out = {"channels": {}}
        total_fps = 0.0
        for ch, rows in sorted(per_ch.items()):
            rows.sort(key=lambda r: r[0])
            if len(rows) < 2:
                continue
            t_a, f_a, _ = rows[0]
            t_b, f_b, _ = rows[-1]
            dt = t_b - t_a
            fps = (f_b - f_a) / dt if dt > 0 else 0.0
            total_fps += fps
            # 窗口内各指标的均值
            npu = [float(r[2]["npu"]) for r in rows]
            det = [float(r[2]["detwait"]) for r in rows]
            ocr = [float(r[2]["ocrwait"]) for r in rows if float(r[2]["ocrwait"]) > 0]
            rss = [int(r[2]["rss"]) for r in rows]
            heap = [int(r[2]["heap"]) for r in rows]
            out["channels"][ch] = {
                "frames_start": f_a, "frames_end": f_b,
                "t_start": round(t_a - self.t0, 3), "t_end": round(t_b - self.t0, 3),
                "fps_delta": round(fps, 3),
                "npu_ms": round(statistics.mean(npu), 3),
                "detwait_ms": round(statistics.mean(det), 3),
                "ocrwait_ms": round(statistics.mean(ocr), 3) if ocr else 0.0,
                "ocrwait_n": len(ocr),
                "rss_first_kb": rss[0], "rss_last_kb": rss[-1],
                "rss_delta_kb": rss[-1] - rss[0],
                "heap_first_kb": heap[0], "heap_last_kb": heap[-1],
                "heap_delta_kb": heap[-1] - heap[0],
                "heap_slope_kb_per_min": round((heap[-1] - heap[0]) / (dt / 60.0), 2),
                "rss_slope_kb_per_min": round((rss[-1] - rss[0]) / (dt / 60.0), 2),
                "samples": len(rows),
            }
        out["total_fps"] = round(total_fps, 3)
        # NPU 每核占用(采样区间取稳态段)
        if self.npu_samples:
            ss = [(t, c) for t, c in self.npu_samples if lo <= t <= hi]
            if ss:
                out["npu_core_pct"] = {
                    f"core{i}": round(statistics.mean([c[i] for _, c in ss]), 1)
                    for i in range(min(3, len(ss[0][1])))
                }
                out["npu_samples_n"] = len(ss)
        # 全局内存斜率(取所有通道的日志行)
        heaps, rsss, ts = [], [], []
        for t, line in self.lines:
            m = STAT_RE.search(line)
            if m and lo <= t <= hi:
                heaps.append(int(m.group("heap")))
                rsss.append(int(m.group("rss")))
                ts.append(t)
        if len(heaps) > 2:
            span = ts[-1] - ts[0]
            out["heap_global"] = {
                "first_kb": heaps[0], "last_kb": heaps[-1],
                "slope_kb_per_min": round((heaps[-1] - heaps[0]) / (span / 60.0), 2),
                "min_kb": min(heaps), "max_kb": max(heaps),
            }
            out["rss_global"] = {
                "first_kb": rsss[0], "last_kb": rsss[-1],
                "slope_kb_per_min": round((rsss[-1] - rsss[0]) / (span / 60.0), 2),
                "min_kb": min(rsss), "max_kb": max(rsss),
            }
        return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--label", required=True)
    ap.add_argument("--duration", type=int, default=100)
    ap.add_argument("--warmup", type=float, default=15.0)
    ap.add_argument("--max-fps", type=int, default=20)
    ap.add_argument("--out", required=True)
    ap.add_argument("--model", default=None, help="检测模型 rknn (默认生产模型)")
    ap.add_argument("--labels", default=None, help="标签文件 (默认生产标签)")
    ap.add_argument("--ocr", default=None, help="OCR 模型 rknn (默认生产 FP16)")
    ap.add_argument("--log", default=None)
    args = ap.parse_args()

    idle = read_npu_load()
    print(f"[{args.label}] 起跑前 NPU 每核占用: {idle}", flush=True)

    # ★ 必须写 global: 否则 main() 里的 MODEL 赋值被当成【局部变量】,
    #   Run.go() 读的仍然是模块级全局 = 生产模型 ->
    #   两组 A/B 悄悄跑的是同一个模型, 数据全废(实测踩到: 两组都是 形态C nc=2)
    global MODEL, LABELS, OCR
    if args.model:
        MODEL = args.model
    if args.labels:
        LABELS = args.labels
    if args.ocr:
        OCR = args.ocr
    r = Run(args.bin, args.duration, args.warmup, max_fps=args.max_fps)
    r.go()
    res = r.analyze()
    res["label"] = args.label
    res["binary"] = args.bin
    res["duration_s"] = args.duration
    res["max_fps"] = args.max_fps
    res["npu_idle_before"] = idle

    logpath = args.log or (args.out.replace(".json", ".log"))
    with open(logpath, "w") as f:
        for t, line in r.lines:
            f.write(f"{t - r.t0:8.3f} {line}\n")
    with open(args.out, "w") as f:
        json.dump(res, f, indent=2, ensure_ascii=False)

    print(f"[{args.label}] max_fps={args.max_fps} 总 FPS(4 路帧计数器差分) = {res['total_fps']}", flush=True)
    for ch, d in res["channels"].items():
        print(f"   {ch}: fps={d['fps_delta']:.2f} npu={d['npu_ms']:.2f}ms "
              f"detwait={d['detwait_ms']:.2f}ms ocrwait={d['ocrwait_ms']:.2f}ms "
              f"heap_slope={d['heap_slope_kb_per_min']:+.1f}KB/min", flush=True)
    if "npu_core_pct" in res:
        print(f"   NPU 每核: {res['npu_core_pct']}", flush=True)
    if "heap_global" in res:
        print(f"   活堆: {res['heap_global']}", flush=True)
        print(f"   RSS : {res['rss_global']}", flush=True)


if __name__ == "__main__":
    main()
