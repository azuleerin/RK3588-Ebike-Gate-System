#!/usr/bin/env bash
# ============================================================================
# preflight_gate.sh —— rk_gate 启动前的环境断言 (systemd ExecStartPre)
#
# 目的: 把"进程起来了但业务全挂"的半栈状态, 提前变成"启动失败 + 明确原因"。
#   文章《RK3588 边缘 AI 设备如何做到 7×24 不死机》的 12 项预检,
#   按【本板实测事实】重排过 —— 照抄原文会翻车的地方都标了 ★。
#
# 退出码:
#   0  = 全部通过(有 WARN 也会放行)
#   78 = 有 FAIL 项 (EX_CONFIG) —— systemd 把它判成配置错误, 不进重启循环
#
# 日志: 结果逐项写 $GATE_LOG_DIR/rk-gate-preflight.log (追加)
#
# ★ 本脚本以【普通用户】身份运行(ExecStartPre 继承 User=seeed),
#   所以只做"普通用户能查得到"的检查。需要 root 的检查不放这里。
# ============================================================================
set -u

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
ENV_FILE="$PROJ/config/gate.env"

# ---- 输出通道 ----
_fail=0
_warn=0
_lines=()

log_line() {
    printf '%s\n' "$*"
    _lines+=("$*")
}

ok()   { log_line "[ OK ] $1"; }
warn() { log_line "[WARN] $1"; _warn=$((_warn + 1)); }
fail() { log_line "[FAIL] $1"; _fail=$((_fail + 1)); }
info() { log_line "[INFO] $1"; }

# ---- 载入配置 ----
if [ ! -r "$ENV_FILE" ]; then
    echo "[FAIL] 配置文件读不到: $ENV_FILE" >&2
    exit 78
fi
# shellcheck disable=SC1090
. "$ENV_FILE"

LOG_DIR="${GATE_LOG_DIR:-/var/log}"
PREFLIGHT_LOG="$LOG_DIR/rk-gate-preflight.log"

log_line "================================================================"
log_line " rk_gate 启动前预检   $(date '+%F %T')"
log_line " 配置: $ENV_FILE"
log_line "================================================================"

# ---------------------------------------------------------------------------
# 1. 二进制
# ---------------------------------------------------------------------------
if [ -x "${GATE_BIN:-}" ]; then
    ok "1. 二进制可执行: $GATE_BIN"
else
    fail "1. 二进制不存在或不可执行: ${GATE_BIN:-<未配置>}"
fi

# ---------------------------------------------------------------------------
# 2. 模型 / 标签 / 字典
# ---------------------------------------------------------------------------
for pair in "检测模型:$GATE_MODEL" "标签:$GATE_LABELS" \
            "OCR模型:$GATE_OCR" "OCR字典:$GATE_OCR_DICT"; do
    name="${pair%%:*}"; path="${pair#*:}"
    if [ -z "$path" ]; then
        fail "2. $name 未配置"
    elif [ -r "$path" ]; then
        ok "2. $name 可读: $path ($(stat -c %s "$path" 2>/dev/null) 字节)"
    else
        fail "2. $name 读不到: $path"
    fi
done

# ---------------------------------------------------------------------------
# 3. librknnrt 版本必须 2.3.2  (基线【坑位 01】)
#    ★ 版本串实测为: librknnrt version: 2.3.2 (429f97ae6b@2025-04-09T09:09:27)
#      toolkit 2.5.0 编的模型在 2.3.2 runtime 上会加载失败或推理全零。
# ---------------------------------------------------------------------------
RKNN_VER="2.3.2"
_found_ver=""
_found_lib=""
for d in $(echo "${GATE_LD_LIBRARY_PATH:-/usr/lib}" | tr ':' ' '); do
    [ -r "$d/librknnrt.so" ] || continue
    v=$(grep -a -o -m1 'librknnrt version: [0-9][0-9.]*' "$d/librknnrt.so" 2>/dev/null \
        | head -1 | sed 's/.*version: //')
    [ -n "$v" ] || continue
    _found_ver="$v"; _found_lib="$d/librknnrt.so"
    break
done
if [ -z "$_found_lib" ]; then
    fail "3. 找不到 librknnrt.so (搜过: ${GATE_LD_LIBRARY_PATH})"
elif [ "$_found_ver" = "$RKNN_VER" ]; then
    ok "3. librknnrt 版本 $_found_ver (期望 $RKNN_VER): $_found_lib"
else
    fail "3. librknnrt 版本 $_found_ver != 期望 $RKNN_VER (坑位01: 模型会加载失败): $_found_lib"
fi

# ---------------------------------------------------------------------------
# 4. 关键动态库
#    ★ librga 在 third_party/lib; librockchip_mpp 在 /usr/lib/aarch64-linux-gnu
#      (CMakeLists.txt:130,139 钉死这个路径, 换成 third_party 里的会 SIGSEGV)
# ---------------------------------------------------------------------------
_libs_ok=1
_lib_search() {   # $1=库名 -> 在 GATE_LD_LIBRARY_PATH 里找, 找到返回 0
    local d
    for d in $(echo "${GATE_LD_LIBRARY_PATH:-/usr/lib}" | tr ':' ' '); do
        [ -r "$d/$1" ] && return 0
    done
    return 1
}
for pat in "librknnrt.so" "librga.so"; do
    if _lib_search "$pat"; then
        ok "4. 动态库就位: $pat"
    else
        fail "4. 找不到动态库: $pat"
        _libs_ok=0
    fi
done
if [ -r /usr/lib/aarch64-linux-gnu/librockchip_mpp.so ]; then
    ok "4. 动态库就位: librockchip_mpp.so (/usr/lib/aarch64-linux-gnu)"
elif ls /usr/lib/aarch64-linux-gnu/librockchip_mpp.so* >/dev/null 2>&1; then
    ok "4. 动态库就位: librockchip_mpp.so (带版本号)"
else
    fail "4. 找不到 librockchip_mpp.so —— MPP 硬解会静默关闭, 退化成软解"
    _libs_ok=0
fi

# ---------------------------------------------------------------------------
# 5. NPU 设备节点
#    ★★ 文章原文查 /dev/rknpu —— 【本板没有这个节点】!
#      RKNPU 驱动 0.9.8 走的是 DRM 后端, 节点是 /dev/dri/renderD129。
#      照抄原文会永远 FAIL, 服务永远起不来。这里按实况查。
# ---------------------------------------------------------------------------
if ls /dev/dri/renderD12[89] /dev/dri/renderD130 >/dev/null 2>&1; then
    ok "5. NPU 节点就位: $(ls /dev/dri/renderD12[89] /dev/dri/renderD130 2>/dev/null | tr '\n' ' ')(DRM 后端)"
elif [ -e /dev/rknpu ]; then
    ok "5. NPU 节点就位: /dev/rknpu"
else
    fail "5. 找不到 NPU 节点 (/dev/dri/renderD12[89]|renderD130 或 /dev/rknpu)"
fi

# ---------------------------------------------------------------------------
# 6. dma_heap (零拷贝的落脚点: RGA 目标 = NPU 输入 = 同一块 dma-buf)
# ---------------------------------------------------------------------------
if [ -e /dev/dma_heap/system ]; then
    ok "6. dma_heap 就位: $(ls /dev/dma_heap/ 2>/dev/null | tr '\n' ' ')"
else
    fail "6. 找不到 /dev/dma_heap/system —— 零拷贝路径不可用"
fi

# ---------------------------------------------------------------------------
# 7. 4 路源可用
#    本地文件 -> 存在可读 ; rtsp:// -> TCP 连通性探测
# ---------------------------------------------------------------------------
_ch_n=0
for i in 1 2 3 4; do
    var="GATE_CH$i"
    url="${!var:-}"
    kind_var="GATE_CH${i}_KIND"; kind="${!kind_var:-}"
    [ -z "$url" ] && { fail "7. 通道$i 未配置($var)"; continue; }
    _ch_n=$((_ch_n + 1))
    case "$url" in
        rtsp://*|rtsps://*)
            hostport="${url#*://}"; hostport="${hostport%%/*}"
            host="${hostport%%:*}"; port="${hostport##*:}"
            [ "$port" = "$host" ] && port=554
            if timeout 2 bash -c "echo >/dev/tcp/$host/$port" 2>/dev/null; then
                ok "7. 通道$i (${kind:-?}) RTSP 可连: $host:$port"
            else
                fail "7. 通道$i (${kind:-?}) RTSP 连不上: $host:$port"
            fi
            ;;
        *)
            if [ -r "$url" ]; then
                ok "7. 通道$i (${kind:-?}) 文件可读: $url"
            else
                fail "7. 通道$i (${kind:-?}) 文件读不到: $url"
            fi
            ;;
    esac
    # high 通道必须有 IPM 标定点, 否则测速功能是哑的
    if [ "$kind" = "high" ]; then
        ipm_var="GATE_CH${i}_IPM"; ipm="${!ipm_var:-}"
        if [ -z "$ipm" ]; then
            fail "7. 通道$i 是 high(测速)但没配 IPM 标定点($ipm_var)"
        else
            ok "7. 通道$i IPM 标定点已配: $ipm"
        fi
    fi
done
[ "$_ch_n" = "4" ] || fail "7. 通道数是 $_ch_n, 期望 4"

# ---------------------------------------------------------------------------
# 8. 日志可写
#    ★ 判据是【目标日志文件可写】, 不是"目录可写":
#      本板 /var/log 属 root:syslog (drwxr-xr-x), 普通用户【建不了】新文件,
#      但 systemd 的 StandardOutput=append: 写的是 install 脚本预建好、
#      属主为服务用户的那些文件 —— 那才是应用真正要写的东西。
#    ★ 同时兜一下"目录可写"的情况(自定义 GATE_LOG_DIR 时)。
# ---------------------------------------------------------------------------
_logfiles_ok=1
for lf in "$LOG_DIR/rk-gate.log" "$LOG_DIR/rk-gate-error.log"; do
    if [ ! -e "$lf" ]; then
        fail "8. 日志文件不存在: $lf (跑 install_gate_service.sh 会预建它)"
        _logfiles_ok=0
    elif [ ! -w "$lf" ]; then
        fail "8. 日志文件不可写: $lf (属主/权限不对)"
        _logfiles_ok=0
    fi
done
if [ "$_logfiles_ok" = "1" ]; then
    ok "8. 日志文件可写: $LOG_DIR/rk-gate.log, $LOG_DIR/rk-gate-error.log"
elif mkdir -p "$LOG_DIR" 2>/dev/null && touch "$LOG_DIR/.preflight_write_test" 2>/dev/null; then
    rm -f "$LOG_DIR/.preflight_write_test"
    ok "8. 日志目录可写: $LOG_DIR"
else
    fail "8. 日志既不可写也不存在: $LOG_DIR"
fi

# ---------------------------------------------------------------------------
# 9. 日志盘剩余空间
#    ★★ 本板 /var/log 是 47MB 的 zram 内存盘, 而且 /var/log/syslog +
#      kern.log 还在自己长。日志写满 -> write() 返回 ENOSPC -> 应用可能崩。
#      所以这一项必须查, 不能省。
# ---------------------------------------------------------------------------
_free_kb=$(df -Pk "$LOG_DIR" 2>/dev/null | awk 'NR==2{print $4}')
if [ -z "${_free_kb:-}" ]; then
    warn "9. 查不到 $LOG_DIR 的剩余空间"
elif [ "$_free_kb" -lt 8192 ]; then
    fail "9. 日志盘剩余空间不足 8MB ($(( _free_kb / 1024 ))MB) —— 写满会让应用 write() 失败"
elif [ "$_free_kb" -lt 32768 ]; then
    warn "9. 日志盘剩余空间偏紧 ($(( _free_kb / 1024 ))MB) —— 建议 GATE_LOG_DIR 指向真实磁盘"
else
    ok "9. 日志盘剩余 $(( _free_kb / 1024 ))MB: $LOG_DIR"
fi

# ---------------------------------------------------------------------------
# 10. 配置完整性
# ---------------------------------------------------------------------------
for v in GATE_DET_CORES GATE_OCR_CORES GATE_MAX_FPS GATE_SN; do
    if [ -z "${!v:-}" ]; then
        fail "10. 配置项 $v 为空"
    fi
done
# 核掩码不能是 0x7(三核全开) —— 驱动层已禁用, 独立压测台实测吞吐只有 1/2.6
case "${GATE_DET_CORES:-}" in *7*) warn "10. GATE_DET_CORES=${GATE_DET_CORES} 含 7 —— 三核全开已禁用";; esac
ok "10. 核心配置项齐全"

# ---------------------------------------------------------------------------
# 10b. 上报 IP 与本机网卡对不对得上 (R12)
#
#   ★ 为什么必须查: 上报地址写死或过期【不会报错】, 只是发给后端的设备 IP 是错的 ——
#     现场表现是"设备在线但数据看不到", 极难定位。
#     实测踩过: gate.env 写 192.168.31.77, 而板子早已换到 192.168.8.77,
#     线上进程带着错的 --ipaddr 跑了很久。
#
#   规则:
#     · GATE_IPADDR=auto  -> 现在解析一次, 取得到就算过(取不到则 FAIL,
#                            因为 auto 的意义就是"一定要有一个真地址")
#     · 写了具体 IP      -> 必须出现在本机网卡地址里, 否则 FAIL
# ---------------------------------------------------------------------------
_nic_ips=$(ip -4 -o addr show scope global 2>/dev/null |
           awk '{split($4,a,"/"); print a[1]}' | grep -v '^127\.0\.0\.1$' | tr '\n' ' ')
_auto_ip=$(ip -4 route get 1.1.1.1 2>/dev/null |
           sed -n 's/.* src \([0-9.]*\).*/\1/p' | head -1)
if [ -z "$_auto_ip" ]; then
    _auto_ip=$(ip -4 -o addr show scope global 2>/dev/null |
               awk '{split($4,a,"/"); if (a[1] != "127.0.0.1") {print a[1]; exit}}')
fi
case "${GATE_IPADDR:-}" in
    auto|"")
        if [ -n "$_auto_ip" ]; then
            ok "10b. GATE_IPADDR=auto -> 启动时自动取到 ${_auto_ip}"
        else
            fail "10b. GATE_IPADDR=auto 但本机取不到 IPv4(网卡没起来?) —— 上报地址会退成 127.0.0.1"
        fi
        ;;
    *)
        if echo "$_nic_ips" | grep -qw -- "${GATE_IPADDR}"; then
            ok "10b. 上报地址 ${GATE_IPADDR} 与本机网卡一致 (${_nic_ips% })"
        else
            fail "10b. 上报地址 ${GATE_IPADDR} 不在本机网卡地址里 (本机: ${_nic_ips:-无}) —— 改用 GATE_IPADDR=auto 或写成真实地址"
        fi
        ;;
esac

# ---------------------------------------------------------------------------
# 11. MPP 服务节点 (可降级 -> WARN)
# ---------------------------------------------------------------------------
if [ -e /dev/mpp_service ]; then
    ok "11. MPP 服务节点就位: /dev/mpp_service"
else
    warn "11. 没有 /dev/mpp_service —— MPP 硬解不可用, 会退化成软解(性能腰斩)"
fi

# ---------------------------------------------------------------------------
# 12. MQTT broker 连通性 (可降级 -> WARN)
# ---------------------------------------------------------------------------
if [ -z "${GATE_MQTT_HOST:-}" ]; then
    warn "12. 未配置 MQTT broker —— 只本地计数, 不外发"
elif timeout 2 bash -c "echo >/dev/tcp/${GATE_MQTT_HOST}/${GATE_MQTT_PORT:-1883}" 2>/dev/null; then
    ok "12. MQTT broker 可连: ${GATE_MQTT_HOST}:${GATE_MQTT_PORT:-1883}"
else
    warn "12. MQTT broker 连不上: ${GATE_MQTT_HOST}:${GATE_MQTT_PORT:-1883}"
fi

# ---------------------------------------------------------------------------
# 汇总
# ---------------------------------------------------------------------------
log_line "----------------------------------------------------------------"
log_line " 结果: FAIL=$_fail  WARN=$_warn"
if [ "$_fail" -gt 0 ]; then
    log_line " 判定: 拒绝启动 (有 FAIL 项)"
else
    log_line " 判定: 允许启动"
fi
log_line "================================================================"

# ---- 落盘(失败不影响启动判定: 日志写不进去不该拦住服务) ----
if mkdir -p "$LOG_DIR" 2>/dev/null && touch "$PREFLIGHT_LOG" 2>/dev/null; then
    printf '%s\n' "${_lines[@]}" >> "$PREFLIGHT_LOG" 2>/dev/null || true
fi

# 78 = EX_CONFIG: 配错了就别重试了, 交给看门狗带退避地重试
[ "$_fail" -gt 0 ] && exit 78
exit 0
