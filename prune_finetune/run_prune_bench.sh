#!/usr/bin/env bash
# run_model_bench.sh —— 在【NPU 无竞争】窗口里跑多模型 A/B 纯推理测试
#
# ★ 关键坑(踩过): /sys/class/devfreq/fdab0000.npu/load 这个节点在本板【恒显示 100%】,
#   即使 NPU 完全空闲也一样 —— 不能用它判断空闲!
#   真正准的是 /sys/kernel/debug/rknpu/load: "NPU load: Core0: 0%, Core1: 0%, Core2: 0%"
#   (需要 root 读)。
#
# 做法:
#   1. touch /run/rk-gate/paused      -> 看门狗闭嘴(工程自带的维护窗口标记)
#   2. systemctl stop rk-gate rk-pipeline -> 停掉占 NPU 的服务
#      (rk-pipeline 是崩溃复现器, 在 auto-restart 循环里, 也会抢 NPU)
#   3. 按 debugfs 逐核负载确认真的空闲
#   4. 轮转跑: 每个模型 30s 一轮, 整轮序列重复 N 轮, 每轮都采样逐核负载,
#      发现别的核有负载就标记该轮被污染 —— 最后每组取【中位数轮】, 单轮结果也留档
#   5. systemctl start + rm paused 恢复
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
BENCH="$HERE/model_perf_bench"
MODELS_DIR="${MODELS_DIR:-$HERE/models_bench}"
REF_DIR="${REF_DIR:-$HERE/none}"
OUT_DIR="${OUT_DIR:-$HERE/out/bench}"
DURATION="${DURATION:-30}"
WARMUP="${WARMUP:-3}"
CORE="${CORE:-1}"
ROUNDS="${ROUNDS:-3}"
DBG=/sys/kernel/debug/rknpu/load

mkdir -p "$OUT_DIR"

run_sudo() {
    if [ "$(id -u)" -eq 0 ]; then "$@"; else sudo "$@"; fi
}
# ★ 先缓存一次 sudo 凭据, 之后用 sudo -n 免密 —— 后台采样器每 0.5s 读一次
#   debugfs, 每次都走 sudo -S -p "" 的话开销太大还会刷屏。
sudo -v 2>/dev/null   # ★ 必须带 -S, 否则凭据根本没缓存, 后台采样的 sudo -n 全部失败
dbg_load() { sudo -n cat "$DBG" 2>/dev/null | head -1; }

# 从 debugfs 串里抠出各核百分比: "NPU load: Core0: 63%, Core1: 67%, Core2: 66%,"
core_pct() {   # $1=core idx
    local s="$1"
    echo "$s" | grep -oE "Core$1: *[0-9]+" | grep -oE "[0-9]+$" | head -1
}
# 外部竞争者只会出现在【我没用的核】上 -> 按当前掩码排除自己那一核
CORE_IDX=0
case "$CORE" in
    1) CORE_IDX=0 ;;
    2) CORE_IDX=1 ;;
    4) CORE_IDX=2 ;;
esac
other_cores_busy() {
    local s="$1" i c busy=0
    for i in 0 1 2; do
        [ "$i" = "$CORE_IDX" ] && continue
        c=$(core_pct "$s" $i); c=${c:-0}
        [ "$c" -gt 10 ] 2>/dev/null && busy=1
    done
    [ "$busy" = "1" ] && return 0
    return 1
}

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

# ---- 1. 竞争者检查 ----
echo "=== 1) 检查 NPU 竞争者 ==="
list_competitors() {
    local p pid cmd found=0
    for p in /proc/[0-9]*; do
        pid=${p#/proc/}
        [ "$pid" = "$$" ] && continue
        cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
        case "$cmd" in
            *npu_layer_bench*|*npu_bench*|*npu_baseline_bench*|*model_perf_bench*)
                echo "    PID $pid: ${cmd:0:90}"; found=1 ;;
        esac
    done
    return $((1-found))
}
# 等到【真的没有竞争者】: 进程列表 + debugfs 逐核负载都要干净
# (只查进程不够: 别的会话的 benchmark 可能刚启动/刚结束)
WAITED=0
for i in $(seq 1 240); do
    # ★ 我用的是 CORE_IDX 那一核; 别的会话占用别的核【不影响】我的测量,
    #   只要"我没用的核"是干净的就行(下面的逐核判据负责)。
    if [ "${SKIP_PROC_CHECK:-0}" != "1" ] && list_competitors >/dev/null; then
        if [ "$CORE_IDX" = "0" ]; then
            if [ $((i % 6)) -eq 1 ]; then echo "    [$i] 仍有竞争者(同核), 等 5s ..."; fi
            sleep 5; WAITED=$((WAITED+5)); continue
        fi
    fi
    S=$(dbg_load)
    BUSY=0; i=0
    while [ "$i" -lt 3 ]; do
        # 只看【我没用的核】: 别的核上有别的会话在跑是可以接受的,
        # 我用自己那一核, 组间绝对可比。
        if [ "$i" != "$CORE_IDX" ]; then
            c=$(core_pct "$S" $i); c=${c:-0}
            [ "$c" -ge 5 ] 2>/dev/null && BUSY=1
        fi
        i=$((i+1))
    done
    if [ "$BUSY" = "0" ]; then
        echo "    无其他 NPU 竞争者 ✓ (已等待 ${WAITED}s, 逐核负载全 0)"
        break
    fi
    if [ $((i % 6)) -eq 1 ]; then echo "    [$i] NPU 仍忙 ($S), 等 5s ..."; fi
    sleep 5; WAITED=$((WAITED+5))
done

# ---- 2. 维护窗口 + 停服务 ----
echo "=== 2) 进入维护窗口, 停 NPU 占用服务 ==="
run_sudo touch /run/rk-gate/paused && echo "    已置维护窗口标记"
BEFORE_PID=$(systemctl show -p MainPID --value rk-gate 2>/dev/null)
echo "    停止前 rk-gate MainPID = $BEFORE_PID"
run_sudo systemctl stop rk-gate >/dev/null 2>&1
run_sudo systemctl stop rk-pipeline >/dev/null 2>&1
sleep 3
STOPPED=1
echo "    rk-gate=$(systemctl is-active rk-gate 2>&1)  rk-pipeline=$(systemctl is-active rk-pipeline 2>&1)"

# ---- 3. 确认真的空闲(用 debugfs, 不用 devfreq) ----
echo "=== 3) 确认 NPU 空闲 (debugfs 逐核) ==="
IDLE=0
for i in $(seq 1 30); do
    S=$(dbg_load)
    C0=$(core_pct "$S" 0); C1=$(core_pct "$S" 1); C2=$(core_pct "$S" 2)
    echo "    [$i] $S"
    if [ "${C0:-0}" -lt 5 ] 2>/dev/null && [ "${C1:-0}" -lt 5 ] 2>/dev/null \
       && [ "${C2:-0}" -lt 5 ] 2>/dev/null; then
        IDLE=$((IDLE+1)); echo "    空闲 ✓ (连续 $IDLE 次)"
        [ "$IDLE" -ge 3 ] && break
    else
        IDLE=0
    fi
    sleep 1
done
if [ "$IDLE" -lt 3 ]; then
    echo "  !! NPU 没等到空闲, 放弃 (避免测出被污染的数据)"
    exit 3
fi

# ---- 4. 轮转 A/B ----
MODELS=()
for f in "$MODELS_DIR"/*.rknn; do [ -e "$f" ] && MODELS+=("$f"); done
for f in "$REF_DIR"/A0_silu_640_perchannel.rknn; do [ -e "$f" ] && MODELS+=("$f"); done

echo "=== 4) 轮转 A/B: ${#MODELS[@]} 个模型 × ${ROUNDS} 轮 × 每轮 ${DURATION}s ==="
for r in $(seq 1 "$ROUNDS"); do
    echo ""
    echo "-------- 第 $r 轮 --------"
    for f in "${MODELS[@]}"; do
        base=$(basename "$f" .rknn)
        # ★ label 要能区分参考组, 否则 A0_silu_640_perchannel 会被当成 A0 混进汇总
        label="$base"    # ★ 用完整基名: relu512_p20 / REF_relu512_unpruned ... 互不撞名
        jf="$OUT_DIR/r${r}_$base.json"
        # 后台采样逐核负载(sudo -n 免密, 凭据已缓存)
        ( for i in $(seq 1 200); do
              sudo -n cat "$DBG" 2>/dev/null | head -1
              sleep 0.5
          done ) > "$OUT_DIR/r${r}_$base.load" 2>/dev/null &
        SAMP=$!
        echo "  --- [$r] $base ---"
        "$BENCH" "$f" --duration "$DURATION" --warmup "$WARMUP" --core "$CORE" \
                --label "$label" --json "$jf" > "$OUT_DIR/r${r}_$base.txt" 2>&1
        kill "$SAMP" 2>/dev/null; wait "$SAMP" 2>/dev/null
        # 判定这一轮有没有被别的核抢(我只用 Core0)
        MAXOTHER=0
        while read -r line; do
            [ -z "$line" ] && continue
            i=0
            while [ "$i" -lt 3 ]; do
                [ "$i" = "$CORE_IDX" ] && { i=$((i+1)); continue; }
                c=$(core_pct "$line" $i); c=${c:-0}
                [ "$c" -gt "$MAXOTHER" ] && MAXOTHER=$c
                i=$((i+1))
            done
        done < "$OUT_DIR/r${r}_$base.load"
        MEAN=$(grep -E "^单帧平均" "$OUT_DIR/r${r}_$base.txt" | awk '{print $3}')
        MED=$(grep -E "^单帧中位" "$OUT_DIR/r${r}_$base.txt" | awk '{print $3}')
        P95=$(grep -E "^P95" "$OUT_DIR/r${r}_$base.txt" | awk '{print $3}')
        FPS=$(grep -E "^单核 FPS" "$OUT_DIR/r${r}_$base.txt" | awk '{print $3}')
        FLAG=""
        [ "$MAXOTHER" -gt 10 ] && FLAG="  ★被污染(其他核峰值 ${MAXOTHER}%)"
        echo "      平均 ${MEAN} ms | 中位 ${MED} ms | P95 ${P95} ms | ${FPS} FPS | 其他核峰值 ${MAXOTHER}%${FLAG}"
    done
done

# ---- 5. 汇总(每组取中位数轮) ----
echo
echo "=== 5) 汇总 (每组取 3 轮的中位数) ==="
python3 - "$OUT_DIR" <<'PY'
import json, os, sys, statistics
d = sys.argv[1]
rows = {}
for f in sorted(os.listdir(d)):
    if not (f.startswith("r") and f.endswith(".json")):
        continue
    try:
        j = json.load(open(os.path.join(d, f)))
    except Exception:
        continue
    key = j["label"]
    rows.setdefault(key, []).append(j)
print("%-10s %-10s %-10s %-10s %-9s %-9s %-9s" %
      ("模型", "中位轮平均", "中位轮中位", "中位轮P95", "FPS", "体积MB", "轮间范围"))
for key in sorted(rows):
    js = sorted(rows[key], key=lambda x: x["mean_ms"])
    med = js[len(js)//2]
    means = [x["mean_ms"] for x in js]
    print("%-10s %-10.3f %-10.3f %-10.3f %-9.2f %-9.2f %-9s" % (
        key, med["mean_ms"], med["median_ms"], med["p95_ms"], med["fps"],
        med["file_mb"], "%.2f~%.2f" % (min(means), max(means))))
    out = os.path.join(d, "summary_%s.json" % key)
    json.dump({"key": key, "rounds": js, "median_round": med},
              open(out, "w"), indent=2)
PY

# ---- 6. 恢复 ----
echo
echo "=== 6) 恢复生产服务 ==="
run_sudo systemctl start rk-gate >/dev/null 2>&1 && echo "    ✓ rk-gate 已启动"
run_sudo systemctl start rk-pipeline >/dev/null 2>&1 && echo "    ✓ rk-pipeline 已启动"
run_sudo rm -f /run/rk-gate/paused >/dev/null 2>&1 && echo "    ✓ 维护窗口标记已清除"
STOPPED=0
sleep 8
echo "    rk-gate = $(systemctl is-active rk-gate)  MainPID = $(systemctl show -p MainPID --value rk-gate)"
grep -E "^\[ch[0-9]\]\[frame" /var/log/rk-gate-error.log 2>/dev/null | tail -2 | sed 's/^/    /'
exit 0
