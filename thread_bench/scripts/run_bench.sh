#!/usr/bin/env bash
# ===========================================================================
# run_bench.sh —— thread_bench 矩阵遍历
#
# 跑四个阶段, 每个阶段一份 JSONL, 最后由 render_md.py 汇总成 Markdown:
#
#   ① matrix    真实负载(默认 4 路 x 25 fps = 100 fps 供给)
#               问: 在【固定业务负载】下, 加线程会不会让延迟变差、CPU 被打爆?
#   ② saturate 饱和负载(fps 拉高到远超 NPU 能力)
#               问: NPU 的【峰值吞吐】随 T 怎么变? T>3 之后还有收益吗?
#               (两个负载生成器交叉验证: 限速高 fps + unlimited 狂推)
#   ③ queue    队列容量边界(取 ② 里最优的 T/策略, 跑 q = 1 / 64 / 128)
#               问: 丢帧策略对调度的影响? 缓冲多大时 e2e 延迟会爆?
#   ④ dual     双模型(检测 + 识别串联), 需要 --model2
#               问: 两个模型串联后, DDR 带宽 / NPU 调度是不是新的瓶颈?
#
# 用法:
#   ./scripts/run_bench.sh --duration 30
#   ./scripts/run_bench.sh --model2 ../models/ppocrv4_rec_int8.rknn --phase all
#   ./scripts/run_bench.sh --phase matrix --use-sudo      # 带上 debugfs NPU load
# ===========================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_DIR="$(cd "${ROOT_DIR}/.." && pwd)"

# ---- 默认参数 ----
MODEL="${MODEL:-$REPO_DIR/models/ebike_int8_rk2332.rknn}"
MODEL2="${MODEL2:-}"
MODEL2_CORE_MASK=""
DURATION=30
WARMUP=5
THREADS="4 6 8 12"
STRATEGIES="auto round all3"
QUEUE_SIZE=64
CHANNELS=4
FPS=25
SAT_FPS=100                 # 饱和阶段的每路帧率(4 路合计 400 fps, 远超 NPU 能力)
PRODUCER_MODE=normal
PHASE="all"
OUT_DIR="$ROOT_DIR/results"
RUN_DIR_OVERRIDE=""
USE_SUDO=0
NO_BUILD=0

usage() {
    sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    cat <<'EOF'

选项:
  --model <path>            主模型                [默认 ../models/ebike_int8_rk2332.rknn]
  --model2 <path>           第二模型(识别)
  --model2-core-mask <n>    第二模型核掩码 0-7
  --duration <sec>          测量窗口秒数          [默认 30]
  --warmup <sec>            预热秒数              [默认 5]
  --threads "<list>"        线程数, 空格分隔      [默认 "4 6 8 12"]
  --strategies "<list>"     策略, 空格分隔        [默认 "auto round all3"]
  --queue-size <n>          队列容量              [默认 64]
  --channels <n>            路数                  [默认 4]
  --fps <f>                 真实负载阶段的每路帧率 [默认 25]
  --sat-fps <f>             饱和阶段的每路帧率     [默认 100]
  --producer-mode <m>       normal|unlimited      [默认 normal]
  --phase <p>               matrix|saturate|queue|dual|dual_business|all [默认 all]
  --run-dir <dir>           复用已有输出目录(把新阶段追加进去)
  --out-dir <dir>           输出目录              [默认 ./results]
  --use-sudo                用 sudo 跑(才能读 debugfs 的 NPU load 节点)
  --no-build                不重新编译
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --model) MODEL="$2"; shift 2;;
        --model2) MODEL2="$2"; shift 2;;
        --model2-core-mask) MODEL2_CORE_MASK="$2"; shift 2;;
        --duration) DURATION="$2"; shift 2;;
        --warmup) WARMUP="$2"; shift 2;;
        --threads) THREADS="$2"; shift 2;;
        --strategies) STRATEGIES="$2"; shift 2;;
        --queue-size) QUEUE_SIZE="$2"; shift 2;;
        --channels) CHANNELS="$2"; shift 2;;
        --fps) FPS="$2"; shift 2;;
        --sat-fps) SAT_FPS="$2"; shift 2;;
        --producer-mode) PRODUCER_MODE="$2"; shift 2;;
        --phase) PHASE="$2"; shift 2;;
        --run-dir) RUN_DIR_OVERRIDE="$2"; shift 2;;
        --out-dir) OUT_DIR="$2"; shift 2;;
        --use-sudo) USE_SUDO=1; shift;;
        --no-build) NO_BUILD=1; shift;;
        -h|--help) usage; exit 0;;
        *) echo "未知参数: $1"; usage; exit 2;;
    esac
done

BIN="$ROOT_DIR/build/thread_bench"

if [[ "$NO_BUILD" -eq 0 ]]; then
    echo "[run_bench] 编译..."
    cmake -B "$ROOT_DIR/build" -S "$ROOT_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null || exit 1
    cmake --build "$ROOT_DIR/build" -j"$(nproc)" >/dev/null || exit 1
fi
if [[ ! -x "$BIN" ]]; then
    echo "[run_bench] 找不到可执行文件: $BIN"; exit 1
fi

RUNNER=""
if [[ "$USE_SUDO" -eq 1 ]]; then
    if sudo -n true 2>/dev/null; then
        RUNNER="sudo -n"
        echo "[run_bench] 用 sudo 跑(可以读 debugfs 的 NPU load)"
    else
        echo "[run_bench] 警告: 需要 sudo 但没有免密权限, 退回普通用户跑(读不到 NPU load)"
    fi
fi

TS="$(date +%Y%m%d_%H%M%S)"
if [[ -n "$RUN_DIR_OVERRIDE" ]]; then
    RUN_DIR="$RUN_DIR_OVERRIDE"
    mkdir -p "$RUN_DIR"
else
    RUN_DIR="$OUT_DIR/$TS"
    mkdir -p "$RUN_DIR"
fi

# ---- 预检 ----
if [[ ! -f "$MODEL" ]]; then echo "[run_bench] 模型不存在: $MODEL"; exit 1; fi
if [[ -n "$MODEL2" && ! -f "$MODEL2" ]]; then echo "[run_bench] 模型2不存在: $MODEL2"; exit 1; fi
AVAIL_KB="$(awk '/MemAvailable/{print $2}' /proc/meminfo 2>/dev/null || echo 0)"
echo "[run_bench] 输出目录: $RUN_DIR"
echo "[run_bench] 模型: $MODEL${MODEL2:+ | $MODEL2}   可用内存: $((AVAIL_KB/1024)) MB"

# ---- 跑一组配置 ----
# run_cfg <jsonl> <T> <strategy> <queue_size> <fps> <producer_mode> [model2] [model2_core_mask]
run_cfg() {
    local jsonl="$1" t="$2" st="$3" q="$4" fps="$5" pmode="$6"
    local m2="${7:-}" m2mask="${8:-}"
    local args=(--model "$MODEL" --threads "$t" --strategy "$st" --queue-size "$q"
                --channels "$CHANNELS" --fps "$fps" --duration "$DURATION" --warmup "$WARMUP"
                --producer-mode "$pmode")
    [[ -n "$m2" ]] && args+=(--model2 "$m2")
    [[ -n "$m2mask" ]] && args+=(--model2-core-mask "$m2mask")
    local tag="T=$t st=$st q=$q fps=$fps pm=$pmode"
    printf '  %-46s ' "$tag"
    # stdout 是 JSON(机器可读), stderr 是每秒监控(人看的) -> 分开存
    if ! $RUNNER "$BIN" "${args[@]}" >>"$jsonl" 2>>"$RUN_DIR/monitor.log"; then
        echo "失败(见 $RUN_DIR/monitor.log)"
        return 1
    fi
    tail -n 1 "$jsonl" | python3 -c "
import sys, json
line = sys.stdin.read().strip().splitlines()[-1]
d = json.loads(line)
print('吞吐=%7.2f fps  e2e_p99=%7.1f ms  M1_p99=%6.1f ms  CPU=%4.1f%%  核=%s' % (
    d['throughput_fps'], d['lat_e2e']['p99_ms'], d['lat_m1']['p99_ms'], d['cpu_pct'],
    '/'.join('%.0f' % v if v >= 0 else '-' for v in d['core_m1_pct'])))
"
}

# ===========================================================================
# ① matrix —— 真实负载
# ===========================================================================
if [[ "$PHASE" == "matrix" || "$PHASE" == "all" ]]; then
    echo
    echo "=== 阶段① matrix: 真实负载 ${CHANNELS}路 x ${FPS}fps = $((CHANNELS*FPS)) fps 供给 ==="
    JSONL="$RUN_DIR/matrix.jsonl"
    : > "$JSONL"
    for t in $THREADS; do
        for st in $STRATEGIES; do
            run_cfg "$JSONL" "$t" "$st" "$QUEUE_SIZE" "$FPS" "$PRODUCER_MODE" || exit 1
        done
    done
fi

# ===========================================================================
# ② saturate —— 饱和负载(限速高 fps + unlimited 狂推, 两个负载生成器互证)
# ===========================================================================
if [[ "$PHASE" == "saturate" || "$PHASE" == "all" ]]; then
    echo
    echo "=== 阶段② saturate: 饱和负载 ${CHANNELS}路 x ${SAT_FPS}fps = $((CHANNELS*SAT_FPS)) fps 供给 ==="
    JSONL="$RUN_DIR/saturate.jsonl"
    : > "$JSONL"
    for t in $THREADS; do
        for st in $STRATEGIES; do
            run_cfg "$JSONL" "$t" "$st" "$QUEUE_SIZE" "$SAT_FPS" "normal" || exit 1
        done
    done
    echo "  --- 交叉验证: unlimited 狂推(不占 CPU, 纯压 NPU) ---"
    JSONL2="$RUN_DIR/saturate_unlimited.jsonl"
    : > "$JSONL2"
    for t in $THREADS; do
        for st in $STRATEGIES; do
            run_cfg "$JSONL2" "$t" "$st" "$QUEUE_SIZE" "$SAT_FPS" "unlimited" || exit 1
        done
    done
fi

# ===========================================================================
# ③ queue —— 队列容量边界(取②里最优的 T/策略)
# ===========================================================================
if [[ "$PHASE" == "queue" || "$PHASE" == "all" ]]; then
    echo
    SRC="$RUN_DIR/saturate.jsonl"
    if [[ ! -s "$SRC" ]]; then
        echo "  (没有 saturate 结果, 用默认 T=6 / round)"
        BEST_T=6; BEST_ST=round
    else
        BEST="$(python3 "$SCRIPT_DIR/render_md.py" --best "$SRC")"
        BEST_T="$(echo "$BEST" | cut -d' ' -f1)"
        BEST_ST="$(echo "$BEST" | cut -d' ' -f2)"
    fi
    echo "=== 阶段③ queue: 最优配置 T=$BEST_T/$BEST_ST, 队列容量 1 / $QUEUE_SIZE / 128 ==="
    JSONL="$RUN_DIR/queue.jsonl"
    : > "$JSONL"
    for q in 1 "$QUEUE_SIZE" 128; do
        run_cfg "$JSONL" "$BEST_T" "$BEST_ST" "$q" "$SAT_FPS" "normal" || exit 1
    done
    echo "  --- unlimited 下的同样三档 ---"
    JSONL2="$RUN_DIR/queue_unlimited.jsonl"
    : > "$JSONL2"
    for q in 1 "$QUEUE_SIZE" 128; do
        run_cfg "$JSONL2" "$BEST_T" "$BEST_ST" "$q" "$SAT_FPS" "unlimited" || exit 1
    done
fi

# ===========================================================================
# ④ dual —— 双模型串联
# ===========================================================================
if [[ "$PHASE" == "dual" || "$PHASE" == "all" ]]; then
    if [[ -z "$MODEL2" ]]; then
        echo
        echo "=== 阶段④ dual: 跳过(没给 --model2) ==="
    else
        echo
        echo "=== 阶段④ dual: 双模型 ${MODEL##*/} + ${MODEL2##*/} (识别与检测同掩码) ==="
        JSONL="$RUN_DIR/dual.jsonl"
        : > "$JSONL"
        for t in $THREADS; do
            for st in $STRATEGIES; do
                # 注意: 这里【不传】model2-core-mask —— 主模型与识别模型同掩码
                run_cfg "$JSONL" "$t" "$st" "$QUEUE_SIZE" "$SAT_FPS" "normal" "$MODEL2" "" || exit 1
            done
        done
    fi
fi

# ===========================================================================
# ④b dual_business —— 双模型 + 业务真实拓扑(识别集中到一颗核)
# ===========================================================================
if [[ "$PHASE" == "dual_business" || "$PHASE" == "all" ]]; then
    if [[ -z "$MODEL2" ]]; then
        echo
        echo "=== 阶段④b dual_business: 跳过(没给 --model2) ==="
    else
        MASK="${MODEL2_CORE_MASK:-1}"
        echo
        echo "=== 阶段④b dual_business: 双模型, 识别集中到掩码 $MASK (复刻 --ocr-core 0) ==="
        JSONL="$RUN_DIR/dual_business.jsonl"
        : > "$JSONL"
        for t in $THREADS; do
            for st in $STRATEGIES; do
                run_cfg "$JSONL" "$t" "$st" "$QUEUE_SIZE" "$SAT_FPS" "normal" "$MODEL2" "$MASK" || exit 1
            done
        done
    fi
fi

# ===========================================================================
# 汇总成 Markdown
# ===========================================================================
echo
echo "[run_bench] 生成 Markdown..."
python3 "$SCRIPT_DIR/render_md.py" "$RUN_DIR" --out "$RUN_DIR/bench_$TS.md" || exit 1
echo "[run_bench] 完成:"
echo "  JSONL : $RUN_DIR/*.jsonl"
echo "  Markdown: $RUN_DIR/bench_$TS.md"
echo "  监控日志: $RUN_DIR/monitor.log"
