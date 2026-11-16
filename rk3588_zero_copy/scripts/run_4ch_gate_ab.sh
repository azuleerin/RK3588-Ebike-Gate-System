#!/usr/bin/env bash
# ============================================================================
# run_4ch_gate_ab.sh —— 小区大门 4 路: 两种拓扑的【公平 A/B】
#
# 对比:
#   A. 4 个独立进程 (scripts/run_4ch_gate_demo.sh)   每路一个 rk_zero_copy
#   B. 1 个 rk_gate 进程  (scripts/run_4ch_gate_single.sh)  全局 NPU 线程池
#
# ★ 公平的前提(不做这三条, 数据就是废的):
#   1) 两边用【完全相同的 4 个源】(本地文件回放, 不用 RTSP ——
#      RTSP 相机的内容会变, 两轮的内容不同就没法比)
#   2) 两边用【完全相同的参数】(帧率上限/置信度/ROI 几何/IPM 标定)
#   3) 两轮跑在【同一台机器的同一块板】上, 中间没有别的负载插进来
#      —— 所以本脚本开头会【等板面安静下来】再开跑
#
# 用法:
#   scripts/run_4ch_gate_ab.sh [时长秒] [每路分析帧率上限]
#   例: scripts/run_4ch_gate_ab.sh 600 20
#
# 产出:
#   logs/gate_4ch_base_ab/load.csv     A(4 进程)的采样
#   logs/gate_4ch_single_ab/load.csv   B(单进程)的采样
#   logs/gate_4ch_ab/summary.txt    对比表
# ============================================================================
set -u

REPO=/home/seeed/rk_deploy
PROJ=$REPO/rk3588_zero_copy
DURATION=${1:-600}
MAX_FPS=${2:-20}
AB_OUT=$REPO/logs/gate_4ch_ab
mkdir -p "$AB_OUT"

# ---- 4 个源(两边必须完全一致) ----
SRC_CH1=${SRC_CH1:-/home/seeed/testvideo3.mp4}          # 720p30 近景(有绿牌)
SRC_CH2=${SRC_CH2:-/home/seeed/testvideo2.mp4}          # 720p21.5 街道(测速)
SRC_CH3=${SRC_CH3:-/home/seeed/Test2_fixed.mp4}         # 1080p60 近景
SRC_CH4=${SRC_CH4:-/home/seeed/电动车素材.mp4}           # 4K60 高位全景

IPM_POINTS_720P=${IPM_POINTS_720P:-"200,415;1000,429;967,585;367,546"}
IPM_POINTS_4K=${IPM_POINTS_4K:-"600,1244;3000,1286;2900,1754;1100,1638"}
IPM_WIDTH_M=${IPM_WIDTH_M:-3.5}
ROI_RATIO=${ROI_RATIO:-0.164}
ROI_Y=${ROI_Y:-1.0}
ROI_HEIGHT=${ROI_HEIGHT:-0.40}
ROI_SHIFT=${ROI_SHIFT:-0.126}
ROI_BELOW=${ROI_BELOW:-0}

echo "========================================================================"
echo " 4 路大门 A/B: 4 进程  vs  单进程(NPU 全局线程池)"
echo "   时长 ${DURATION}s   每路分析帧率上限 ${MAX_FPS} fps"
echo "   源(两边一致):"
echo "     ch1 $SRC_CH1"
echo "     ch2 $SRC_CH2"
echo "     ch3 $SRC_CH3"
echo "     ch4 $SRC_CH4"
echo "========================================================================"

# ---------------------------------------------------------------------------
# ① 等板面安静下来
# ---------------------------------------------------------------------------
busy_procs() {
    pgrep -f "build/rk_zero_copy|build/rk_gate|build_minimal/rk_minimal" 2>/dev/null
}

echo "[等待] 等板面安静(没有别的 rk_* / rk_minimal 在跑)…"
for i in $(seq 1 30); do
    if [ -z "$(busy_procs)" ]; then
        echo "[等待] 板面已安静"
        break
    fi
    if [ $((i % 5)) -eq 1 ]; then
        echo "[等待] 还在跑: $(busy_procs | tr '\n' ' ')"
    fi
    sleep 10
done
if [ -n "$(busy_procs)" ]; then
    echo "[警告] 等待超时, 板面仍有别的进程在跑: $(busy_procs | tr '\n' ' ')"
    echo "       (它们的负载会计入 CPU% 与 NPU 占用 —— 报告里必须注明)"
fi

# rk_minimal 是 RGA 压测, 会把 A/B 的 RGA/CPU 两列污染 —— 停掉它
if pgrep -f "build_minimal/rk_minimal" > /dev/null 2>&1; then
    echo "[清场] 停掉 rk_minimal(RGA 压测, 会污染 RGA/CPU 两列)"
    echo 123456 | sudo -S -p "" pkill -f "build_minimal/rk_minimal" 2>/dev/null
    sleep 3
fi

# ---------------------------------------------------------------------------
# ② A: 4 个独立进程
# ---------------------------------------------------------------------------
echo ""
echo "=== A: 4 个独立进程 (rk_zero_copy x4) ==="
echo "[现场] 开跑时的残留进程: $(busy_procs | tr '\n' ' ')(空 = 干净)"
OUT=$REPO/logs/gate_4ch_base_ab
SRC_CH1="$SRC_CH1" SRC_CH2="$SRC_CH2" SRC_CH3="$SRC_CH3" SRC_CH4="$SRC_CH4" \
IPM_POINTS_1080P="$IPM_POINTS_720P" IPM_POINTS_4K="$IPM_POINTS_4K" \
IPM_WIDTH_M="$IPM_WIDTH_M" \
ROI_RATIO="$ROI_RATIO" ROI_Y="$ROI_Y" ROI_HEIGHT="$ROI_HEIGHT" \
ROI_SHIFT="$ROI_SHIFT" ROI_BELOW="$ROI_BELOW" \
OUT="$OUT" \
    "$PROJ/scripts/run_4ch_gate_demo.sh" "$DURATION" "$MAX_FPS" 2>&1 | tail -30

# ---------------------------------------------------------------------------
# ③ B: 1 个 rk_gate 进程
# ---------------------------------------------------------------------------
echo ""
echo "=== B: 1 个进程 (rk_gate, 全局 NPU 线程池) ==="
echo "[现场] 开跑时的残留进程: $(busy_procs | tr '\n' ' ')(空 = 干净)"
OUT=$REPO/logs/gate_4ch_single_ab
SRC_CH1="$SRC_CH1" SRC_CH2="$SRC_CH2" SRC_CH3="$SRC_CH3" SRC_CH4="$SRC_CH4" \
IPM_POINTS_720P="$IPM_POINTS_720P" IPM_POINTS_4K="$IPM_POINTS_4K" \
IPM_WIDTH_M="$IPM_WIDTH_M" \
ROI_RATIO="$ROI_RATIO" ROI_Y="$ROI_Y" ROI_HEIGHT="$ROI_HEIGHT" \
ROI_SHIFT="$ROI_SHIFT" ROI_BELOW="$ROI_BELOW" \
OUT="$OUT" \
    "$PROJ/scripts/run_4ch_gate_single.sh" "$DURATION" "$MAX_FPS" 2>&1 | tail -30

# ---------------------------------------------------------------------------
# ④ 对比表
# ---------------------------------------------------------------------------
echo ""
echo "=== 对比表 ==="
python3 "$PROJ/scripts/gate_ab_report.py" "$REPO/logs/gate_4ch_base_ab" "$REPO/logs/gate_4ch_single_ab" \
    | tee "$AB_OUT/summary.txt"
echo ""
echo "汇总已存: $AB_OUT/summary.txt"
