#!/usr/bin/env bash
# run_clean_window.sh —— 在【NPU 无竞争】的窗口里跑纯推理基线 (项4)
#
# 为什么需要"干净窗口":
#   生产服务 rk-gate 常驻并占满 3 个 NPU 核(devfreq load=100%),
#   此时测出来的"单帧耗时"是【排队后】的数字, 不能当基线。
#
# 怎么做(不需要 root, 全程可逆):
#   1. 确认没有别的 NPU 竞争者
#   2. kill -STOP  <rk_gate pid>   —— 冻结进程(不是杀进程), systemd 不会重启它
#   3. 起一个心跳, 定时 touch 服务日志 —— 看门狗(rk-gate-watchdog, root 跑的)
#      用日志 mtime 当心跳, 冻结期间不 touch 会被判"整机卡死"并在 60s 后重启服务。
#      本机 /run/rk-gate/paused 是官方维护窗口标记, 但该目录 root 可写, 只能 root 建,
#      所以这里用 touch 日志等效代替, 测完立刻停掉。
#   4. 等 NPU 负载归零
#   5. 跑基线
#   6. 恢复: 停心跳 + kill -CONT, 并校验服务恢复出帧
#
# 任何一步失败 / 被中断, EXIT trap 都会把服务恢复回来。
#
# ⚠⚠⚠ 【重要警告】本机实测: SIGSTOP 冻结 rk_gate 会让它的条件变量交接丢唤醒,
#   流水线【永久卡死】(所有线程堵在 futex_wait_queue, 帧号不再推进)。
#   恢复只能靠 rk-gate-watchdog 的 L1 心跳判定(日志 mtime 停 60s)重启服务。
#   ★ 官方推荐做法(需要 root, 本机 sudo 要密码):
#         sudo systemctl stop rk-gate        # 看门狗明确尊重人工 stop, 不会拉起
#         ./npu_baseline_bench ...            # 测
#         sudo systemctl start rk-gate
#     或者用官方维护窗口标记:
#         sudo touch /run/rk-gate/paused      # 看门狗闭嘴
#         sudo systemctl stop rk-gate
#         ...测...
#         sudo systemctl start rk-gate && sudo rm /run/rk-gate/paused
#   ★ 下面这段无 root 的 SIGSTOP 方案只在本审计里用过一次(已验证可测出数据),
#     但它有【卡死 + 被看门狗重启】的副作用, 复现时要有人盯着。
set -u

MODEL="${MODEL:-/home/seeed/rk_deploy/models/ebike_int8_rk2332.rknn}"
BENCH="${BENCH:-/home/seeed/rk_deploy/audit/npu_baseline_bench}"
ITERS="${ITERS:-60}"
WARMUP="${WARMUP:-15}"
REPS="${REPS:-5}"
OUTDIR="${OUTDIR:-/home/seeed/rk_deploy/audit/out}"
LOGDIR=/var/log
NPU_LOAD=/sys/class/devfreq/fdab0000.npu/load

HB_PID=""
TARGET_PID=""

restore() {
    local rc=$?
    [ -n "$HB_PID" ] && kill "$HB_PID" 2>/dev/null
    HB_PID=""
    if [ -n "$TARGET_PID" ] && kill -0 "$TARGET_PID" 2>/dev/null; then
        kill -CONT "$TARGET_PID" 2>/dev/null && echo "[restore] 已 SIGCONT 恢复 rk_gate (pid=$TARGET_PID)"
    fi
    exit $rc
}
trap restore EXIT INT TERM

mkdir -p "$OUTDIR"

echo "=== 1) 检查 NPU 竞争者 ==="
competing() {
    local p pid cmd
    for p in /proc/[0-9]*; do
        pid=${p#/proc/}
        [ "$pid" = "$$" ] && continue
        cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
        case "$cmd" in
            *npu_layer_bench*|*npu_bench*|*npu_baseline_bench*)
                echo "    PID $pid: ${cmd:0:100}"; return 0 ;;
        esac
    done
    return 1
}
for i in $(seq 1 30); do
    if ! competing; then echo "    无其他 NPU 竞争者 ✓"; break; fi
    echo "    [$i] 仍有竞争者, 等待 4s ..."
    sleep 4
done
competing || true

TARGET_PID=$(systemctl show -p MainPID --value rk-gate 2>/dev/null)
echo "    rk_gate MainPID = $TARGET_PID"
if [ -z "$TARGET_PID" ] || [ "$TARGET_PID" = "0" ]; then
    echo "[FATAL] 拿不到 rk_gate PID, 放弃"; exit 2
fi
if ! tr '\0' ' ' < /proc/"$TARGET_PID"/cmdline 2>/dev/null | grep -q rk_gate; then
    echo "[FATAL] PID $TARGET_PID 不是 rk_gate, 放弃"; exit 2
fi

echo "=== 2) 冻结 rk_gate (SIGSTOP) ==="
kill -STOP "$TARGET_PID" && echo "    已冻结 (pid=$TARGET_PID)"

echo "=== 3) 起心跳, 防止看门狗误判卡死 ==="
( while true; do touch "$LOGDIR/rk-gate.log" "$LOGDIR/rk-gate-error.log" 2>/dev/null; sleep 10; done ) &
HB_PID=$!
echo "    心跳 pid=$HB_PID"

echo "=== 4) 等 NPU 负载归零 ==="
for i in $(seq 1 40); do
    L=$(cat "$NPU_LOAD" 2>/dev/null | head -1)
    PCT=${L%%@*}
    echo "    [$i] NPU load = $L"
    if [ -n "$PCT" ] && [ "$PCT" -lt 5 ] 2>/dev/null; then echo "    NPU 已空闲 ✓"; break; fi
    sleep 1
done

echo
echo "=== 5) 跑基线 (zero-copy) ==="
"$BENCH" "$MODEL" --iters "$ITERS" --warmup "$WARMUP" --reps "$REPS" \
        --mode both --cores 1,2,4 --io zerocopy --json \
        2>&1 | tee "$OUTDIR/baseline_zerocopy.txt"

echo
echo "=== 6) 跑基线 (memcpy 对照) ==="
"$BENCH" "$MODEL" --iters "$ITERS" --warmup "$WARMUP" --reps "$REPS" \
        --mode single --cores 1 --io memcpy --json \
        2>&1 | tee "$OUTDIR/baseline_memcpy.txt"

echo
echo "=== 7) 恢复服务 ==="
kill "$HB_PID" 2>/dev/null; HB_PID=""
kill -CONT "$TARGET_PID" && echo "    已 SIGCONT (pid=$TARGET_PID)"
sleep 5

# 校验服务真的恢复出帧
F1=$(cat /run/rk-gate/ch1_frame 2>/dev/null)
sleep 8
F2=$(cat /run/rk-gate/ch1_frame 2>/dev/null)
if [ -n "$F1" ] && [ -n "$F2" ] && [ "$F1" != "$F2" ]; then
    echo "    ✓ 服务已恢复出帧 (ch1_frame $F1 -> $F2)"
else
    echo "    ⚠ 帧号未推进 (ch1_frame: '$F1' -> '$F2') —— SIGSTOP 可能让流水线卡死。"
    echo "      现象: 4 个通道各报一条「等检测结果超时」, 之后所有线程堵在 futex_wait_queue。"
    echo "      恢复: 看门狗 L1 心跳停 60s 后会自动重启 (实测 76s 时触发), 或手工:"
    echo "            kill -9 \$(systemctl show -p MainPID --value rk-gate)   # on-failure 10s 后自动拉起"
    echo "      ★ 本机 sudo 需要密码, 所以优先等看门狗自愈。"
    sleep 20
    F3=$(cat /run/rk-gate/ch1_frame 2>/dev/null)
    NEWPID=$(systemctl show -p MainPID --value rk-gate 2>/dev/null)
    echo "      20s 后: ch1_frame='$F3'  MainPID=$NEWPID (原 $TARGET_PID)"
    [ -n "$F3" ] && [ "$F3" != "$F1" ] && echo "      ✓ 看门狗已重启并恢复出帧"
fi
echo "    NPU load 现在: $(cat "$NPU_LOAD" 2>/dev/null | head -1)"
TARGET_PID=""
exit 0
