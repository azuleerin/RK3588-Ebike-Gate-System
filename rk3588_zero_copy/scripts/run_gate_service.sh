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
LOCK="${GATE_LOCK_FILE:-/var/run/rk-gate.lock}"
mkdir -p "$(dirname "$LOCK")" 2>/dev/null || true
# ★★ 这里【绝对不能】写成 `exec 9>"$LOCK" 2>/dev/null` ——
#    exec 不带命令时, 重定向会【永久】作用于当前 shell, 于是 2>/dev/null
#    把 wrapper 的 stderr 永久换成 /dev/null, 后面 exec 出去的 rk_gate
#    会继承它 —— 结果是【业务日志一行都不出来】(fd 2 指向 /dev/null)。
#    这个坑很隐蔽: stdout 一切正常, 只有 stderr 静默, 像是"程序没打日志"。
#    所以先用普通命令试探可写性(重定向只作用于该命令), 再单独 exec fd 9。
if ! ( : >> "$LOCK" ) 2>/dev/null; then
    echo "[gate] 打不开锁文件 $LOCK (权限不足?)" >&2
    echo "[gate]   手工建: sudo touch $LOCK && sudo chmod 666 $LOCK" >&2
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
    --ipaddr   "$GATE_IPADDR"
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
