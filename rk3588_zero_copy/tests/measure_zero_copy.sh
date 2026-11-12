#!/bin/bash
# measure_zero_copy.sh —— 零拷贝前后的 A/B 实测
#
#   A 组: CPU 转换 (不开 staging)            -> RGA 输入来源 = CPU 映射地址
#   B 组: RGA importbuffer_fd (RK_RGA_STAGE=1) -> RGA 输入来源 = DMA-BUF fd
#
# 每组跑 --frames N 帧, 抓:
#   · rga=xx ms / npu=xx ms / total=xx ms   (程序自己的统计)
#   · %CPU                                  (top -b 采样)
#   · RSS                                   (/proc/<pid>/status)
#
# 用法: ./tests/measure_zero_copy.sh [帧数] [视频源]
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$HERE"

FRAMES="${1:-300}"
URL="${2:-rtsp://192.168.31.200:8557/h264}"
MODEL="${MODEL:-./model/ebike_int8_rk2332.rknn}"
LABELS="${LABELS:-./model/labels_ebike.txt}"
export LD_LIBRARY_PATH="$HERE/third_party/lib:${LD_LIBRARY_PATH:-}"

run_one() {
    local tag="$1"; shift
    local env_extra="$1"; shift
    local log="/tmp/zerocopy_${tag}.log"

    echo "================ $tag ================"
    # 直接跑二进制, 不要用 timeout 包一层 —— 否则 top 采到的是 timeout 自己。
    # 程序自己按 --frames 退出, 这里只加一个兜底看门狗。
    env $env_extra "$HERE/build/rk_zero_copy" "$URL" "$MODEL" \
        --labels "$LABELS" --no-show --frames "$FRAMES" > "$log" 2>&1 &
    local pid=$!
    ( sleep 240; kill -9 "$pid" 2>/dev/null ) &
    local watchdog=$!

    # 采样 %CPU / RSS
    sleep 6
    local top_out="" rss_out=""
    for _ in 1 2 3 4 5 6; do
        if kill -0 "$pid" 2>/dev/null; then
            top_out="$top_out$(top -b -n 1 -p "$pid" 2>/dev/null | tail -1)\n"
            rss_out="$rss_out$(awk '/VmRSS/{print $2" "$3}' /proc/$pid/status 2>/dev/null)\n"
            sleep 2
        fi
    done
    wait "$pid" 2>/dev/null
    local rc=$?
    kill "$watchdog" 2>/dev/null

    echo "退出码: $rc"
    echo "--- 关键日志 ---"
    grep -E "\[RGA\] 输入来源|\[NV12\] 数据源|\[NPU\] 输入来源|\[RGA-Stage\]" "$log" | head -6
    echo "--- 最后一帧统计 ---"
    grep -E "^\[frame " "$log" | tail -1
    echo "--- 平均统计 ---"
    sed -n '/性能统计/,/画框/p' "$log" | tail -8
    echo "--- top -b %CPU 采样 (pid=$pid) ---"
    echo -e "$top_out"
    echo "--- VmRSS 采样 (kB) ---"
    echo -e "$rss_out"
    echo
}

run_one "cpu_path"   "RK_FORCE_CPU_RGA=1"
run_one "rga_fdpath" "RK_RGA_STAGE=1"
