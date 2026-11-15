#!/usr/bin/env python3
"""npu_bench.py —— NPU 多核并行收益基准(板端跑)

测什么:
    在【同一条视频源】上, 分别用 --npu-pipe 1 / 2 / 3 跑 N 帧, 对比:
        fps          流水线吞吐(墙钟)
        npu_ms       NPU 单帧耗时(并行模式下是【每个实例】的真实推理耗时)
        cpu_pct      进程 CPU 总占用(可超过 100% = 用了几核)
        rss_growth   RSS 增长(跑完比跑开始多多少 KB)

用法:
    # 跑一遍矩阵(1/2/3 实例, 各 1000 帧)
    python3 scripts/npu_bench.py --src /tmp/vid/clip1080_60.mp4 --frames 1000

    # 只跑一组
    python3 scripts/npu_bench.py --src video.mp4 --pipe 2 --frames 1000

    # 换源 / 限速 / 循环
    python3 scripts/npu_bench.py --src video.mp4 --sync 0 --loop

输出:
    终端一张 Markdown 表; --json out.json 另外存一份原始数据。
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# ---- 主程序最后那段"性能统计"块 ----
RE_STATS = {
    "fps": re.compile(r"流水线 FPS\s*:\s*([\d.]+)"),
    "src_fps": re.compile(r"相机推送 FPS\s*:\s*([\d.]+)"),
    "wait_ms": re.compile(r"解码等待\s*:\s*([\d.]+)"),
    "dec_ms": re.compile(r"^\s*解码\s*:\s*([\d.]+)", re.M),
    "rga_ms": re.compile(r"RGA 转换\s*:\s*([\d.]+)"),
    "npu_ms": re.compile(r"NPU 推理\s*:\s*([\d.]+)"),
    "npu_run_ms": re.compile(r"rknn_run\s*([\d.]+)"),
    "npu_out_ms": re.compile(r"取回输出\s*([\d.]+)"),
    "post_ms": re.compile(r"后处理\s*:\s*([\d.]+)"),
    "draw_ms": re.compile(r"画框\+显示\s*:\s*([\d.]+)"),
    "total_ms": re.compile(r"单帧总耗时\s*:\s*([\d.]+)"),
}
# ---- 逐帧统计行: [frame N] fps=.. deliv=.. npu=..ms(run../out..) rss=..KB heap=..KB ----
RE_FRAME = re.compile(
    r"\[frame (\d+)\].*?fps=([\d.]+).*?deliv=([\d.]+).*?"
    r"npu=([\d.]+)ms\(run([\d.]+)/out([\d.]+)\)"
    r".*?objs=(\d+).*?rss=(\d+)KB heap=(\d+)KB")
RE_PIPE = re.compile(r"\[NPU-Pipe\] 实例 (\d+) core_mask=0x([0-9a-f]+)")
RE_PIPE_N = re.compile(r"\[NPU-Pipe\] 并行检测已开启: (\d+) 个实例")


def read_proc_stat(pid: int) -> tuple[float, int]:
    """返回 (cpu 秒, RSS KB)"""
    cpu = 0.0
    rss = 0
    try:
        with open(f"/proc/{pid}/stat", "r") as f:
            parts = f.read().split()
        # utime(14) stime(15) 都在第 14/15 个字段(1-based)
        ticks = os.sysconf("SC_CLK_TCK")
        cpu = (int(parts[13]) + int(parts[14])) / ticks
    except Exception:  # noqa: BLE001
        pass
    try:
        with open(f"/proc/{pid}/status", "r") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    rss = int(line.split()[1])
                    break
    except Exception:  # noqa: BLE001
        pass
    return cpu, rss


class Sampler(threading.Thread):
    """后台采样进程的 CPU 时间 / RSS / VPU 负载"""

    def __init__(self, pid: int, interval: float = 0.3):
        super().__init__(daemon=True)
        self.pid = pid
        self.interval = interval
        self.stop_flag = False
        self.samples: list[tuple[float, float, int]] = []
        self.vpu_max = -1.0
        self.vpu_avg = -1.0

    def run(self):
        vpu_vals: list[float] = []
        while not self.stop_flag:
            try:
                pid = self.pid
                cpu, rss = read_proc_stat(pid)
                self.samples.append((time.time(), cpu, rss))
                v = read_mpp_load()
                if v >= 0:
                    vpu_vals.append(v)
            except Exception:  # noqa: BLE001
                pass
            time.sleep(self.interval)
        if vpu_vals:
            self.vpu_max = max(vpu_vals)
            self.vpu_avg = sum(vpu_vals) / len(vpu_vals)


def read_mpp_load() -> float:
    """MPP 解码核占用(%) —— 不用 root 就能读"""
    try:
        with open("/proc/mpp_service/load", "r") as f:
            best = -1.0
            for line in f:
                if "rkvdec" not in line:
                    continue
                pos = line.find("load:")
                if pos < 0:
                    continue
                num = ""
                for ch in line[pos + 5:]:
                    if ch.isdigit() or ch == ".":
                        num += ch
                    elif num:
                        break
                if num:
                    v = float(num)
                    if v > best:
                        best = v
            return best
    except Exception:  # noqa: BLE001
        return -1.0


def count_other_users(me_pid: int) -> list[str]:
    """板子上还有谁在用 NPU(别的会话也在跑基准时会互相干扰, 必须记下来)"""
    out = []
    for pid in os.listdir("/proc"):
        if not pid.isdigit() or int(pid) == me_pid:
            continue
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as f:
                cmd = f.read().replace(b"\0", b" ").decode("utf-8", "replace")
        except Exception:  # noqa: BLE001
            continue
        if "rk_zero_copy" in cmd and "npu_bench" not in cmd:
            out.append(f"{pid}:{cmd.split()[0].split('/')[-1]}")
    return out


def run_once(args, pipe: int, cores: str | None, depth: int = 0) -> dict:
    cmd = [
        args.bin,
        args.src,
        args.model,
        "--labels", args.labels,
        "--decoder", "mpp",
        "--no-show",
        "--no-speed",
        "--frames", str(args.frames),
        "--stats-interval", str(args.stats_interval),
        "--npu-pipe", str(pipe),
        "--conf", "0.15",
        "--nms", "0.45",
    ]
    if depth:
        cmd += ["--npu-pipe-depth", str(depth)]
    if args.sync is not None:
        cmd += ["--sync-src", str(args.sync)]
    if args.loop:
        cmd += ["--loop-src"]
    if cores:
        cmd += ["--npu-pipe-cores", cores]
    if args.extra:
        cmd += shlex.split(args.extra)

    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = (os.path.join(ROOT, "third_party", "lib") + ":" +
                               os.path.join(ROOT, "lib") + ":/usr/lib:" +
                               env.get("LD_LIBRARY_PATH", ""))

    print(f"\n▶ --npu-pipe {pipe}" + (f" (cores={cores})" if cores else "") +
          f"  源={os.path.basename(args.src)}  帧数={args.frames}" +
          (f"  sync={args.sync}" if args.sync is not None else "") +
          (f"  loop" if args.loop else ""), flush=True)

    t0 = time.time()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            env=env, text=True, errors="replace")
    others = count_other_users(proc.pid)
    sampler = Sampler(proc.pid)
    sampler.start()

    out = []
    for line in proc.stdout:                      # type: ignore[union-attr]
        out.append(line)
    proc.wait()
    sampler.stop_flag = True
    sampler.join(timeout=2)
    wall = time.time() - t0

    text = "".join(out)
    rec: dict = {
        "pipe": pipe,
        "depth": depth,
        "cores": cores or "auto",
        "cmd": " ".join(shlex.quote(c) for c in cmd),
        "wall_s": round(wall, 2),
        "returncode": proc.returncode,
        "stderr_tail": "\n".join(out[-25:]),
        # 板子上还有谁在用 NPU —— 不记下来, 数字没法解释
        "other_npu_users": others,
        "n_other_users": len(others),
    }

    # ---- 主程序自己报的统计 ----
    for k, rx in RE_STATS.items():
        m = rx.search(text)
        if m:
            rec[k] = float(m.group(1))
    mp = RE_PIPE_N.search(text)
    if mp:
        rec["instances"] = int(mp.group(1))
    rec["core_masks"] = [int(m, 16) for _, m in RE_PIPE.findall(text)]

    # ---- 逐帧行: 取后半段(避开启动期的抖动)算 RSS/堆增长 ----
    frames = RE_FRAME.findall(text)
    if frames:
        rec["frames_logged"] = len(frames)
        rec["frame_last"] = int(frames[-1][0])
        tail = frames[len(frames) // 2:]            # 后一半
        rec["rss_start_kb"] = int(tail[0][7])
        rec["rss_end_kb"] = int(tail[-1][7])
        rec["rss_growth_kb"] = rec["rss_end_kb"] - rec["rss_start_kb"]
        rec["heap_start_kb"] = int(tail[0][8])
        rec["heap_end_kb"] = int(tail[-1][8])
        rec["heap_growth_kb"] = rec["heap_end_kb"] - rec["heap_start_kb"]
        rec["fps_logged_avg"] = sum(float(f[1]) for f in tail) / len(tail)
        # 交付帧率(主程序自己算的墙钟口径, 不含进程启动/模型加载的时间)
        rec["delivered_fps"] = round(sum(float(f[2]) for f in tail) / len(tail), 2)
        rec["npu_ms_logged_avg"] = sum(float(f[3]) for f in tail) / len(tail)
        rec["npu_run_logged_avg"] = sum(float(f[4]) for f in tail) / len(tail)
        rec["objs_avg"] = sum(int(f[6]) for f in tail) / len(tail)

    # ---- 外部采样: CPU 占用 / RSS ----
    ss = sampler.samples
    # 丢掉头 1 秒的样本(进程刚起, RSS/线程数还在爬, 算进去会把基线拉低)
    if len(ss) > 4:
        ss = ss[3:]
    if len(ss) >= 2:
        dt = ss[-1][0] - ss[0][0]
        dc = ss[-1][1] - ss[0][1]
        rec["cpu_pct"] = round(100.0 * dc / dt, 1) if dt > 0 else -1.0
        rec["cpu_cores_used"] = round(dc / dt, 2) if dt > 0 else -1.0
        rec["rss_kb"] = ss[-1][2]
        rec["rss_first_kb"] = ss[0][2]
        rec["rss_peak_kb"] = max(s[2] for s in ss)
    rec["vpu_load_max"] = round(sampler.vpu_max, 1)
    rec["vpu_load_avg"] = round(sampler.vpu_avg, 1)

    print(f"  fps={rec.get('fps', -1):.2f}  npu={rec.get('npu_ms', -1):.2f}ms  "
          f"dec={rec.get('dec_ms', -1):.2f}ms  rga={rec.get('rga_ms', -1):.2f}ms  "
          f"total={rec.get('total_ms', -1):.2f}ms", flush=True)
    print(f"  CPU={rec.get('cpu_pct', -1)}%  (≈{rec.get('cpu_cores_used', -1)} 核)  "
          f"RSS {rec.get('rss_first_kb', 0)}->{rec.get('rss_kb', 0)} KB  "
          f"(帧内增长 {rec.get('rss_growth_kb', 0)} KB)", flush=True)
    if others:
        print(f"  ⚠ 板上还有 {len(others)} 个别的 NPU 任务在跑: {', '.join(others)}"
              f"  -> 本组数字受干扰", flush=True)
    return rec


def median_of(runs: list[dict], key: str, default: float = -1.0) -> float:
    vals = sorted(r[key] for r in runs if key in r)
    if not vals:
        return default
    n = len(vals)
    return vals[n // 2] if n % 2 else (vals[n // 2 - 1] + vals[n // 2]) / 2.0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default="/tmp/vid/clip1080_60.mp4")
    ap.add_argument("--model", default=os.path.join(ROOT, "model", "ebike_int8_rk2332.rknn"))
    ap.add_argument("--labels", default=os.path.join(ROOT, "model", "labels_ebike.txt"))
    ap.add_argument("--frames", type=int, default=1000)
    ap.add_argument("--pipe", type=int, default=0, help="只跑这一个并行度(0=跑 1/2/3)")
    ap.add_argument("--configs", default="",
                    help="配置列表(覆盖 --pipe), 语法 '实例数:流水线深度', 逗号分隔\n"
                         "  例: 1:0,2:1,2:0,3:0   -> 单实例 / 双实例老行为 / 双实例新行为 / 三实例新行为\n"
                         "  深度 0=自动(=实例数, 并行跑), 1=老行为(提交完就等结果)")
    ap.add_argument("--cores", default="", help="指定核, 如 1,2 (只对 --pipe >= 2 生效)")
    ap.add_argument("--sync", type=int, default=None,
                    help="0=极速跑, 1=按源帧率; 不给=程序默认")
    ap.add_argument("--loop", action="store_true", help="文件播完从头再来")
    ap.add_argument("--stats-interval", type=int, default=50)
    ap.add_argument("--extra", default="", help="追加给主程序的参数(带引号)")
    ap.add_argument("--json", default="", help="把原始结果存到这个文件")
    ap.add_argument("--repeat", type=int, default=1, help="每组跑几遍(取中位数)")
    ap.add_argument("--bin", default=os.path.join(ROOT, "build", "rk_zero_copy"),
                    help="主程序路径(可以做 A/B: 换二进制比)")
    ap.add_argument("--wait-idle", type=int, default=0,
                    help="开跑前等板子上没有别的 NPU 任务(秒); 0=不等")
    args = ap.parse_args()

    if not os.path.exists(args.src):
        print(f"找不到视频源: {args.src}", file=sys.stderr)
        return 1

    if args.wait_idle > 0:
        deadline = time.time() + args.wait_idle
        while time.time() < deadline:
            others = count_other_users(os.getpid())
            if not others:
                print("板子空闲, 开始测试", flush=True)
                break
            print(f"等待板子空闲(还有 {len(others)} 个任务: {', '.join(others)})...",
                  flush=True)
            time.sleep(10)
        else:
            print(f"⚠ 等了 {args.wait_idle}s 板子还是忙, 带着干扰开跑", flush=True)

    pipes = [args.pipe] if args.pipe > 0 else [1, 2, 3]
    # 配置列表: (实例数, 流水线深度)
    configs: list[tuple[int, int]] = []
    if args.configs:
        for tok in args.configs.split(","):
            tok = tok.strip()
            if not tok:
                continue
            parts = tok.split(":")
            configs.append((int(parts[0]), int(parts[1]) if len(parts) > 1 else 0))
    else:
        configs = [(p, 0) for p in pipes]

    all_runs: list[dict] = []
    agg: list[dict] = []
    # ★ 交错跑(1,2,3,1,2,3...): 板子上如果有别的任务在跑, 各组受到的干扰一样,
    #   取中位数后对比才公平。
    for rep in range(args.repeat):
        for (p, d) in configs:
            cores = args.cores if (p >= 2 and args.cores) else None
            rec = run_once(args, p, cores, d)
            rec["rep"] = rep
            all_runs.append(rec)

    for (p, d) in configs:
        runs = [r for r in all_runs if r["pipe"] == p and r["depth"] == d]
        if not runs:
            continue
        a = {"pipe": p, "depth": d, "runs": len(runs)}
        for k in ("fps", "delivered_fps", "npu_ms", "npu_run_ms", "dec_ms", "rga_ms",
                  "post_ms", "total_ms", "cpu_pct", "cpu_cores_used", "rss_kb",
                  "rss_growth_kb", "heap_growth_kb", "vpu_load_avg", "wall_s",
                  "npu_run_logged_avg", "objs_avg"):
            a[k] = median_of(runs, k)
        a["fps_min"] = min((r.get("fps", 0) for r in runs), default=0)
        a["fps_max"] = max((r.get("fps", 0) for r in runs), default=0)
        a["deliv_min"] = min((r.get("delivered_fps", 0) for r in runs), default=0)
        a["deliv_max"] = max((r.get("delivered_fps", 0) for r in runs), default=0)
        a["contaminated"] = sum(1 for r in runs if r.get("n_other_users", 0) > 0)
        a["core_masks"] = runs[0].get("core_masks", []) if runs else []
        agg.append(a)

    # ---- 汇总表 ----
    print("\n" + "=" * 110)
    print(f" NPU 多核并行基准汇总   源={os.path.basename(args.src)}  "
          f"帧数={args.frames}  sync={args.sync}  重复={args.repeat}  "
          f"二进制={os.path.basename(args.bin)}")
    print("=" * 110)
    hdr = (f"{'实例':<5}{'深度':<6}{'核掩码':<13}{'处理FPS':>9}{'交付FPS':>9}{'交付范围':>14}"
           f"{'单实例跑 ms':>13}{'解码 ms':>9}{'RGA ms':>8}{'总 ms':>8}{'CPU%':>7}"
           f"{'核数':>7}{'RSS增长':>10}{'堆增长':>10}{'干扰':>6}")
    print(hdr)
    print("-" * 126)
    for r in agg:
        masks = ",".join(f"0x{m:x}" for m in r.get("core_masks", [])) or "-"
        print(f"{r['pipe']:<5}{('自动' if not r['depth'] else r['depth']):<6}{masks:<13}"
              f"{r.get('fps', -1):>9.2f}{r.get('delivered_fps', -1):>9.2f}"
              f"{f'{r.get('deliv_min', 0):.1f}~{r.get('deliv_max', 0):.1f}':>14}"
              f"{r.get('npu_run_logged_avg', -1):>13.2f}"
              f"{r.get('dec_ms', -1):>9.2f}{r.get('rga_ms', -1):>8.2f}"
              f"{r.get('total_ms', -1):>8.2f}{r.get('cpu_pct', -1):>7.1f}"
              f"{r.get('cpu_cores_used', -1):>7.2f}"
              f"{str(r.get('rss_growth_kb', '?')) + 'KB':>10}"
              f"{str(r.get('heap_growth_kb', '?')) + 'KB':>10}"
              f"{r.get('contaminated', 0):>6}")
    print("-" * 126)
    base = next((r for r in agg if r["pipe"] == 1), None)
    if base and base.get("fps"):
        for r in agg:
            if r["pipe"] == 1:
                continue
            tag = f"{r['pipe']}实例(深度{r['depth'] or '自动'})"
            print(f"  {tag} vs 单实例: 吞吐 x{r.get('fps', 0) / base['fps']:.2f}"
                  f"  (+{r.get('fps', 0) - base['fps']:.1f} fps)")

    if args.json:
        with open(args.json, "w") as f:
            json.dump({"agg": agg, "runs": all_runs}, f, ensure_ascii=False, indent=2)
        print(f"\n原始数据: {args.json}")
    return 0
    base = next((r for r in results if r["pipe"] == 1), None)
    if base and base.get("fps"):
        for r in results:
            if r["pipe"] == 1:
                continue
            print(f"  {r['pipe']} 实例 vs 1 实例: 吞吐 x{r.get('fps', 0) / base['fps']:.2f}"
                  f"  (+{r.get('fps', 0) - base['fps']:.1f} fps)")

    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f, ensure_ascii=False, indent=2)
        print(f"\n原始数据: {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
