#!/usr/bin/env bash
# ===========================================================================
# run_ab_refactor.sh —— NPU 调度重构的 A/B 对比
#
# 三条腿, 同一板、同一批视频、同一个时长, 只换二进制和核/worker 配置:
#
#   A 基线    main 分支的二进制(改前): 检测核 1,2 + 识别核 0
#             -> 2 个检测 worker + 1 个识别 worker, 识别上下文是 AUTO(没绑核)
#   B 改后    本分支: 检测核 0,1,2 + 识别核 0,1,2, 各 3 个 worker
#             -> 每核 1 检测 + 1 识别(共 6 worker, 独立压测台测出的最优)
#   C 变体    本分支: 检测核 1,2(2 worker, 不变) + 识别核 0,1,2(3 worker)
#             -> 只修识别的串行化, 不动检测的 CPU 后处理负载
#
# 用法:
#   ./scripts/run_ab_refactor.sh [时长秒]        # 默认 180
#   BASELINE_BIN=... ./scripts/run_ab_refactor.sh 300
#
# 产物: logs/ab_refactor/<leg>/{gate.log,load.csv} + 对比表
# ===========================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO="$(cd "${PROJ}/.." && pwd)"

DURATION=${1:-180}
OUT_ROOT="${OUT_ROOT:-$REPO/logs/ab_refactor}"
BASELINE_BIN=${BASELINE_BIN:-/tmp/rk_base/rk3588_zero_copy/build/rk_gate}
NEW_BIN=${NEW_BIN:-$PROJ/build/rk_gate}

if [[ ! -x "$BASELINE_BIN" ]]; then
    echo "[ab] 找不到基线二进制: $BASELINE_BIN"
    echo "     先构建它: git worktree add /tmp/rk_base main && cmake -S /tmp/rk_base/rk3588_zero_copy -B /tmp/rk_base/rk3588_zero_copy/build -DCMAKE_BUILD_TYPE=Release && cmake --build /tmp/rk_base/rk3588_zero_copy/build -j8"
    exit 1
fi
if [[ ! -x "$NEW_BIN" ]]; then
    echo "[ab] 找不到改后二进制: $NEW_BIN"; exit 1
fi

mkdir -p "$OUT_ROOT"

run_leg() {
    local name="$1" bin="$2" det="$3" ocr="$4" detw="$5" ocrw="$6"
    local out="$OUT_ROOT/$name"
    mkdir -p "$out"
    echo ""
    echo "=============== 腿 $name: 检测核=$det x${detw:-默认} 识别核=$ocr x${ocrw:-默认} ==============="
    BIN="$bin" OUT="$out" DET_CORES="$det" OCR_CORES="$ocr" \
        DET_WORKERS="$detw" OCR_WORKERS="$ocrw" \
        "$PROJ/scripts/run_4ch_gate_single.sh" "$DURATION" 20
}

# A: 基线(改前)
run_leg A_baseline "$BASELINE_BIN" "1,2" "0" "" ""
# B: 改后, 每核 1 检测 + 1 识别
run_leg B_after_3plus3 "$NEW_BIN" "0,1,2" "0,1,2" "" ""
# C: 变体, 检测不动(2 worker), 识别散到 3 核
run_leg C_after_2plus3 "$NEW_BIN" "1,2" "0,1,2" "2" "3"

# ---- 汇总 ----
echo ""
echo "=============== 对比表 ==============="
python3 "$SCRIPT_DIR/ab_refactor_report.py" "$OUT_ROOT"
