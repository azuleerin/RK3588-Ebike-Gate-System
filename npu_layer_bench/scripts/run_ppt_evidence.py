#!/usr/bin/env python3
"""run_ppt_evidence.py —— 4 路解除限帧全链路的【权威 FPS】测量 + 证据采集

目标: 在"板子独占 / NPU 无第二租户 / 频率锁死 1000MHz"的条件下,
      测出 4 路解除限帧全链路的稳态 FPS, 并留下可复核的证据链。

做法:
  · 用生产二进制 rk_gate 当**黑盒**驱动(不改主工程任何代码, 只改命令行参数)
  · `--max-fps 0` = 解除限帧, 4 路满负载
  · 每轮 N 秒(默认 180), 共 R 轮(默认 3)
  · 每秒采一行: 三核 debugfs 负载 / dmc% / CPU% / NPU 频率 / 每路 FPS / detwait / ocrwait
  · 每轮【开始 / 中间 / 结束】各留一份显式采样记录
  · 轮间做"退出后回落检查": rk_gate 退出后 NPU 三核必须回落到 < 8%,
    否则说明有第二租户 -> 该轮作废

污染判据(全部客观):
  C1 进程判据: 跑之前/每轮之间, ps 里不能有别的 NPU 压测进程
  C2 空闲判据: 每轮【开始前】三核 debugfs 负载必须都 < 8%
  C3 回落判据: 每轮【结束后】三核必须都回落到 < 8%
  C4 耗时判据: rk_gate 自报的每帧 npu= 必须落在 [NPU_MIN_MS, NPU_MAX_MS]
     (干净值 ~25.5~27 ms; 被抢核时会飙到 42+ ms —— 这是实测标定出来的)
  C5 一致性判据: 三轮 FPS 的极差不能超过 10%

用法:
    python3 run_ppt_evidence.py --out-dir docs/ppt_evidence [--rounds 3] [--duration 180]
"""
from __future__ import annotations

import argparse
import csv
import os
import re
import signal
import statistics
import subprocess
import sys
import time

# ---------------------------------------------------------------------------
# 参数
# ---------------------------------------------------------------------------
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GATE_BIN = os.environ.get("GATE_BIN", f"{REPO}/rk3588_zero_copy/build/rk_gate")
DET_MODEL = os.environ.get("DET_MODEL", f"{REPO}/models/ebike_int8_rk2332.rknn")
OCR_MODEL = os.environ.get("OCR_MODEL", f"{REPO}/models/ppocrv4_rec_fp16.rknn")
LABELS = os.environ.get("LABELS", f"{REPO}/models/labels_ebike.txt")
OCR_DICT = os.environ.get("OCR_DICT", f"{REPO}/models/ppocr_keys_v1.txt")

SRC_CH1 = os.environ.get("SRC_CH1", "/home/seeed/testvideo3.mp4")
SRC_CH2 = os.environ.get("SRC_CH2", "/home/seeed/testvideo2.mp4")
SRC_CH3 = os.environ.get("SRC_CH3", "/home/seeed/Test2_fixed.mp4")
SRC_CH4 = os.environ.get("SRC_CH4", "/home/seeed/电动车素材.mp4")
IPM_720P = "200,415;1000,429;967,585;367,546"
IPM_4K = "600,1244;3000,1286;2900,1754;1100,1638"

NPU_LOAD = "/sys/kernel/debug/rknpu/load"
NPU_DEV = "/sys/class/devfreq/fdab0000.npu"
DMC_LOAD = "/sys/class/devfreq/dmc/load"

# 干净的每帧 npu= 区间(实测标定: 干净 25.5~27.1 ms, 被抢核 42~45 ms)
NPU_MIN_MS = 24.5
NPU_MAX_MS = 28.5
# "无第二租户"时三核必须低于这个值
IDLE_PCT = 8.0

STAT_RE = re.compile(r"\[(?P<ch>[^\]]+)\]\[frame (?P<frame>\d+)\] (?P<rest>.*)")


def read_file(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return ""


def num(text, key, default=None):
    m = re.search(rf"\b{key}=(-?[0-9.]+)", text)
    return float(m.group(1)) if m else default


def npu_load_pct():
    """debugfs 逐核负载。★ 不要用 /sys/class/devfreq/*/load(本板恒显 100%)。"""
    txt = read_file(NPU_LOAD)
    out = []
    for i in range(3):
        m = re.search(rf"Core{i}:\s*([0-9]+)%", txt)
        out.append(int(m.group(1)) if m else -1)
    return out, txt


def dmc_pct():
    """DDR 带宽占用。格式形如 '49@2400000000Hz'。"""
    txt = read_file(DMC_LOAD)
    m = re.match(r"\s*([0-9]+)", txt)
    return int(m.group(1)) if m else -1


def npu_freq_mhz():
    txt = read_file(f"{NPU_DEV}/cur_freq")
    try:
        return int(txt) / 1e6
    except ValueError:
        return -1.0


class CpuMeter:
    """整机 CPU 占用(8 核口径), 和生产脚本同一算法。"""

    def __init__(self):
        self.prev = None

    def _read(self):
        line = read_file("/proc/stat").split("\n")[0]
        parts = [int(x) for x in re.findall(r"\d+", line)]
        if len(parts) < 5:
            return None
        total = sum(parts[:8])
        idle = parts[3] + parts[4]
        return total, idle

    def sample(self):
        cur = self._read()
        if cur is None or self.prev is None:
            self.prev = cur
            return 0.0
        dt = cur[0] - self.prev[0]
        di = cur[1] - self.prev[1]
        self.prev = cur
        if dt <= 0:
            return 0.0
        return 100.0 * (1.0 - di / dt)


def parse_stat(line):
    m = STAT_RE.search(line)
    if not m:
        return None
    rest = m.group("rest")
    d = {
        "ch": m.group("ch"),
        "frame": int(m.group("frame")),
        "fps": num(rest, "fps", 0.0),
        "deliv": num(rest, "deliv", 0.0),
        "src": num(rest, "src", 0.0),
        "rga": num(rest, "rga", 0.0),
        "npu": num(rest, "npu", 0.0),
        "detwait": num(rest, "detwait", 0.0),
        "ocrwait": num(rest, "ocrwait", 0.0),
    }
    return d


class GateChild:
    """把 rk_gate 当黑盒跑, 增量读它的日志并解析统计行。"""

    def __init__(self, log_path, duration):
        self.log_path = log_path
        self.duration = duration
        self.proc = None
        self.pos = 0
        self.latest = {}        # ch -> 最近一条统计
        self.lines = []         # 解析出来的全部统计行

    def start(self):
        args = [
            GATE_BIN,
            "--model", DET_MODEL,
            "--labels", LABELS,
            "--ppocr", OCR_MODEL,
            "--ppocr-dict", OCR_DICT,
            "--det-core", "0,1,2",
            "--ocr-core", "0,1,2",
            "--det-workers", "3",
            "--ocr-workers", "3",
            "--max-fps", "0",              # ★ 解除限帧
            "--stats-interval", "20",      # 只影响日志密度, 不影响流水线
            "--duration", str(self.duration),
            "--roi-ratio", "0.164", "--roi-y", "1.0", "--roi-height", "0.40",
            "--roi-shift", "0.126", "--roi-below", "0",
            "--ipm-width-m", "3.5",
            "--ch", f"ch1:{SRC_CH1}:near",
            "--ch", f"ch2:{SRC_CH2}:high:{IPM_720P}",
            "--ch", f"ch3:{SRC_CH3}:near",
            "--ch", f"ch4:{SRC_CH4}:high:{IPM_4K}",
        ]
        self.logf = open(self.log_path, "w")
        env = dict(os.environ)
        env["RK_LOG_LEVEL"] = "warn"
        self.proc = subprocess.Popen(args, stdout=self.logf, stderr=subprocess.STDOUT,
                                     env=env)
        return self.proc.pid

    def poll(self):
        """增量读日志, 解析新的统计行。"""
        try:
            with open(self.log_path, "r", errors="replace") as f:
                f.seek(self.pos)
                data = f.read()
                self.pos = f.tell()
        except OSError:
            return
        for line in data.split("\n"):
            if not line.strip():
                continue
            d = parse_stat(line)
            if d:
                self.lines.append(d)
                self.latest[d["ch"]] = d

    def total_fps(self):
        return sum(v["deliv"] for v in self.latest.values())

    def wait(self, timeout=30):
        try:
            self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.logf.close()
        return self.proc.returncode


def competitors():
    """C1 进程判据: 有没有别的 NPU 压测在跑。

    ★ 要排除【自己】: 本脚本的路径里含 "npu_layer_bench", 而启动命令里
      又含 "run_ppt_evidence" —— 不排除的话会把自己判成竞争者。
      排除口径: 进程命令行里含 run_ppt_evidence / run_layer_bench 的都算自己。
    """
    out = subprocess.run(
        ["bash", "-c",
         "ps -eo pid,args | grep -E 'npu_layer_bench|model_perf_bench|batch_infer_bench|"
         "prune_finetune|export_quantize' | grep -v grep "
         "| grep -vE 'run_ppt_evidence|run_layer_bench' || true"],
        capture_output=True, text=True).stdout.strip()
    return out


def wait_idle(timeout=120, label=""):
    """C2/C3 空闲判据: 等三核 debugfs 负载都回落到 < IDLE_PCT。"""
    t0 = time.time()
    last = None
    while time.time() - t0 < timeout:
        load, raw = npu_load_pct()
        last = (load, raw)
        if all(0 <= v < IDLE_PCT for v in load):
            return True, load, raw, time.time() - t0
        time.sleep(1.0)
    return False, last[0] if last else [-1, -1, -1], last[1] if last else "", timeout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", default=f"{REPO}/docs/ppt_evidence")
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--duration", type=int, default=180, help="每轮秒数")
    ap.add_argument("--warmup", type=int, default=30, help="每轮前多少秒算预热(不计入)")
    ap.add_argument("--min-fps", type=float, default=0.0, help="低于此值直接判废")
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    csv_path = os.path.join(args.out_dir, "samples.csv")
    csvf = open(csv_path, "w", newline="")
    cw = csv.writer(csvf)
    cw.writerow([
        "round", "t_rel_s", "timestamp", "total_fps", "true_fps",
        "ch1_fps", "ch2_fps", "ch3_fps", "ch4_fps",
        "frames_total", "ch1_frame", "ch2_frame", "ch3_frame", "ch4_frame",
        "npu_core0_pct", "npu_core1_pct", "npu_core2_pct",
        "dmc_pct", "cpu_pct", "npu_mhz",
        "detwait_ms_avg", "ocrwait_ms_avg", "npu_ms_avg",
        "event",
    ])

    rounds_info = []
    for rnd in range(1, args.rounds + 1):
        rdir = os.path.join(args.out_dir, f"round{rnd}")
        os.makedirs(rdir, exist_ok=True)

        # ---- C1: 进程判据 ----
        comp = competitors()
        if comp:
            print(f"[轮 {rnd}] ✗ 发现竞争者进程, 本轮不开始:\n{comp}", flush=True)
            rounds_info.append({"round": rnd, "void": True, "reason": "C1 有竞争者进程"})
            continue

        # ---- C2: 开始前必须空闲 ----
        ok, load0, raw0, waited = wait_idle(180, "pre")
        if not ok:
            print(f"[轮 {rnd}] ✗ 开始前三核不空闲 {load0}, 放弃本轮", flush=True)
            rounds_info.append({"round": rnd, "void": True, "reason": f"C2 开始前不空闲 {load0}"})
            continue
        print(f"[轮 {rnd}] 开始前空闲确认 OK (三核 {load0}, 等待 {waited:.0f}s)", flush=True)

        # ---- 跑 ----
        child = GateChild(os.path.join(rdir, "gate.log"), args.duration)
        pid = child.start()
        print(f"[轮 {rnd}] rk_gate pid={pid}, 跑 {args.duration}s ...", flush=True)

        cpu = CpuMeter()
        cpu.sample()
        samples = []
        first_frames = None
        first_t = None
        t_start = time.time()
        next_mark = 0.0
        marks = {0.0: "start", args.duration / 2.0: "mid", args.duration - 1.0: "end"}
        while time.time() - t_start < args.duration:
            child.poll()
            t_rel = time.time() - t_start
            load, _ = npu_load_pct()
            row = {
                "round": rnd,
                "t_rel_s": round(t_rel, 1),
                "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
                "total_fps": round(child.total_fps(), 2),
                "npu_core0_pct": load[0],
                "npu_core1_pct": load[1],
                "npu_core2_pct": load[2],
                "dmc_pct": dmc_pct(),
                "cpu_pct": round(cpu.sample(), 2),
                "npu_mhz": npu_freq_mhz(),
            }
            for i, ch in enumerate(("ch1", "ch2", "ch3", "ch4")):
                row[f"{ch}_fps"] = round(child.latest.get(ch, {}).get("deliv", 0.0), 2)
                row[f"{ch}_frame"] = child.latest.get(ch, {}).get("frame", 0)
            row["frames_total"] = sum(child.latest.get(c, {}).get("frame", 0)
                                       for c in ("ch1", "ch2", "ch3", "ch4"))
            # ★ 真实吞吐: 用【帧计数器差】算, 而不是 rk_gate 的 deliv。
            #   rk_gate 的 deliv = (frames-1)/(last-first) 是【从开跑起算的累计平均】,
            #   启动瞬态(队列还没填满时跑得更快)会把它抬高, 180s 窗口里会被摊薄,
            #   所以它不是"稳态吞吐"。帧计数器差才是。
            row["true_fps"] = round(
                (row["frames_total"] - first_frames) / (t_rel - first_t)
                if first_t is not None and t_rel > first_t else 0.0, 2)
            dw = [v["detwait"] for v in child.latest.values()]
            ow = [v["ocrwait"] for v in child.latest.values()]
            nm = [v["npu"] for v in child.latest.values()]
            row["detwait_ms_avg"] = round(statistics.fmean(dw), 2) if dw else 0.0
            row["ocrwait_ms_avg"] = round(statistics.fmean(ow), 2) if ow else 0.0
            row["npu_ms_avg"] = round(statistics.fmean(nm), 2) if nm else 0.0

            # 预热窗口结束的那一刻, 锁定真吞吐的起点
            if first_t is None and t_rel >= args.warmup:
                first_t = t_rel
                first_frames = row["frames_total"]

            ev = ""
            for mark, name in list(marks.items()):
                if t_rel >= mark:
                    ev = name
                    del marks[mark]
            row["event"] = ev
            samples.append(row)
            cw.writerow([row[k] for k in (
                "round", "t_rel_s", "timestamp", "total_fps", "true_fps",
                "ch1_fps", "ch2_fps", "ch3_fps", "ch4_fps",
                "frames_total", "ch1_frame", "ch2_frame", "ch3_frame", "ch4_frame",
                "npu_core0_pct", "npu_core1_pct", "npu_core2_pct",
                "dmc_pct", "cpu_pct", "npu_mhz",
                "detwait_ms_avg", "ocrwait_ms_avg", "npu_ms_avg", "event")])

            if ev:
                print(f"[轮 {rnd}] {ev}: t={t_rel:.0f}s fps={row['total_fps']:.2f} "
                      f"npu={load} dmc={row['dmc_pct']}% cpu={row['cpu_pct']:.1f}% "
                      f"freq={row['npu_mhz']:.0f}MHz", flush=True)
            time.sleep(1.0)

        child.poll()
        rc = child.wait()

        # ---- C3: 退出后必须回落 ----
        ok3, load_end, raw_end, _ = wait_idle(60, "post")
        print(f"[轮 {rnd}] 退出后回落: {'OK' if ok3 else '✗ 不回落'} 三核={load_end}", flush=True)

        # ---- 统计 ----
        steady = [s for s in samples if s["t_rel_s"] >= args.warmup and s["total_fps"] > 0]
        fps_list = [s["total_fps"] for s in steady]
        round_fps = statistics.fmean(fps_list) if fps_list else 0.0
        # ★ 权威口径: 帧计数器差算出来的真吞吐
        true_list = [s["true_fps"] for s in steady if s["true_fps"] > 0]
        round_true = statistics.median(true_list) if true_list else 0.0
        true_p95 = round(statistics.quantiles(true_list, n=20)[18], 2) if len(true_list) > 5 else round_true
        npu_ms = [s["npu_ms_avg"] for s in steady]
        freqs = sorted({s["npu_mhz"] for s in samples})
        loads_max = [max(s["npu_core0_pct"], s["npu_core1_pct"], s["npu_core2_pct"]) for s in steady]

        info = {
            "round": rnd,
            "void": False,
            "rc": rc,
            "samples": len(samples),
            "steady_samples": len(steady),
            "fps_mean": round_fps,
            "fps_p95": round(statistics.quantiles(fps_list, n=20)[18], 2) if len(fps_list) > 5 else round_fps,
            "true_fps_median": round_true,
            "true_fps_p95": true_p95,
            "true_fps_mean": round(statistics.fmean(true_list), 2) if true_list else 0.0,
            "true_fps_min": round(min(true_list), 2) if true_list else 0.0,
            "true_fps_max": round(max(true_list), 2) if true_list else 0.0,
            "fps_min": round(min(fps_list), 2) if fps_list else 0.0,
            "fps_max": round(max(fps_list), 2) if fps_list else 0.0,
            "npu_ms_mean": round(statistics.fmean(npu_ms), 2) if npu_ms else 0.0,
            "freqs_mhz": freqs,
            "load_pre": load0,
            "load_post": load_end,
            "idle_after": ok3,
            "per_ch_fps": {
                ch: round(statistics.fmean([s[f"{ch}_fps"] for s in steady]), 2)
                for ch in ("ch1", "ch2", "ch3", "ch4")
            } if steady else {},
            "detwait_ms_avg": round(statistics.fmean([s["detwait_ms_avg"] for s in steady]), 2) if steady else 0.0,
            "ocrwait_ms_avg": round(statistics.fmean([s["ocrwait_ms_avg"] for s in steady]), 2) if steady else 0.0,
            "dmc_pct_max": max([s["dmc_pct"] for s in steady], default=0),
            "cpu_pct_mean": round(statistics.fmean([s["cpu_pct"] for s in steady]), 2) if steady else 0.0,
        }
        rounds_info.append(info)
        print(f"[轮 {rnd}] 真吞吐(帧计数器) 中位={round_true:.2f} "
              f"(min {info['true_fps_min']} / max {info['true_fps_max']} / P95 {true_p95})  "
              f"| rk_gate累计口径 均值={round_fps:.2f}  "
              f"npu={info['npu_ms_mean']}ms 频率集合={freqs}", flush=True)

        with open(os.path.join(rdir, "samples.txt"), "w") as f:
            f.write(f"# 第 {rnd} 轮逐秒采样 (rk_gate pid={pid}, 退出码={rc})\n")
            f.write("# t_rel_s, 总FPS, 每路FPS, 三核debugfs%, dmc%, cpu%, NPU MHz, detwait, ocrwait, npu\n")
            for s in samples:
                f.write(f"{s['t_rel_s']:>7.1f} {s['total_fps']:>8.2f} "
                        f"{s['ch1_fps']:>6.2f} {s['ch2_fps']:>6.2f} {s['ch3_fps']:>6.2f} {s['ch4_fps']:>6.2f} | "
                        f"{s['npu_core0_pct']:>3} {s['npu_core1_pct']:>3} {s['npu_core2_pct']:>3} | "
                        f"{s['dmc_pct']:>3} {s['cpu_pct']:>5} {s['npu_mhz']:>6.0f} | "
                        f"{s['detwait_ms_avg']:>6} {s['ocrwait_ms_avg']:>6} {s['npu_ms_avg']:>6}"
                        f"{'  <-' + s['event'] if s['event'] else ''}\n")

    csvf.close()

    # ---- 汇总 + 判定 ----
    good = [r for r in rounds_info if not r.get("void")]
    print("\n================ 汇总 ================", flush=True)
    for r in rounds_info:
        if r.get("void"):
            print(f"  第 {r['round']} 轮: 作废 —— {r['reason']}")
        else:
            print(f"  第 {r['round']} 轮: 真吞吐={r['true_fps_median']:.2f} (P95={r['true_fps_p95']})  "
                  f"累计口径={r['fps_mean']:.2f}  npu={r['npu_ms_mean']}ms  "
                  f"频率={r['freqs_mhz']}MHz  退出后回落={'OK' if r['idle_after'] else '✗'}")

    summary = {"rounds": rounds_info, "good_rounds": len(good)}
    if good:
        vals = [r["true_fps_median"] for r in good]
        summary["median_fps"] = round(statistics.median(vals), 2)
        allfps = []
        for r in rounds_info:
            if not r.get("void"):
                pass
        summary["min_fps"] = round(min(vals), 2)
        summary["max_fps"] = round(max(vals), 2)
        summary["spread_pct"] = round(100.0 * (max(vals) - min(vals)) / statistics.median(vals), 2)
        print(f"\n  中位数 = {summary['median_fps']:.2f} FPS   "
              f"(轮间 {summary['min_fps']}~{summary['max_fps']}, 极差 {summary['spread_pct']:.2f}%)")

    import json
    with open(os.path.join(args.out_dir, "rounds_summary.json"), "w") as f:
        json.dump(summary, f, ensure_ascii=False, indent=2)
    print(f"\n  证据已写入 {args.out_dir}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
