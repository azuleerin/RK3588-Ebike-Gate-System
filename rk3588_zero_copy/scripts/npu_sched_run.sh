#!/usr/bin/env bash
# npu_sched_run.sh —— NPU 多核调度专项: 逐策略跑 + 抓 rknpu/load 截图
#
# 用法: scripts/npu_sched_run.sh <模型> <图片目录> <帧数>
set -u

# ---- 需要 root: 以 root 身份运行, 或先执行 `sudo -v` 缓存凭据(凭据不落盘) ----
if [ "$(id -u)" -ne 0 ] && ! sudo -n true 2>/dev/null; then
    echo "需要 root 权限，请使用 sudo 运行或先执行 sudo -v" >&2
    exit 1
fi
REPO=/home/seeed/rk_deploy
BIN=$REPO/rk3588_zero_copy/build/npu_sched_bench
MODEL=${1:-$REPO/models/ebike_int8_rk2332.rknn}
IMAGES=${2:-$REPO/dataset}
COUNT=${3:-300}
OUT=$REPO/logs/npu_sched
DOC=$REPO/rk3588_zero_copy/docs
mkdir -p "$OUT" "$DOC"

export LD_LIBRARY_PATH=$REPO/rk3588_zero_copy/third_party/lib:$REPO/rk3588_zero_copy/lib:/usr/lib

echo "strategy,frames,rknn_run_ms,outputs_get_ms,preprocess_ms,total_ms,fps" > "$OUT/results.csv"

run_one() {
    local strat="$1" tag="$2"
    echo ""
    echo "================= 策略: $strat ($tag) ================="
    # rktop 抓负载(它能显示 NPU 三核 + RGA + 进程 CPU)
    tmux kill-session -t npusched 2>/dev/null
    tmux new-session -d -s npusched -x 150 -y 42 \
        "sudo $REPO/_bench/rktop_install/bin/rktop 2>/dev/null || \
         sudo /tmp/rktop-install/bin/rktop"
    sleep 4
    # 采样 rknpu/load(每 0.5s)
    ( for i in $(seq 1 400); do
        echo "npu0=$(cat /sys/kernel/debug/rknpu/load 2>/dev/null | sed -nE 's/.*Core0: *([0-9]+)%.*/\1/p') \
npu1=$(cat /sys/kernel/debug/rknpu/load 2>/dev/null | sed -nE 's/.*Core1: *([0-9]+)%.*/\1/p') \
npu2=$(cat /sys/kernel/debug/rknpu/load 2>/dev/null | sed -nE 's/.*Core2: *([0-9]+)%.*/\1/p')"
        sleep 0.5
      done ) > "$OUT/load_$tag.txt" 2>/dev/null &
    local sampler=$!

    "$BIN" --model "$MODEL" --images "$IMAGES" --count "$COUNT" \
           --strategy "$strat" --tag "$tag" 2>/dev/null | tee -a "$OUT/results.csv"

    sleep 1
    kill $sampler 2>/dev/null
    # rktop 抓屏 -> PNG
    tmux capture-pane -t npusched -e -p > "$OUT/rktop_$tag.ansi" 2>/dev/null
    python3 "$REPO/rk3588_zero_copy/scripts/ansi_to_png.py" "$OUT/rktop_$tag.ansi" \
            "$DOC/npu_sched_$tag.png" "rktop — NPU 调度实验: $strat" >/dev/null 2>&1
    tmux kill-session -t npusched 2>/dev/null
    # rknpu/load 采样汇总
    python3 - "$OUT/load_$tag.txt" "$tag" <<'PYEOF'
import sys, re
path, tag = sys.argv[1], sys.argv[2]
n0=n1=n2=0; c=0
for line in open(path):
    m=re.findall(r'npu(\d)=(\d+)', line)
    if len(m)==3:
        n0+=int(m[0][1]); n1+=int(m[1][1]); n2+=int(m[2][1]); c+=1
if c:
    print(f"  [负载] {tag}: Core0 平均 {n0/c:.1f}%  Core1 {n1/c:.1f}%  Core2 {n2/c:.1f}%  ({c} 次采样)")
PYEOF
}

run_one auto      auto
run_one core0     expA_yolo_core0
run_one core0_1   expC_yolo_core0_1
run_one dual_seq  expB_dual_seq
run_one dual_par  expB_dual_par

echo ""
echo "================= 汇总 ================="
column -s, -t "$OUT/results.csv"
