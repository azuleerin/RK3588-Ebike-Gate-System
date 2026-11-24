#!/usr/bin/env bash
# ============================================================================
# run_gate_service.sh —— rk_gate 的启动包装 (systemd ExecStart)
#
# 干三件事, 每件都有存在理由:
#
#  1) 【exec 启动】—— 用 exec 把进程替换成本体, 不留 bash 外壳。
#     ★ 这是必须的: systemd 的 MainPID 必须【就是 rk_gate 自己】。
#       仓库里已经踩过这个坑(night_soak_monitor.sh:78-92 的注释):
#       "要的是应用本身的 PID, 不是 systemd 的 MainPID(那是包装脚本 bash,
#        它的 RSS 只有 3MB, 读出来全是错的)"。
#       MainPID 错了 -> 看门狗读不到真实 RSS, preflight 也定位不到进程。
#
#  2) 【flock 互斥】—— 同一台板子上只允许一个 rk_gate。
#     双实例 = 抢 NPU 核 + 两条 MQTT 连接发同一个 topic + 两个 SQLite 写。
#     ★ 锁是【继承】给 rk_gate 的: exec 之后 fd 9 连同 flock 一起过继,
#       所以锁的生命周期 = 进程生命周期, 进程一死锁自动释放。
#
#  3) 【systemd 托管标记】—— systemd 模式下禁止手动再起一个。
#     手动启动时给出明确提示, 而不是让人对着两个实例发呆。
#
# 退出码:
#   0  = 正常(被 exec 替换, 实际不会走到这)
#   75 = 已有实例在跑 (EX_TEMPFAIL)
#   78 = systemd 托管模式已启用, 拒绝手动启动 (EX_CONFIG)
# ============================================================================
set -u

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
ENV_FILE="$PROJ/config/gate.env"

# ---- 载入配置 ----
if [ ! -r "$ENV_FILE" ]; then
    echo "[gate] 配置文件读不到: $ENV_FILE" >&2
    exit 78
fi
# shellcheck disable=SC1090
. "$ENV_FILE"

# ---------------------------------------------------------------------------
# 1) 互斥锁
# ---------------------------------------------------------------------------
# ★ 锁文件放在 /run/rk-gate/ 子目录里, 不再直接放 /run 根上。
#   原因: /run 是 tmpfs, 每次重启清空; 而 /run 本身是 root:root 755,
#   服务以 seeed 跑, 无权在 /run 根上新建文件 —— 2026-11-21 实锤:
#   重启后 /var/run/rk-gate.lock 消失, 脚本建不出来 -> exit 75 -> 服务起不来。
#   现在这个子目录由 unit 的 RuntimeDirectory=rk-gate 建 (owner=seeed:seeed),
#   systemd 每次启动都会确保它存在且属主正确, 所以重启后照样能建锁。
#   ★ 互斥机制本身没变: 还是 exec fd 9 + flock -n, 只是锁文件换了个位置。
LOCK="${GATE_LOCK_FILE:-/run/rk-gate/rk-gate.lock}"
LOCK_DIR="$(dirname "$LOCK")"

# ---------------------------------------------------------------------------
# ★ P0 自检: 锁目录必须【存在】且【可写】, 否则明确报错退出, 不静默失败
#
#   为什么要单独加这一段(而不是靠下面建锁文件失败):
#     1) 报错要【说清楚是哪一环坏了】。以前只看到"打不开锁文件(权限不足?)",
#        到底是"目录没了"还是"目录在但不可写", 分不出来 —— 排查只能靠猜。
#     2) 不能静默继续。/run 是 tmpfs, 重启会被清空; 目录没了就意味着
#        unit 的 RuntimeDirectory=rk-gate 没生效(unit 没装上/被改坏/手工跑脚本)。
#        这种情况下必须【大声失败】, 而不是重试到天荒地老。
#
#   ★ 顺序: systemd 在 ExecStartPre/ExecStart【之前】建 RuntimeDirectory,
#     所以正常启动时这段自检【一定通过】; 它只是兜底"unit 没生效"的情况。
#     手工跑本脚本时, 目录可能不存在 -> 这里会明确报出来。
#
#   ★ 退出码用 78 (EX_CONFIG): 这是【配置/部署】问题, 不是瞬时问题,
#     重试没有意义。unit 里有 RestartPreventExitStatus=78, systemd 不会
#     陷入重启风暴, 交给看门狗的指数退避处理。
# ---------------------------------------------------------------------------
# ★ 告警文件跟着 GATE_LOG_DIR 走 —— 写死 /var/log 会在日志目录迁走后
#   悄悄写失败(/var/log 是 zram, 且 seeed 对它不可写)。
#   GATE_LOG_DIR 由 unit 的 EnvironmentFile 带进来(本脚本自己也会 source gate.env)。
_LOCK_LOG_DIR="${GATE_LOG_DIR:-/var/log}"
LOCK_CHECK_LOG="${GATE_LOCK_CHECK_LOG:-$_LOCK_LOG_DIR/rk-gate-lock-check.log}"
lock_alert() {
    # ★ stderr 就是告警通道(unit 的 StandardError=append:<日志目录>/rk-gate-error.log)
    echo "[gate] $1" >&2
    # 再单独留一份到告警文件(尽力而为: 日志目录可能对服务用户不可写)
    printf '[%s] [gate 锁目录自检] %s\n' "$(date '+%F %T')" "$1" \
        >> "$LOCK_CHECK_LOG" 2>/dev/null || true
}

if [ ! -d "$LOCK_DIR" ]; then
    lock_alert "[自检失败] 锁目录不存在: $LOCK_DIR"
    lock_alert "  unit 的 RuntimeDirectory=rk-gate 应该在服务启动前把目录建好(owner=服务用户)。"
    lock_alert "  若 unit 没装上, 手工补: sudo mkdir -p $LOCK_DIR && sudo chown $USER:$USER $LOCK_DIR"
    exit 78
fi
if [ ! -w "$LOCK_DIR" ]; then
    lock_alert "[自检失败] 锁目录不可写: $LOCK_DIR (当前用户 $USER)"
    lock_alert "  修: sudo chown $USER:$USER $LOCK_DIR"
    exit 78
fi

# ★★ 这里【绝对不能】写成 `exec 9>"$LOCK" 2>/dev/null` ——
#    exec 不带命令时, 重定向会【永久】作用于当前 shell, 于是 2>/dev/null
#    把 wrapper 的 stderr 永久换成 /dev/null, 后面 exec 出去的 rk_gate
#    会继承它 —— 结果是【业务日志一行都不出来】(fd 2 指向 /dev/null)。
#    这个坑很隐蔽: stdout 一切正常, 只有 stderr 静默, 像是"程序没打日志"。
#    所以先用普通命令试探可写性(重定向只作用于该命令), 再单独 exec fd 9。
if ! ( : >> "$LOCK" ) 2>/dev/null; then
    lock_alert "打不开锁文件 $LOCK (目录 $LOCK_DIR 在且可写, 但建文件失败?)"
    exit 75
fi
exec 9>"$LOCK"
if ! flock -n 9; then
    echo "[gate] 已有 rk_gate 实例在跑(锁 $LOCK 被占) —— 拒绝启动双实例" >&2
    echo "[gate]   双实例会抢 NPU 核 + 双发 MQTT topic" >&2
    exit 75
fi

# ---------------------------------------------------------------------------
# 2) systemd 托管标记
# ---------------------------------------------------------------------------
MARK="${GATE_SYSTEMD_MARK:-/etc/rk-gate/systemd_managed}"
if [ -f "$MARK" ] && [ "${RK_GATE_SYSTEMD:-0}" != "1" ]; then
    echo "[gate] systemd 模式已启用, 拒绝手动启动。" >&2
    echo "[gate]   请用: sudo systemctl start rk-gate" >&2
    echo "[gate]   要退回手动模式: sudo scripts/install_gate_service.sh --uninstall" >&2
    exit 78
fi

# ---------------------------------------------------------------------------
# 3) 拼 CLI
# ---------------------------------------------------------------------------
export LD_LIBRARY_PATH="$GATE_LD_LIBRARY_PATH"

# ★ 应用日志级别 -> RK_LOG_LEVEL (info/warn/error/off)
#   ★ 生产必须 warn: 帧守卫对每帧都打 INFO, 4 路实测 36.5MB/小时,
#     而 /var/log 只有 47MB zram —— 写满会让应用 write() 失败。
#     业务统计行是直接 fprintf(stderr), 不走分级日志, 不受影响。
#   ★ 优先级: 环境变量 RK_LOG_LEVEL > gate.env 里的 GATE_LOG_LEVEL > warn。
#     以前只有 gate.env 一条路, systemd drop-in 设 Environment=RK_LOG_LEVEL=debug
#     会被这一行【覆盖掉】, drop-in 形同虚设。现场要临时开 debug 时用 drop-in。
export RK_LOG_LEVEL="${RK_LOG_LEVEL:-${GATE_LOG_LEVEL:-warn}}"

# ---------------------------------------------------------------------------
# 3b) GATE_IPADDR = auto -> 自动取主网卡的 IPv4
#
#   解析逻辑在 scripts/resolve_ipaddr.sh 里(生产与沙盒渲染共用同一份,
#   免得两边漂移 —— 沙盒渲染要是传出字面量 "auto", 冒烟测的就不是同一个东西)。
#   详见该文件头的说明。
# ---------------------------------------------------------------------------
# shellcheck disable=SC1091
. "$PROJ/scripts/resolve_ipaddr.sh"
GATE_IPADDR_RESOLVED=$(resolve_ipaddr "${GATE_IPADDR:-auto}")
if [ "${GATE_IPADDR:-auto}" = "auto" ]; then
    echo "[gate] GATE_IPADDR=auto -> 自动取到 ${GATE_IPADDR_RESOLVED}" >&2
fi

ARGS=(
    --model   "$GATE_MODEL"
    --labels  "$GATE_LABELS"
    --ppocr   "$GATE_OCR"
    --ppocr-dict "$GATE_OCR_DICT"
    --det-core     "$GATE_DET_CORES"
    --ocr-core     "$GATE_OCR_CORES"
    --det-workers  "$GATE_DET_WORKERS"
    --ocr-workers  "$GATE_OCR_WORKERS"
    --det-slots    "$GATE_DET_SLOTS"
    --conf    "$GATE_CONF"
    --nms     "$GATE_NMS"
    --max-fps "$GATE_MAX_FPS"
    --stats-interval "$GATE_STATS_INTERVAL"
    --roi-ratio  "$GATE_ROI_RATIO"
    --roi-y      "$GATE_ROI_Y"
    --roi-height "$GATE_ROI_HEIGHT"
    --roi-shift  "$GATE_ROI_SHIFT"
    --roi-below  "$GATE_ROI_BELOW"
    --ipm-width-m "$GATE_IPM_WIDTH_M"
    --sn       "$GATE_SN"
    --device-name "$GATE_DEVICE_NAME"
    --ipaddr   "$GATE_IPADDR_RESOLVED"
    --duration "$GATE_DURATION"
)

# ---- 4 路通道 ----
for i in 1 2 3 4; do
    var="GATE_CH$i";      url="${!var:-}"
    kind_var="GATE_CH${i}_KIND"; kind="${!kind_var:-}"
    ipm_var="GATE_CH${i}_IPM";  ipm="${!ipm_var:-}"
    [ -z "$url" ] && continue
    spec="ch${i}:${url}:${kind}"
    # high(测速)通道要带 IPM 四点
    if [ "$kind" = "high" ] && [ -n "$ipm" ]; then
        spec="${spec}:${ipm}"
    fi
    ARGS+=(--ch "$spec")
done

# ---- MQTT ----
if [ -n "${GATE_MQTT_HOST:-}" ]; then
    ARGS+=(--mqtt-host "$GATE_MQTT_HOST"
           --mqtt-port "${GATE_MQTT_PORT:-1883}"
           --mqtt-qos  "${GATE_MQTT_QOS:-1}")
fi

# ---- 过线计数(按通道配, 留空 = 不启用) ----
for i in 1 2 3 4; do
    var="GATE_TRIPWIRE_CH$i"; line="${!var:-}"
    [ -z "$line" ] && continue
    ARGS+=(--tripwire-line "ch${i}:${line}")
done
if [ -n "${GATE_TRIPWIRE_INTERVAL_MS:-}" ]; then
    ARGS+=(--tripwire-interval-ms "$GATE_TRIPWIRE_INTERVAL_MS")
fi

# ---------------------------------------------------------------------------
# 4) 启动
# ---------------------------------------------------------------------------
echo "[gate] 启动 $GATE_BIN"
echo "[gate]   通道: $(printf '%s ' "${ARGS[@]}" | grep -o 'ch[0-9]:[^ ]*' | tr '\n' ' ')"
echo "[gate]   核绑定: 检测=$GATE_DET_CORES 识别=$GATE_OCR_CORES  worker=$GATE_DET_WORKERS/$GATE_OCR_WORKERS"

# ★ exec: 进程替换。systemd 的 MainPID 从此就是这个 rk_gate。
exec "$GATE_BIN" "${ARGS[@]}"
