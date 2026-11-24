#!/usr/bin/env bash
# ============================================================================
# run_layer_bench.sh —— 一键执行 npu_layer_bench 的全部分层压测
#
# 用法:
#   scripts/run_layer_bench.sh [选项]
#     --duration <sec>   每组测量窗口秒数 (默认 30)
#     --warmup <sec>     每组预热秒数 (默认 5)
#     --repeats <n>      每组重复次数 (默认 3)
#     --layers <spec>    跑哪几层, 逗号分隔 (默认 all)
#     --out <dir>        结果目录 (默认 logs/layer_bench/<时间戳>)
#     --no-quiet         不停服务(会污染数据, 只在调试时用)
#     --no-freq-lock     不锁 NPU 频率(默认锁到最高)
#
# ★★ 板子必须"安静"
#   这块板子上常驻着好几个吃资源的系统服务:
#     · rk-gate.service           生产 4 路推理服务(抢同一颗 NPU!)
#     · rk-gate-watchdog.service  生产服务的看门狗(会把它秒级拉起)
#     · rk-night-soak-monitor     长跑看护(会采样, 也会拉起别的)
#     · rk-pipeline.service       崩溃复现单元
#     · rk-pipeline-watchdog      它的看门狗
#     · rk-kernel-log.service     kernel 日志抓取(带 fsync, 很吃 IO)
#   只要有它们在跑, 测出来的就是"负重"数据 —— 之前 79.20 FPS 那个结论
#   就是因为这个被重新做过一次(见 SOAK_REPORT_20261118.md §2)。
#
#   所以本脚本默认: 停掉这些服务 -> 压测 -> 【自动恢复】。
#   恢复用主工程的 scripts/restore_services.sh(幂等, 有备份)。
#
# ★ NPU 频率
#   RK3588 的 NPU 默认 rknpu_ondemand (300MHz~1GHz)。视频流这种
#   "一帧接一帧、每帧间隔几毫秒"的负载, governor 看到空档就降频,
#   下一帧又得等爬坡 —— 测出来的 fps 会偏低且抖动大。
#   所以压测期间把 NPU 锁到最高频, 结束后还原。
# ============================================================================
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$ROOT/.." && pwd)"

# ---------------------------------------------------------------- 参数
DURATION=30
WARMUP=5
REPEATS=3
LAYERS=all
OUT_DIR=""
QUIET_BOARD=1
LOCK_FREQ=1
WORKERS=${WORKERS:-1,2,3,4,6,9}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --duration) DURATION="$2"; shift 2 ;;
        --warmup)   WARMUP="$2"; shift 2 ;;
        --repeats)  REPEATS="$2"; shift 2 ;;
        --layers)   LAYERS="$2"; shift 2 ;;
        --workers)  WORKERS="$2"; shift 2 ;;
        --out)      OUT_DIR="$2"; shift 2 ;;
        --no-quiet) QUIET_BOARD=0; shift ;;
        --no-freq-lock) LOCK_FREQ=0; shift ;;
        -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

TS="$(date +%Y%m%d_%H%M%S)"
[ -n "$OUT_DIR" ] || OUT_DIR="$REPO/logs/layer_bench/$TS"
mkdir -p "$OUT_DIR"

# ---------------------------------------------------------------- 资源
BIN="$ROOT/build/npu_layer_bench"
DET_MODEL=${DET_MODEL:-$REPO/models/ebike_int8_rk2332.rknn}
OCR_MODEL=${OCR_MODEL:-$REPO/models/ppocrv4_rec_fp16.rknn}
LABELS=${LABELS:-$REPO/models/labels_ebike.txt}
OCR_DICT=${OCR_DICT:-$REPO/models/ppocr_keys_v1.txt}
GATE_BIN=${GATE_BIN:-$REPO/rk3588_zero_copy/build/rk_gate}

RES640=${RES640:-$ROOT/models/res/yolov8n_640.rknn}
RES576=${RES576:-$ROOT/models/res/yolov8n_576.rknn}
RES512=${RES512:-$ROOT/models/res/yolov8n_512.rknn}

# 4 路视频源(和生产 scripts/run_4ch_gate_single.sh 完全一致, 保证可比)
SRC_CH1=${SRC_CH1:-/home/seeed/testvideo3.mp4}
SRC_CH2=${SRC_CH2:-/home/seeed/testvideo2.mp4}
SRC_CH3=${SRC_CH3:-/home/seeed/Test2_fixed.mp4}
SRC_CH4=${SRC_CH4:-/home/seeed/电动车素材.mp4}
IPM_720P=${IPM_720P:-"200,415;1000,429;967,585;367,546"}
IPM_4K=${IPM_4K:-"600,1244;3000,1286;2900,1754;1100,1638"}

# 压测期间要停掉的服务
STOP_SERVICES=(rk-gate.service rk-gate-watchdog.service rk-night-soak-monitor.service
               rk-pipeline.service rk-pipeline-watchdog.service rk-kernel-log.service
               rk-minimal.service)

SUDO="sudo"
run_sudo() { sudo -n "$@" 2>/dev/null; }

# ★ 把数据写进 root 才能写的节点(比如 devfreq 的 governor / target_freq)。
#   不能用 run_sudo: `echo pw | sudo -S tee file` 里, sudo 会把【密码行】读走,
#   留给 tee 的 stdin 是空的 —— tee 于是什么都不写, 而且**不报错**。
#   所以这里必须把数据拼进 `sh -c "..."` 的命令字符串里。
run_sudo_write() {
    local value="$1" node="$2"
    run_sudo sh -c "printf '%s\n' '$value' > '$node'"
}

# ---------------------------------------------------------------- 状态保存/恢复
RESTORE_SERVICES=()
RESTORE_FREQ=""
RESTORE_MINFREQ=""
RESTORE_MAXFREQ=""
PAUSE_FILES=()
QUIETED=0

restore() {
    local rc=$?
    trap - EXIT INT TERM
    if [ "$QUIETED" -eq 1 ]; then
        echo ""
        echo "==> 恢复现场 ..."
        # 1) NPU 频率还原
        if [ -n "$RESTORE_FREQ" ] && [ "$LOCK_FREQ" -eq 1 ]; then
            NPU_DEV=/sys/class/devfreq/fdab0000.npu
            if [ -n "$RESTORE_MAXFREQ" ]; then
                run_sudo_write "$RESTORE_MAXFREQ" "$NPU_DEV/max_freq"
            fi
            if [ -n "$RESTORE_MINFREQ" ]; then
                run_sudo_write "$RESTORE_MINFREQ" "$NPU_DEV/min_freq"
            fi
            run_sudo_write "$RESTORE_FREQ" "$NPU_DEV/governor"
            echo "    NPU governor -> $RESTORE_FREQ, min/max_freq -> $RESTORE_MINFREQ/$RESTORE_MAXFREQ"
            echo "    NPU cur_freq: $(cat $NPU_DEV/cur_freq 2>/dev/null)"
        fi
        # 2) 撤掉临时 drop-in(否则 Restart=no 会一直留着, 服务就再也拉不起来了)
        #    ★ 必须撤【所有】rk-* 的, 不能只撤 RESTORE_SERVICES 里那几个 ——
        #      quiet_board 是给"所有带 Restart=always 的 rk-* 单元"都建了 drop-in,
        #      而其中有些单元在停服务那一刻本来就是 inactive, 不在恢复名单里,
        #      但 drop-in 留下来了 —— 实测踩到: rk-pipeline 被永久 Restart=no。
        for d in /run/systemd/system/rk-*.d; do
            [ -d "$d" ] && run_sudo rm -rf "$d"
        done
        run_sudo systemctl daemon-reload
        # 2b) 撤掉看门狗的维护窗口标记(留着的话看门狗会一直闭嘴)
        for f in "${PAUSE_FILES[@]}"; do
            run_sudo rm -f "$f"
        done
        run_sudo rm -f /run/rk-gate/paused 2>/dev/null
        # 3) 把之前在跑的服务重新拉起来
        for s in "${RESTORE_SERVICES[@]}"; do
            printf '    %-32s ' "$s"
            if run_sudo systemctl start "$s"; then
                sleep 1
                echo "-> $(systemctl is-active "$s" 2>/dev/null)"
            else
                echo "-> 启动失败(需要手工确认)"
            fi
        done
        # 4) 再走一遍主工程的恢复脚本(幂等, 它还负责 unmask)
        if [ -x "$REPO/rk3588_zero_copy/scripts/restore_services.sh" ]; then
            "$REPO/rk3588_zero_copy/scripts/restore_services.sh" 2>/dev/null \
                | sed 's/^/    [restore_services] /'
        fi
        echo "    负载: $(cat /proc/loadavg)"
    fi
    exit $rc
}
trap restore EXIT INT TERM

quiet_board() {
    QUIETED=1
    local verb=${1:-verbose}
    if [ "$verb" = "quiet" ]; then
        exec 3>&1 1>/dev/null   # 临时静音
    fi

    # ★★★ 第 ① 步: 先让看门狗闭嘴, 再停服务。
    #
    #   这一步的顺序是【必须的】, 踩过:
    #   看门狗(gate_watchdog.sh)的判断逻辑是看 rk-gate.service 的 Result:
    #     · Result == success  -> 认为"人为停机", 尊重运维意图, 不拉起
    #     · Result != success  -> 认为"崩了", 退避后自动拉起
    #   而我们为了清掉孤儿进程会 `pkill` rk_gate —— 那会让 systemd 把单元
    #   标成 Result=exit-code, 看门狗于是【把我们刚停掉的服务又拉起来】。
    #   实测: 压测跑到一半 rk_gate 自己回来了, Core0/Core1/Core2 全被占,
    #   那一组数据全废。
    #
    #   所以先用它自带的维护窗口开关:
    #     /run/rk-gate/paused        (门看门狗)
    #     logs/night_soak.paused     (夜间长跑看护)
    #   这两个是各看门狗自己文档里写的"维护窗口"机制。
    run_sudo touch /run/rk-gate/paused 2>/dev/null
    run_sudo touch "$REPO/logs/night_soak.paused" 2>/dev/null
    PAUSE_FILES+=("/run/rk-gate/paused" "$REPO/logs/night_soak.paused")
    echo "    看门狗已闭嘴(维护窗口标记)"

    # ★ 第 ② 步: 动态枚举【所有】rk-*.service, 而不是手写一份清单。
    #   手写清单漏掉了一个(rk-night-soak-monitor 会把别的服务拉起来),
    #   结果压测跑到一半 CPU 突然飙到 98% —— 那一组数据全废。
    local all_units
    all_units="$(systemctl list-units --type=service --all --no-legend 2>/dev/null \
                 | awk '{print $1}' | grep -E '^rk-.*\.service$' || true)"

    echo "==> 记录现场服务状态 ..."
    for s in $all_units; do
        local st
        st="$(systemctl is-active "$s" 2>/dev/null || true)"
        if [ "$st" = "active" ] || [ "$st" = "activating" ] || [ "$st" = "deactivating" ]; then
            # 去重: quiet_board 可能被反复调用(见下面的"等安静"循环)
            case " ${RESTORE_SERVICES[*]:-} " in
                *" $s "*) ;;
                *) RESTORE_SERVICES+=("$s") ;;
            esac
        fi
    done
    echo "    需要恢复: ${RESTORE_SERVICES[*]:-无}"

    echo "==> 停服务 ..."
    # ★ 把所有看门狗的 Restart=always 临时改掉, 否则 stop 之后 5 秒就爬起来
    for s in $all_units; do
        if grep -q "Restart=always" "/etc/systemd/system/$s" 2>/dev/null; then
            local drop="/run/systemd/system/$s.d"
            if run_sudo mkdir -p "$drop"; then
                run_sudo sh -c "printf '[Service]\nRestart=no\n' > '$drop/override.conf'"
            fi
        fi
    done
    run_sudo systemctl daemon-reload
    for s in $all_units; do
        run_sudo systemctl stop "$s"
    done
    sleep 2

    # ★ 第 ③ 步: 清孤儿进程(有些看门狗脚本用 setsid 起子进程,
    #   父脚本被 stop 之后子进程被 init 收养, systemctl 停不掉)。
    #   先 SIGTERM, 再 SIGKILL。
    for sig in "" "-9"; do
        run_sudo pkill $sig -f "$REPO/rk3588_zero_copy/build/rk_gate" 2>/dev/null
        run_sudo pkill $sig -f "$REPO/rk3588_zero_copy/build/rk_zero_copy" 2>/dev/null
        run_sudo pkill $sig -f "$REPO/rk3588_zero_copy/build_minimal/rk_minimal" 2>/dev/null
        run_sudo pkill $sig -f "gate_watchdog.sh" 2>/dev/null
        run_sudo pkill $sig -f "pipeline_watchdog.sh" 2>/dev/null
        run_sudo pkill $sig -f "night_soak_monitor.sh" 2>/dev/null
        sleep 2
    done

    # ---- 第 ④ 步: 核对, 必须确认真的安静了 ----
    local leftover
    leftover="$(ps -eo pid,args 2>/dev/null | grep -E "rk_gate|rk_zero_copy|rk_minimal|watchdog\.sh|night_soak" \
                | grep -v grep || true)"
    if [ -n "$leftover" ]; then
        echo "    [!] 仍然有进程在跑:"
        echo "$leftover" | sed 's/^/        /' | cut -c1-140
    else
        echo "    [OK] 流水线进程全部停了"
    fi

    if [ "$LOCK_FREQ" -eq 1 ]; then
        NPU_DEV=/sys/class/devfreq/fdab0000.npu
        # ★ 只在【第一次】调用时保存原值。
        #   quiet_board 在"等板子安静"的循环里会被反复调用, 而第二轮读到
        #   的 min_freq 已经是【我们自己锁上去的】1000000000 —— 存下来就
        #   把"原始值"覆盖了, 恢复时 NPU 就被永久锁在 1GHz。
        if [ -z "$RESTORE_FREQ" ]; then
            RESTORE_FREQ="$(cat $NPU_DEV/governor 2>/dev/null || true)"
            RESTORE_MINFREQ="$(cat $NPU_DEV/min_freq 2>/dev/null || true)"
            RESTORE_MAXFREQ="$(cat $NPU_DEV/max_freq 2>/dev/null || true)"
        fi
        if [ -n "$RESTORE_FREQ" ]; then
            # ★★ 正确的锁法是【min_freq = max_freq = 1000MHz】, 不是改 governor。
            #
            #   实测踩过的坑:
            #     · 这台板子上【没有 user_freq 节点】;
            #     · 【target_freq 是只读的】—— 写它会报 Permission denied,
            #       所以 "governor=userspace + target_freq=1GHz" 这套是【假的】:
            #       看起来锁住了, 其实根本没锁(后来实测到 NPU 实际跑在 900MHz,
            #       单帧推理 25.4ms 而非 23.0ms);
            #     · 只写 governor=performance 在这个 BSP 上也不生效。
            #   min_freq / max_freq 是可写的, 把两者都设成 1GHz 之后,
            #   不管 governor 怎么想, NPU 都下不去 —— 这才是真锁。
            #
            # ★ 不锁的后果【实测过】: 同一组配置的单帧推理耗时在
            #   23.0ms(1000MHz) 和 25.4ms(900MHz) 之间跳 ——
            #   正好是 1000/900 = 1.111 的比。这个 10% 的漂移会直接盖掉
            #   我们要测的那些差异。
            run_sudo_write 1000000000 "$NPU_DEV/max_freq"
            run_sudo_write 1000000000 "$NPU_DEV/min_freq"
            echo "    NPU governor: $RESTORE_FREQ (不改)"
            echo "    NPU min/max_freq: $RESTORE_MINFREQ/$RESTORE_MAXFREQ -> 1000000000/1000000000"
            echo "    NPU cur_freq: $(cat $NPU_DEV/cur_freq 2>/dev/null)"
        fi
    fi
    if [ "$verb" = "quiet" ]; then
        exec 1>&3 3>&-   # 恢复输出
    fi
}

# ★ 等板子安静
#
#   这块板子可能被别的会话同时用来跑压测(实测遇到过: 另一个会话在跑
#   model_perf_bench, 直接占用 NPU)。所以停完自己的服务之后, 还要
#   【确认 NPU 三核的负载真的降到 0】再开跑。
#   连续 need 次采样都是空闲才算安静(单次采样会撞上别人的启动间隙)。
wait_for_quiet() {
    local timeout=${1:-2400} need=${2:-5}
    local t0 elapsed clean=0 busy load v
    t0=$(date +%s)
    echo "==> 等板子安静 (最多 ${timeout}s, 需要连续 ${need} 次空闲采样) ..."
    while true; do
        elapsed=$(( $(date +%s) - t0 ))
        [ "$elapsed" -ge "$timeout" ] && break
        load="$(run_sudo cat /sys/kernel/debug/rknpu/load 2>/dev/null | head -1)"
        busy=0
        for v in $(echo "$load" | grep -oE "Core[0-9]+: *[0-9]+%" | grep -oE "[0-9]+%" | tr -d '%'); do
            [ "$v" -gt 3 ] && busy=1
        done
        if [ "$busy" -eq 0 ]; then
            clean=$((clean+1))
            if [ "$clean" -ge "$need" ]; then
                echo "    板子已安静 (连续 ${clean} 次空闲采样, 用时 ${elapsed}s)"
                return 0
            fi
        else
            [ "$clean" -gt 0 ] && echo "    [${elapsed}s] 又有负载了, 继续等 ..."
            clean=0
        fi
        sleep 2
    done
    echo "    [警告] 等了 ${timeout}s 板子还是不够安静 —— 可能有别的会话在跑压测"
    return 1
}

# ★ 前置检查: 板子上还有没有别的进程在抢 NPU / 吃 CPU
#
#   这不是形式主义 —— 实测过: 另一个会话在板上跑一个 RKNN 量化导出任务,
#   它【会占用 NPU】(实测 Core1/Core2 各到 86%), 于是同一个用例的
#   吞吐被压掉一大截, 而板子上的服务全停干净也看不出来。
#   所以这里显式查一遍, 并把结论写进报告。
preflight_quiet() {
    echo "==> 前置检查: 板子是否安静 ..."
    local npu_load foreign cpu_hogs
    npu_load="$(run_sudo cat /sys/kernel/debug/rknpu/load 2>/dev/null | head -1)"
    echo "    NPU load(debugfs): ${npu_load:-读不到}"

    # 除本脚本自己以外, 任何还在跑的 rk_gate / rk_zero_copy 都是抢资源的;
    # 【别的会话的压测台】更隐蔽 —— 名字可能叫 model_perf_bench 之类。
    # 所以这里扫的是"所有可能碰 NPU 的进程", 靠进程名里的关键字 + 父进程。
    foreign="$(ps -eo pid,ppid,args 2>/dev/null \
               | grep -E 'rk_gate|rk_zero_copy|rk_minimal|_bench|npu_model|model_perf' \
               | grep -v grep | grep -v "run_layer_bench.sh" || true)"
    if [ -n "$foreign" ]; then
        echo "    [!] 还有可能抢 NPU 的进程:"
        echo "$foreign" | sed 's/^/        /' | cut -c1-140
    else
        echo "    [OK] 没有生产流水线/别的压测台在跑"
    fi

    # CPU 占用最高的几个进程(排除内核线程和本脚本)
    cpu_hogs="$(ps -eo pcpu,pid,args --sort=-pcpu 2>/dev/null | head -8 | tail -7)"
    echo "    CPU 占用前几名:"
    echo "$cpu_hogs" | sed 's/^/        /'
    echo ""
}

# ---------------------------------------------------------------- 前置检查
echo "=========================================================="
echo " npu_layer_bench —— 一键分层压测"
echo "   层: $LAYERS   每组: 预热 ${WARMUP}s + 测量 ${DURATION}s, 重复 ${REPEATS} 次"
echo "   结果: $OUT_DIR"
echo "=========================================================="

if [ ! -x "$BIN" ]; then
    echo "==> 构建 $BIN"
    cmake -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release -S "$ROOT" >/dev/null || exit 1
    cmake --build "$ROOT/build" -j"$(nproc)" >/dev/null || exit 1
fi

for f in "$DET_MODEL" "$GATE_BIN"; do
    [ -e "$f" ] || { echo "[FATAL] 缺文件: $f" >&2; exit 1; }
done

if [ "$QUIET_BOARD" -eq 1 ]; then
    # ★ 循环: 停服务 -> 等安静 -> 还是不安静就再停一遍。
    #
    #   为什么需要循环: 停服务的那一刻, 别的会话(或者本机的看门狗)可能正好
    #   在收尾并把生产服务拉回来 —— 实测踩到过: 我们停完之后, 另一个会话的
    #   脚本退出, 顺手把 rk-gate.service 恢复了, NPU 又被占满,
    #   而我们已经"停过了", 于是就卡在等安静上白等 40 分钟。
    #   所以每一轮都重新确认一次服务是停的。
    QUIET_ROUNDS=${QUIET_ROUNDS:-12}
    QUIET_WAIT=${QUIET_WAIT:-180}
    for round in $(seq 1 "$QUIET_ROUNDS"); do
        if [ "$round" -eq 1 ]; then
            quiet_board
        else
            quiet_board quiet
        fi
        if wait_for_quiet "$QUIET_WAIT" 5; then
            break
        fi
        echo "==> 第 ${round}/${QUIET_ROUNDS} 轮还没安静, 重新清理并继续等 ..."
    done
    preflight_quiet
else
    echo "[警告] --no-quiet: 不停服务, 测出来的是负重数据"
fi

# ---------------------------------------------------------------- 跑
JSONL="$OUT_DIR/results.jsonl"
: > "$JSONL"

# ---- 记录环境快照(报告里要引用) ----
{
    echo "时间: $(date -Is)"
    echo "板子: $(uname -srm)"
    echo "NPU devfreq: gov=$(cat /sys/class/devfreq/fdab0000.npu/governor 2>/dev/null) "\
"target=$(cat /sys/class/devfreq/fdab0000.npu/target_freq 2>/dev/null) "\
"cur=$(cat /sys/class/devfreq/fdab0000.npu/cur_freq 2>/dev/null)"
    echo "CPU governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null) "\
"cpu0=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null)"
    echo "RKNN runtime: $(ls -la /usr/lib/librknnrt.so 2>/dev/null)"
    echo "内存: $(free -m | awk '/Mem:/{print $2" MB total, "$7" MB available"}')"
    echo "参数: duration=$DURATION warmup=$WARMUP repeats=$REPEATS layers=$LAYERS"
    echo "服务(压测期间已停): ${RESTORE_SERVICES[*]:-无}"
} > "$OUT_DIR/env.txt" 2>&1
cat "$OUT_DIR/env.txt"

echo ""
echo "==> 跑分层压测 ..."
"$BIN" \
    --layer "$LAYERS" \
    --det-model "$DET_MODEL" \
    --ocr-model "$OCR_MODEL" \
    --labels "$LABELS" \
    --ocr-dict "$OCR_DICT" \
    --gate-bin "$GATE_BIN" \
    --ch "ch1:$SRC_CH1:near" \
    --ch "ch2:$SRC_CH2:high:$IPM_720P" \
    --ch "ch3:$SRC_CH3:near" \
    --ch "ch4:$SRC_CH4:high:$IPM_4K" \
    --fullchain-max-fps 0 \
    --duration "$DURATION" \
    --warmup "$WARMUP" \
    --repeats "$REPEATS" \
    --workers "$WORKERS" \
    --res-model-640 "$RES640" \
    --res-model-576 "$RES576" \
    --res-model-512 "$RES512" \
    --out "$JSONL" \
    2>&1 | tee "$OUT_DIR/bench.log"

# ---------------------------------------------------------------- 出报告
echo ""
echo "==> 出报告 ..."
python3 "$HERE/render_report.py" "$JSONL" "$OUT_DIR/layer_bench_report.md" \
    --duration "$DURATION" --repeats "$REPEATS"
echo ""
echo "==> 产物:"
ls -la "$OUT_DIR"
