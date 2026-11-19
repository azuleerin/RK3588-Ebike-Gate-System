#!/usr/bin/env bash
# longrun.sh —— 最小化测试单元的长跑守护（systemd 包装）
#
# 规则要求: 长跑前必须打 git tag，死机后回滚二分。
# 本脚本强制执行这个纪律: 启动前检查工作区干净 + 自动打 tag。
#
# 用法:
#   ./scripts/longrun.sh --mode npu-only --src int8:/path/blob.bin [--duration 86400] [--no-tag]
#
# 流程:
#   1. 检查 git 工作区是否干净（不干净就拒绝启动，避免"带着脏改动长跑"）
#   2. 打 tag longrun/<时间戳>/<mode>（记录这次长跑对应的代码状态）
#   3. 生成/更新 systemd unit 的 ExecStart 并 enable --now
#   4. 崩溃后用 scripts/collect_crash.sh 收现场，用 git bisect 回滚

set -euo pipefail

REPO="/home/seeed/rk_deploy"
SERVICE="rk-minimal"
UNIT="/etc/systemd/system/${SERVICE}.service"
MODE=""
SRC=""
DURATION="86400"
LOGFILE=""
DO_TAG=1
STATS_INTERVAL="60"
WSTRIDE_MODE=""
IMPORT_MODE=""

while [ $# -gt 0 ]; do
    case "$1" in
        --mode)        MODE="${2:-}"; shift 2 ;;
        --src)         SRC="${2:-}"; shift 2 ;;
        --duration)    DURATION="${2:-}"; shift 2 ;;
        --log)         LOGFILE="${2:-}"; shift 2 ;;
        --stats-interval) STATS_INTERVAL="${2:-}"; shift 2 ;;
        --wstride-mode) WSTRIDE_MODE="${2:-}"; shift 2 ;;
        --import-mode)  IMPORT_MODE="${2:-}"; shift 2 ;;
        --no-tag)      DO_TAG=0; shift ;;
        -h|--help)
            sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "未知参数: $1"; exit 2 ;;
    esac
done

[ -n "$MODE" ] || { echo "必须指定 --mode"; exit 2; }
[ -n "$SRC" ]  || { echo "必须指定 --src"; exit 2; }
case "$MODE" in
    decoder-only|rga-only|npu-only) ;;
    *) echo "--mode 必须是 decoder-only / rga-only / npu-only 之一"; exit 2 ;;
esac

BIN=""
for _c in "$REPO/rk3588_zero_copy/build_minimal/rk_minimal" \
         "$REPO/rk3588_zero_copy/build/rk_minimal"; do
    [ -x "$_c" ] && { BIN="$_c"; break; }
done
[ -n "$BIN" ] || { echo "[FATAL] 找不到 rk_minimal（先编译 step/02~04 的最小单元）"; exit 1; }

# systemctl 需要 root。
# 板子上通过 SSH/无 tty 跑时 sudo 没法交互, 用 SUDO_PASSWORD 环境变量把密码喂给
# `sudo -S`(密码只走 stdin, 不落盘、不进进程参数)。
run_sudo() {
    if [ "$(id -u)" = "0" ]; then
        "$@"
    elif [ -n "${SUDO_PASSWORD:-}" ]; then
        echo "$SUDO_PASSWORD" | sudo -S -p "" "$@"
    else
        sudo "$@"
    fi
}

cd "$REPO"

# ---- 1. 工作区必须干净 ----
DIRTY="$(git status --porcelain)"
if [ -n "$DIRTY" ]; then
    echo "[FATAL] git 工作区不干净，拒绝启动长跑:"
    echo "$DIRTY" | head -20
    echo "先提交或 stash，否则死机后无法二分定位。"
    exit 3
fi

# ---- 2. 打 tag 记录长跑对应的代码状态 ----
if [ "$DO_TAG" = "1" ]; then
    TAG="longrun/$(date +%Y%m%d_%H%M%S)/${MODE}"
    git tag -a "$TAG" -m "长跑起点: mode=${MODE} src=${SRC} duration=${DURATION}" \
        && echo "[OK] 已打 tag: $TAG"
    echo "     死机后回滚: git bisect start; git bisect good baseline/pre-crash-debug; git bisect bad $TAG"
fi

# ---- 3. 写 systemd unit ----
LOGFILE="${LOGFILE:-$REPO/logs/minimal_${MODE}.log}"
EXTRA_ARGS=""
[ -n "$WSTRIDE_MODE" ] && EXTRA_ARGS="${EXTRA_ARGS} --wstride-mode ${WSTRIDE_MODE}"
[ -n "$IMPORT_MODE" ]  && EXTRA_ARGS="${EXTRA_ARGS} --import-mode ${IMPORT_MODE}"
mkdir -p "$REPO/logs"

TMP_UNIT="$(mktemp /tmp/rk-minimal.service.XXXXXX)"
cat > "$TMP_UNIT" <<EOF
[Unit]
Description=RK3588 minimal test unit - ${MODE}
After=network-online.target
Wants=network-online.target

# ★ 启动频率限制必须写在 [Unit] 段:
#   StartLimitIntervalSec 是 systemd 229 起新增的【[Unit] 段】指令,
#   写进 [Service] 会被静默忽略(只报一句 Unknown key ... ignoring),
#   于是实际生效的是默认值 10 秒 —— 配上 RestartSec=5, 崩溃循环约 25 秒就被
#   systemd 放弃重启, 长跑守护直接失效。放在 [Unit] 才是意图的 5 分钟内 5 次。
StartLimitBurst=5
StartLimitIntervalSec=300

[Service]
Type=simple
WorkingDirectory=$REPO/rk3588_zero_copy
ExecStart=$BIN --${MODE} --src ${SRC} --loop --duration ${DURATION} --log ${LOGFILE} --stats-interval ${STATS_INTERVAL} ${EXTRA_ARGS}
Restart=always
RestartSec=5
WatchdogSec=300
MemoryMax=2G
MemorySwapMax=0
MemoryAccounting=yes
OOMPolicy=continue
LimitCORE=infinity
KillSignal=SIGTERM
TimeoutStopSec=10

[Install]
WantedBy=multi-user.target
EOF
# /etc/systemd/system 需要 root, 先写临时文件再拷过去
run_sudo cp "$TMP_UNIT" "$UNIT"
rm -f "$TMP_UNIT"
echo "[OK] 已生成 $UNIT"

# ---- 4. 启动 ----
run_sudo systemctl daemon-reload
run_sudo systemctl enable --now "$SERVICE"
sleep 2
systemctl --no-pager status "$SERVICE" | head -15

echo
echo "== 长跑已启动 =="
echo "  模式    : $MODE"
echo "  输入源  : $SRC"
echo "  日志    : $LOGFILE"
echo "  tag     : (见上方 git tag 输出)"
echo
echo "查看:  journalctl -u ${SERVICE} -f   |   tail -f ${LOGFILE}"
echo "收现场: sudo $REPO/scripts/collect_crash.sh"
echo "停止  : sudo systemctl stop ${SERVICE}"
