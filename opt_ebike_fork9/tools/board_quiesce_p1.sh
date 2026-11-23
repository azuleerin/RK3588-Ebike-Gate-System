#!/usr/bin/env bash
# ============================================================================
# board_quiesce.sh —— 板端 A/B 的「让板子安静 + 锁频」与「恢复现场」
#
# 用法:
#   board_quiesce.sh quiesce   # 停所有 NPU 消费者 + 锁频 min=max=1GHz + 等空闲
#   board_quiesce.sh resume    # 还原频率 + 拉起服务 + 确认 4 路出帧
#
# ★ 纪律来源: docs/agent_prompt_three_directions.md 附录 + opt_audit 的既有实践
#   1) 停服务顺序不能错: 先 touch /run/rk-gate/paused 让看门狗闭嘴, 再 systemctl stop
#      (直接 pkill 会让 systemd 标成 Result=exit-code, 看门狗会把服务再拉起来)
#   2) 锁频用 min_freq = max_freq = 1GHz; 不碰只读的 target_freq, 也不只改 governor
#      (实测: 不锁的话单帧耗时在 23.0ms/25.4ms 之间跳, 10% 漂移会盖掉要测的差异)
#   3) 判空闲用 /sys/kernel/debug/rknpu/load 逐核 (devfreq load 恒显 100%, 没用)
#   4) 污染判据: 无上下文的核 >8% 作废重跑
#   5) 测完必恢复: 还原频率 -> 删 paused 标记 -> 拉起服务 -> 确认 4 路出帧
# ============================================================================
set -uo pipefail


# ---- 需要 root: 以 root 身份运行, 或先执行 `sudo -v` 缓存凭据(凭据不落盘) ----
if [ "$(id -u)" -ne 0 ] && ! sudo -n true 2>/dev/null; then
    echo "需要 root 权限，请使用 sudo 运行或先执行 sudo -v" >&2
    exit 1
fi
REPO=/home/seeed/rk_deploy
PROJ=$REPO/rk3588_zero_copy
STATE=/tmp/p1fork9_quiesce.state
FREQBAK=/tmp/p1fork9_freq.bak
SVCBAK=/tmp/p1fork9_services.txt
NPU_DEV=/sys/class/devfreq/fdab0000.npu
RKNPU_LOAD=/sys/kernel/debug/rknpu/load
LOCK_HZ=1000000000

SUDO=(sudo)

rs() { "${SUDO[@]}" "$@"; }
# 写 sysfs: 必须把数据拼进 sh -c 的命令串里
rs_write() { local v="$1" node="$2"; rs sh -c "printf '%s\n' '$v' > '$node'" >/dev/null 2>&1; }

npu_load() {
    rs cat "$RKNPU_LOAD" 2>/dev/null | grep -oE 'Core[0-9]+: *[0-9]+%' \
        | grep -oE '[0-9]+%' | tr -d '%'
}

# ---------------------------------------------------------------- quiesce
do_quiesce() {
    echo "==> ① 记录现场服务状态"
    # ★ 清单必须合并: 重复跑 quiesce 时, 上一轮已经把服务停了,
    #   这轮再截断就只剩"上一轮停掉的", 恢复时会漏掉原本在跑的服务。
    touch "$SVCBAK"
    local all_units
    # ★ 必须加 --plain: 否则失败/activating 的单元前面带 "●" 符号,
    #   awk '{print $1}' 取到的是 "●" 而不是单元名 -> 该单元被整个漏掉
    #   (实测踩到: rk-pipeline 在 auto-restart, 漏掉它 = 压测全程有污染)
    all_units="$(systemctl list-units --type=service --all --no-legend --plain 2>/dev/null \
                 | awk '{print $1}' | grep -E '^rk-.*\.service$' || true)"
    local s st
    for s in $all_units; do
        st="$(systemctl is-active "$s" 2>/dev/null || true)"
        case "$st" in
            active|activating|deactivating)
                grep -qxF "$s" "$SVCBAK" || echo "$s" >> "$SVCBAK" ;;
        esac
    done
    echo "    需要恢复: $(tr '\n' ' ' < "$SVCBAK")"

    echo "==> ② 看门狗闭嘴 (维护窗口标记)"
    rs touch /run/rk-gate/paused 2>/dev/null
    rs touch "$REPO/logs/night_soak.paused" 2>/dev/null

    echo "==> ③ 临时把 Restart=always 改成 no (否则 stop 后 5 秒就爬起来)"
    for s in $all_units; do
        if grep -q "Restart=always" "/etc/systemd/system/$s" 2>/dev/null; then
            local drop="/run/systemd/system/$s.d"
            rs mkdir -p "$drop" 2>/dev/null
            rs sh -c "printf '[Service]\nRestart=no\n' > '$drop/override.conf'" 2>/dev/null
        fi
    done
    rs systemctl daemon-reload 2>/dev/null
    for s in $all_units; do rs systemctl stop "$s" 2>/dev/null; done
    sleep 2

    echo "==> ④ 清孤儿进程"
    for sig in "" "-9"; do
        rs pkill $sig -f "$PROJ/build/rk_gate" 2>/dev/null
        rs pkill $sig -f "$PROJ/build/rk_zero_copy" 2>/dev/null
        rs pkill $sig -f "$PROJ/build_minimal/rk_minimal" 2>/dev/null
        rs pkill $sig -f "gate_watchdog.sh" 2>/dev/null
        rs pkill $sig -f "pipeline_watchdog.sh" 2>/dev/null
        rs pkill $sig -f "night_soak_monitor.sh" 2>/dev/null
        sleep 1
    done

    echo "==> ⑤ 锁频: NPU min=max=${LOCK_HZ} / CPU min=max=最高频(本板无 1GHz 档)"
    # ★ 备份只在【第一次】写: 重复跑 quiesce 时, 读到的已经是我们自己锁上去的 1GHz,
    #   存下来就等于把"原始值"覆盖掉, 恢复时频率被永久锁死。
    # ★ 本板 CPU 没有 cpufreq/policy* 子目录, 节点直接在 cpuN/cpufreq/ 下
    #   (写死 policy* 会一个都匹配不到 = 频率根本没锁, 白测)
    if [ ! -f "$FREQBAK" ]; then : > "$FREQBAK"; fi
    for p in /sys/devices/system/cpu/cpu*/cpufreq; do
        [ -d "$p" ] || continue
        for n in scaling_min_freq scaling_max_freq; do
            [ -r "$p/$n" ] || continue
            grep -qF "$p/$n|" "$FREQBAK" || echo "$p/$n|$(cat "$p/$n")" >> "$FREQBAK"
        done
    done
    for n in governor min_freq max_freq; do
        [ -r "$NPU_DEV/$n" ] || continue
        grep -qF "$NPU_DEV/$n|" "$FREQBAK" || echo "$NPU_DEV/$n|$(cat "$NPU_DEV/$n")" >> "$FREQBAK"
    done
    # ★ CPU 锁到【最高频】, 不是 1GHz:
    #   本板 CPU 可用频率表 = 408/600/816/1008/1200/1416/1608/1800 MHz,
    #   1GHz【不在表里】, 写进去会被驱动夹到最近的可用值(=max), 等于没锁成 1GHz。
    #   这里改用本仓库既有 A/B 的口径 (scripts/lock_max_freq.sh: CPU/DDR/NPU 锁最高频),
    #   保证跟 opt_audit / npu_layer_bench 那几轮报告同口径可比。
    #   NPU 那边 1GHz 是可用频率, 严格按纪律锁 1GHz。
    for p in /sys/devices/system/cpu/cpu*/cpufreq; do
        [ -d "$p" ] || continue
        local cmax
        cmax="$(cat "$p/cpuinfo_max_freq" 2>/dev/null)"
        [ -n "$cmax" ] || cmax="$(cat "$p/scaling_max_freq" 2>/dev/null)"
        [ -n "$cmax" ] || continue
        rs_write "$cmax" "$p/scaling_min_freq"
        rs_write "$cmax" "$p/scaling_max_freq"
    done
    # NPU: 只改可写的 min/max, 不碰只读的 target_freq
    rs_write "$LOCK_HZ" "$NPU_DEV/max_freq"
    rs_write "$LOCK_HZ" "$NPU_DEV/min_freq"
    echo "    NPU cur_freq=$(cat "$NPU_DEV/cur_freq" 2>/dev/null)"

    echo "==> ⑥ 等板子安静 (逐核 <8%, 连续 3 次)"
    local ok=0 i loads
    for i in $(seq 1 20); do
        loads="$(npu_load | tr '\n' ' ')"
        if [ -n "$loads" ]; then
            local bad=0 v
            for v in $loads; do [ "$v" -gt 8 ] && bad=1; done
            if [ "$bad" -eq 0 ]; then
                ok=$((ok + 1))
                echo "    [$i] 每核占用: $loads  (连续干净 $ok/3)"
                [ "$ok" -ge 3 ] && break
            else
                ok=0
                echo "    [$i] 每核占用: $loads  (仍有污染, 继续等)"
            fi
        fi
        sleep 3
    done
    [ "$ok" -ge 3 ] && echo "    [OK] 板子已安静" || echo "    [!] 仍有污染, 继续前请人工确认"

    leftover="$(ps -eo pid,args 2>/dev/null | grep -E "rk_gate|rk_zero_copy|rk_minimal|watchdog\.sh|night_soak" | grep -v grep || true)"
    if [ -n "$leftover" ]; then
        echo "    [!] 仍有进程:"
        echo "$leftover" | sed 's/^/        /' | cut -c1-140
    else
        echo "    [OK] 流水线进程全停"
    fi
    date '+%F %T' > "$STATE"
    echo "==> quiesce 完成"
}

# ---------------------------------------------------------------- resume
do_resume() {
    echo "==> ① 还原频率"
    if [ -f "$FREQBAK" ]; then
        while IFS='|' read -r node val; do
            [ -n "$node" ] || continue
            case "$node" in
                */cpufreq/scaling_min_freq)
                    local d="${node%/scaling_min_freq}"
                    local lo; lo="$(cat "$d/cpuinfo_min_freq" 2>/dev/null)"
                    [ -n "$lo" ] && val="$lo" ;;
            esac
            rs_write "$val" "$node"
        done < "$FREQBAK"
        echo "    已还原 $(wc -l < "$FREQBAK") 个节点"
        echo "    NPU cur_freq=$(cat "$NPU_DEV/cur_freq" 2>/dev/null)"
    fi

    echo "==> ② 撤掉临时 drop-in (否则 Restart=no 会留着, 服务再也拉不起来)"
    for d in /run/systemd/system/rk-*.d; do
        [ -d "$d" ] && rs rm -rf "$d"
    done
    rs systemctl daemon-reload 2>/dev/null

    echo "==> ③ 删维护窗口标记"
    rs rm -f /run/rk-gate/paused 2>/dev/null
    rs rm -f "$REPO/logs/night_soak.paused" 2>/dev/null

    echo "==> ④ 拉起测前就在跑的服务"
    if [ -f "$SVCBAK" ]; then
        while read -r s; do
            [ -n "$s" ] || continue
            printf '    %-32s ' "$s"
            rs systemctl start "$s" >/dev/null 2>&1
            sleep 1
            echo "-> $(systemctl is-active "$s" 2>/dev/null)"
        done < "$SVCBAK"
    fi
    echo "==> resume 完成 (请自行确认 4 路出帧)"
}

case "${1:-}" in
    quiesce) do_quiesce ;;
    resume)  do_resume ;;
    *) echo "用法: $0 {quiesce|resume}"; exit 2 ;;
esac
