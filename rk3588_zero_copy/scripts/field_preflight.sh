#!/usr/bin/env bash
# ============================================================================
# field_preflight.sh —— P2-B 现场联调【一键预检】
#
# 干什么:
#   在切到真实 RTSP 之前, 把"一定会踩的坑"提前踩一遍:
#     1) 每一路 RTSP 的连通性 / 编码格式 / 分辨率 / 位深 / 帧率
#     2) 10bit HEVC Main10 —— 【硬拦截】, 并给出明确的整改动作
#     3) MQTT broker 的 TCP 连通性(可选再发一条测试消息)
#     4) 给高位通道抓 1 张原始分辨率静态图, 现场直接照着拾 4 个 IPM 点
#
# ★ 只读: 本脚本【不写任何系统配置、不重启服务、不动 NPU】。
#   它只做: 读配置 / 打开网络连接 / 读流信息 / 往 --grab-ipm 指定的目录写图。
#
# ★ 探针后端(两条路, 输出格式完全一致, 所以解析逻辑只有一套):
#     优先 ffprobe(现场装了 ffmpeg 的机器);
#     没有就自动用本工程自带的 tools/field_probe
#     (用工程本来就依赖的 libavformat/libavcodec 编的, 不需要 root, 见
#      tools/build_field_probe.sh —— 本板就没装 ffmpeg)。
#
# 用法:
#   scripts/field_preflight.sh                          # 用 config/gate.env 的 4 路
#   scripts/field_preflight.sh --grab-ipm /tmp/ipm      # 顺带给高位通道抓图
#   scripts/field_preflight.sh rtsp://ip:554/xxx        # 直接探一路
#   scripts/field_preflight.sh --config config/gate.env.field \
#                              --mqtt-host 192.168.8.21 --publish-test
#
# 退出码:
#   0 = 全部通过(可以有 WARN)
#   1 = 有 FAIL(至少一条硬拦截)
#   2 = 用法错 / 缺工具
# ============================================================================
set -u

PROG="$(basename "$0")"
HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/.." && pwd)"

# ------------------------------------------------------------------ 输出
if [ -t 1 ] && [ "${NO_COLOR:-}" = "" ]; then
    C_RED=$'\033[1;31m'; C_GRN=$'\033[1;32m'; C_YEL=$'\033[1;33m'
    C_BLU=$'\033[1;36m'; C_DIM=$'\033[2m';    C_RST=$'\033[0m'
else
    C_RED=""; C_GRN=""; C_YEL=""; C_BLU=""; C_DIM=""; C_RST=""
fi

N_FAIL=0
N_WARN=0
N_OK=0

say()  { printf '%s\n' "$*"; }
ok()   { N_OK=$((N_OK+1));   printf '%s[ OK ]%s %s\n'   "$C_GRN" "$C_RST" "$*"; }
warn() { N_WARN=$((N_WARN+1)); printf '%s[WARN]%s %s\n' "$C_YEL" "$C_RST" "$*"; }
fail() { N_FAIL=$((N_FAIL+1)); printf '%s[FAIL]%s %s\n' "$C_RED" "$C_RST" "$*"; }
info() { printf '%s[ .. ]%s %s\n' "$C_DIM" "$C_RST" "$*"; }
head1() { printf '\n%s== %s ==%s\n' "$C_BLU" "$*" "$C_RST"; }

usage() {
    sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}

# ------------------------------------------------------------------ 参数
CONFIG=""
GRAB_DIR=""
GRAB_CH=""
MQTT_HOST=""
MQTT_PORT=""
MQTT_QOS=""
MQTT_SN=""
PUBLISH_TEST=0
TIMEOUT=15
declare -a CLI_URLS=()

while [ $# -gt 0 ]; do
    case "$1" in
        --config|--env-file) CONFIG="${2:-}"; shift 2 ;;
        --grab-ipm)          GRAB_DIR="${2:-}"; shift 2 ;;
        --grab-ch)           GRAB_CH="${2:-}"; shift 2 ;;
        --mqtt-host)         MQTT_HOST="${2:-}"; shift 2 ;;
        --mqtt-port)         MQTT_PORT="${2:-}"; shift 2 ;;
        --mqtt-qos)          MQTT_QOS="${2:-}"; shift 2 ;;
        --mqtt-sn)           MQTT_SN="${2:-}"; shift 2 ;;
        --publish-test)      PUBLISH_TEST=1; shift ;;
        --timeout)           TIMEOUT="${2:-}"; shift 2 ;;
        -h|--help)           usage ;;
        --)                  shift; while [ $# -gt 0 ]; do CLI_URLS+=("$1"); shift; done; break ;;
        -*)                  say "未知参数: $1"; usage ;;
        *)                   CLI_URLS+=("$1"); shift ;;
    esac
done

# ------------------------------------------------------------------ 配置读取
# ★ 只用 grep/sed 取值, 【不 source】配置文件 ——
#   配置文件是被"读"的数据, 不是要被执行的脚本。
cfg_get() {
    local key="$1" file="$2" val
    [ -r "$file" ] || return 1
    val="$(sed -n "s/^[[:space:]]*${key}=//p" "$file" | tail -1)"
    # 去掉可能的首尾引号
    val="${val%\"}"; val="${val#\"}"
    val="${val%\'}"; val="${val#\'}"
    printf '%s' "$val"
}

if [ -z "$CONFIG" ]; then
    for cand in "$PROJ/config/gate.env" "$PROJ/config/gate.env.field" \
                "$PROJ/config/gate.env.field.example"; do
        if [ -r "$cand" ]; then CONFIG="$cand"; break; fi
    done
fi

# ------------------------------------------------------------------ 探针后端
PROBE_BACKEND=""
FFPROBE_BIN="$(command -v ffprobe 2>/dev/null || true)"
FFMPEG_BIN="$(command -v ffmpeg 2>/dev/null || true)"
FIELD_PROBE="$PROJ/tools/field_probe"

ensure_field_probe() {
    [ -x "$FIELD_PROBE" ] && return 0
    info "field_probe 不存在, 正在用工程自带的 libav* 现场编译(不需要 root)..."
    if ! bash "$PROJ/tools/build_field_probe.sh" >/dev/null 2>&1; then
        return 1
    fi
    [ -x "$FIELD_PROBE" ]
}

choose_backend() {
    if [ -n "$FFPROBE_BIN" ]; then
        PROBE_BACKEND="ffprobe"
    elif ensure_field_probe; then
        PROBE_BACKEND="field_probe"
    else
        return 1
    fi
    return 0
}

# ------------------------------------------------------------------ 位深判定
# 依据: bits_per_raw_sample 优先; 为 0/N/A 时按 pix_fmt 判; 再兜底看 profile。
infer_bit_depth() {
    local bps="${1:-0}" pix="${2:-}" profile="${3:-}"
    case "$bps" in
        ''|0|N/A|n/a) ;;
        *) printf '%s' "$bps"; return ;;
    esac
    case "$pix" in
        *10le|*10be) printf '10'; return ;;
        *12le|*12be) printf '12'; return ;;
        *14le|*14be) printf '14'; return ;;
        *16le|*16be) printf '16'; return ;;
    esac
    case "$pix" in
        yuv420p|yuvj420p|yuv422p|yuvj422p|yuv444p|yuva420p|gray|rgb24|bgr24|nv12|nv21)
            printf '8'; return ;;
    esac
    case "$profile" in
        *"Main 10"*|*"Main10"*) printf '10'; return ;;
        *"Main 12"*|*"Main12"*) printf '12'; return ;;
    esac
    printf '0'
}

# 是不是 10bit HEVC Main10 —— 这是本次预检要【硬拦】的那一类源
is_main10_hevc() {
    local codec="${1:-}" profile="${2:-}" depth="${3:-0}"
    [ "$codec" = "hevc" ] || return 1
    case "$profile" in
        *"Main 10"*|*"Main10"*) return 0 ;;
    esac
    case "$depth" in
        ''|0) return 1 ;;
        *) [ "$depth" -ge 10 ] && return 0 ;;
    esac
    return 1
}

# ------------------------------------------------------------------ 探测一路
# 输出到全局变量: P_CODEC P_PROFILE P_W P_H P_PIX P_DEPTH P_FPS P_RAW
declare P_CODEC="" P_PROFILE="" P_W="" P_H="" P_PIX="" P_DEPTH="" P_FPS="" P_RAW=""

probe_one() {
    local url="$1" out=""
    P_CODEC=""; P_PROFILE=""; P_W=""; P_H=""; P_PIX=""; P_DEPTH=""; P_FPS=""; P_RAW=""

    if [ "$PROBE_BACKEND" = "ffprobe" ]; then
        out="$(timeout "$TIMEOUT" "$FFPROBE_BIN" \
                 -v error -rtsp_transport tcp \
                 -select_streams v:0 \
                 -show_entries stream=codec_name,profile,width,height,pix_fmt,bits_per_raw_sample,avg_frame_rate,r_frame_rate \
                 -show_entries format=format_name,duration \
                 -of default=noprint_wrappers=1 "$url" 2>/dev/null)"
    else
        out="$(timeout "$TIMEOUT" "$FIELD_PROBE" probe "$url" 2>/dev/null)"
    fi

    local k v
    while IFS= read -r line; do
        case "$line" in
            *=*) k="${line%%=*}"; v="${line#*=}"; P_RAW="${P_RAW}${line}"$'\n' ;;
        esac
    done <<< "$out"

    # 从原始输出里取字段(兼容两种后端的 key 命名)
    P_CODEC="$(printf '%s' "$P_RAW" | sed -n 's/^codec_name=//p' | tail -1)"
    P_PROFILE="$(printf '%s' "$P_RAW" | sed -n 's/^profile=//p' | tail -1)"
    P_W="$(printf '%s' "$P_RAW" | sed -n 's/^width=//p' | tail -1)"
    P_H="$(printf '%s' "$P_RAW" | sed -n 's/^height=//p' | tail -1)"
    P_PIX="$(printf '%s' "$P_RAW" | sed -n 's/^pix_fmt=//p' | tail -1)"
    local bps; bps="$(printf '%s' "$P_RAW" | sed -n 's/^bits_per_raw_sample=//p' | tail -1)"
    local afr; afr="$(printf '%s' "$P_RAW" | sed -n 's/^avg_frame_rate=//p' | tail -1)"
    local fps; fps="$(printf '%s' "$P_RAW" | sed -n 's/^avg_fps=//p' | tail -1)"
    [ -z "$fps" ] && [ -n "$afr" ] && [ "${afr#*/}" != "0" ] && \
        fps="$(awk -v n="${afr%%/*}" -v d="${afr#*/}" 'BEGIN{ if (d+0>0) printf "%.3f", n/d; else print "0" }')"

    P_DEPTH="$(infer_bit_depth "$bps" "$P_PIX" "$P_PROFILE")"
    P_FPS="${fps:-0}"

    [ -n "$P_CODEC" ]
}

# ------------------------------------------------------------------ 抓一帧
grab_frame() {
    local url="$1" out="$2"
    if [ -n "$FFMPEG_BIN" ]; then
        timeout "$TIMEOUT" "$FFMPEG_BIN" -y -loglevel error -rtsp_transport tcp \
            -i "$url" -frames:v 1 -q:v 2 "$out" >/dev/null 2>&1
    else
        timeout $((TIMEOUT * 3)) "$FIELD_PROBE" grab "$url" "$out" >/dev/null 2>&1
    fi
    [ -s "$out" ]
}

# ================================================================== 主流程
say "============================================================"
say " RK3588 现场预检 (field_preflight)"
say " 时间: $(date '+%F %T %Z')"
say " 工程: $PROJ"
say "============================================================"

# ---- 0) 工具 ----
head1 "0. 工具链"
if [ -n "$FFPROBE_BIN" ]; then
    ok "ffprobe: $FFPROBE_BIN"
else
    info "ffprobe: 没装(本板默认状态)"
fi
if ensure_field_probe; then
    ok "field_probe: $FIELD_PROBE (兜底探针, 零额外依赖)"
else
    fail "field_probe 编不出来, 且系统没有 ffprobe —— 没有可用探针"
    say ""
    say "装 ffmpeg(需 root):  sudo apt install ffmpeg"
    say "或者装 FFmpeg 开发库(需 root):"
    say "  sudo apt install libavformat-dev libavcodec-dev libavutil-dev libswscale-dev"
    exit 2
fi
if ! choose_backend; then
    fail "选不出探针后端"
    exit 2
fi
info "探针后端: $PROBE_BACKEND"

# ---- 1) 通道清单 ----
head1 "1. 通道清单"
declare -a CH_URL=() CH_KIND=() CH_LABEL=()

if [ ${#CLI_URLS[@]} -gt 0 ]; then
    ci=1
    for u in "${CLI_URLS[@]}"; do
        CH_URL+=("$u"); CH_KIND+=("cli"); CH_LABEL+=("ch${ci}")
        ci=$((ci+1))
    done
    info "来源: 命令行传入 ${#CLI_URLS[@]} 路"
else
    if [ -z "$CONFIG" ] || [ ! -r "$CONFIG" ]; then
        fail "没给通道来源: 命令行没传 URL, 默认的 config/gate.env 也不可读"
        say "  用法: $PROG rtsp://... 或 $PROG --config <你的 gate.env>"
        exit 2
    fi
    info "来源: $CONFIG"
    for i in 1 2 3 4; do
        u="$(cfg_get "GATE_CH$i" "$CONFIG")"
        [ -z "$u" ] && continue
        k="$(cfg_get "GATE_CH${i}_KIND" "$CONFIG")"
        CH_URL+=("$u"); CH_KIND+=("${k:-near}"); CH_LABEL+=("ch$i")
    done
fi

if [ ${#CH_URL[@]} -eq 0 ]; then
    fail "配置里一路通道都没有(GATE_CH1..GATE_CH4 全空)"
    exit 1
fi
for i in "${!CH_URL[@]}"; do
    say "  ${CH_LABEL[$i]}: ${CH_URL[$i]}  [KIND=${CH_KIND[$i]}]"
done

# ---- 2) 逐路探测 ----
head1 "2. 视频源探测"
declare -a CH_IS_MAIN10=()

for i in "${!CH_URL[@]}"; do
    url="${CH_URL[$i]}"
    say ""
    say "--- ${CH_LABEL[$i]}: $url"
    if ! probe_one "$url"; then
        fail "${CH_LABEL[$i]} 连不上 / 读不出流信息 —— 查地址、端口、用户名密码、网段"
        CH_IS_MAIN10+=("no")
        continue
    fi

    ok "${CH_LABEL[$i]} 连通  编码=${P_CODEC:-?}  ${P_W:-?}x${P_H:-?}  像素=${P_PIX:-?}  位深=${P_DEPTH:-?}bit  帧率=${P_FPS:-?}fps"

    # --- 编码 ---
    case "$P_CODEC" in
        h264|hevc) : ;;
        "") fail "${CH_LABEL[$i]} 读不出编码格式" ;;
        *)  warn "${CH_LABEL[$i]} 编码是 $P_CODEC(不是 h264/hevc) —— 本板 MPP 只认 H.264/H.265" ;;
    esac

    # --- ★ 10bit HEVC Main10: 硬拦截 ---
    if is_main10_hevc "$P_CODEC" "$P_PROFILE" "$P_DEPTH"; then
        CH_IS_MAIN10+=("yes")
        fail "${CH_LABEL[$i]} 是 10bit HEVC Main10(profile=${P_PROFILE}, pix_fmt=${P_PIX}, ${P_DEPTH}bit)"
        say "        ${C_RED}本板 MPP/RGA 硬件路径不支持 10bit HEVC，降软解仅约 10 FPS，请先在相机/NVR 后台切换为 8bit H.264 或 8bit H.265 Main Profile${C_RST}"
        say "        ${C_DIM}依据: docs/p2a_stability_hardening_report.md §七-1(P2-A1 实测: 软解 9~10ms/帧 + CPU 色彩转换 9~10ms/帧 => ~10 FPS 交付率; MPP 路径是 20 FPS)${C_RST}"
        say "        ${C_DIM}注: 生产代码已做位深探测+拒收护栏, 10bit 源会走软解而不是静默零检出(P2-A1), 但吞吐只有 1/2, 4 路并发会顶不住${C_RST}"
    else
        CH_IS_MAIN10+=("no")
        # 非 10bit, 但只要不是 8bit 也值得提醒
        if [ "${P_DEPTH:-0}" != "0" ] && [ "${P_DEPTH:-0}" != "8" ]; then
            warn "${CH_LABEL[$i]} 位深是 ${P_DEPTH}bit(不是 8bit 也不是 10bit) —— 请确认 MPP 路径是否支持"
        fi
    fi

    # --- 分辨率 ---
    if [ -n "$P_W" ] && [ "$P_W" -gt 0 ] 2>/dev/null; then
        if [ "$P_W" -lt 640 ]; then
            warn "${CH_LABEL[$i]} 宽度只有 ${P_W}px(< 640) —— 车牌 ROI 会切得很窄, 识别率上不去"
        else
            info "${CH_LABEL[$i]} 分辨率 ${P_W}x${P_H}(模型输入 640x640, 管线内部会缩放; 原始分辨率用于 IPM 拾点)"
        fi
    fi

    # --- 帧率 ---
    if [ -n "$P_FPS" ] && [ "$P_FPS" != "0" ]; then
        if awk -v f="$P_FPS" 'BEGIN{exit !(f+0 < 5)}'; then
            warn "${CH_LABEL[$i]} 帧率只有 ${P_FPS} fps(< 5) —— 确认相机/NVR 的码流配置"
        else
            info "${CH_LABEL[$i]} 帧率 ${P_FPS} fps(生产限帧 --max-fps 20, 所以 20 以上不浪费)"
        fi
    else
        warn "${CH_LABEL[$i]} 读不出帧率 —— 可能是变帧率流或探测数据不足, 请人工确认"
    fi
done

# ---- 3) MQTT ----
head1 "3. MQTT Broker"
if [ -z "$MQTT_HOST" ] && [ -n "$CONFIG" ] && [ -r "$CONFIG" ]; then
    MQTT_HOST="$(cfg_get "GATE_MQTT_HOST" "$CONFIG")"
    [ -z "$MQTT_PORT" ] && MQTT_PORT="$(cfg_get "GATE_MQTT_PORT" "$CONFIG")"
    [ -z "$MQTT_QOS" ]  && MQTT_QOS="$(cfg_get "GATE_MQTT_QOS" "$CONFIG")"
    [ -z "$MQTT_SN" ]   && MQTT_SN="$(cfg_get "GATE_SN" "$CONFIG")"
fi
MQTT_PORT="${MQTT_PORT:-1883}"
MQTT_QOS="${MQTT_QOS:-1}"
MQTT_SN="${MQTT_SN:-RK3588-GATE-01}"

if [ -z "$MQTT_HOST" ]; then
    info "没配 MQTT broker(GATE_MQTT_HOST 为空)—— 本次跳过 MQTT 检查"
    info "现场填了 broker 的话, 看门狗的 MQTT 探针才会生效(见 scripts/gate_watchdog.sh)"
else
    say "  broker = ${MQTT_HOST}:${MQTT_PORT}  qos=${MQTT_QOS}  sn=${MQTT_SN}"
    MQTT_PY="$HERE/field_mqtt_probe.py"
    if ! command -v python3 >/dev/null 2>&1; then
        warn "没有 python3 —— 跳过 MQTT 探测"
    elif [ ! -r "$MQTT_PY" ]; then
        warn "找不到 $MQTT_PY —— 跳过 MQTT 探测"
    else
        MQTT_ARGS=(--host "$MQTT_HOST" --port "$MQTT_PORT" --timeout "$TIMEOUT")
        if [ "$PUBLISH_TEST" = "1" ]; then
            # ★ 用【预检专用】topic, 绝不发到生产 topic 上 ——
            #   生产 topic 是 device/<sn>/message/up/ivs_result 之类,
            #   在预检阶段发假数据到那里会被后端当真的检测结果收下。
            MQTT_ARGS+=(--qos "$MQTT_QOS"
                        --topic "device/${MQTT_SN}/message/up/preflight_test"
                        --message "field preflight $(date '+%F %T') from $(hostname)")
        fi
        MQTT_OUT="$(python3 "$MQTT_PY" "${MQTT_ARGS[@]}" 2>&1)"
        MQTT_RC=$?
        printf '%s\n' "$MQTT_OUT" | sed 's/^/    /'
        if [ "$MQTT_RC" -eq 0 ]; then
            if [ "$PUBLISH_TEST" = "1" ]; then
                ok "MQTT 连通且发布成功(topic=device/${MQTT_SN}/message/up/preflight_test)"
            else
                ok "MQTT TCP + CONNECT 正常"
            fi
        else
            fail "MQTT 探测失败 —— 查 broker 地址/端口/防火墙/账号密码"
        fi
    fi
fi

# ---- 4) IPM 抓图 ----
if [ -n "$GRAB_DIR" ]; then
    head1 "4. IPM 拾点静态图"
    mkdir -p "$GRAB_DIR" || { fail "建目录失败: $GRAB_DIR"; }
    for i in "${!CH_URL[@]}"; do
        ch="${CH_LABEL[$i]}"
        # 默认只抓 high(高位)通道; CLI 传 URL 时没有 KIND, 全部抓
        want=0
        if [ -n "$GRAB_CH" ]; then
            case ",${GRAB_CH}," in *",${ch},"*) want=1 ;; esac
        elif [ "${CH_KIND[$i]}" = "high" ]; then
            want=1
        elif [ "${CH_KIND[$i]}" = "cli" ]; then
            want=1
        fi
        if [ "$want" -ne 1 ]; then
            info "${ch} 跳过(KIND=${CH_KIND[$i]}, 不是 high 通道)"
            continue
        fi
        out="$GRAB_DIR/${ch}_ipm.jpg"
        if grab_frame "${CH_URL[$i]}" "$out"; then
            size="$(stat -c '%s' "$out" 2>/dev/null || echo 0)"
            ok "${ch} 已抓图: $out ($size 字节)"
            say "        ${C_DIM}用图片工具打开, 按【原图像素】量出地面车道 4 个点, 写成 \"x1,y1;x2,y2;x3,y3;x4,y4\" 填进 GATE_CH${ch#ch}_IPM${C_RST}"
        else
            fail "${ch} 抓图失败 —— 确认这一路能出帧(可能是 10bit/编码不支持)"
        fi
    done
    say ""
    info "IPM 四点格式: \"x1,y1;x2,y2;x3,y3;x4,y4\" —— 原图像素坐标, 顺序 = 近左, 近右, 远右, 远左"
    info "换相机/换分辨率必须重标(见 rk3588_zero_copy/docs/现场环境适配指南.md)"
fi

# ================================================================== 汇总
head1 "汇总"
say "  OK   = $N_OK"
say "  WARN = $N_WARN"
say "  FAIL = $N_FAIL"
say ""
if [ "$N_FAIL" -gt 0 ]; then
    printf '%s预检未通过: 有 %d 项 FAIL, 不要切生产。%s\n' "$C_RED" "$N_FAIL" "$C_RST"
    exit 1
fi
printf '%s预检通过: 可以按 docs/p2b_field_cutover_sop.md 切真实 RTSP。%s\n' "$C_GRN" "$C_RST"
exit 0
