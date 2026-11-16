#!/usr/bin/env bash
# lock_max_freq.sh —— 把 CPU / DDR / NPU 钉在最高频率(交付稳定性用)
#
# ★ 为什么需要它
#   RK3588 默认全靠 governor 自动调频:
#       CPU : ondemand          (小核最低能掉到 408MHz, 大核 1800~2304MHz)
#       DDR : dmc_ondemand      (533MHz ~ 2400MHz)
#       NPU : rknpu_ondemand    (300MHz ~ 1000MHz)
#   视频流这种"一帧接一帧、每帧间隔几毫秒"的负载, governor 看到短暂的空档
#   就会降频, 下一帧又得等升频爬坡 —— 表现出来就是 npu_ms 抖动大、
#   P99 延迟难看。客户现场最怕的就是这个。
#
#   钉死在最高频率之后:
#     · FPS 更稳(不再有升频爬坡的台阶)
#     · npu_ms 的抖动变小(P99 改善)
#     · 代价是功耗和发热上升 —— 嵌入式长期跑要考虑散热
#
# ★ 需要 root
#   写 /sys/class/devfreq/*/governor 和 /sys/devices/system/cpu/cpufreq/*/
#   scaling_governor 都是 root 权限。
#   用法:
#       sudo ./scripts/lock_max_freq.sh              # 锁定
#       sudo ./scripts/lock_max_freq.sh --status     # 只看当前状态(不需要 root)
#       sudo ./scripts/lock_max_freq.sh --restore    # 还原成原来的 governor
#
# ★ 无 tty 的场景(SSH 自动化)
#   本机的 sudo 需要密码, 没有免密规则。用 SUDO_PASSWORD 环境变量把密码
#   喂给 `sudo -S`(密码只走 stdin, 不落盘、不进进程参数):
#       SUDO_PASSWORD='xxx' ./scripts/lock_max_freq.sh
#   (约定跟 scripts/longrun.sh 的 run_sudo() 一致)
#
# ★ 幂等: 重复执行没问题。--restore 会还原到【脚本第一次运行时记录的】状态,
#   记录在 /tmp/lock_max_freq.bak, 脚本自己会重建。
set -u

BAK="${BAK:-/tmp/lock_max_freq.bak}"
# 目录可以覆盖 —— 自测时指向假的 sysfs 树(不需要 root)
DEV="${DEV:-/sys/class/devfreq}"
CPUF="${CPUF:-/sys/devices/system/cpu/cpufreq}"

# ---------------------------------------------------------------- sudo 封装
SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    if [ -n "${SUDO_PASSWORD:-}" ]; then
        SUDO="sudo -S -p ''"
    elif command -v sudo >/dev/null 2>&1; then
        SUDO="sudo"          # 没有密码时交互式提示(在 tty 里可用)
    else
        echo "需要 root, 但找不到 sudo" >&2
        exit 1
    fi
fi

run_sudo() {
    # NO_SUDO=1: 直接执行(自测用, 在假的 sysfs 树上跑, 不需要 root)
    if [ "${NO_SUDO:-0}" = "1" ]; then
        "$@"
    elif [ -n "${SUDO_PASSWORD:-}" ] && [ "$(id -u)" -ne 0 ]; then
        echo "$SUDO_PASSWORD" | $SUDO "$@"
    else
        $SUDO "$@"
    fi
}

# ---------------------------------------------------------------- 工具函数
# 读某个【目录】下 governor 节点支持的 governor 列表
# ★ cpufreq 的文件名是 scaling_available_governors, devfreq 的是 available_governors ——
#   两个都得查, 只查一个会漏掉一半的节点(ARMbian 的 cpufreq 就没有 available_governors)。
#   ★ 参数是【目录】, 不是 governor 文件本身 —— 写成文件会去找
#     "<governor文件>/scaling_available_governors", 永远读不到, 于是误判成"不支持"。
gov_available() {
    local dir="$1"
    if [ -r "$dir/scaling_available_governors" ]; then
        cat "$dir/scaling_available_governors"
    elif [ -r "$dir/available_governors" ]; then
        cat "$dir/available_governors"
    fi
}

# 设 governor; 节点只读或者没有 performance 就跳过(不算失败)
set_gov() {
    local node="$1" want="$2"
    [ -e "$node" ] || return 0
    local avail dir
    dir="$(dirname "$node")"
    avail="$(gov_available "$dir")"
    case " $avail " in
        *" $want "*) ;;
        *) echo "    跳过 $(basename "$dir"): 不支持 '$want' (可用: $avail)"; return 0;;
    esac
    # 先备份
    if [ ! -f "$BAK" ] || ! grep -qF "$node|" "$BAK" 2>/dev/null; then
        local cur
        cur="$(cat "$node" 2>/dev/null || echo "")"
        [ -n "$cur" ] && echo "$node|$cur" >> "$BAK"
    fi
    if run_sudo sh -c "echo '$want' > '$node'" 2>/dev/null; then
        echo "    ✓ $(basename "$dir")/$(basename "$node") = $(cat "$node" 2>/dev/null)"
    else
        echo "    ✗ $node 写失败(权限?)"
    fi
}

# 设 scaling_min_freq == scaling_max_freq(某些平台没有 performance governor 时用)
lock_minmax() {
    local policy="$1"
    local mn mx
    mn="$(cat "$policy/scaling_min_freq" 2>/dev/null)"
    mx="$(cat "$policy/scaling_max_freq" 2>/dev/null)"
    [ -n "$mn" ] && [ -n "$mx" ] || return 0
    if [ ! -f "$BAK" ] || ! grep -qF "$policy/scaling_min_freq" "$BAK" 2>/dev/null; then
        echo "$policy/scaling_min_freq|$mn" >> "$BAK"
        echo "$policy/scaling_max_freq|$mx" >> "$BAK"
    fi
    run_sudo sh -c "echo '$mx' > '$policy/scaling_min_freq'" 2>/dev/null \
        && echo "    ✓ $(basename "$policy")/scaling_min_freq = $mx (= max)" \
        || echo "    ✗ $(basename "$policy")/scaling_min_freq 写失败"
}

# ---------------------------------------------------------------- 状态输出
show_status() {
    echo "== CPU (cpufreq) =="
    for p in "$CPUF"/policy*; do
        [ -d "$p" ] || continue
        local gov cur mn mx cpus
        gov="$(cat "$p/scaling_governor" 2>/dev/null)"
        cur="$(cat "$p/scaling_cur_freq" 2>/dev/null)"
        mn="$(cat "$p/scaling_min_freq" 2>/dev/null)"
        mx="$(cat "$p/scaling_max_freq" 2>/dev/null)"
        cpus="$(cat "$p/affected_cpus" 2>/dev/null)"
        echo "  $(basename "$p"): cpus=[$cpus] gov=$gov cur=${cur}Hz min=$mn max=$mx"
    done
    echo
    echo "== devfreq =="
    for d in "$DEV"/*/; do
        [ -d "$d" ] || continue
        local n gov cur mn mx load
        n="$(cat "$d/name" 2>/dev/null || basename "$d")"
        gov="$(cat "$d/governor" 2>/dev/null)"
        cur="$(cat "$d/cur_freq" 2>/dev/null)"
        mn="$(cat "$d/min_freq" 2>/dev/null)"
        mx="$(cat "$d/max_freq" 2>/dev/null)"
        load="$(cat "$d/load" 2>/dev/null | head -1)"
        echo "  $n: gov=$gov cur=${cur}Hz min=$mn max=$mx load=$load"
    done
}

# ---------------------------------------------------------------- 自测
# 在【假的 sysfs 树】上跑一遍完整的 上锁 -> 校验 -> 还原 -> 校验
# 目的: 节点名/文件名/备份/还原的逻辑要是错了, 拿着 root 上真板子才发现就晚了。
# 用法: ./scripts/lock_max_freq.sh --self-test      (不需要 root)
self_test() {
    local fake=/tmp/lock_max_freq_selftest
    rm -rf "$fake"
    mkdir -p "$fake/devfreq/dmc" "$fake/devfreq/fdab0000.npu" \
             "$fake/cpufreq/policy0" "$fake/cpufreq/policy4" "$fake/cpufreq/policy6"

    for p in "$fake"/cpufreq/policy*; do
        # ★ ARMbian 的 cpufreq 用 scaling_available_governors, 没有 available_governors
        echo "conservative ondemand userspace powersave performance schedutil" \
            > "$p/scaling_available_governors"
        echo "ondemand" > "$p/scaling_governor"
        echo "408000"  > "$p/scaling_min_freq"
        echo "2256000" > "$p/scaling_max_freq"
        echo "1200000" > "$p/scaling_cur_freq"
        echo "0-3"      > "$p/affected_cpus"
    done
    for d in "$fake"/devfreq/*/; do
        echo "rknpu_ondemand dmc_ondemand userspace powersave performance simple_ondemand" \
            > "$d/available_governors"
        echo "rknpu_ondemand" > "$d/governor"
        echo "300000000"  > "$d/min_freq"
        echo "1000000000" > "$d/max_freq"
        echo "1000000000" > "$d/cur_freq"
    done
    echo "dmc"           > "$fake/devfreq/dmc/name"
    echo "fdab0000.npu"  > "$fake/devfreq/fdab0000.npu/name"

    local env_pfx="DEV=$fake/devfreq CPUF=$fake/cpufreq BAK=$fake/bak NO_SUDO=1"

    echo "== 自测: 上锁 =="
    env $env_pfx "$0" --lock > "$fake/lock.log" 2>&1 || { echo "自测: 上锁失败"; cat "$fake/lock.log"; return 1; }
    sed 's/^/  /' "$fake/lock.log"

    local rc=0
    for p in "$fake"/cpufreq/policy*; do
        local g mn mx
        g="$(cat "$p/scaling_governor")"; mn="$(cat "$p/scaling_min_freq")"; mx="$(cat "$p/scaling_max_freq")"
        if [ "$g" = "performance" ] && [ "$mn" = "$mx" ]; then
            echo "  ✓ $(basename "$p"): governor=$g min=max=$mx"
        else
            echo "  ✗ $(basename "$p"): governor=$g min=$mn max=$mx"; rc=1
        fi
    done
    for d in "$fake"/devfreq/*/; do
        local g mn mx
        g="$(cat "$d/governor")"; mn="$(cat "$d/min_freq")"; mx="$(cat "$d/max_freq")"
        if [ "$g" = "performance" ] && [ "$mn" = "$mx" ]; then
            echo "  ✓ $(cat "$d/name"): governor=$g min=max=$mx"
        else
            echo "  ✗ $(cat "$d/name"): governor=$g min=$mn max=$mx"; rc=1
        fi
    done

    echo "== 自测: 还原 =="
    env $env_pfx "$0" --restore > "$fake/restore.log" 2>&1 || { echo "自测: 还原失败"; cat "$fake/restore.log"; return 1; }
    for p in "$fake"/cpufreq/policy*; do
        local g mn
        g="$(cat "$p/scaling_governor")"; mn="$(cat "$p/scaling_min_freq")"
        if [ "$g" = "ondemand" ] && [ "$mn" = "408000" ]; then
            echo "  ✓ $(basename "$p"): 已还原 governor=$g min=$mn"
        else
            echo "  ✗ $(basename "$p"): 还原后 governor=$g min=$mn"; rc=1
        fi
    done
    for d in "$fake"/devfreq/*/; do
        local g mn
        g="$(cat "$d/governor")"; mn="$(cat "$d/min_freq")"
        if [ "$g" = "rknpu_ondemand" ] && [ "$mn" = "300000000" ]; then
            echo "  ✓ $(cat "$d/name"): 已还原 governor=$g min=$mn"
        else
            echo "  ✗ $(cat "$d/name"): 还原后 governor=$g min=$mn"; rc=1
        fi
    done

    if [ "$rc" = "0" ]; then
        echo "自测通过: 上锁/还原逻辑都对"
    else
        echo "自测失败"
    fi
    return $rc
}

# ---------------------------------------------------------------- 主流程
MODE="${1:-lock}"
case "$MODE" in
--self-test|selftest)
    self_test
    exit $?
    ;;
--status|status)
    show_status
    exit 0
    ;;
--restore|restore)
    echo "== 还原 governor =="
    if [ ! -f "$BAK" ]; then
        echo "没有备份文件 $BAK, 无需还原(或直接用 --status 看当前值)"
        exit 0
    fi
    while IFS='|' read -r node val; do
        [ -n "$node" ] || continue
        if run_sudo sh -c "echo '$val' > '$node'" 2>/dev/null; then
            echo "    ✓ $node = $val"
        else
            echo "    ✗ $node 还原失败"
        fi
    done < "$BAK"
    rm -f "$BAK"
    echo "已还原"
    show_status
    exit 0
    ;;
--lock|lock|"") ;;
*)
    echo "用法: $0 [--lock|--restore|--status]" >&2
    exit 2
    ;;
esac

echo "== 锁定到最高频率 (需要 root) =="

# ---- CPU: 用 performance governor; 没有就退化成 min=max ----
echo "-- CPU --"
for p in "$CPUF"/policy*; do
    [ -d "$p" ] || continue
    echo "  policy: $(basename "$p")  cpus=[$(cat "$p/affected_cpus" 2>/dev/null)]"
    avail="$(gov_available "$p")"
    case " $avail " in
        *" performance "*) set_gov "$p/scaling_governor" performance ;;
        *)
            echo "    (没有 performance governor, 改用 min=max)"
            ;;
    esac
    # 双保险: 即使 governor 没真生效, 也把 min 顶到 max —— 频率就钉死了
    lock_minmax "$p"
done

# ---- devfreq: DDR(dmc) / NPU / GPU ----
echo "-- devfreq --"
for d in "$DEV"/*/; do
    [ -d "$d" ] || continue
    n="$(cat "$d/name" 2>/dev/null || basename "$d")"
    case "$n" in
        dmc|npu|fdab0000.npu|fb000000.gpu-panthor|gpu) ;;
        *) continue ;;
    esac
    echo "  $n:"
    set_gov "$d/governor" performance
    # 有些节点没有 performance governor, 直接把 min 顶到 max
    mn="$(cat "$d/min_freq" 2>/dev/null)"
    mx="$(cat "$d/max_freq" 2>/dev/null)"
    if [ -n "$mn" ] && [ -n "$mx" ] && [ "$mn" != "$mx" ]; then
        if [ ! -f "$BAK" ] || ! grep -qF "$d/min_freq" "$BAK" 2>/dev/null; then
            echo "$d/min_freq|$mn" >> "$BAK"
            echo "$d/max_freq|$mx" >> "$BAK"
        fi
        run_sudo sh -c "echo '$mx' > '$d/min_freq'" 2>/dev/null \
            && echo "    ✓ $n/min_freq = $mx" \
            || echo "    ✗ $n/min_freq 写失败"
    fi
done

echo
show_status
echo
echo "提示: 还原用  sudo $0 --restore"
