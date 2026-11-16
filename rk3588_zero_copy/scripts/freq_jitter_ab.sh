#!/usr/bin/env bash
# freq_jitter_ab.sh —— 【定频前 / 定频后】的 FPS 与 npu_ms 抖动 A/B 对比
#
# ★ 为什么要这个脚本
#   客户现场最怕的不是"平均帧率低", 而是"忽快忽慢":
#   均值 23ms 的两条曲线, 一条每帧都 23ms, 另一条一半 20ms 一半 26ms,
#   平均帧率一模一样, 但 P99 延迟差远了(现场表现为"偶尔卡一下")。
#
#   RK3588 默认 governor 全自动调频(onDemand):
#   CPU 一有空档就降频, 下一帧又要等升频爬坡 —— 视频流这种
#   "一帧接一帧、每帧间隔几毫秒"的负载最容易踩这个坑。
#
#   所以要量的是【抖动】(P99 / max), 不是只看均值。
#
# ★ 三个阶段
#   phase=baseline  当前频率策略(ondemand)下跑一遍
#   phase=locked    钉死最高频率后跑一遍
#   phase=ab        先 baseline, 再上锁, 再 locked, 最后打对比表
#
# ★ 用法
#   ./scripts/freq_jitter_ab.sh --phase baseline            # 不需要 root
#   sudo ./scripts/freq_jitter_ab.sh --phase ab --restore    # 一条命令跑完 A/B
#   SUDO_PASSWORD='xxx' ./scripts/freq_jitter_ab.sh --phase ab
#
#   参数:
#     --frames N    跑多少帧(默认 300)
#     --pipe N      --npu-pipe 并行度(默认 1; 交付是 4 路时用 3)
#     --src PATH    视频源(默认 /tmp/npu_bench/clip1080_60.mp4, 没有就回退)
#     --phase P     baseline | locked | ab
#     --restore     ab 跑完后把 governor 还原(默认不还原, 保持锁定状态)
#
# ★ 输出
#   每阶段一个日志: /tmp/freq_ab/<phase>.log
#   汇总表在最后, 报告写: /tmp/freq_ab/summary.txt
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=/tmp/freq_ab
SRC_DEFAULT=/tmp/npu_bench/clip1080_60.mp4
FALLBACK_SRC=/home/seeed/testvideo3.mp4
MODEL=$ROOT/model/ebike_int8_rk2332.rknn
LABELS=$ROOT/model/labels_ebike.txt
BIN=$ROOT/build/rk_zero_copy

FRAMES=300
PIPE=1
SRC=""
PHASE=ab
DO_RESTORE=0

while [ $# -gt 0 ]; do
    case "$1" in
        --frames)  FRAMES="$2"; shift 2;;
        --pipe)    PIPE="$2"; shift 2;;
        --src)     SRC="$2"; shift 2;;
        --phase)   PHASE="$2"; shift 2;;
        --restore) DO_RESTORE=1; shift;;
        -h|--help) sed -n '2,40p' "$0"; exit 0;;
        *) echo "未知参数: $1" >&2; exit 2;;
    esac
done

[ -z "$SRC" ] && SRC="$SRC_DEFAULT"
[ -f "$SRC" ] || SRC="$FALLBACK_SRC"
[ -f "$SRC" ] || { echo "找不到视频源" >&2; exit 1; }
[ -x "$BIN" ] || { echo "找不到可执行文件 $BIN(先 build)" >&2; exit 1; }

mkdir -p "$OUT"

export LD_LIBRARY_PATH="$ROOT/third_party/lib:$ROOT/lib:/usr/lib:${LD_LIBRARY_PATH:-}"

# ------------------------------------------------------------------ 频率采样器
# 跑测试的同时每 200ms 采一次: 每个 cpufreq policy 的当前频率 + NPU 频率/负载 + DDR 频率
sample_loop() {
    local dest="$1" flag="$2"
    : > "$dest"
    while [ -f "$flag" ]; do
        {
            for p in /sys/devices/system/cpu/cpufreq/policy*; do
                [ -d "$p" ] || continue
                printf '%s %s\n' "$(basename "$p")" "$(cat "$p/scaling_cur_freq" 2>/dev/null)"
            done
            # devfreq 的 load 节点格式是 "<负载%>@<频率Hz>", 取 % 那一段
            printf 'npu %s %s\n' \
                "$(cat /sys/class/devfreq/fdab0000.npu/cur_freq 2>/dev/null)" \
                "$(head -1 /sys/class/devfreq/fdab0000.npu/load 2>/dev/null | cut -d@ -f1)"
            printf 'dmc %s %s\n' \
                "$(cat /sys/class/devfreq/dmc/cur_freq 2>/dev/null)" \
                "$(head -1 /sys/class/devfreq/dmc/load 2>/dev/null | cut -d@ -f1)"
        } >> "$dest" 2>/dev/null
        sleep 0.2
    done
}

freq_summary() {
    # <采样文件> -> 每个节点的 min/max/avg
    awk '
    { k=$1; v=$2; if (v ~ /^[0-9]+$/) {
        n[k]++; s[k]+=v; if (!(k in mn) || v<mn[k]) mn[k]=v; if (v>mx[k]) mx[k]=v;
        if (NF>=3 && $3 ~ /^[0-9]+$/) { l[k]++; ls[k]+=$3; if ($3>lmx[k]) lmx[k]=$3 }
    }}
    END { for (k in n) printf "  %-8s freq min=%-9d max=%-9d avg=%-9.0f\n", k, mn[k], mx[k], s[k]/n[k];
          for (k in l) printf "  %-8s load avg=%.1f%%  max=%d%%\n", k, ls[k]/l[k], lmx[k] }
    ' "$1" | sort
}

# ------------------------------------------------------------------ 跑一轮
run_phase() {
    local phase="$1"
    local log="$OUT/$phase.log"
    local freq="$OUT/$phase.freq"
    local flag="/tmp/freq_ab/.running.$phase"

    rm -f "$flag"; touch "$flag"
    sample_loop "$freq" "$flag" &
    local sampler=$!

    echo
    echo "=================================================================="
    echo " 阶段: $phase   (frames=$FRAMES  pipe=$PIPE  src=$(basename "$SRC"))"
    echo "=================================================================="

    # 板子上还有谁在用 NPU —— 不记下来, 数字没法解释
    local others
    others="$(ps -eo pid,args | grep -E "rk_zero_copy" | grep -v grep | grep -v "$$" || true)"
    if [ -n "$others" ]; then
        echo " ⚠ 板上还有别的 rk_zero_copy 实例在跑(数字受干扰, 但 A/B 两侧条件一致):"
        echo "$others" | sed 's/^/     /'
    fi

    "$BIN" "$SRC" "$MODEL" \
        --labels "$LABELS" \
        --decoder mpp --no-show --no-speed \
        --frames "$FRAMES" --loop-src \
        --stats-interval 50 \
        --npu-pipe "$PIPE" \
        --conf 0.15 --nms 0.45 \
        > "$log" 2>&1

    rm -f "$flag"
    wait "$sampler" 2>/dev/null

    # ---- 抓主程序最后的统计块 ----
    npu_line="$(grep -E "NPU 单帧" "$log" | tail -1)"
    tot_line="$(grep -E "整帧单帧" "$log" | tail -1)"
    fps_line="$(grep -E "流水线 FPS" "$log" | tail -1)"
    deliv_line="$(grep -E "交付 FPS" "$log" | tail -1)"
    infer_line="$(grep -E "NPU 推理" "$log" | tail -1)"

    echo "-- 主程序统计 --"
    echo "$fps_line" | sed 's/^/  /'
    echo "$deliv_line" | sed 's/^/  /'
    echo "$infer_line" | sed 's/^/  /'
    echo "$npu_line" | sed 's/^/  /'
    echo "$tot_line" | sed 's/^/  /'
    echo "-- 运行期间频率采样 --"
    freq_summary "$freq" | sed 's/^/ /'
}

# ------------------------------------------------------------------ 主流程
case "$PHASE" in
    baseline|locked) run_phase "$PHASE" ;;
    ab)
        run_phase baseline
        echo
        echo "== 上锁(定到最高频率) =="
        if [ "$(id -u)" -ne 0 ] && [ -z "${SUDO_PASSWORD:-}" ]; then
            echo " ✗ 需要 root 才能写 governor 节点。"
            echo "   请用:  sudo $0 --phase ab --frames $FRAMES --pipe $PIPE"
            echo "   或:   SUDO_PASSWORD='你的密码' $0 --phase ab --frames $FRAMES --pipe $PIPE"
            exit 3
        fi
        "$ROOT/scripts/lock_max_freq.sh" --lock || { echo "上锁失败" >&2; exit 4; }
        run_phase locked
        if [ "$DO_RESTORE" = "1" ]; then
            echo
            echo "== 还原 governor =="
            "$ROOT/scripts/lock_max_freq.sh" --restore
        fi
        ;;
    *) echo "phase 只能是 baseline|locked|ab" >&2; exit 2;;
esac

# ------------------------------------------------------------------ 汇总
{
    echo
    echo "=================================================================="
    echo " 汇总: 定频前 vs 定频后"
    echo "=================================================================="
    printf "%-10s %-12s %-12s %-12s %-12s\n" "阶段" "流水线FPS" "交付FPS" "NPU P99(ms)" "NPU max(ms)"
    for p in baseline locked; do
        log="$OUT/$p.log"
        [ -f "$log" ] || continue
        pf="$(grep -E '流水线 FPS' "$log" | tail -1 | grep -oE '[0-9]+\.[0-9]+' | head -1)"
        df="$(grep -E '交付 FPS' "$log" | tail -1 | grep -oE '[0-9]+\.[0-9]+' | head -1)"
        p99="$(grep -E 'NPU 单帧' "$log" | tail -1 | grep -oE 'P99=[0-9.]+' | cut -d= -f2)"
        mx="$(grep -E 'NPU 单帧' "$log" | tail -1 | grep -oE 'max=[0-9.]+' | cut -d= -f2)"
        printf "%-10s %-12s %-12s %-12s %-12s\n" "$p" "$pf" "$df" "$p99" "$mx"
    done
    echo
    echo "日志: $OUT/{baseline,locked}.log"
} | tee "$OUT/summary.txt"
