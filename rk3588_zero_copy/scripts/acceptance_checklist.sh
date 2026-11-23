#!/usr/bin/env bash
# ============================================================================
# acceptance_checklist.sh —— 二期交付验收清单(可执行)
#
# 用法:
#   sudo scripts/acceptance_checklist.sh
#
# ★ 这不是一份"纸上清单", 是一条命令跑完 12 项并逐项打绿/打红。
#   全部打绿 => 正式交付; 任何一项打红 => 有东西没到位。
#
# 破坏性说明:
#   第 7 项会 kill -9 主进程(验自愈), 会中断服务几秒 —— 这是验收该做的。
#   第 12 项会 uninstall 再重装, 全程约 1 分钟。
#   跑完服务一定是 running 状态。
# ============================================================================
set -u

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(cd "$PROJ/.." && pwd)"
ENV_FILE="$PROJ/config/gate.env"
[ -r "$ENV_FILE" ] && . "$ENV_FILE"

LOG_DIR="${GATE_LOG_DIR:-/var/log}"
UNIT=rk-gate.service
WD_UNIT=rk-gate-watchdog.service

# ★ 日志路径重写规则 —— 与 install_gate_service.sh 共用同一份。
#   【3】项要比对"装到 /etc 的东西"与"仓库模板按 GATE_LOG_DIR 渲染后的结果",
#   规则必须和安装脚本用的那条【逐字节一致】, 所以两边都 source 这一个文件。
. "$PROJ/scripts/logpath_render.sh"

pass=0; fail=0
G=$'\033[32m'; R=$'\033[31m'; Y=$'\033[33m'; D=$'\033[2m'; O=$'\033[0m'

ok()   { printf "  ${G}✅ %-52s${O} %s\n" "$1" "${2:-}"; pass=$((pass+1)); }
no()   { printf "  ${R}❌ %-52s${O} %s\n" "$1" "${2:-}"; fail=$((fail+1)); }
warn() { printf "  ${Y}⚠️  %-52s${O} %s\n" "$1" "${2:-}"; }
info() { printf "     ${D}%s${O}\n" "$1"; }

echo "================================================================"
echo " RK3588 大门系统 二期交付验收清单    $(date '+%F %T')"
echo "================================================================"

if [ "$(id -u)" != "0" ]; then
    echo "  ${R}本清单需要 root${O} —— 里面要做 kill -9 / systemctl / 卸载等特权操作"
    echo "  请用: sudo scripts/acceptance_checklist.sh"
    exit 1
fi

# ---------------------------------------------------------------- 1 编译
echo
echo "【1】编译(零警告)"
cd "$PROJ/build" 2>/dev/null || cd "$PROJ"
if command -v make >/dev/null 2>&1; then
    out=$(timeout 600 make -j"$(nproc)" rk_gate 2>&1)
    if echo "$out" | grep -qiE "error"; then
        no "编译通过" "$(echo "$out" | grep -iE 'error' | head -1)"
    elif echo "$out" | grep -qiE "warning"; then
        warn "编译通过(有警告)" "$(echo "$out" | grep -iE 'warning' | head -1)"
    else
        ok "编译通过, 零警告"
    fi
else
    no "make 可用" "板子上没有 make"
fi

# ---------------------------------------------------------------- 2 预检
echo
echo "【2】启动前预检(12 项)"
pf_out=$("$PROJ/scripts/preflight_gate.sh" 2>&1)
pf_rc=$?
pf_fail=$(echo "$pf_out" | grep -cE "^\[FAIL\]")
pf_warn=$(echo "$pf_out" | grep -cE "^\[WARN\]")
if [ "$pf_rc" = "0" ] && [ "$pf_fail" = "0" ]; then
    ok "预检全部通过" "FAIL=$pf_fail WARN=$pf_warn"
else
    no "预检通过" "FAIL=$pf_fail 项; $(echo "$pf_out" | grep -E '^\[FAIL\]' | head -1)"
fi

# ---------------------------------------------------------------- 3 unit 一致性
echo
echo "【3】安装产物与仓库一致(安装脚本可完美复现)"
# ★★ 这里必须【先按 GATE_LOG_DIR 渲染仓库模板】再比对(2026-11-23 修):
#   install_gate_service.sh 装到 /etc 的 unit 与 logrotate 规则, 是按
#   GATE_LOG_DIR 把模板里的 /var/log/ 前缀重写过的(因为 .service 里写死了
#   StandardOutput=append:/var/log/rk-gate.log 这样的绝对路径)。
#   原来的写法直接拿仓库模板跟 /etc 比 —— 只要日志目录不是 /var/log
#   (本板现在配的是 /var/log.hdd), 就会【永远不一致】, 是个假信号。
#
#   重写规则走【共享件】scripts/logpath_render.sh —— 与 install_gate_service.sh
#   用的是同一条规则, 两边不可能漂移(漂移成"永远打红"或"永远打绿"都更糟)。
#   LOG_DIR 取自 config/gate.env(上面已 source), 默认 /var/log。
all_match=1
render_tmp=$(mktemp -d /tmp/acc_render.XXXXXX)
trap 'rm -rf "$render_tmp"' EXIT
info "按 GATE_LOG_DIR=$LOG_DIR 渲染仓库模板后比对"

_check_one() {   # $1=仓库模板  $2=/etc 里的文件  $3=显示名
    if [ ! -r "$2" ]; then
        all_match=0; info "$3: /etc 里没有这个文件 ($2)"; return
    fi
    if ! render_log_paths "$1" "$LOG_DIR" "$render_tmp/rendered" 2>/dev/null; then
        all_match=0; info "$3: 渲染失败"; return
    fi
    if cmp -s "$render_tmp/rendered" "$2"; then
        return
    fi
    all_match=0
    info "$3: 与仓库渲染结果不一致 (渲染后 vs /etc)"
    diff "$render_tmp/rendered" "$2" 2>/dev/null | head -8 | sed 's/^/       /'
}

for u in rk-gate.service rk-gate-watchdog.service rk-collect-crash.service; do
    _check_one "$PROJ/scripts/systemd/$u" "/etc/systemd/system/$u" "$u"
done
_check_one "$PROJ/scripts/logrotate/rk-gate" "/etc/logrotate.d/rk-gate" "logrotate 规则"

if [ "$all_match" = "1" ]; then
    ok "4 个文件与仓库渲染结果逐字节一致" "GATE_LOG_DIR=$LOG_DIR"
else
    no "unit 与仓库一致" "GATE_LOG_DIR=$LOG_DIR"
fi

# ---------------------------------------------------------------- 4 服务状态
echo
echo "【4】服务已托管"
for u in "$UNIT" "$WD_UNIT"; do
    a=$(systemctl is-active "$u" 2>&1); e=$(systemctl is-enabled "$u" 2>&1)
    if [ "$a" = "active" ] && [ "$e" = "enabled" ]; then
        ok "$u active + enabled"
    else
        no "$u 状态" "active=$a enabled=$e"
    fi
done

# ---------------------------------------------------------------- 5 MainPID 是本体
echo
echo "【5】MainPID 指向应用本体(不是包装脚本)"
MP=$(systemctl show -p MainPID --value "$UNIT" 2>/dev/null)
exe=$(readlink "/proc/$MP/exe" 2>/dev/null)
case "$exe" in
    */rk_gate) ok "MainPID=$MP -> rk_gate 本体" "$exe" ;;
    "")        no "MainPID 指向 rk_gate" "进程不在" ;;
    *)         no "MainPID 指向 rk_gate" "实际指向 $exe" ;;
esac

# ---------------------------------------------------------------- 6 健康检查
echo
echo "【6】健康检查"
hp=$("$PROJ/scripts/gate_health.sh" 2>&1)
if echo "$hp" | grep -q "结论.*正常"; then
    ok "gate_health.sh 判定正常"
else
    warn "gate_health.sh 判定非正常" "$(echo "$hp" | grep -A1 '结论' | tail -1 | tr -d '\033' | sed 's/\[[0-9;]*m//g')"
fi

# ---------------------------------------------------------------- 7 崩溃自愈
echo
echo "【7】崩溃自愈(kill -9 -> systemd 自动拉起)"
LOGF="$LOG_DIR/rk-gate-error.log"
before_pid=$(systemctl show -p MainPID --value "$UNIT" 2>/dev/null)
before_bytes=$(wc -c < "$LOGF" 2>/dev/null || echo 0)
kill -9 "$before_pid" 2>/dev/null
sleep 18
after_pid=$(systemctl show -p MainPID --value "$UNIT" 2>/dev/null)
after_bytes=$(wc -c < "$LOGF" 2>/dev/null || echo 0)
if [ "$after_pid" != "0" ] && [ "$after_pid" != "$before_pid" ]; then
    ok "systemd 自动拉起" "PID $before_pid -> $after_pid (18 秒内)"
else
    no "systemd 自动拉起" "PID $before_pid -> $after_pid"
fi
if [ "$after_bytes" -gt "$before_bytes" ]; then
    ok "日志连续追加" "$before_bytes -> $after_bytes 字节"
else
    no "日志连续追加" "$before_bytes -> $after_bytes 字节"
fi

# ---------------------------------------------------------------- 8 帧号推进
echo
echo "【8】4 路推理都在出帧"
sleep 12
frame_of() { tail -c 200000 "$LOGF" 2>/dev/null | grep -aoE "\[$1\]\[frame [0-9]+\]" | tail -1 | sed -E 's/.*frame ([0-9]+)\]/\1/'; }
f1=$(frame_of ch1)
sleep 12
f2=$(frame_of ch1)
if [ -n "${f1:-}" ] && [ -n "${f2:-}" ] && [ "$f2" -gt "$f1" ]; then
    ok "ch1 帧号在推进" "$f1 -> $f2 (12 秒)"
else
    no "ch1 帧号推进" "$f1 -> $f2"
fi

# ---------------------------------------------------------------- 9 内存水位
echo
echo "【9】内存水位"
MP2=$(systemctl show -p MainPID --value "$UNIT" 2>/dev/null)
rss=$(awk '/^VmRSS:/{print $2}' "/proc/$MP2/status" 2>/dev/null)
heap=$(tail -c 200000 "$LOGF" 2>/dev/null | grep -aoE 'heap=[0-9]+KB' | tail -1 | cut -d= -f2 | tr -d 'KB')
if [ -n "${rss:-}" ] && [ "$rss" -lt $((400*1024)) ]; then
    ok "RSS 在阈值内" "$((rss/1024)) MB (阈值 400 MB)"
else
    no "RSS 在阈值内" "${rss:-0} KB"
fi
if [ -n "${heap:-}" ] && [ "$heap" -lt $((200*1024)) ]; then
    ok "活堆在阈值内" "$((heap/1024)) MB (阈值 200 MB)"
else
    no "活堆在阈值内" "${heap:-0} KB"
fi

# ---------------------------------------------------------------- 10 日志速率
echo
echo "【10】日志速率(防止 zram 写满)"
b=$(wc -c < "$LOGF" 2>/dev/null || echo 0); sleep 20; a=$(wc -c < "$LOGF" 2>/dev/null || echo 0)
rate_kb_h=$(( (a-b)*360/1024 ))
rate_mb_h=$(( rate_kb_h/1024 ))
if [ "$rate_kb_h" -le 5120 ]; then
    ok "日志速率正常" "${rate_kb_h} KB/小时 (zram 47MB 装得下)"
else
    no "日志速率" "${rate_kb_h} KB/小时 偏高"
fi
free_mb=$(df -Pm "$LOG_DIR" 2>/dev/null | awk 'NR==2{print $4}')
info "日志盘剩余 ${free_mb} MB"

# ---------------------------------------------------------------- 11 互斥
echo
echo "【11】双实例互斥"
out=$("$PROJ/scripts/run_gate_service.sh" 2>&1); rc=$?
if [ "$rc" = "75" ] || [ "$rc" = "78" ]; then
    ok "手动启动被正确拒绝" "退出码=$rc"
else
    no "手动启动被拒绝" "退出码=$rc(期望 75/78)"
fi

# ---------------------------------------------------------------- 12 卸载/重装闭环
echo
echo "【12】卸载/重装闭环(零残留)"
u_out=$("$PROJ/scripts/install_gate_service.sh" --uninstall 2>&1)
leftover=0
for u in rk-gate.service rk-gate-watchdog.service rk-collect-crash.service; do
    [ -f "/etc/systemd/system/$u" ] && { leftover=1; info "$u 还在"; }
done
[ -f "/etc/logrotate.d/rk-gate" ] && { leftover=1; info "logrotate 还在"; }
[ -f "/etc/rk-gate/systemd_managed" ] && { leftover=1; info "托管标记还在"; }
if [ "$leftover" = "0" ]; then
    ok "卸载零残留"
else
    no "卸载零残留" "有文件残留"
fi
"$PROJ/scripts/install_gate_service.sh" >/dev/null 2>&1
sleep 15
if [ "$(systemctl is-active "$UNIT" 2>&1)" = "active" ]; then
    ok "重装后服务已恢复" "active"
else
    no "重装后服务恢复" "$(systemctl is-active "$UNIT" 2>&1)"
fi

# ---------------------------------------------------------------- 汇总
echo
echo "================================================================"
if [ "$fail" = "0" ]; then
    printf " ${G} 验收通过: %d 项全部通过, 0 项失败${O}\n" "$pass"
    echo "================================================================"
    echo "  二期项目正式交付。"
else
    printf " ${R}验收未通过: %d 项通过, %d 项失败${O}\n" "$pass" "$fail"
    echo "================================================================"
    exit 1
fi
