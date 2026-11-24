#!/usr/bin/env bash
# ============================================================================
# quiesce_p1.sh —— P1 专用静默态(比 opt_dfl 的 board_quiesce.sh 更严)
#
# ★ 为什么自己写一份:
#   2026-11-23 19:05 另一个会话在板端跑了 scripts/p0_deploy.sh(带 sudo), 它
#   ① 重启了 rk-gate ② 删掉了 /run/rk-gate/paused ③ daemon-reload 冲掉了
#   /run/systemd/system/*.d 的临时 drop-in —— 于是 A/B 的"板子安静"前提被打破
#   (NPU 逐核 67~76%)。opt_dfl 那版只给 Restart=always 的单元加 drop-in,
#   pkill 之后 systemd 仍会按 Restart=on-failure 把看门狗拉回来 -> 看门狗再把
#   rk-gate 拉回来。
#
# ★ 本版的加固点:
#   1) 给【所有】rk-*.service 挂 Restart=no 的 drop-in(不只是 Restart=always 的)
#   2) 停完之后【再 touch 一次】paused 标记(防止别的会话刚删掉)
#   3) 每一步都复核: 单元状态 + 残留进程 + 逐核 NPU 占用
#
# 用法: quiesce_p1.sh quiesce | resume
# ============================================================================
set -uo pipefail


# ---- 需要 root: 以 root 身份运行, 或先执行 `sudo -v` 缓存凭据(凭据不落盘) ----
if [ "$(id -u)" -ne 0 ] && ! sudo -n true 2>/dev/null; then
    echo "需要 root 权限，请使用 sudo 运行或先执行 sudo -v" >&2
    exit 1
fi
REPO=/home/seeed/rk_deploy
RUN_DIR=/run/rk-gate
PAUSE=$RUN_DIR/paused
NPU_DEV=/sys/class/devfreq/fdab0000.npu
LOCK_HZ=1000000000
FREQBAK=/tmp/p1fork9_freq.bak
SVCBAK=/tmp/p1fork9_services.txt
SUDO=(sudo)
# ★ 静默期【不停】rk-kernel-log.service: 它只把内核 ring buffer 落盘做 hard-lockup
#   取证, 不占 NPU、不参与推理, 停了反而会在静默窗口里丢证据(2026-11-23 踩过:
#   quiesce 停了它, 之后的 resume 没把它记进恢复清单 -> 至今 off)。
#   它在下面 EXEMPT_FROM_STOP 里单列, 恢复阶段会单独校验它是否 active。
NODES=(rk-gate.service rk-gate-watchdog.service rk-night-soak.service
       rk-pipeline.service rk-night-soak-monitor.service rk-pipeline-watchdog.service)
EXEMPT_FROM_STOP=(rk-kernel-log.service)

rs() { "${SUDO[@]}" "$@"; }
rs_write() { local v="$1" node="$2"; rs sh -c "printf '%s\n' '$v' > '$node'" >/dev/null 2>&1; }
npu_load() {
    rs cat /sys/kernel/debug/rknpu/load 2>/dev/null | grep -oE 'Core[0-9]+: *[0-9]+%' \
        | grep -oE '[0-9]+%' | tr -d '%' | tr '\n' ' '
}

do_quiesce() {
    echo "==> ① 记录现场服务状态(要恢复的清单)"
    : > "$SVCBAK"
    for s in "${NODES[@]}"; do
        st=$(systemctl is-active "$s" 2>/dev/null || true)
        case "$st" in
            active|activating|deactivating) grep -qxF "$s" "$SVCBAK" || echo "$s" >> "$SVCBAK" ;;
        esac
    done
    echo "    需要恢复: $(tr '\n' ' ' < "$SVCBAK")"

    echo "==> ② 看门狗闭嘴 + 给所有 rk-* 单元挂 Restart=no 兜底"
    rs touch "$PAUSE"
    for s in "${NODES[@]}"; do
        drop="/run/systemd/system/$s.d"
        rs mkdir -p "$drop" 2>/dev/null
        rs sh -c "printf '[Service]\nRestart=no\n' > '$drop/override.conf'" 2>/dev/null
    done
    rs systemctl daemon-reload 2>/dev/null

    echo "==> ③ 停所有 rk-* 单元"
    for s in "${NODES[@]}"; do rs systemctl stop "$s" 2>/dev/null; done
    sleep 2

    echo "==> ④ 清残留进程"
    for sig in "" "-9"; do
        rs pkill $sig -f "$REPO/rk3588_zero_copy/build/rk_gate" 2>/dev/null
        rs pkill $sig -f "gate_watchdog.sh" 2>/dev/null
        rs pkill $sig -f "pipeline_watchdog.sh" 2>/dev/null
        rs pkill $sig -f "night_soak_monitor.sh" 2>/dev/null
        rs pkill $sig -f "night_soak_run.sh" 2>/dev/null
        sleep 1
    done
    # ★ 再 touch 一次: 别的会话刚跑过 p0_deploy.sh, 它退出时会 rm 掉 paused
    rs touch "$PAUSE"

    echo "==> ⑤ 锁频: NPU min=max=${LOCK_HZ}"
    if [ ! -f "$FREQBAK" ]; then
        : > "$FREQBAK"
        for n in governor min_freq max_freq; do
            [ -r "$NPU_DEV/$n" ] && echo "$NPU_DEV/$n|$(cat "$NPU_DEV/$n")" >> "$FREQBAK"
        done
    fi
    rs_write "$LOCK_HZ" "$NPU_DEV/max_freq"
    rs_write "$LOCK_HZ" "$NPU_DEV/min_freq"
    echo "    NPU cur_freq=$(cat "$NPU_DEV/cur_freq" 2>/dev/null)"

    echo "==> ⑥ 等板子安静(逐核 <8%, 连续 3 次)"
    ok=0
    for i in $(seq 1 20); do
        loads="$(npu_load | tr '\n' ' ')"
        bad=0
        for v in $loads; do [ "$v" -gt 8 ] && bad=1; done
        if [ "$bad" -eq 0 ] && [ -n "$loads" ]; then
            ok=$((ok + 1)); echo "    [$i] 每核: $loads (连续干净 $ok/3)"
            [ "$ok" -ge 3 ] && break
        else
            ok=0; echo "    [$i] 每核: ${loads:-?} (仍有污染, 继续等)"
        fi
        sleep 3
    done
    echo "==> 复核"
    for s in "${NODES[@]}"; do
        printf "    %-34s %s\n" "$s" "$(systemctl is-active "$s" 2>/dev/null)"
    done
    echo "    残留进程: $(ps -eo args | grep -E '[r]k_gate|[g]ate_watchdog|[n]ight_soak' | wc -l) 个"
    echo "    paused 标记: $([ -f "$PAUSE" ] && echo 在 || echo 没了)"
    echo "    逐核 NPU: $(npu_load | tr '\n' ' ')"
    [ "$ok" -ge 3 ] && echo "==> [OK] 板子已安静" || echo "==> [!] 仍有污染"
}

do_resume() {
    echo "==> ① 还原频率"
    if [ -f "$FREQBAK" ]; then
        while IFS='|' read -r node val; do
            [ -n "$node" ] || continue
            rs_write "$val" "$node"
        done < "$FREQBAK"
        echo "    已还原 $(wc -l < "$FREQBAK") 个节点; NPU cur_freq=$(cat "$NPU_DEV/cur_freq" 2>/dev/null)"
    fi
    echo "==> ② 撤掉临时 drop-in"
    for d in /run/systemd/system/rk-*.d; do [ -d "$d" ] && rs rm -rf "$d"; done
    rs systemctl daemon-reload 2>/dev/null
    echo "==> ③ 删维护窗口标记"
    rs rm -f "$PAUSE" 2>/dev/null
    echo "==> ④ 拉起测前就在跑的服务"
    local rc=0
    if [ -f "$SVCBAK" ]; then
        while read -r s; do
            [ -n "$s" ] || continue
            rs systemctl reset-failed "$s" >/dev/null 2>&1
            printf "    %-34s " "$s"
            rs systemctl start "$s" >/dev/null 2>&1
            sleep 1
            local st; st="$(systemctl is-active "$s" 2>/dev/null)"
            echo "-> $st"
            if [ "$st" != "active" ]; then
                echo "    [红] $s 恢复失败(状态=$st) —— 请人工执行: sudo systemctl start $s" >&2
                rc=1
            fi
        done < "$SVCBAK"
    fi
    # ★ rk-kernel-log 必须单独校验: 静默期不停它, 但如果它本来就不在跑,
    #   这次 resume 要把它报出来(取证链断了必须让人知道)。
    for s in "${EXEMPT_FROM_STOP[@]}"; do
        local st; st="$(systemctl is-active "$s" 2>/dev/null)"
        printf "    %-34s -> %s (静默期不停, 仅校验)\n" "$s" "$st"
        if [ "$st" != "active" ]; then
            echo "    [红] $s 不在 active —— hard-lockup 取证链断了, 请人工执行: sudo systemctl start $s" >&2
            rc=1
        fi
    done
    if [ "$rc" -ne 0 ]; then
        echo "==> resume 完成但有单元未恢复(见上面 [红] 行), 退出码 1" >&2
        return 1
    fi
    echo "==> resume 完成(全部单元已校验 active)"
}

case "${1:-}" in
    quiesce) do_quiesce ;;
    resume)  do_resume ;;
    *) echo "用法: $0 {quiesce|resume}"; exit 2 ;;
esac
