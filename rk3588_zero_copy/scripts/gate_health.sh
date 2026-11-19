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
# ★ 只读诊断, 但【唯一】的例外是"手动停止告警"要写一个去重标记
#   (/run/rk-gate/stop_alerted), 免得每次巡检都发一遍同样的告警。
#   诊断本身不改动任何服务状态。
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
STOP_FLAG="$RUN_DIR/stop_alerted"
# ★ 维护标记: 两个路径都认 —— 本项目用 /run/rk-gate/paused,
#   现场也可能在用 /run/maintenance.lock, 有任一存在即认为"有人在维护"。
MAINT_MARKS=("$RUN_DIR/paused" "/run/maintenance.lock")

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

# ---- 6) 手动停止告警 (Level 2) ----
#   ★ 背景: 看门狗【不会】把被人为 systemctl stop 的服务拉起来
#     (人为 stop -> Result=success -> 判定为"运维有意停的", 尊重意图)。
#     所以"有人误 stop 后忘了 start"这个盲区, 由本脚本兜底:
#     发现服务不在跑、又没有维护标记 -> 往 MQTT 发一条 Level 2 告警,
#     让系统【叫一声】, 而不是偷偷把服务拉起来。
#   ★ 去重: 只在"从有到无"的那一刻发一次, 靠 $STOP_FLAG 记住;
#     服务恢复后清掉标记, 下次再停会重新告警。
_has_maint=0
for m in "${MAINT_MARKS[@]}"; do
    [ -e "$m" ] && { _has_maint=1; break; }
done

if [ "$active" = "active" ]; then
    # 恢复正常 -> 清掉"已告警过"的标记
    rm -f "$STOP_FLAG" 2>/dev/null || true
elif [ "$_has_maint" = "1" ]; then
    # 有人在维护 -> 不告警(而且清掉标记, 维护结束后若仍停着会再叫一次)
    rm -f "$STOP_FLAG" 2>/dev/null || true
    echo " 维护标记    : 存在 ($(printf '%s ' "${MAINT_MARKS[@]}")) -> 不发停止告警"
elif [ ! -f "$STOP_FLAG" ]; then
    echo " 停止告警    : 服务未被运行且无维护标记 -> 发 Level 2 告警"
    mkdir -p "$RUN_DIR" 2>/dev/null || true
    _alarm_msg="rk_gate 服务当前处于 ${active} 状态, 且没有检测到维护标记(${MAINT_MARKS[*]})。请确认是否为预期操作。"
    # 先落本地日志(不依赖 broker 就能留证据)
    printf '[%s] 🔴 [现场巡检] %s\n' "$(date '+%F %T')" "$_alarm_msg" >> "$ALERT" 2>/dev/null || true
    if [ -n "${GATE_MQTT_HOST:-}" ]; then
        if python3 "$PROJ/scripts/mqtt_alarm.py" \
                --host "$GATE_MQTT_HOST" \
                --port "${GATE_MQTT_PORT:-1883}" \
                --sn "${GATE_SN:-rk3588-01}" \
                --level 2 \
                --title "系统已被手动停止, 请确认是否为预期操作" \
                --msg "$_alarm_msg" \
                --extra "service=rk-gate.service,active_state=${active}" ; then
            touch "$STOP_FLAG" 2>/dev/null || true
        else
            echo "              ${YELLOW}MQTT 告警发送失败(本地已留档)${OFF}"
            touch "$STOP_FLAG" 2>/dev/null || true
        fi
    else
        echo "              ${YELLOW}未配置 MQTT broker(GATE_MQTT_HOST 为空), 告警无法外发; 本地已留档${OFF}"
        touch "$STOP_FLAG" 2>/dev/null || true
    fi
else
    echo " 停止告警    : 已告警过, 不重复发送(服务恢复后会重置)"
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
