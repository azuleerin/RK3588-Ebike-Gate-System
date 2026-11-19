#!/usr/bin/env bash
# ============================================================================
# gate_health.sh —— 一条命令看清"现在到底是不是卡死了"
#
# 用法:
#   sudo scripts/gate_health.sh
#
# 现场排障时不用去猜、不用翻 journal —— 跑这一条, 直接给你结论:
#   [正常] / [可疑] / [卡死]
#
# 判据(与看门狗同源, 所以它报卡死看门狗就会动手):
#   1) 服务在不在跑
#   2) 日志还在不在长(mtime 距今多久)
#   3) 每一路的帧号还在不在推进(距今多久没动)
#   4) 内存 / 活堆水位
#   5) 最近有没有告警
#
# ★ 只读, 不改任何东西 —— 诊断完不会留下副作用。
# ============================================================================
set -u

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
ENV_FILE="$PROJ/config/gate.env"
[ -r "$ENV_FILE" ] && . "$ENV_FILE"

UNIT=rk-gate.service
LOG_DIR="${GATE_LOG_DIR:-/var/log}"
ERR_LOG="$LOG_DIR/rk-gate-error.log"
OUT_LOG="$LOG_DIR/rk-gate.log"
ALERT="$LOG_DIR/rk-gate-alerts.log"
RUN_DIR=/run/rk-gate

GREEN=$'\033[32m'; YELLOW=$'\033[33m'; RED=$'\033[31m'; DIM=$'\033[2m'; OFF=$'\033[0m'
now=$(date +%s)

echo "================================================================"
echo " rk_gate 现场健康检查    $(date '+%F %T')"
echo "================================================================"

verdict="正常"; vcolor="$GREEN"

# ---- 1) 服务状态 ----
active=$(systemctl is-active "$UNIT" 2>&1)
pid=$(systemctl show -p MainPID --value "$UNIT" 2>/dev/null)
case "$pid" in ''|0) pid="" ;; esac
printf " 服务        : %-10s  MainPID=%s\n" "$active" "${pid:-无}"
if [ "$active" != "active" ]; then
    verdict="卡死"; vcolor="$RED"
    echo "              ${RED}服务没在跑 —— 看门狗会带退避自动拉起${OFF}"
fi

# ---- 2) 日志心跳 ----
mt=0
for f in "$ERR_LOG" "$OUT_LOG"; do
    [ -f "$f" ] && m=$(stat -c %Y "$f" 2>/dev/null || echo 0) && [ "$m" -gt "$mt" ] && mt=$m
done
log_age=$((now - mt))
printf " 日志心跳    : %s 秒前还在写 (%s)\n" "$log_age" "$ERR_LOG"
if [ "$log_age" -gt 300 ]; then
    verdict="卡死"; vcolor="$RED"
elif [ "$log_age" -gt 90 ]; then
    [ "$verdict" = "正常" ] && { verdict="可疑"; vcolor="$YELLOW"; }
fi

# ---- 3) 各路帧号 ----
echo " 各路帧号    :"
declare -A FRAME=()
while read -r _ch _fr; do
    [ -n "${_ch:-}" ] && FRAME[$_ch]=$_fr
done < <(
    { tail -c 200000 "$OUT_LOG" 2>/dev/null
      tail -c 200000 "$ERR_LOG" 2>/dev/null; } \
      | grep -aoE '\[ch[0-9]+\]\[frame [0-9]+\]' \
      | sed -E 's/\[ch([0-9]+)\]\[frame ([0-9]+)\]/\1 \2/' \
      | awk '{ m[$1] = $2+0 } END { for (c in m) print c, m[c] }'
)
if [ "${#FRAME[@]}" -eq 0 ]; then
    echo "              ${DIM}(日志里还没有帧统计行 —— 服务可能刚起来, 或日志被清过)${OFF}"
elif [ "$active" != "active" ]; then
    echo "              ${DIM}(服务没在跑, 帧号是历史值, 无参考意义)${OFF}"
    for _ch in $(echo "${!FRAME[@]}" | tr ' ' '\n' | sort -n | tr '\n' ' '); do
        printf "              ch%-3s frame=%-8s ${DIM}(历史)${OFF}\n" "$_ch" "${FRAME[$_ch]}"
    done
else
    for _ch in $(echo "${!FRAME[@]}" | tr ' ' '\n' | sort -n | tr '\n' ' '); do
        _fr="${FRAME[$_ch]}"
        _lf=$(cat "$RUN_DIR/ch${_ch}_frame" 2>/dev/null || echo 0)
        _lt=$(cat "$RUN_DIR/ch${_ch}_ts"  2>/dev/null || echo 0)
        if [ "$_fr" != "$_lf" ]; then
            printf "              ch%-3s frame=%-8s ${GREEN}推进中${OFF}\n" "$_ch" "$_fr"
        else
            _stuck=$((now - _lt))
            if [ "$_stuck" -gt 180 ]; then
                printf "              ch%-3s frame=%-8s ${RED}停滞 %s 秒${OFF}\n" "$_ch" "$_fr" "$_stuck"
                verdict="卡死"; vcolor="$RED"
            else
                printf "              ch%-3s frame=%-8s ${YELLOW}停滞 %s 秒${OFF}\n" "$_ch" "$_fr" "$_stuck"
                [ "$verdict" = "正常" ] && { verdict="可疑"; vcolor="$YELLOW"; }
            fi
        fi
    done
fi

# ---- 4) 内存水位 ----
if [ -n "$pid" ] && [ -r "/proc/$pid/status" ]; then
    rss=$(awk '/^VmRSS:/{print $2}' "/proc/$pid/status" 2>/dev/null)
    [ -n "${rss:-}" ] && printf " RSS         : %s MB\n" "$((rss / 1024))"
fi
heap=$( { tail -c 200000 "$OUT_LOG" 2>/dev/null
          tail -c 200000 "$ERR_LOG" 2>/dev/null; } \
        | grep -aoE 'heap=[0-9]+KB' | tail -1 | cut -d= -f2 | tr -d 'KB')
[ -n "${heap:-}" ] && printf " 活堆       : %s MB\n" "$((heap / 1024))"
free_kb=$(df -Pk "$LOG_DIR" 2>/dev/null | awk 'NR==2{print $4}')
[ -n "${free_kb:-}" ] && printf " 日志盘剩余 : %s MB (%s)\n" "$((free_kb / 1024))" "$LOG_DIR"

# ---- 5) 最近告警 ----
if [ -f "$ALERT" ] && [ -s "$ALERT" ]; then
    echo " 最近告警    :"
    tail -3 "$ALERT" | sed 's/^/              /'
fi

# ---- 结论 ----
echo "----------------------------------------------------------------"
printf " 结论        : ${vcolor}%s${OFF}\n" "$verdict"
case "$verdict" in
    正常) echo "              ${DIM}无需处理${OFF}" ;;
    可疑) echo "              ${YELLOW}再观察一轮; 若持续, 手动 restart:${OFF}"
          echo "              sudo systemctl restart rk-gate" ;;
    卡死) echo "              ${RED}手动拉起:${OFF}"
          echo "              sudo systemctl restart rk-gate"
          echo "              ${DIM}若反复卡死, 看 /var/log/rk-gate-alerts.log 找规律${OFF}" ;;
esac
echo "================================================================"
