#!/usr/bin/env bash
# ============================================================================
# run_4ch_gate_demo.sh —— 小区大门 4 路并发压测(单板 RK3588)
#
# 模拟的现场配置:
#   Ch1 近景(看车牌)  : --ppocr 开, --no-speed            源 testvideo3.mp4   (720p30, 有绿牌)
#   Ch2 高位(看全景)  : --ppocr 关, --ipm-points 测速     源 testvideo2.mp4   (720p21.5, 街道行驶)
#   Ch3 近景(看车牌)  : 同 Ch1                             源 Test2_fixed.mp4  (1080p60)
#   Ch4 高位(看全景)  : 同 Ch2                             源 电动车素材.mp4   (4K60)
#
# 实验室没有 4 路真实 RTSP, 所以用 4 个本地进程各读一个文件来模拟。
# 每个进程都是完整的一套流水线(解码 -> RGA -> NPU 检测 -> 后处理 -> [识别/测速] -> MQTT)。
#
# NPU 核绑定(RK3588 只有 3 个核, 4 个进程不可能一一独占):
#   Ch1/Ch3 近景: 检测绑 Core0(mask=1), 车牌识别绑 Core2(mask=4)
#   Ch2/Ch4 高位: 检测绑 Core1(mask=2)
#   => Core0 跑 2 路检测, Core1 跑 2 路检测, Core2 跑 2 路识别 —— 互不抢核
#
# 用法:
#   scripts/run_4ch_gate_demo.sh [时长秒] [最大分析帧率]
#   例: scripts/run_4ch_gate_demo.sh 600 20
#
# 产出 (默认在 logs/gate_4ch/):
#   ch1.log ch2.log ch3.log ch4.log   每路的程序日志
#   load.csv                          每秒采样: NPU 三核 / VPU / RGA / CPU% / 各路 RSS
#   summary.txt                       收尾汇总
# ============================================================================
set -u

REPO=/home/seeed/rk_deploy
PROJ=$REPO/rk3588_zero_copy
BIN=$PROJ/build/rk_zero_copy
MODEL=$REPO/models/ebike_int8_rk2332.rknn
LABELS=$REPO/models/labels_ebike.txt
OCR=$REPO/models/ppocrv4_rec_fp16.rknn

DURATION=${1:-600}          # 压测时长(秒)
MAX_FPS=${2:-20}            # 每路分析帧率上限
OUT=$REPO/logs/gate_4ch
mkdir -p "$OUT"

# ---- 视频源 ----
#   Ch1/Ch3 近景(看车牌): 采集点低, 车牌像素多
#   Ch2/Ch4 高位(看全景): 采集点高, 要跟踪 + IPM 测速
#   实验室只有 1 路真实 RTSP(192.168.31.200:8557, 1080p25),
#   所以 Ch2/Ch3 共用这一路(模拟大门上两台同型号相机), 其余用本地文件回放。
SRC_CH1=${SRC_CH1:-/home/seeed/testvideo3.mp4}          # 720p30 实拍骑行视角(有绿牌)
SRC_CH2=${SRC_CH2:-rtsp://192.168.31.200:8557/h264}    # 实时相机 1080p25
SRC_CH3=${SRC_CH3:-rtsp://192.168.31.200:8557/h264}    # 实时相机 1080p25
SRC_CH4=${SRC_CH4:-/home/seeed/电动车素材.mp4}           # 4K60 高位全景

# ---- IPM 标定(测速用) ----
# IPM_POINTS_1080P 取自仓库已验证的验收脚本(run_acceptance_demo.sh),
#   标定结果 f=1288.8px / 标定区 3.50m x 7.56m / 矩形误差 0.000
# IPM_POINTS_4K 是同一梯形按 3840x2160 等比放大 2 倍
IPM_POINTS_1080P=${IPM_POINTS_1080P:-"300,622;1500,643;1450,877;550,819"}
IPM_POINTS_4K=${IPM_POINTS_4K:-"600,1244;3000,1286;2900,1754;1100,1638"}
IPM_WIDTH_M=${IPM_WIDTH_M:-3.5}

# ---- 近景车牌的 ROI 几何 ----
#  默认值 0.72/0.70/0.16 裁出来是 22:1 的扁横带(真牌约 3:1),
#  在 testvideo3 上实测可用的是下面这组(详见 docs/FINAL_12CH_EVAL.md §6.3)
ROI_RATIO=${ROI_RATIO:-0.164}
ROI_Y=${ROI_Y:-1.0}
ROI_HEIGHT=${ROI_HEIGHT:-0.40}
ROI_SHIFT=${ROI_SHIFT:-0.126}
ROI_BELOW=${ROI_BELOW:-0}

# ---- MQTT(可选; 不给就不开) ----
MQTT_HOST=${MQTT_HOST:-}

# ---- 核绑定掩码: 1=Core0, 2=Core1, 4=Core2 ----
DET_CORE_ODD=${DET_CORE_ODD:-1}    # 近景两路的检测核
DET_CORE_EVEN=${DET_CORE_EVEN:-2}  # 高位两路的检测核
REC_CORE=${REC_CORE:-4}           # 识别核(给两路近景共用)

echo "========================================================================"
echo " 小区大门 4 路并发压测"
echo "   时长 ${DURATION}s   每路分析帧率上限 ${MAX_FPS} fps"
echo "   Ch1 近景/车牌  $SRC_CH1"
echo "   Ch2 高位/测速  $SRC_CH2"
echo "   Ch3 近景/车牌  $SRC_CH3"
echo "   Ch4 高位/测速  $SRC_CH4"
echo "   核绑定: 近景检测=mask$DET_CORE_ODD 识别=mask$REC_CORE | 高位检测=mask$DET_CORE_EVEN"
echo "========================================================================"

MQTT_ARGS=()
if [ -n "$MQTT_HOST" ]; then
    MQTT_ARGS=(--mqtt-host "$MQTT_HOST" --sn "GATE-4CH")
fi

# ---------------------------------------------------------------------------
# 启动 4 路
# ---------------------------------------------------------------------------
start_ch() {
    local name=$1 src=$2; shift 2
    "$BIN" "$src" "$MODEL" \
        --decoder mpp --labels "$LABELS" \
        --track-cls 1 --conf 0.15 --no-show \
        --max-fps "$MAX_FPS" \
        --stats-interval 100 \
        --log-file "$OUT/$name.log" \
        "$@" > "$OUT/$name.stderr" 2>&1 &
    echo $!
}

# 近景: 识别开, 测速关, 检测绑 Core0, 识别绑 Core2
PID_CH1=$(start_ch ch1 "$SRC_CH1" \
    --ppocr "$OCR" --plate-cls 1 --no-speed \
    --roi-ratio "$ROI_RATIO" --roi-y "$ROI_Y" --roi-height "$ROI_HEIGHT" \
    --roi-shift "$ROI_SHIFT" --roi-below "$ROI_BELOW" \
    --npu-core "$DET_CORE_ODD" --lpr-core "$REC_CORE" \
    "${MQTT_ARGS[@]+"${MQTT_ARGS[@]}"}")
PID_CH2=$(start_ch ch2 "$SRC_CH2" \
    --ipm-points "$IPM_POINTS_1080P" --ipm-width-m "$IPM_WIDTH_M" \
    --npu-core "$DET_CORE_EVEN" \
    "${MQTT_ARGS[@]+"${MQTT_ARGS[@]}"}")
PID_CH3=$(start_ch ch3 "$SRC_CH3" \
    --ppocr "$OCR" --plate-cls 1 --no-speed \
    --roi-ratio "$ROI_RATIO" --roi-y "$ROI_Y" --roi-height "$ROI_HEIGHT" \
    --roi-shift "$ROI_SHIFT" --roi-below "$ROI_BELOW" \
    --npu-core "$DET_CORE_ODD" --lpr-core "$REC_CORE" \
    "${MQTT_ARGS[@]+"${MQTT_ARGS[@]}"}")
PID_CH4=$(start_ch ch4 "$SRC_CH4" \
    --ipm-points "$IPM_POINTS_4K" --ipm-width-m "$IPM_WIDTH_M" \
    --npu-core "$DET_CORE_EVEN" \
    "${MQTT_ARGS[@]+"${MQTT_ARGS[@]}"}")

sleep 3
echo "已启动: ch1=$PID_CH1 ch2=$PID_CH2 ch3=$PID_CH3 ch4=$PID_CH4"

cleanup() {
    echo ""
    echo "收尾: 停掉 4 路…"
    for p in $PID_CH1 $PID_CH2 $PID_CH3 $PID_CH4; do
        kill "$p" 2>/dev/null
    done
    sleep 2
    for p in $PID_CH1 $PID_CH2 $PID_CH3 $PID_CH4; do
        kill -9 "$p" 2>/dev/null
    done
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# 负载采样器(每秒一次)
#   NPU 三核  : /sys/kernel/debug/rknpu/load        (需 root)
#   VPU 解码  : /proc/mpp_service/load               (需 root)
#   RGA       : /sys/kernel/debug/rkrga/load         (需 root)
#   CPU%      : /proc/stat 两次采样差值
#   各路 RSS  : /proc/<pid>/status
# ---------------------------------------------------------------------------
echo 123456 | sudo -S -p "" bash -c 'echo 1000 > /proc/mpp_service/load_interval' 2>/dev/null

CSV=$OUT/load.csv
echo "t,npu0,npu1,npu2,vpu,rga,cpu_pct,rss1,rss2,rss3,rss4,fps1,fps2,fps3,fps4" > "$CSV"

rss_of() { awk '/VmRSS/{print int($2/1024)}' "/proc/$1/status" 2>/dev/null || echo 0; }
fps_of() {
    local log=$1
    grep -aoE "deliv=[0-9.]+" "$log" 2>/dev/null | tail -1 | cut -d= -f2
}

prev_total=0; prev_idle=0
for i in $(seq 1 "$DURATION"); do
    now=$(date +%s)
    npu=$(echo 123456 | sudo -S -p "" cat /sys/kernel/debug/rknpu/load 2>/dev/null)
    n0=$(echo "$npu" | sed -nE 's/.*Core0: *([0-9]+)%.*/\1/p'); n0=${n0:-0}
    n1=$(echo "$npu" | sed -nE 's/.*Core1: *([0-9]+)%.*/\1/p'); n1=${n1:-0}
    n2=$(echo "$npu" | sed -nE 's/.*Core2: *([0-9]+)%.*/\1/p'); n2=${n2:-0}
    vpu=$(echo 123456 | sudo -S -p "" cat /proc/mpp_service/load 2>/dev/null | \
          awk '/vdpu|rkvdec/{gsub("%","",$3); if($3+0>max) max=$3+0} END{printf "%.0f", max+0}')
    rga=$(echo 123456 | sudo -S -p "" cat /sys/kernel/debug/rkrga/load 2>/dev/null | \
          awk '/load *= *[0-9]+%/ {gsub("%","",$3); if($3+0>max) max=$3+0} END{printf "%.0f", max+0}')
    cl=$(awk '/^cpu /{print $2,$3,$4,$5,$6,$7,$8,$9,$10,$11}' /proc/stat)
    read -r u1 n1c s1 i1 w1 ir1 sr1 st1 g1 gn1 <<< "$cl"
    total=$((u1+n1c+s1+i1+w1+ir1+sr1+st1+g1+gn1)); idle=$((i1+w1))
    dt=$((total-prev_total)); di=$((idle-prev_idle))
    if [ "$dt" -gt 0 ]; then cpu=$(awk -v d="$dt" -v i="$di" 'BEGIN{printf "%.1f",(1-i/d)*100}'); else cpu=0; fi
    prev_total=$total; prev_idle=$idle

    r1=$(rss_of $PID_CH1); r2=$(rss_of $PID_CH2); r3=$(rss_of $PID_CH3); r4=$(rss_of $PID_CH4)
    f1=$(fps_of "$OUT/ch1.log"); f2=$(fps_of "$OUT/ch2.log")
    f3=$(fps_of "$OUT/ch3.log"); f4=$(fps_of "$OUT/ch4.log")
    echo "${now},${n0},${n1},${n2},${vpu:-0},${rga:-0},${cpu},${r1},${r2},${r3},${r4},${f1:-0},${f2:-0},${f3:-0},${f4:-0}" >> "$CSV"

    if [ $((i % 60)) -eq 0 ]; then
        echo "  [${i}s] NPU ${n0}/${n1}/${n2}%  VPU ${vpu:-0}%  RGA ${rga:-0}%  CPU ${cpu}%"
        echo "         FPS ch1=${f1:-?} ch2=${f2:-?} ch3=${f3:-?} ch4=${f4:-?}   RSS ${r1}/${r2}/${r3}/${r4} MB"
    fi
    sleep 1
done

# ---------------------------------------------------------------------------
# 汇总
# ---------------------------------------------------------------------------
echo ""
echo "========================================================================"
echo " 4 路并发压测汇总 (${DURATION}s)"
echo "========================================================================"
python3 - "$CSV" "$OUT" <<'PYEOF'
import csv, sys, re
csv_path, out_dir = sys.argv[1], sys.argv[2]
rows = []
with open(csv_path) as f:
    for r in csv.reader(f):
        if len(r) < 15 or not r[0].isdigit():
            continue
        try:
            rows.append([float(x) for x in r])
        except ValueError:
            pass
if not rows:
    print("没有采样到数据")
    sys.exit(0)

# 去掉首尾各 15%(启动爬坡 / 收尾)
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
names = [("NPU Core0", 1), ("NPU Core1", 2), ("NPU Core2", 3),
         ("VPU 解码", 4), ("RGA", 5), ("CPU 总占用%", 6)]
for n, i in names:
    print(f"{n:<22}{avg(i):>9.1f}%{mx(i):>9.1f}%")
for n, i in [("RSS ch1 (MB)", 7), ("RSS ch2 (MB)", 8), ("RSS ch3 (MB)", 9), ("RSS ch4 (MB)", 10)]:
    print(f"{n:<22}{avg(i):>9.0f}{mx(i):>9.0f}")
tot_rss = avg(7) + avg(8) + avg(9) + avg(10)
print(f"{'4 路 RSS 合计 (MB)':<22}{tot_rss:>9.0f}{mx(7)+mx(8)+mx(9)+mx(10):>9.0f}")
print()
for n, i in [("ch1 近景/车牌 FPS", 11), ("ch2 高位/测速 FPS", 12),
             ("ch3 近景/车牌 FPS", 13), ("ch4 高位/测速 FPS", 14)]:
    vals = [x[i] for x in s if x[i] > 0]
    print(f"{n:<22}{avg(i):>9.2f}" + (f"   (峰值 {mx(i):.2f})" if vals else "   (无数据)"))

# 各路日志里的最终统计
print("\n各路程序自报(最后一条 [frame] 统计):")
for ch in ("ch1", "ch2", "ch3", "ch4"):
    txt = open(f"{out_dir}/{ch}.log", encoding="utf-8", errors="replace").read()
    fr = re.findall(r"\[frame (\d+)\](.*)", txt)
    if not fr:
        print(f"  {ch}: (无统计)")
        continue
    fid, rest = fr[-1]
    g = lambda k, d="?": (re.search(k + r'=([0-9.]+)', rest).group(1) if re.search(k + r'=([0-9.]+)', rest) else d)
    print(f"  {ch}: frame={fid}  交付FPS={g('deliv')}  流水线FPS={g('fps')}  "
          f"NPU={g('npu')}ms  解码={g('dec')}ms  RGA={g('rga')}ms  objs={g('objs')}")
    # 车牌 / 测速
    if ch in ("ch1", "ch3"):
        plates = re.findall(r'-> "([^"]*)"', txt)
        plates = [p for p in plates if p]
        print(f"       车牌识别次数={len(plates)}  不同结果={len(set(plates))}  "
              f"样例={list(set(plates))[:5]}")
    else:
        spd = re.findall(r"spd=([0-9.]+)", rest)
        if spd:
            print(f"       测速: spd={spd[-1]} km/h")
PYEOF
echo ""
echo "日志与采样: $OUT"
