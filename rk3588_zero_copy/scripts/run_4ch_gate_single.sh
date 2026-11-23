#!/usr/bin/env bash
# ============================================================================
# run_4ch_gate_single.sh —— 小区大门 4 路并发压测(单进程 + 全局 NPU 线程池)
#
# 与 run_4ch_gate_demo.sh 的区别只有【拓扑】, 其余(源/参数/采样格式)完全一样:
#   run_4ch_gate_demo.sh  : 4 个独立进程, 每路一个 rk_zero_copy  -> 进程间抢 NPU
#   run_4ch_gate_single.sh: 1 个 rk_gate 进程, 4 路共用一条优先级队列
#
# 核绑定(本次落地方案):
#   识别(PP-OCR) -> Core0        (独占, 近景两路共用)
#   检测(YOLO)   -> Core1 + Core2 (4 路共用两个核, 由池里的 worker 调度)
#
# 用法:
#   scripts/run_4ch_gate_single.sh [时长秒] [每路分析帧率上限]
#   例: scripts/run_4ch_gate_single.sh 600 20
#
# 产出 (默认在 logs/gate_4ch_single/):
#   gate.log      主程序日志(4 路的 [chN][frame ...] 统计都在里面)
#   load.csv      每秒采样: NPU 三核 / VPU / RGA / CPU% / RSS / 各路 FPS
#   summary.txt   收尾汇总
#   core_stat.txt 收尾时程序自报的每核占用(应用内口径)
# ============================================================================
set -u


# ---- 需要 root: 以 root 身份运行, 或先执行 `sudo -v` 缓存凭据(凭据不落盘) ----
if [ "$(id -u)" -ne 0 ] && ! sudo -n true 2>/dev/null; then
    echo "需要 root 权限，请使用 sudo 运行或先执行 sudo -v" >&2
    exit 1
fi
REPO=/home/seeed/rk_deploy
PROJ=$REPO/rk3588_zero_copy
# ★ BIN 可以用环境变量换掉 —— A/B 测试时要跑"改前"(main 分支单独构建的那份)
#   和"改后"(本分支)两个不同的二进制。
BIN=${BIN:-$PROJ/build/rk_gate}
MODEL=$REPO/models/ebike_int8_rk2332.rknn
LABELS=$REPO/models/labels_ebike.txt
OCR=$REPO/models/ppocrv4_rec_fp16.rknn

DURATION=${1:-600}          # 压测时长(秒)
MAX_FPS=${2:-20}            # 每路分析帧率上限
OUT=${OUT:-$REPO/logs/gate_4ch_single}
mkdir -p "$OUT"

# ---- 视频源(和 run_4ch_gate_demo.sh 一致, 保证前后可比) ----
SRC_CH1=${SRC_CH1:-/home/seeed/testvideo3.mp4}          # 720p30 近景(有绿牌)
SRC_CH2=${SRC_CH2:-/home/seeed/testvideo2.mp4}          # 720p21.5 街道(测速)
SRC_CH3=${SRC_CH3:-/home/seeed/Test2_fixed.mp4}         # 1080p60 近景(有绿牌)
SRC_CH4=${SRC_CH4:-/home/seeed/电动车素材.mp4}          # 4K60 高位全景(测速)

# ---- IPM 标定(测速用) ----
IPM_POINTS_720P=${IPM_POINTS_720P:-"200,415;1000,429;967,585;367,546"}
IPM_POINTS_4K=${IPM_POINTS_4K:-"600,1244;3000,1286;2900,1754;1100,1638"}
IPM_WIDTH_M=${IPM_WIDTH_M:-3.5}

# ---- 近景车牌的 ROI 几何(与 run_4ch_gate_demo.sh 同值) ----
ROI_RATIO=${ROI_RATIO:-0.164}
ROI_Y=${ROI_Y:-1.0}
ROI_HEIGHT=${ROI_HEIGHT:-0.40}
ROI_SHIFT=${ROI_SHIFT:-0.126}
ROI_BELOW=${ROI_BELOW:-0}

# ---- 核绑定 ----
#   DET_CORES: 检测用哪几颗核(核号)
#   OCR_CORES: 识别用哪几颗核(核号)
#
# ★ 为什么检测也放 core0(和识别共用):
#   实测(4 路, 600s):
#     检测只放 Core1+Core2 -> 这两颗 97% 满载, 而识别那颗 core0 只用了 17%
#                            (车牌有 5s TTL 缓存, 识别其实很少跑)
#     检测放 Core0+1+2     -> core0 上检测占 56% + 识别占 38% = 94%,
#                            三颗核【全都】用上了, 检测吞吐 +39%
#   共用同一颗核时由【每核闸门】仲裁: 识别任务永远排在检测任务前面,
#   所以识别不会被检测挤掉 —— 这是 4 进程版做不到的(进程间共享不了上下文)。
#
# ★ 但实测端到端【并没有】变好(见 logs/gate_4ch_single_sharedcore/):
#     检测放 Core1+2 : NPU 86/85% 满载, CPU 55%,  ch1=15.8 fps
#     检测放 Core0+1+2: NPU 62/62/62% 没满, CPU 78%(3 个检测 worker
#                       同时做 DFL+NMS 后处理, 把 CPU 吃掉了), ch1=13.3 fps
#   也就是说: 真正的瓶颈是【CPU 后处理】, 不是 NPU。给检测多开一颗核,
#   反而让后处理线程多一个, 把 CPU 抢满, RGA 都被拖慢(21.9ms vs 6.6ms)。
#   所以默认还是按现场规格: 检测 Core1+2, 识别 Core0。
DET_CORES=${DET_CORES:-1,2}
OCR_CORES=${OCR_CORES:-0}
# ★ worker 数量(新版本才有的选项)。留空 = 不传这个参数(老二进制也能跑),
#   设了才传 —— 这样同一份脚本既能跑改前的二进制, 也能跑改后的。
DET_WORKERS=${DET_WORKERS:-}
OCR_WORKERS=${OCR_WORKERS:-}

MQTT_HOST=${MQTT_HOST:-}

# ★ worker 数量参数(只有新版本二进制认识这两个选项, 所以按需传)
WORKER_ARGS=()
[ -n "$DET_WORKERS" ] && WORKER_ARGS+=(--det-workers "$DET_WORKERS")
[ -n "$OCR_WORKERS" ] && WORKER_ARGS+=(--ocr-workers "$OCR_WORKERS")

echo "========================================================================"
echo " 小区大门 4 路并发压测 —— 单进程 + 全局 NPU 线程池"
echo "   时长 ${DURATION}s   每路分析帧率上限 ${MAX_FPS} fps"
echo "   Ch1 近景/车牌  $SRC_CH1"
echo "   Ch2 高位/测速  $SRC_CH2"
echo "   Ch3 近景/车牌  $SRC_CH3"
echo "   Ch4 高位/测速  $SRC_CH4"
echo "   核绑定: 检测=核$DET_CORES  识别=核$OCR_CORES"
echo "========================================================================"

MQTT_ARGS=()
if [ -n "$MQTT_HOST" ]; then
    MQTT_ARGS=(--mqtt-host "$MQTT_HOST" --sn "GATE-1P")
fi

# ---------------------------------------------------------------------------
# 启动(一个进程, 4 路)
# ---------------------------------------------------------------------------
"$BIN" \
    --model "$MODEL" --labels "$LABELS" \
    --ppocr "$OCR" --ppocr-dict "$(dirname "$OCR")/ppocr_keys_v1.txt" \
    --det-core "$DET_CORES" --ocr-core "$OCR_CORES" \
    --conf 0.15 --nms 0.45 \
    --max-fps "$MAX_FPS" --stats-interval 100 \
    --roi-ratio "$ROI_RATIO" --roi-y "$ROI_Y" --roi-height "$ROI_HEIGHT" \
    --roi-shift "$ROI_SHIFT" --roi-below "$ROI_BELOW" \
    --ipm-width-m "$IPM_WIDTH_M" \
    --ch "ch1:$SRC_CH1:near" \
    --ch "ch2:$SRC_CH2:high:$IPM_POINTS_720P" \
    --ch "ch3:$SRC_CH3:near" \
    --ch "ch4:$SRC_CH4:high:$IPM_POINTS_4K" \
    --duration "$DURATION" \
    "${WORKER_ARGS[@]+"${WORKER_ARGS[@]}"}" \
    "${MQTT_ARGS[@]+"${MQTT_ARGS[@]}"}" \
    > "$OUT/gate.log" 2>&1 &
PID=$!
echo "已启动: rk_gate pid=$PID"

cleanup() {
    echo ""
    echo "收尾: 停掉…"
    kill "$PID" 2>/dev/null
    sleep 2
    kill -9 "$PID" 2>/dev/null
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# 负载采样器(每秒一次) —— 与 4 进程版【同样的口径】
# ---------------------------------------------------------------------------
sudo bash -c 'echo 1000 > /proc/mpp_service/load_interval' 2>/dev/null

CSV=$OUT/load.csv
echo "t,npu0,npu1,npu2,vpu,rga,cpu_pct,rss_mb,fps1,fps2,fps3,fps4" > "$CSV"

rss_of() { awk '/VmRSS/{print int($2/1024)}' "/proc/$1/status" 2>/dev/null || echo 0; }
fps_of() {
    local log=$1 ch=$2
    # ★ 只读日志的【尾部 8MB】: 长跑时日志能到几百 MB,
    #   每次采样都全文 grep 会把采样器自己拖死(实测 7 分钟就变成 1.9s/次)。
    #   窗口大小要够: 守卫的 INFO 日志有 ~400KB/s, 统计行每 100 帧一条
    #   (4 路合起来大约 1.5s 一条) -> 8MB 窗口里一定有最近的一条。
    tail -c 8000000 "$log" 2>/dev/null | grep -a "\[$ch\]\[frame" | tail -1 \
        | grep -aoE "deliv=[0-9.]+" | cut -d= -f2
}

prev_total=0; prev_idle=0
for i in $(seq 1 "$DURATION"); do
    now=$(date +%s)
    npu=$(sudo cat /sys/kernel/debug/rknpu/load 2>/dev/null)
    n0=$(echo "$npu" | sed -nE 's/.*Core0: *([0-9]+)%.*/\1/p'); n0=${n0:-0}
    n1=$(echo "$npu" | sed -nE 's/.*Core1: *([0-9]+)%.*/\1/p'); n1=${n1:-0}
    n2=$(echo "$npu" | sed -nE 's/.*Core2: *([0-9]+)%.*/\1/p'); n2=${n2:-0}
    vpu=$(sudo cat /proc/mpp_service/load 2>/dev/null | \
          awk '/vdpu|rkvdec/{gsub("%","",$3); if($3+0>max) max=$3+0} END{printf "%.0f", max+0}')
    rga=$(sudo cat /sys/kernel/debug/rkrga/load 2>/dev/null | \
          awk '/load *= *[0-9]+%/ {gsub("%","",$3); if($3+0>max) max=$3+0} END{printf "%.0f", max+0}')
    cl=$(awk '/^cpu /{print $2,$3,$4,$5,$6,$7,$8,$9,$10,$11}' /proc/stat)
    read -r u1 n1c s1 i1 w1 ir1 sr1 st1 g1 gn1 <<< "$cl"
    total=$((u1+n1c+s1+i1+w1+ir1+sr1+st1+g1+gn1)); idle=$((i1+w1))
    dt=$((total-prev_total)); di=$((idle-prev_idle))
    if [ "$dt" -gt 0 ]; then cpu=$(awk -v d="$dt" -v i="$di" 'BEGIN{printf "%.1f",(1-i/d)*100}'); else cpu=0; fi
    prev_total=$total; prev_idle=$idle

    rss=$(rss_of $PID)
    f1=$(fps_of "$OUT/gate.log" ch1); f2=$(fps_of "$OUT/gate.log" ch2)
    f3=$(fps_of "$OUT/gate.log" ch3); f4=$(fps_of "$OUT/gate.log" ch4)
    echo "${now},${n0},${n1},${n2},${vpu:-0},${rga:-0},${cpu},${rss},${f1:-0},${f2:-0},${f3:-0},${f4:-0}" >> "$CSV"

    if [ $((i % 60)) -eq 0 ]; then
        echo "  [${i}s] NPU ${n0}/${n1}/${n2}%  VPU ${vpu:-0}%  RGA ${rga:-0}%  CPU ${cpu}%"
        echo "         FPS ch1=${f1:-?} ch2=${f2:-?} ch3=${f3:-?} ch4=${f4:-?}   RSS ${rss} MB"
    fi
    sleep 1
done

wait "$PID" 2>/dev/null

# ---------------------------------------------------------------------------
# 汇总
# ---------------------------------------------------------------------------
echo ""
echo "========================================================================"
echo " 4 路并发压测汇总 —— 单进程 + 全局线程池 (${DURATION}s)"
echo "========================================================================"
python3 - "$CSV" "$OUT" <<'PYEOF'
import csv, sys
csv_path, out_dir = sys.argv[1], sys.argv[2]
rows = []
with open(csv_path) as f:
    for r in csv.reader(f):
        if len(r) < 12 or not r[0].isdigit():
            continue
        try:
            rows.append([float(x) for x in r])
        except ValueError:
            pass
if not rows:
    print("没有采样到数据")
    sys.exit(0)

k = max(1, len(rows) // 7)
s = rows[k:len(rows) - k] or rows
def avg(i, src=None):
    v = (src or s)
    return sum(x[i] for x in v) / max(len(v), 1)
def mx(i, src=None):
    v = (src or s)
    return max(x[i] for x in v)

print(f"采样点 {len(rows)} 个(稳定段 {len(s)} 个)\n")
print(f"{'指标':<22}{'平均':>10}{'峰值':>10}")
print("-" * 42)
for n, i in [("NPU Core0", 1), ("NPU Core1", 2), ("NPU Core2", 3),
             ("VPU 解码", 4), ("RGA", 5), ("CPU 总占用%", 6)]:
    print(f"{n:<22}{avg(i):>9.1f}%{mx(i):>9.1f}%")
print(f"{'RSS 合计 (MB)':<22}{avg(7):>9.0f}{mx(7):>9.0f}")
print()
for n, i in [("ch1 近景/车牌 FPS", 8), ("ch2 高位/测速 FPS", 9),
             ("ch3 近景/车牌 FPS", 10), ("ch4 高位/测速 FPS", 11)]:
    vals = [x[i] for x in s if x[i] > 0]
    print(f"{n:<22}{avg(i):>9.2f}" + (f"   (峰值 {mx(i):.2f})" if vals else "   (无数据)"))
PYEOF

# 程序自报的每核占用(应用内口径)
grep -A 12 "每核 NPU 占用" "$OUT/gate.log" > "$OUT/core_stat.txt" 2>/dev/null || true
echo ""
echo "程序自报每核占用:"
cat "$OUT/core_stat.txt" 2>/dev/null || echo "(没有)"
echo ""
echo "日志与采样: $OUT"
