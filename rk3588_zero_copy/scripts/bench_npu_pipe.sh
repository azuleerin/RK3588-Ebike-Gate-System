#!/usr/bin/env bash
# bench_npu_pipe.sh —— NPU 单实例 vs --npu-pipe 2 双实例 对照跑测
#
# 用法: scripts/bench_npu_pipe.sh <tag> [额外的 rk_zero_copy 参数...]
#   例: scripts/bench_npu_pipe.sh baseline
#       scripts/bench_npu_pipe.sh pipe2 --npu-pipe 2
#
# 做的事:
#   1. tmux 里起 rktop(实时面板: NPU 三核 / RGA / CPU / 进程)
#   2. 起 rk_zero_copy 跑 1080P 视频
#   3. 每秒采样 NPU 三核 + VPU 解码负载 + CPU% + 流水线 FPS -> CSV
#   4. 跑到 70% 时抓 rktop 面板 -> ANSI -> PNG 截图
#   5. 打印汇总
set -u

REPO=/home/seeed/rk_deploy
# ★ 用快照二进制: 另一个会话正在改这个仓库并重建 build/rk_zero_copy,
#   不快照的话跑到一半二进制会被换掉, A/B 就不在同一份代码上了。
BIN=${BIN:-/tmp/bench/rk_zero_copy}
MODEL=$REPO/rk3588_zero_copy/model/ebike_int8_rk2332.rknn
LABELS=$REPO/rk3588_zero_copy/model/labels_ebike.txt
# ★ 当前版本已经把 --lprnet 换成 --ppocr(PP-OCRv4, 6625 类含省份汉字)
PLATE=$REPO/rk3588_zero_copy/model/ppocrv4_rec_int8.rknn
VIDEO=${VIDEO:-/home/seeed/Test2_fixed.mp4}
FRAMES=${FRAMES:-3000}
RKTOP=${RKTOP:-/tmp/rktop-install/bin/rktop}
OUTDIR=$REPO/logs/npu_pipe_bench
mkdir -p "$OUTDIR"

TAG=$1; shift
EXTRA=("$@")

LOG=$OUTDIR/run_$TAG.log
CSV=$OUTDIR/load_$TAG.csv
ANSI=$OUTDIR/rktop_$TAG.ansi
PNG=$OUTDIR/rktop_$TAG.png
SESSION=rt_$TAG

echo "=============================================================="
echo " 跑测 [$TAG]  视频=$VIDEO  帧数=$FRAMES  附加参数=${EXTRA[*]:-无}"
echo "=============================================================="

# ---- VPU 负载采样需要先设 load_interval ----
echo 123456 | sudo -S -p "" bash -c 'echo 1000 > /proc/mpp_service/load_interval' 2>/dev/null

# ---- 1. rktop 实时面板 ----
tmux kill-session -t "$SESSION" 2>/dev/null
tmux new-session -d -s "$SESSION" -x 150 -y 42 \
    "echo 123456 | sudo -S -p '' $RKTOP"
sleep 4

# ---- 2. 负载采样器 ----
(
  prev_total=0; prev_idle=0
  echo "t,npu0,npu1,npu2,vpu_dec,rga,cpu_pct,fps,frame" > "$CSV"
  for i in $(seq 1 400); do
      now=$(date +%s)
      npu=$(echo 123456 | sudo -S -p "" cat /sys/kernel/debug/rknpu/load 2>/dev/null)
      n0=$(echo "$npu" | sed -nE 's/.*Core0: *([0-9]+)%.*/\1/p'); n0=${n0:-0}
      n1=$(echo "$npu" | sed -nE 's/.*Core1: *([0-9]+)%.*/\1/p'); n1=${n1:-0}
      n2=$(echo "$npu" | sed -nE 's/.*Core2: *([0-9]+)%.*/\1/p'); n2=${n2:-0}
      # VPU: 解码核 vdpu 的 load
      vpu=$(echo 123456 | sudo -S -p "" cat /proc/mpp_service/load 2>/dev/null | \
            awk '/vdpu|rkvdec/{gsub("%","",$3); if($3+0>max) max=$3+0} END{printf "%.0f", max+0}')
      # RGA: 取 rga3_0 的 load  (格式是 " load = 9%" -> 第 3 个字段是百分比)
      rga=$(echo 123456 | sudo -S -p "" cat /sys/kernel/debug/rkrga/load 2>/dev/null | \
            awk '/load *= *[0-9]+%/ {gsub("%","",$3); if($3+0>max) max=$3+0} END{printf "%.0f", max+0}')
      # CPU%
      cl=$(awk '/^cpu /{print $2,$3,$4,$5,$6,$7,$8,$9,$10,$11}' /proc/stat)
      read -r u1 n1c s1 i1 w1 ir1 sr1 st1 g1 gn1 <<< "$cl"
      total=$((u1+n1c+s1+i1+w1+ir1+sr1+st1+g1+gn1)); idle=$((i1+w1))
      dt=$((total-prev_total)); di=$((idle-prev_idle))
      if [ "$dt" -gt 0 ]; then cpu=$(awk -v d="$dt" -v i="$di" 'BEGIN{printf "%.1f",(1-i/d)*100}'); else cpu=0; fi
      prev_total=$total; prev_idle=$idle
      last=$(grep -aE "^\[frame [0-9]+\]" "$LOG" 2>/dev/null | tail -1)
      fps=$(echo "$last" | sed -nE 's/.*fps=([0-9.]+).*/\1/p'); fps=${fps:-0}
      fr=$(echo "$last" | sed -nE 's/^\[frame ([0-9]+)\].*/\1/p'); fr=${fr:-0}
      echo "${now},${n0},${n1},${n2},${vpu},${rga:-0},${cpu},${fps},${fr}" >> "$CSV"
      sleep 1
  done
) &
SAMPLER=$!

# ---- 3. 主程序 ----
"$BIN" "$VIDEO" "$MODEL" \
  --decoder mpp --no-speed --labels "$LABELS" \
  --ppocr "$PLATE" --plate-cls 1 \
  --track-cls 1 --conf 0.15 --no-show \
  --frames "$FRAMES" --stats-interval 100 \
  --log-file "$LOG" \
  "${EXTRA[@]}" > /dev/null 2>&1 &
MAIN=$!

# ---- 4. 跑到 70% 时抓 rktop 面板 ----
target=$((FRAMES * 70 / 100))
while kill -0 $MAIN 2>/dev/null; do
    fr=$(grep -aoE "^\[frame [0-9]+\]" "$LOG" 2>/dev/null | tail -1 | grep -oE "[0-9]+")
    fr=${fr:-0}
    if [ "$fr" -ge "$target" ]; then break; fi
    sleep 1
done
sleep 2
tmux capture-pane -t "$SESSION" -e -p > "$ANSI" 2>/dev/null
echo "已抓取 rktop 面板 (frame~$fr)"

wait $MAIN 2>/dev/null
MAIN_RC=$?
sleep 1
kill $SAMPLER 2>/dev/null
tmux kill-session -t "$SESSION" 2>/dev/null

# ---- 5. ANSI -> PNG ----
python3 "$REPO/rk3588_zero_copy/scripts/ansi_to_png.py" "$ANSI" "$PNG" \
        "rktop — $TAG : ${EXTRA[*]:-单实例基线}" > /dev/null 2>&1 && echo "截图已存: $PNG"

# ---- 汇总 ----
python3 - "$CSV" "$LOG" "$TAG" <<'PYEOF'
import re, sys
csv_path, log_path, tag = sys.argv[1], sys.argv[2], sys.argv[3]
rows = []
for line in open(csv_path):
    p = line.strip().split(',')
    if len(p) < 9 or not p[0].isdigit():
        continue
    try:
        rows.append([float(x) for x in p])
    except ValueError:
        continue
# 只取"已经在跑"的采样点(frame>0)
rows = [r for r in rows if r[8] > 0] or rows
def avg(i): return sum(r[i] for r in rows) / max(len(rows), 1)
def mx(i):  return max(r[i] for r in rows)
def mn(i):  return min(r[i] for r in rows)
print(f"--- [{tag}] 采样 {len(rows)} 点 ---")
print(f"  NPU Core0 平均 {avg(1):5.1f}%  (最低 {mn(1):.0f}% 最高 {mx(1):.0f}%)")
print(f"  NPU Core1 平均 {avg(2):5.1f}%  (最低 {mn(2):.0f}% 最高 {mx(2):.0f}%)")
print(f"  NPU Core2 平均 {avg(3):5.1f}%  (最低 {mn(3):.0f}% 最高 {mx(3):.0f}%)")
print(f"  VPU 解码负载   平均 {avg(4):5.1f}%")
print(f"  RGA 负载       平均 {avg(5):5.1f}%")
print(f"  CPU 总占用     平均 {avg(6):5.1f}%")
print(f"  流水线 FPS     平均 {avg(7):5.2f}  最低 {mn(7):.2f}  最高 {mx(7):.2f}")

# 日志里的阶段耗时
txt = open(log_path, encoding='utf-8', errors='replace').read()
m = re.findall(r"\[frame (\d+)\] fps=([\d.]+) src=([\d.]+).*?gap=([\d.]+)ms wait=([\d.]+)ms "
               r"dec=([\d.]+)ms rga=([\d.]+)ms npu=([\d.]+)ms\(run([\d.]+)/out([\d.]+)\).*?"
               r"post=([\d.]+)ms draw=([\d.]+)ms total=([\d.]+)ms objs=(\d+) tracks=(\d+)", txt)
if m:
    n = len(m)
    g = lambda i: sum(float(x[i]) for x in m) / n
    print(f"  日志统计点 {n} 个:")
    print(f"    fps 平均 {g(1):.2f}   源帧率 {g(2):.2f}")
    print(f"    解码 dec 平均 {g(5):.2f} ms")
    print(f"    RGA 转换 平均 {g(6):.2f} ms")
    print(f"    NPU 推理 平均 {g(7):.2f} ms (rknn_run {g(8):.2f} / 取回输出 {g(9):.2f})")
    print(f"    后处理 平均 {g(10):.2f} ms")
    print(f"    单帧总耗时 平均 {g(12):.2f} ms")
    print(f"    检测目标 平均 {g(13):.1f}")
PYEOF
echo ""
