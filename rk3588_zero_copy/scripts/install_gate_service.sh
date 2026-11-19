#!/usr/bin/env bash
# ============================================================================
# install_gate_service.sh —— rk_gate 生产守护的一键安装 / 卸载
#
# 用法:
#   sudo scripts/install_gate_service.sh              # 安装 + 立即启动
#   sudo scripts/install_gate_service.sh --no-start   # 只安装不启动
#   sudo scripts/install_gate_service.sh --uninstall   # 卸载(彻底清理)
#   sudo scripts/install_gate_service.sh --uninstall --purge-logs   # 连日志一起删
#
# 安装做了什么:
#   1) 在日志目录下预建 4 个日志文件, 并把属主设成服务用户
#      (systemd 的 StandardOutput=append: 会以服务用户身份打开它们;
#       logrotate 的 "su seeed root" 也要求属主正确)
#   2) 拷 3 个 unit 到 /etc/systemd/system/
#   3) 拷 logrotate 规则到 /etc/logrotate.d/
#   4) 建 /etc/rk-gate/systemd_managed 标记 -> 禁止手动再起一个实例
#   5) 建 /var/run/rk-gate.lock (0666) -> flock 互斥锁
#   6) daemon-reload + enable --now
#
# 卸载做了什么(【绝不残留】):
#   1) systemctl disable --now (主服务 + 看门狗)
#   2) 删 /etc/systemd/system/ 下的 3 个 unit
#   3) 删 /etc/logrotate.d/rk-gate
#   4) 删 /etc/rk-gate/ (含 systemd_managed 标记)
#   5) 删 /var/run/rk-gate.lock 与 /run/rk-gate/ (含维护标记)
#   6) reset-failed + daemon-reload
#   7) 日志默认【保留】(是证据); 加 --purge-logs 才删
# ============================================================================
set -u

SRC="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$SRC/.." && pwd)"
ENV_FILE="$PROJ/config/gate.env"

UNIT_DIR=/etc/systemd/system
LOGROTATE_DIR=/etc/logrotate.d
STATE_DIR=/etc/rk-gate
MARK="$STATE_DIR/systemd_managed"
RUN_DIR=/run/rk-gate
LOCK=/var/run/rk-gate.lock

UNITS=(rk-gate.service rk-gate-watchdog.service rk-collect-crash.service)
LOGROTATE_SRC="$SRC/logrotate/rk-gate"

DO_INSTALL=1
DO_UNINSTALL=0
DO_START=1
PURGE_LOGS=0

for a in "$@"; do
    case "$a" in
        --uninstall)  DO_UNINSTALL=1; DO_INSTALL=0 ;;
        --purge-logs) PURGE_LOGS=1 ;;
        --no-start)   DO_START=0 ;;
        -h|--help)    sed -n '2,32p' "$0"; exit 0 ;;
        *) echo "未知参数: $a (用 --help 看用法)" >&2; exit 1 ;;
    esac
done

# ---- 必须 root ----
if [ "$(id -u)" != "0" ]; then
    echo "需要 root, 请用: sudo $0 $*" >&2
    exit 1
fi

# ---- 读配置 ----
if [ ! -r "$ENV_FILE" ]; then
    echo "配置文件读不到: $ENV_FILE" >&2
    exit 1
fi
# shellcheck disable=SC1090
. "$ENV_FILE"

LOG_DIR="${GATE_LOG_DIR:-/var/log}"
SVC_USER="${GATE_SERVICE_USER:-seeed}"

if ! id "$SVC_USER" >/dev/null 2>&1; then
    echo "服务用户不存在: $SVC_USER" >&2
    exit 1
fi

echo "==========================================================="
echo " rk_gate 生产守护 —— $([ $DO_UNINSTALL = 1 ] && echo '卸载' || echo '安装')"
echo "   配置   : $ENV_FILE"
echo "   日志目录: $LOG_DIR"
echo "   服务用户: $SVC_USER"
echo "==========================================================="

# ===========================================================================
# 卸载
# ===========================================================================
if [ "$DO_UNINSTALL" = "1" ]; then
    echo ""
    echo "[1/6] 停服务并取消开机自启"
    for u in rk-gate.service rk-gate-watchdog.service; do
        systemctl disable --now "$u" 2>&1 | sed 's/^/    /' || true
    done
    systemctl stop rk-collect-crash.service 2>/dev/null || true

    echo "[2/6] 删除 unit 文件"
    for u in "${UNITS[@]}"; do
        if [ -f "$UNIT_DIR/$u" ]; then
            rm -f "$UNIT_DIR/$u" && echo "    删 $UNIT_DIR/$u"
        fi
        # drop-in 也要删, 否则残留的 Restart=no 之类会一直生效
        d="/run/systemd/system/${u}.d"
        if [ -d "$d" ]; then
            rm -rf "$d" && echo "    删 $d"
        fi
    done

    echo "[3/6] 删除 logrotate 规则"
    [ -f "$LOGROTATE_DIR/rk-gate" ] && rm -f "$LOGROTATE_DIR/rk-gate" && echo "    删 $LOGROTATE_DIR/rk-gate"

    echo "[4/6] 删除状态目录与标记"
    [ -f "$MARK" ] && rm -f "$MARK" && echo "    删 $MARK"
    rmdir "$STATE_DIR" 2>/dev/null && echo "    删 $STATE_DIR" || true
    [ -f "$LOCK" ] && rm -f "$LOCK" && echo "    删 $LOCK"
    rm -rf "$RUN_DIR" 2>/dev/null && echo "    删 $RUN_DIR"

    echo "[5/6] daemon-reload + reset-failed"
    systemctl daemon-reload
    systemctl reset-failed rk-gate.service 2>/dev/null || true
    systemctl reset-failed rk-gate-watchdog.service 2>/dev/null || true

    echo "[6/6] 日志"
    if [ "$PURGE_LOGS" = "1" ]; then
        for f in rk-gate.log rk-gate-error.log rk-gate-watchdog.log \
                 rk-gate-alerts.log rk-gate-crash.log rk-gate-preflight.log; do
            rm -f "$LOG_DIR/$f" "$LOG_DIR/$f".* 2>/dev/null && echo "    删 $LOG_DIR/$f*"
        done
        echo "    日志已全部删除"
    else
        echo "    日志保留在 $LOG_DIR (是证据, 不默认删)"
        echo "    要一起删: sudo $0 --uninstall --purge-logs"
    fi

    echo ""
    echo " 卸载完成。残留检查:"
    for u in "${UNITS[@]}"; do
        [ -f "$UNIT_DIR/$u" ] && echo "   ★ 还在: $UNIT_DIR/$u"
    done
    [ -f "$MARK" ] && echo "   ★ 还在: $MARK"
    [ -f "$LOCK" ] && echo "   ★ 还在: $LOCK"
    systemctl list-unit-files 2>/dev/null | grep -E "rk-gate" | sed 's/^/   ★ 还在: /' || true
    echo "==========================================================="
    exit 0
fi

# ===========================================================================
# 安装
# ===========================================================================
echo ""
echo "[1/6] 检查前置文件"
for f in "$SRC/preflight_gate.sh" "$SRC/run_gate_service.sh" "$SRC/gate_watchdog.sh" \
         "$ENV_FILE" "$GATE_BIN"; do
    if [ -e "$f" ]; then
        echo "    OK  $f"
    else
        echo "    ★ 缺: $f" >&2
        exit 1
    fi
done
for u in "${UNITS[@]}"; do
    [ -f "$SRC/systemd/$u" ] || { echo "    ★ 缺 unit 源文件: $SRC/systemd/$u" >&2; exit 1; }
done
[ -f "$LOGROTATE_SRC" ] || { echo "    ★ 缺 logrotate 源文件: $LOGROTATE_SRC" >&2; exit 1; }

echo ""
echo "[2/6] 预建日志文件并赋权 (属主 $SVC_USER)"
mkdir -p "$LOG_DIR"
for f in rk-gate.log rk-gate-error.log rk-gate-watchdog.log \
         rk-gate-alerts.log rk-gate-crash.log rk-gate-preflight.log; do
    p="$LOG_DIR/$f"
    if [ ! -e "$p" ]; then
        touch "$p"
        echo "    建 $p"
    fi
    chown "$SVC_USER":"$SVC_USER" "$p" 2>/dev/null || true
    chmod 0640 "$p"
done
echo "    属主/权限已设为 $SVC_USER:0640"

echo ""
echo "[3/6] 拷贝 systemd unit (日志路径按 GATE_LOG_DIR=$LOG_DIR 重写)"
for u in "${UNITS[@]}"; do
    if [ "$LOG_DIR" = "/var/log" ]; then
        cp "$SRC/systemd/$u" "$UNIT_DIR/$u"
    else
        sed "s#/var/log/#${LOG_DIR}/#g" "$SRC/systemd/$u" > "$UNIT_DIR/$u"
    fi
    chmod 0644 "$UNIT_DIR/$u"
    echo "    -> $UNIT_DIR/$u"
done

# ★ 清理遗留的 drop-in:
#   drop-in 放在 /run/systemd/system/<unit>.d/ 下, 优先级高于我们装的 unit,
#   装 unit 是【覆盖不掉】它的 —— 烤机时留下的 `Restart=no` 会让看门狗
#   挂掉后不自动重启, 守护链就断了一截。重装必须把它清掉。
for u in "${UNITS[@]}"; do
    d="/run/systemd/system/${u}.d"
    if [ -d "$d" ]; then
        rm -rf "$d"
        echo "    清理遗留 drop-in: $d"
    fi
done

echo ""
echo "[4/6] 拷贝 logrotate 规则"
if [ "$LOG_DIR" = "/var/log" ]; then
    cp "$LOGROTATE_SRC" "$LOGROTATE_DIR/rk-gate"
else
    sed "s#/var/log/#${LOG_DIR}/#g" "$LOGROTATE_SRC" > "$LOGROTATE_DIR/rk-gate"
fi
chmod 0644 "$LOGROTATE_DIR/rk-gate"
echo "    -> $LOGROTATE_DIR/rk-gate"

echo ""
echo "[5/6] 建状态标记与互斥锁"
mkdir -p "$STATE_DIR" && chmod 0755 "$STATE_DIR"
printf 'installed_by=install_gate_service.sh\ninstalled_at=%s\nlog_dir=%s\n' \
    "$(date '+%F %T')" "$LOG_DIR" > "$MARK"
chmod 0644 "$MARK"
echo "    标记 $MARK"

mkdir -p "$RUN_DIR" && chmod 0755 "$RUN_DIR"
touch "$LOCK" && chmod 0666 "$LOCK" && chown "$SVC_USER":"$SVC_USER" "$LOCK" 2>/dev/null || true
echo "    锁   $LOCK (0666 $SVC_USER)"

echo ""
echo "[6/6] daemon-reload + enable"
systemctl daemon-reload
if [ "$DO_START" = "1" ]; then
    systemctl enable rk-gate.service rk-gate-watchdog.service 2>&1 | sed 's/^/    /'
    # ★ 用 restart 而不是 start —— start 对【已经在跑】的服务是空操作,
    #   改了 gate.env / unit 之后重装, 配置就不会生效(踩过)。
    #   restart 对没跑的服务等价于 start。
    systemctl restart rk-gate.service
    systemctl restart rk-gate-watchdog.service
    sleep 3
    echo ""
    echo "    状态:"
    systemctl --no-pager --lines=0 status rk-gate.service 2>&1 | head -6 | sed 's/^/    /'
    systemctl --no-pager --lines=0 status rk-gate-watchdog.service 2>&1 | head -4 | sed 's/^/    /'
else
    systemctl enable rk-gate.service rk-gate-watchdog.service 2>&1 | sed 's/^/    /'
    echo "    (--no-start: 已 enable, 未启动)"
fi

echo ""
echo "==========================================================="
echo " 安装完成。验收命令:"
echo "   systemctl status rk-gate.service"
echo "   tail -f $LOG_DIR/rk-gate-error.log"
echo "   cat $LOG_DIR/rk-gate-preflight.log | tail -30"
echo "   systemctl stop rk-gate          # 停止"
echo "   sudo touch $RUN_DIR/paused     # 维护窗口(看门狗闭嘴)"
echo "==========================================================="
