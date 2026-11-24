#!/usr/bin/env bash
# run_batch_bench.sh —— Batch 推理自动化测试(空载 + DDR 压力两种场景)
#
# 矩阵:
#   场景 A 空载      : dmc busy ≈ 10%(板子自身基线)
#   场景 B DDR 压力  : 后台 N 个线程模拟 4 路 MPP 解码 + RGA 预处理 的内存读写
#   × Batch = 1/2/4
#   × 模式 = single(整批跑 1 核) / multi3(整批拆 3 核) / workers3(3 个独立 context 分核)
#   × 重复 3 次, 每次 30s 稳态
#
# 干净窗口:
#   touch /run/rk-gate/paused   -> 看门狗闭嘴
#   systemctl stop rk-gate rk-pipeline -> 停掉占 NPU/DDR 的生产服务
#   (工程自带的维护窗口机制, 见 rk3588_zero_copy/scripts/gate_watchdog.sh)
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
BASE=$(dirname "$HERE")
REPO=$(dirname "$BASE")
BENCH="$BASE/build/batch_infer_bench"
MODELS="$BASE/models"
OUTDIR="${OUTDIR:-$BASE/out}"
DURATION="${DURATION:-30}"
WARMUP="${WARMUP:-5}"
REPS="${REPS:-3}"
STRESS="${STRESS:-4}"
CORE="${CORE:-1}"
DBG=/sys/kernel/debug/rknpu/load
DMC=/sys/class/devfreq/dmc/load

mkdir -p "$OUTDIR"

run_sudo() { if [ "$(id -u)" -eq 0 ]; then "$@"; else sudo "$@"; fi; }
dbg_load() { sudo -n cat "$DBG" 2>/dev/null | head -1; }
dmc_load() { cat "$DMC" 2>/dev/null | cut -d@ -f1; }
core_pct() { echo "$1" | grep -oE "Core$2: *[0-9]+" | grep -oE "[0-9]+$" | head -1; }

cleanup() {
    local rc=$?
    if [ "${STOPPED:-0}" = "1" ]; then
        echo "[restore] 恢复生产服务 ..."
        run_sudo systemctl start rk-gate >/dev/null 2>&1 && echo "  ✓ rk-gate 已启动"
        run_sudo systemctl start rk-pipeline >/dev/null 2>&1 && echo "  ✓ rk-pipeline 已启动"
        run_sudo rm -f /run/rk-gate/paused >/dev/null 2>&1 && echo "  ✓ 维护窗口标记已清除"
        STOPPED=0
    fi
    exit $rc
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------- 1. 等竞争者
echo "=== 1) 等待 NPU 空闲 (无其他会话的压测) ==="
list_competitors() {
    local p pid cmd found=0
    for p in /proc/[0-9]*; do
        pid=${p#/proc/}
        [ "$pid" = "$$" ] && continue
        cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
        case "$cmd" in
            *npu_layer_bench*|*npu_bench*|*npu_baseline_bench*|*batch_infer_bench*)
                echo "    PID $pid: ${cmd:0:80}"; found=1 ;;
        esac
    done
    return $((1-found))
}
IDLE=0
for i in $(seq 1 720); do
    # ★ 本板另一个会话长期占用 Core0(实测 Core1/Core2 始终 0%)。
    #   所以判据只看"Core1/Core2 空闲", 不再要求竞争者进程消失;
    #   Core0 上的外部负载由【每轮采样】记录, 在报告里如实标注。
    S=$(dbg_load)
    C0=$(core_pct "$S" 0); C1=$(core_pct "$S" 1); C2=$(core_pct "$S" 2)
    C0=${C0:-0}; C1=${C1:-0}; C2=${C2:-0}
    if [ "$C1" -lt 5 ] 2>/dev/null && [ "$C2" -lt 5 ] 2>/dev/null; then
        IDLE=$((IDLE+1))
        [ "$IDLE" -ge 3 ] && { echo "    Core1/Core2 空闲 ✓ (Core0 有外部负载, 见每轮采样)"; break; }
    else
        IDLE=0
    fi
    [ $((i % 12)) -eq 1 ] && echo "    [$i] 等待中 ... (dmc=$(dmc_load)% Core0=$C0%)"
    sleep 5
done
if [ "$IDLE" -lt 3 ]; then
    echo "  !! 等不到空闲窗口, 为避免污染数据, 放弃"
    exit 3
fi

# ---------------------------------------------------------------- 2. 维护窗口
echo "=== 2) 进入维护窗口, 停掉占 NPU/DDR 的服务 ==="
run_sudo touch /run/rk-gate/paused && echo "    已置维护窗口标记"
run_sudo systemctl stop rk-gate >/dev/null 2>&1
run_sudo systemctl stop rk-pipeline >/dev/null 2>/dev/null
sleep 4
STOPPED=1
echo "    rk-gate=$(systemctl is-active rk-gate 2>&1)  rk-pipeline=$(systemctl is-active rk-pipeline 2>&1)"
echo "    空载 dmc 基线: $(dmc_load)%"

# ---------------------------------------------------------------- 3. 跑矩阵
run_cfg() {
    local scen="$1" batch="$2" mode="$3" rep="$4" stress="$5"
    local m="$MODELS/batch$batch.rknn"
    local tag="s${scen}_b${batch}_${mode}_r${rep}"
    local jf="$OUTDIR/$tag.json"
    # 后台采样逐核负载 -> 每轮的污染记录(外部会话只占 Core0, 这里量化它)
    ( for i in $(seq 1 200); do
          sudo -n cat "$DBG" 2>/dev/null | head -1
          sleep 0.5
      done ) > "$OUTDIR/$tag.load" 2>/dev/null &
    local SAMP=$!
    "$BENCH" "$m" --duration "$DURATION" --warmup "$WARMUP" --mode "$mode" \
            --ddr-stress "$stress" --label "b${batch}_${mode}" --json "$jf" \
            > "$OUTDIR/$tag.txt" 2>&1
    local rc=$?
    kill "$SAMP" 2>/dev/null; wait "$SAMP" 2>/dev/null
    if [ $rc -ne 0 ]; then
        echo "    [$tag] 失败 rc=$rc"
        return 1
    fi
    local zc=$(grep -o '"zc_probe":"[^"]*"' "$jf" | cut -d'"' -f4)
    local dm=$(python3 -c "import json;d=json.load(open('$jf'));print('%.1f %.2f %.2f %.2f'%(d['dmc_mean'],d['batch_ms_mean'],d['fps'],d['frame_latency_ms']))" 2>/dev/null)
    local mx=$(awk -F'Core' '{for(i=2;i<=4;i++){split($i,a,":");gsub(/[^0-9]/,"",a[2]);if(a[2]+0>m[i])m[i]=a[2]+0}} END{print m[1]"/"m[2]"/"m[3]}' "$OUTDIR/$tag.load")
    echo "    [$tag] dmc=${dm%% *}% | batch=${dm#* } | zc=${zc} | 逐核峰值=${mx}"
}

for scen in empty stress; do
    echo
    echo "=== 4.$([ "$scen" = empty ] && echo A || echo B) 场景: $([ "$scen" = empty ] && echo '空载' || echo "DDR 压力($STRESS 线程)") ==="
    STRESSN=0
    [ "$scen" = stress ] && STRESSN=$STRESS
    for rep in $(seq 1 "$REPS"); do
        echo "  --- 第 $rep 轮 ---"
        for batch in 1 2 4; do
            for mode in single multi3; do
                run_cfg "$scen" "$batch" "$mode" "$rep" "$STRESSN"
            done
        done
        # 对照组: 3 个独立 context 分核(主工程现在的做法), 只对 batch=1 有意义
        # (batch>1 的模型驱动不允许绑核, 见报告)
        run_cfg "$scen" 1 workers3 "$rep" "$STRESSN"
    done
done

# ---------------------------------------------------------------- 5. 汇总
echo
echo "=== 5) 汇总 ==="
python3 "$HERE/summarize_batch.py" "$OUTDIR" | tee "$OUTDIR/summary.txt"

# ---------------------------------------------------------------- 6. 恢复
echo
echo "=== 6) 恢复生产服务 ==="
run_sudo systemctl start rk-gate >/dev/null 2>&1 && echo "    ✓ rk-gate 已启动"
run_sudo systemctl start rk-pipeline >/dev/null 2>&1 && echo "    ✓ rk-pipeline 已启动"
run_sudo rm -f /run/rk-gate/paused >/dev/null 2>&1 && echo "    ✓ 维护窗口标记已清除"
STOPPED=0
sleep 6
echo "    rk-gate = $(systemctl is-active rk-gate)  MainPID = $(systemctl show -p MainPID --value rk-gate)"
exit 0
