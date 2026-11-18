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

SERVICES=(rk-gate.service rk-gate-watchdog.service rk-minimal.service)

# 板子上的 sudo 约定(和 scripts/run_4ch_gate_single.sh 里采样 debugfs 一样)
SUDO="sudo -S -p ''"

status_only=0
[[ "${1:-}" == "--status" ]] && status_only=1

echo "==== 当前状态 ===="
for s in "${SERVICES[@]}"; do
    printf '  %-28s %s\n' "$s" "$(systemctl is-active "$s" 2>/dev/null || echo unknown)"
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
        echo 123456 | $SUDO systemctl unmask "$s" 2>/dev/null
    fi
done
echo 123456 | $SUDO systemctl daemon-reload 2>/dev/null

for s in "${SERVICES[@]}"; do
    printf '  %-28s ' "$s"
    if echo 123456 | $SUDO systemctl start "$s" 2>/dev/null; then
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
    printf '  %-28s %s\n' "$s" "$(systemctl is-active "$s" 2>/dev/null || echo unknown)"
done
echo
echo "负载: $(cat /proc/loadavg)"
