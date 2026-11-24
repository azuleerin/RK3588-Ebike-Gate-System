#!/usr/bin/env bash
# ===========================================================================
# restore_services.sh —— 把烤机期间暂停的后台服务原样恢复
#
# ★ 为什么需要这个脚本
#   跑纯净烤机时, 板子上有三个吃资源的系统服务必须暂时停掉, 否则测出来的
#   FPS/CPU 全是"负重"数据:
#     · rk-minimal.service      RGA 压力单元, 连续跑, 常驻约 45% 单核
#                             (每 15 分钟自己重启一次)
#     · rk-gate.service         生产 4 路推理服务(常驻)
#     · rk-gate-watchdog.service 生产服务的看门狗(会把它秒级拉起)
#
#   烤机跑完必须把它们恢复, 否则生产服务就停了。
#
# 用法:
#   ./scripts/restore_services.sh            # 恢复三个服务
#   ./scripts/restore_services.sh --status   # 只看当前状态
# ===========================================================================
set -uo pipefail


# ---- 需要 root: 以 root 身份运行, 或先执行 `sudo -v` 缓存凭据(凭据不落盘) ----
if [ "$(id -u)" -ne 0 ] && ! sudo -n true 2>/dev/null; then
    echo "需要 root 权限，请使用 sudo 运行或先执行 sudo -v" >&2
    exit 1
fi
SERVICES=(rk-gate.service rk-gate-watchdog.service rk-minimal.service)

# 板子上的 sudo 约定(和 scripts/run_4ch_gate_single.sh 里采样 debugfs 一样)
SUDO="sudo"

status_only=0
[[ "${1:-}" == "--status" ]] && status_only=1

# unit 状态 -> 一行干净输出
# ★ 为什么不用 "$(systemctl is-active "$s" || echo unknown)":
#   is-active 对 inactive/failed 同样返回非零, 那个 || 分支会【追加】一行 "unknown",
#   结果打印成两行(inactive\nunknown), 看着像真出了故障。
#   这里按退出码区分: 0=active, 3=inactive/failed(照实打印), 其它=查不到这个 unit。
unit_state() {
    local out rc=0
    out="$(systemctl is-active "$1" 2>/dev/null)" || rc=$?
    case "$rc" in
        0) printf '%s' "${out:-active}" ;;
        3) printf '%s' "${out:-inactive}" ;;
        *) printf 'unknown' ;;
    esac
}

echo "==== 当前状态 ===="
for s in "${SERVICES[@]}"; do
    printf '  %-28s %s\n' "$s" "$(unit_state "$s")"
done

if [[ "$status_only" -eq 1 ]]; then
    exit 0
fi

echo
echo "==== 恢复服务 ===="
# 烤机期间如果为了压住"自动拉起"而 mask 过, 这里先解 mask
for s in "${SERVICES[@]}"; do
    if [ -L "/etc/systemd/system/$s" ]; then
        echo "  (解 mask: $s)"
        $SUDO systemctl unmask "$s" 2>/dev/null
    fi
done
$SUDO systemctl daemon-reload 2>/dev/null

for s in "${SERVICES[@]}"; do
    printf '  %-28s ' "$s"
    if $SUDO systemctl start "$s" 2>/dev/null; then
        sleep 2
        echo "-> $(systemctl is-active "$s" 2>/dev/null)"
    else
        echo "-> 启动失败(需要手工确认)"
    fi
done

echo
echo "==== 恢复后 ===="
sleep 5
for s in "${SERVICES[@]}"; do
    printf '  %-28s %s\n' "$s" "$(unit_state "$s")"
done
echo
echo "负载: $(cat /proc/loadavg)"
