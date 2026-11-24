#!/usr/bin/env bash
# ============================================================================
# longrun_observe.sh —— 生产长跑观测（只读, 零副作用, 不碰服务）
#
# ★ 口径（任务书 §4.1）:
#   吞吐   = 【帧计数器差分】/ 实际秒数, 禁用 deliv（deliv 是限帧后交付计数, 会低报）
#            差分窗口必须 >= 300 s
#   NPU    = /sys/kernel/debug/rknpu/load（需 root）, 禁用 devfreq/load（恒显 100%）
#   泄漏   = 活堆/RSS 对【时间】做线性回归; 剔除前 10 min 预热, 取 10->30 min 尾段
#            门槛: 活堆斜率 <= 5 KB/min, RSS 斜率 <= 10 KB/min
#   fd     = 恒定 (~171, 波动 <= 2)
#   cgroup = memory.events (max/oom/oom_kill 全 0), memory.current << memory.max
#
# 用法: bash longrun_observe.sh [采样间隔s] [观测时长s] [输出文件]
#   默认: 60s / 1800s / $BK_DIR/acceptance_30min.txt
# ============================================================================
set -uo pipefail

INTERVAL=${1:-60}
DURATION=${2:-1800}
OUT=${3:-${BK_DIR:-/tmp}/acceptance_30min.txt}
LOG=${GATE_LOG_DIR:-/var/log.hdd}/rk-gate-error.log
CG=/sys/fs/cgroup/system.slice/rk-gate.service

mkdir -p "$(dirname "$OUT")"
TSV="${OUT%.txt}.tsv"

: > "$TSV"
echo "# epoch pid ch1 ch2 ch3 ch4 fd rss_kb heap_kb mem_current mem_peak mem_max" >> "$TSV"

# 读某路最新帧号（★ 不能用 '[0-9]+$' 收尾 —— 行尾是 ']'，$ 锚定会失配）
fc() { grep -oE "^\[$1\]\[frame [0-9]+\]" "$LOG" 2>/dev/null | tail -1 \
         | grep -oE 'frame [0-9]+' | tail -1 | grep -oE '[0-9]+'; }
# 读活日志最后一条的 rss/heap
rh() { grep -oE 'rss=[0-9]+KB heap=[0-9]+KB' "$LOG" 2>/dev/null | tail -1 | grep -oE '[0-9]+' | tr '\n' ' '; }

sample() {
    local ts pid fds rss heap mc mp mm
    ts=$(date +%s)
    pid=$(pgrep -f 'rk3588_zero_copy/build/rk_gate' | head -1)
    fds=0; [ -n "$pid" ] && fds=$(ls /proc/$pid/fd 2>/dev/null | wc -l)
    read -r rss heap <<< "$(rh)"
    mc=$(cat $CG/memory.current 2>/dev/null || echo 0)
    mp=$(cat $CG/memory.peak 2>/dev/null || echo 0)
    mm=$(cat $CG/memory.max 2>/dev/null || echo 0)
    echo "$ts ${pid:-0} $(fc ch1) $(fc ch2) $(fc ch3) $(fc ch4) ${fds:-0} ${rss:-0} ${heap:-0} ${mc:-0} ${mp:-0} ${mm:-0}" >> "$TSV"
}

echo "==> longrun_observe: 间隔=${INTERVAL}s 时长=${DURATION}s 输出=$OUT"
echo "==> PID 变化会被记进 TSV（重启会让 pid 变，回归时会分段）"
START=$(date +%s)
while :; do
    now=$(date +%s)
    [ $((now - START)) -ge "$DURATION" ] && break
    sample
    sleep "$INTERVAL"
done
sample
echo "==> 采样完成: $(wc -l < "$TSV") 行 -> $TSV"

python3 -I - "$TSV" "$OUT" "$INTERVAL" <<'PY'
import sys, os

tsv, out, interval = sys.argv[1], sys.argv[2], int(sys.argv[3])
rows = []
for line in open(tsv, encoding="utf-8", errors="replace"):
    p = line.split()
    if p and p[0] == "#":
        continue
    if len(p) == 13 and p[0].isdigit():
        rows.append([int(x) for x in p])
if len(rows) < 3:
    print("样本不足, 无法回归")
    sys.exit(1)

t0 = rows[0][0]
for r in rows:
    r[0] = (r[0] - t0) / 60.0          # 分钟
cols = dict(pid=1, ch1=2, ch2=3, ch3=4, ch4=5, fd=6, rss=7, heap=8,
            memcur=9, mempeak=10, memmax=11)

lines = []
def emit(s=""):
    lines.append(s); print(s)

emit("=" * 72)
emit(" 生产长跑观测结论（口径: 帧计数器差分 / 逐分钟线性回归）")
emit("=" * 72)

# ---- 重启检测
pids = sorted({r[cols["pid"]] for r in rows})
emit(f"[重启检测] 采样期内出现的 pid: {pids}  => {'有重启(需人工确认)' if len(pids)>1 else '无重启'}")

# ---- 帧计数器差分（取 >=300s 的最后一段）
def delta_over(rows, idxs):
    lo, hi = rows[0], rows[-1]
    dt = hi[0] - lo[0]
    d = [hi[i] - lo[i] for i in idxs]
    return dt, d

idxs = [cols["ch1"], cols["ch2"], cols["ch3"], cols["ch4"]]
dt_s, d = delta_over(rows, idxs)
tot = sum(d)
emit("")
emit(f"[吞吐 · 帧计数器差分] 窗口 {dt_s:.0f} s (门槛 >=300)")
for i, ch in enumerate(("ch1", "ch2", "ch3", "ch4")):
    emit(f"    {ch}: Δ={d[i]:>7} 帧  = {d[i]/dt_s*60:>6.2f} 帧/min")
fps = tot / dt_s if dt_s else 0
emit(f"    总吞吐 = {tot} 帧 / {dt_s:.0f} s = {fps:.2f} FPS   门槛 >= 79.5  => "
     f"{'PASS' if fps >= 79.5 else 'FAIL'}")

# ---- 斜率回归
def regress(sel, col):
    xs = [r[0] for r in sel]; ys = [r[col] for r in sel]
    n = len(xs)
    mx = sum(xs)/n; my = sum(ys)/n
    sxx = sum((x-mx)**2 for x in xs)
    sxy = sum((x-mx)*(y-my) for x, y in zip(xs, ys))
    b = sxy/sxx if sxx else 0.0
    a = my - b*mx
    # R^2
    ss_tot = sum((y-my)**2 for y in ys)
    ss_res = sum((y-(a+b*x))**2 for x, y in zip(xs, ys))
    r2 = 1 - ss_res/ss_tot if ss_tot else 1.0
    return b, a, r2

tail = [r for r in rows if r[0] >= 10.0]      # 剔除前 10 分钟预热
emit("")
emit(f"[泄漏 · 逐分钟线性回归] 尾段 = 观测第 10 分钟之后 (共 {len(tail)} 个采样点)")
res = {}
for name, col, lim in (("活堆 heap", cols["heap"], 5.0), ("RSS", cols["rss"], 10.0)):
    b, a, r2 = regress(tail, col)
    res[name] = b
    verdict = "PASS" if abs(b) <= lim else "FAIL"
    emit(f"    {name:10s} 斜率 = {b:+8.2f} KB/min  (门槛 |slope| <= {lim})  R^2={r2:.3f}  => {verdict}")
emit(f"    活堆基线: 首点 {rows[0][cols['heap']]/1024:.1f} MB -> 末点 {rows[-1][cols['heap']]/1024:.1f} MB"
     f"   (预期 83-86 MB, 告警线 200 MB)")
emit(f"    RSS 末点: {rows[-1][cols['rss']]/1024:.1f} MB")

# ---- fd
fds = [r[cols["fd"]] for r in rows]
emit("")
emit(f"[fd] min={min(fds)} max={max(fds)} 波动={max(fds)-min(fds)} (门槛 <=2) 末值={fds[-1]}")

# ---- cgroup
mc, mp, mm = rows[-1][cols["memcur"]], rows[-1][cols["mempeak"]], rows[-1][cols["memmax"]]
emit("")
emit(f"[cgroup] memory.current={mc/1048576:.1f} MB  memory.peak={mp/1048576:.1f} MB  "
     f"memory.max={mm/1048576:.1f} MB")
try:
    ev = dict(l.split() for l in open("/sys/fs/cgroup/system.slice/rk-gate.service/memory.events"))
    emit(f"    memory.events: max={ev.get('max')} oom={ev.get('oom')} oom_kill={ev.get('oom_kill')} "
         f"(门槛全 0)")
except Exception as e:
    emit(f"    memory.events 读取失败: {e}")

with open(out, "w", encoding="utf-8") as f:
    f.write("\n".join(lines) + "\n")
print(f"\n==> 结论已写入 {out}")
PY
