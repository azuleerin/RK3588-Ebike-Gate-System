#!/bin/bash
# ============================================================================
# run_evidence.sh —— 交付证据长跑: 把完整终端输出落盘, 并附带内存采样
#
# 做四件事:
#   1) 先写一段环境头(时间/内核/模型/二进制/命令), 让报告有据可查
#   2) 用 `2>&1 | tee -a` 跑 —— ★ 程序几乎所有日志都打在 stderr,
#      不加 2>&1 的话文件里是空的
#   3) 后台每 5 秒采一次 RSS, 单独存一个文件, 跑完给出 min/max/末值
#   4) 从 [frame N] 统计行算出 fps / npu / total 的均值, 直接贴给架构组
#
# 用法:
#   ./run_evidence.sh --ebike --frames 15000
#   ./run_evidence.sh --ebike --frames 15000 --sink-stdout   # 逐帧结果也存
#
# 产物(默认在 /tmp):
#   rk3588_evidence_<时间戳>.log   完整输出 + 环境头 + 结尾汇总
#   rk3588_evidence_<时间戳>.rss   内存采样(时间戳 + RSS KB)
# ============================================================================
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

# 工程内的 librga.so / librknnrt.so: 二进制已用 $ORIGIN 相对 RUNPATH,
# 这里再兜一层, 保证从任何位置调用都能找到库。
export LD_LIBRARY_PATH="${HERE}/third_party/lib:${HERE}/lib:/usr/lib:${LD_LIBRARY_PATH:-}"

FRAMES="${FRAMES:-15000}"
# 日志默认落到工作区的 logs/ 目录(工程同级), 不要再写到会被清空的 /tmp
LOGDIR="${LOGDIR:-$(cd "$HERE/.." && pwd)/logs}"
mkdir -p "$LOGDIR"
STAMP="$(date +%Y%m%d_%H%M%S)"
LOG="${LOGDIR}/rk3588_evidence_${STAMP}.log"
RSS_LOG="${LOGDIR}/rk3588_evidence_${STAMP}.rss"

ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --frames) FRAMES="$2"; shift 2 ;;
        *)        ARGS+=("$1"); shift ;;
    esac
done

# ---------------------------------------------------------------- 环境头
{
    echo "================================================================"
    echo " RK3588 零拷贝工程 —— 交付证据长跑记录"
    echo "================================================================"
    echo "开始时间   : $(date '+%Y-%m-%d %H:%M:%S %Z')"
    echo "主机       : $(hostname)"
    echo "内核       : $(uname -a)"
    echo "架构       : $(uname -m)   dpkg: $(dpkg --print-architecture 2>/dev/null)"
    echo "工程目录   : $HERE"
    echo "运行命令   : ./run_evidence.sh ${ARGS[*]} --frames $FRAMES"
    echo "基线命令   : ./run_baseline.sh ${ARGS[*]} --frames $FRAMES"
    echo "二进制     : $(ls -la ./build/rk_zero_copy 2>/dev/null | awk '{print $5" 字节  "$6" "$7" "$8}')"
    echo "模型       : $(ls -la ./model/*.rknn 2>/dev/null | awk '{print "  "$NF" ("$5" 字节)"}')"
    echo "标签       : $(cat ./model/labels_ebike.txt 2>/dev/null | tr '\n' ',' )"
    echo "环境变量   : RK_FORCE_CPU_RGA=1 (RGA 走 OpenCV CPU 转换)"
    echo "================================================================"
    echo ""
} > "$LOG"

echo "日志   : $LOG"
echo "内存采样: $RSS_LOG"
echo ""

# ---------------------------------------------------------------- 内存采样
: > "$RSS_LOG"
(
    while true; do
        # ★ 按命令行匹配而不是 `ps -C rk_zero_copy`: 进程名可能被截断,
        #   而且 `ps -C` 在 busybox/精简版 procps 上行为不一致。
        rss=$(ps -eo rss=,args= 2>/dev/null | grep -F "rk_zero_copy" | grep -v grep \
              | head -1 | awk '{print $1}')
        if [ -n "$rss" ]; then
            echo "$(date +%s) $rss" >> "$RSS_LOG"
        fi
        sleep 5
    done
) &
SAMPLER_PID=$!
trap 'kill $SAMPLER_PID 2>/dev/null' EXIT INT TERM

# ---------------------------------------------------------------- 跑
# ★ 2>&1 必须有: 程序的日志基本都走 stderr, 否则文件里是空的。
# ★ stdbuf -oL: 避免管道场景下 stdout 被块缓冲, 实时落盘。
stdbuf -oL -eL ./run_baseline.sh ${ARGS[@]+"${ARGS[@]}"} --frames "$FRAMES" 2>&1 | tee -a "$LOG"
RC=${PIPESTATUS[0]}

kill $SAMPLER_PID 2>/dev/null
trap - EXIT INT TERM

# ---------------------------------------------------------------- 汇总
{
    echo ""
    echo "================================================================"
    echo " 汇总"
    echo "================================================================"
    echo "结束时间   : $(date '+%Y-%m-%d %H:%M:%S %Z')"
    echo "退出码     : $RC"
    echo ""
    echo "--- 帧统计(取 [frame N] 行的均值) ---"
    awk '
        /\[frame [0-9]+\]/ {
            n++
            for (i = 1; i <= NF; i++) {
                if ($i ~ /^fps=/) { split($i, a, "="); fps += a[2]; cf++ }
                if ($i ~ /^dec=/) { split($i, a, "="); dec += a[2]; cd++ }
                if ($i ~ /^npu=/) { split($i, a, "="); npu += a[2]; cn++ }
                if ($i ~ /^rga=/) { split($i, a, "="); rga += a[2]; cr++ }
                if ($i ~ /^total=/) { split($i, a, "="); tot += a[2]; ct++ }
                if ($i ~ /^objs=/) { ob += substr($i, 6); co++ }
            }
            if (last == "") last = $0
        }
        END {
            if (n == 0) { print "  (没有采集到 [frame N] 统计行)"; exit }
            printf "  统计样本数 : %d 行 (每 60 帧一行)\n", n
            if (cf) printf "  fps 均值    : %.2f\n", fps/cf
            if (cd) printf "  dec 均值    : %.2f ms\n", dec/cd
            if (cr) printf "  rga 均值    : %.2f ms\n", rga/cr
            if (cn) printf "  npu 均值    : %.2f ms\n", npu/cn
            if (ct) printf "  total 均值  : %.2f ms\n", tot/ct
            if (co) printf "  objs 均值   : %.2f\n", ob/co
            print "  最后一行    : " last
        }
    ' "$LOG"
    echo ""
    echo "--- 内存(RSS KB) ---"
    if [ -s "$RSS_LOG" ]; then
        awk 'NR==1 {mn=$2; mx=$2; first=$2} {if($2<mn)mn=$2; if($2>mx)mx=$2; last=$2; n++}
             END {printf "  采样 %d 次  最小 %d KB  最大 %d KB  末值 %d KB  (首值 %d KB, 增长 %d KB)\n",
                     n, mn, mx, last, first, last-first}' "$RSS_LOG"
        echo "  (增长接近 0 = 没有内存泄漏)"
    else
        echo "  (没采到数据)"
    fi
    echo "================================================================"
} | tee -a "$LOG"

echo ""
echo "完整日志: $LOG"
echo "内存采样: $RSS_LOG"
