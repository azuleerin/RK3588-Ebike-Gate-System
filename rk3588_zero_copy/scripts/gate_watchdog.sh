#!/usr/bin/env bash
# ============================================================================
# gate_watchdog.sh —— rk_gate 服务的看门狗 (systemd 托管, root 常驻)
#
# 它补的是 systemd 自己补不了的洞:
#   systemd 的 Restart=on-failure 只能救"进程崩了"。
#   "进程还活着但业务卡死"(MQTT 线程死锁 / 视频流缓冲卡死 / NPU worker 死等)
#   systemd 一点都看不出来 —— 因为从它的角度看, 进程活得好好的。
#
# ---- 卡死判据(两条, 互补) ----------------------------------------------
#
#  L1 整机心跳 —— 日志文件 mtime 停滞
#     ★ 为什么用 mtime 而不是"最后一行日志里的时间戳":
#         rk_gate 的统计行格式是
#             [ch1][frame 100] fps=... rss=245712KB heap=74775KB
#         【行内没有时间戳】(加时间戳要改 C++, 本方案坚持不改业务代码)。
#         mtime 就是"最后一行被写下的时刻", 与"日志里的时间戳"同义。
#       取 stdout / stderr 两个日志文件里【较新的】那个 mtime —— rk_gate 的
#       业务输出几乎全走 stderr, 只看 stdout 会误判。
#     ★ 阈值 60s: rk_gate 每 100 帧打一行统计(4 路合计约 5 秒一行),
#       60s 留了 10 倍余量, 既不会误报又能及时抓到真卡死。
#
#  L2 单路心跳 —— 帧号是否还在推进
#     L1 只能发现"【整机】不出日志了"。但如果 4 路里有 1 路卡死、
#     另外 3 路还在跑, 日志照样在刷 —— L1 看不出来。
#     L2 解析日志里的 [chN][frame M], 要求【每一路】的帧号都在推进。
#     ★ 日志行里本来就有帧号, 所以这也不用改 C++。
#
#  L1.5 静默卡死 —— 日志还在写, 但【业务帧号】全都不涨
#     ★ 这条补的是 L1 的盲区: L1 只回答"进程还在写日志吗",
#       不回答"业务还在跑吗"。线程全堵死但日志还在写时, L1 永远健康。
#     判据: 日志 mtime 新鲜 + 所有路的帧号都不涨, 持续超过 SILENT_STALL_SEC
#           (默认 60s) -> 判定静默卡死 -> 自愈。
#     ★ 与 L2 的分工: L2 是【单路】判据(默认 180s, 且区分"这路慢"和"整机慢"),
#       L1.5 是【整机】判据(默认 60s), 比 L2 快 —— "全都停"和"某一路慢"是两回事。
#     ★ 顺手兜住 L2 的盲区: 日志尾部一条帧号都解析不到时,
#       L2 的循环等于什么都没查, 也就是"看门狗以为正常"。
#
# ---- 另外三件事 -------------------------------------------------------
#   1) failed 自愈   —— unit 进入 failed 后(systemd 限流不再重试),
#                       看门狗带【指数退避】把它拉起来, 不形成重启风暴。
#   2) 水位告警     —— RSS / 活堆 / 日志盘剩余空间。
#   3) MQTT 探针    —— 可选, 默认关(见文末"为什么默认关")。
#
# ---- 维护窗口 ---------------------------------------------------------
#   sudo touch /run/rk-gate/paused   -> 看门狗闭嘴, 不做任何干预
#   sudo rm   /run/rk-gate/paused   -> 恢复正常看护
#
# ---- 退出/重启 --------------------------------------------------------
#   本脚本自己不负责存活(它由 rk-gate-watchdog.service 的 Restart=always 兜底)
# ============================================================================
set -u

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
ENV_FILE="$PROJ/config/gate.env"
[ -r "$ENV_FILE" ] && . "$ENV_FILE"

UNIT=rk-gate.service
LOG_DIR="${GATE_LOG_DIR:-/var/log}"
OUT_LOG="$LOG_DIR/rk-gate.log"
ERR_LOG="$LOG_DIR/rk-gate-error.log"
ALERT="$LOG_DIR/rk-gate-alerts.log"

RUN_DIR=/run/rk-gate
PAUSE="$RUN_DIR/paused"
mkdir -p "$RUN_DIR" 2>/dev/null || true

# ---- 参数(都能用环境变量覆盖) ----
CHECK_SEC="${CHECK_SEC:-20}"
STALL_SEC="${STALL_SEC:-60}"             # L1 整机心跳阈值(秒)
CH_STALL_SEC="${CH_STALL_SEC:-180}"      # L2 单路帧号停滞阈值(秒)
CH_SLOW_ROUNDS="${CH_SLOW_ROUNDS:-3}"    # 其余路在推进时, 连续几轮停滞才动手
L2_COOLDOWN_SEC="${L2_COOLDOWN_SEC:-60}"  # 重启后的 L2 判定冷却窗口(秒)
SILENT_STALL_SEC="${SILENT_STALL_SEC:-60}" # L1.5 静默卡死阈值: 日志在写但帧号全不涨(秒)
BACKOFF_START="${BACKOFF_START:-60}"     # failed 自愈的首次退避
BACKOFF_MAX="${BACKOFF_MAX:-300}"        # 退避上限
HEALTH_RESET_SEC="${HEALTH_RESET_SEC:-600}"  # 连续健康这么久 -> 退避清零
# ★★★ 水位告警线 —— 2026-11-23 按【修完活堆泄漏 + 页缓存治理后】的
#   实测稳态重新校准。旧值(400/200/8)是【泄漏还在、页缓存把 cgroup 顶满】
#   那个状态下定的, 现在照抄只会天天误报(旧稳态 RSS 631MB / 活堆 489MB)。
#
#   实测稳态(4 路 + max_fps=20 + 4 个 1080p 视频源; 2026-11-23 长跑 46.7 分钟
#   / 56,400 帧每路, 278 次 cgroup 采样, 零重启零告警):
#       RSS   228.6~231.7 MB (anon 84MB + dma-buf shmem 98MB + 其余 ~48MB)
#       活堆   73.3~74.2 MB   (mallinfo2().uordblks)
#       cgroup 190.4~243.4 MB, 均值 209.7 MB
#         (其中 file 页缓存 106.3~157.2MB, 已被 fadvise 限住)
#
#   取值原则 = 实测稳态 × 1.5~2 倍:
#     RSS_WARN_MB=350   (1.5x) —— RSS 含 98MB 的 dma-buf, 瞬时波动比活堆大,
#                                  给的余量要比活堆宽一点
#     HEAP_WARN_MB=150  (2.0x) —— 活堆是最灵敏的泄漏指标, 宁可早报
#                                  ★ 旧泄漏速率 1.4 MB/min -> 越线后约 55 分钟
#                                    才撞到 MemoryMax, 留足排查时间
#     LOG_FREE_WARN_MB=2048 (2GB) —— 日志盘已从 47MB zram 换成 /var/log.hdd
#                                  (105GB 实测剩 78GB)。旧值 8MB 是按 zram 定的,
#                                  照抄等于永远不报警; 2GB 约占盘的 2%, 是
#                                  "再写就危险了"的合理早期信号。
#   ★ 这三个值也可以写在 config/gate.env 里覆盖(看门狗自己 source 那个文件)。
RSS_WARN_MB="${RSS_WARN_MB:-350}"          # RSS 水位告警线
HEAP_WARN_MB="${HEAP_WARN_MB:-150}"        # 活堆水位告警线
LOG_FREE_WARN_MB="${LOG_FREE_WARN_MB:-2048}" # 日志盘剩余空间告警线
# ★ cgroup 内存水位告警线(2026-11-23 新增)。
#   盯的是 cgroup【总量】而不是 RSS —— 因为 2026-11-23 实测发现 cgroup 顶满
#   1G 的主因是【视频页缓存】, 那段时间 RSS 619MB / 活堆 489MB 都看不出问题,
#   真正快撞墙的是 cgroup 里没人看的 file 分量。
#   取值 400: 新配的 MemoryMax=512M 的 ~78%, 实测稳态 209~248MB 不会碰到。
CGROUP_WARN_MB="${CGROUP_WARN_MB:-400}"       # cgroup 内存水位告警线
MQTT_PROBE_SEC="${MQTT_PROBE_SEC:-60}"   # MQTT 探针窗口
LOG_TAIL_BYTES="${LOG_TAIL_BYTES:-200000}"

alert() {
    local msg="$1"
    printf '[%s] 🔴 [rk-gate 看门狗] %s\n' "$(date '+%F %T')" "$msg" >> "$ALERT" 2>/dev/null || true
    printf '[%s] 🔴 [rk-gate 看门狗] %s\n' "$(date '+%F %T')" "$msg" >&2
}
note() {
    printf '[%s] [rk-gate 看门狗] %s\n' "$(date '+%F %T')" "$1" >&2
}

state_get() { cat "$RUN_DIR/$1" 2>/dev/null || echo "$2"; }
state_set() { echo "$2" > "$RUN_DIR/$1" 2>/dev/null || true; }

note "启动: 监控 $UNIT (L1 心跳 ${STALL_SEC}s / L2 单路 ${CH_STALL_SEC}s / 退避 ${BACKOFF_START}~${BACKOFF_MAX}s)"

# ===========================================================================
# 主循环
# ===========================================================================
while true; do
    sleep "$CHECK_SEC"
    now=$(date +%s)

    # ---- 维护窗口: 闭嘴 ----
    if [ -f "$PAUSE" ]; then
        continue
    fi

    # -----------------------------------------------------------------------
    # 1) 服务没在跑 -> 带指数退避拉起
    #
    #    ★★ 判据【不是】 `systemctl is-failed == "failed"` —— 那个判据有致命缺陷:
    #       StartLimit 命中后 unit 变成 inactive/dead, 此时 is-failed 返回的是
    #       "inactive" 而不是 "failed" —— 判据永远不再成立,
    #       看门狗就只自愈了一次就彻底卡死(2026-11-18 实测踩过)。
    #
    #    ★★ 改用 (ActiveState != active) && (Result != success):
    #         人为 systemctl stop  -> Result=success   -> 尊重运维, 不动它
    #         启动失败 / 被限流停下 -> Result=exit-code -> 该拉起就拉起
    #       这两个场景互斥, 是 systemd 的原生语义, 实测确认过。
    # -----------------------------------------------------------------------
    ACTIVE=$(systemctl show -p ActiveState --value "$UNIT" 2>/dev/null)
    RESULT=$(systemctl show -p Result --value "$UNIT" 2>/dev/null)
    if [ "$ACTIVE" != "active" ]; then
        if [ "$RESULT" = "success" ]; then
            # 人为停机 —— 尊重运维意图, 不拉起
            # (维护窗口请用 /run/rk-gate/paused 标记, 那个优先级更高)
            continue
        fi
        last_fail=$(state_get last_fail_ts 0)
        backoff=$(state_get backoff "$BACKOFF_START")
        [ "$backoff" -lt "$BACKOFF_START" ] 2>/dev/null && backoff=$BACKOFF_START
        [ "$backoff" -gt "$BACKOFF_MAX" ] 2>/dev/null && backoff=$BACKOFF_MAX
        if [ $((now - last_fail)) -ge "$backoff" ]; then
            fail_count=$(state_get fail_count 0)
            fail_count=$((fail_count + 1))
            state_set fail_count "$fail_count"
            state_set last_fail_ts "$now"
            nb=$((backoff * 2)); [ "$nb" -gt "$BACKOFF_MAX" ] && nb=$BACKOFF_MAX
            state_set backoff "$nb"
            alert "服务未在运行(ActiveState=$ACTIVE Result=$RESULT, 连续第 $fail_count 次) -> 退避 ${backoff}s 后自动拉起"
            # ★ 必须先 reset-failed: 不清掉 StartLimit 计数器的话,
            #   这次 start 会被 systemd 以 "start request repeated too quickly" 拒掉。
            systemctl reset-failed "$UNIT" 2>/dev/null || true
            systemctl start "$UNIT" 2>&1 | head -3 >&2
            state_set last_start_ts "$now"
            # ★ 不删 ch*_frame / ch*_ts —— 删了下一轮 _lt=0 -> _stuck=epoch -> 立刻误判停滞。
            #   详见 L1 分支的注释(2026-11-19 连环重启 4 次的根因)。
        else
            note "服务未在运行($ACTIVE/$RESULT), 退避中(还需 $((backoff - (now - last_fail)))s)"
        fi
        continue
    fi

    # 健康运行够久 -> 退避清零(下次失败从头来)
    last_fail=$(state_get last_fail_ts 0)
    if [ "$last_fail" -gt 0 ] && [ $((now - last_fail)) -gt "$HEALTH_RESET_SEC" ]; then
        state_set fail_count 0
        state_set backoff "$BACKOFF_START"
        state_set last_fail_ts 0
        note "服务已健康运行 $((now - last_fail))s, 退避计数清零"
    fi

    # -----------------------------------------------------------------------
    # 2) 取主进程 PID
    #    ★ 用 systemd 的 MainPID —— 因为 run_gate_service.sh 用 exec 启动,
    #      所以 MainPID 就是 rk_gate 本身(不是包装脚本的 bash)。
    #    ★ 进程不在 -> systemd 自己会拉起, 看门狗【不插手】, 避免两边打架。
    # -----------------------------------------------------------------------
    pid=$(systemctl show -p MainPID --value "$UNIT" 2>/dev/null)
    case "$pid" in ''|0) pid="" ;; esac
    if [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; then
        continue
    fi

    # ★ 进程换过 -> 进入 L2 判定冷却窗口。
    #   判据用【MainPID 变了】而不是"看门狗自己重启过"—— 因为换进程有三种来源:
    #     ① systemd 因崩溃自动拉起(kill -9)
    #     ② 看门狗自己 restart
    #     ③ 运维手工 restart
    #   三种都要冷却: 新进程要跑满 --stats-interval 帧(约 5~10 秒)才打出第一行
    #   统计, 在那之前日志里"最后一次出现"的帧号还是【旧进程】的值,
    #   拿它跟旧记录比较会误判成停滞。
    #   (2026-11-19 00:29~00:39 连环重启 4 次, 根因就在这个窗口)
    _pid_prev=$(state_get last_pid 0)
    if [ "$_pid_prev" != "$pid" ]; then
        state_set last_pid "$pid"
        state_set last_start_ts "$now"
        note "检测到进程已更换 (PID $_pid_prev -> $pid), 进入 ${L2_COOLDOWN_SEC}s 冷却窗口"
        continue
    fi

    # -----------------------------------------------------------------------
    # 3) L1 整机心跳: 日志 mtime 停滞
    # -----------------------------------------------------------------------
    mt_out=0; mt_err=0
    [ -f "$OUT_LOG" ] && mt_out=$(stat -c %Y "$OUT_LOG" 2>/dev/null || echo 0)
    [ -f "$ERR_LOG" ] && mt_err=$(stat -c %Y "$ERR_LOG" 2>/dev/null || echo 0)
    last=$mt_out
    [ "$mt_err" -gt "$last" ] && last=$mt_err
    age=$((now - last))
    if [ "$age" -gt "$STALL_SEC" ]; then
        alert "L1 整机心跳停了 ${age}s (阈值 ${STALL_SEC}s) —— 判定整机卡死, 重启服务"
        systemctl restart "$UNIT" 2>&1 | head -3 >&2
        state_set last_start_ts "$now"
        # ★★ 这里【绝对不能】删状态文件:
        #    删掉 ch*_ts 之后, 下一轮读到 _lt=0, 于是 _stuck = now(epoch 秒),
        #    远超阈值 -> 立刻又判"停滞" -> 反复重启。
        #    2026-11-19 00:29~00:39 实测踩过: 连环重启 4 次, 每次间隔正好 3 分 25 秒,
        #    每次都报"帧号停在 14700"(其实是状态被删后读到的旧值)。
        #    正确做法: 保留记录, 让下面"帧号回退 = 进程重启过"的分支把 ts 刷新成 now。
        continue
    fi

    # -----------------------------------------------------------------------
    # 4) L2 单路心跳: 每路的帧号是否还在推进
    #
    #    ★★ 关键: 分辨"这路卡死了"和"这路变慢了" ——
    #       2026-11-18 实测踩过: 板子上有别的任务把 NPU 压到 96%,
    #       ch4 从 20fps 掉到 1~2fps -> 100 帧要 50~100 秒 -> 帧号停滞超过阈值,
    #       被误判成"该路卡死"并【重启了整机】。生产上这种误报会反复中断业务。
    #       所以: 先看【其它路还在不在推进】——
    #         · 别的路也在推进 -> 整机是活的, 这路只是慢/源断了 -> 只告警, 连续多轮才动手
    #         · 别的路也不动了 -> 整机性问题, 由 L1 负责, 这里直接重启
    # -----------------------------------------------------------------------
    declare -A FRAME=()
    while read -r _ch _fr; do
        [ -n "${_ch:-}" ] && FRAME[$_ch]=$_fr
    done < <(
        { tail -c "$LOG_TAIL_BYTES" "$OUT_LOG" 2>/dev/null
          tail -c "$LOG_TAIL_BYTES" "$ERR_LOG" 2>/dev/null; } \
          | grep -aoE '\[ch[0-9]+\]\[frame [0-9]+\]' \
          | sed -E 's/\[ch([0-9]+)\]\[frame ([0-9]+)\]/\1 \2/' \
          | awk '{ m[$1] = $2+0 } END { for (c in m) print c, m[c] }'
    )
    # ★★ 这里【必须】取"最后一次出现"的帧号, 不能取【最大值】:
    #     日志尾部同时含【旧进程】和【新进程】的行(append 追加, 重启不截断)。
    #     取 max 的话, 旧进程的大帧号(如 14700)会一直盖住新进程的小帧号,
    #     于是"帧号停在 14700"永远成立 -> 反复误判卡死 -> 反复重启。
    #     2026-11-19 00:29~00:39 实测踩过: 连环重启 4 次, 每次间隔 3 分 25 秒。
    #     取"最后一次出现"则新进程的行在日志末尾 -> 拿到的是新帧号 ->
    #     走"帧号回退 = 进程重启过"分支 -> 正确重置计时。

    # ---- 先数一遍: 有几路还在推进 ----
    _alive=0
    for _ch in "${!FRAME[@]}"; do
        _fr="${FRAME[$_ch]}"
        _lf=$(state_get "ch${_ch}_frame" 0)
        [ "$_fr" != "$_lf" ] && _alive=$((_alive + 1))
    done

    # ★ 重启后的冷却窗口: 新进程要跑满 --stats-interval 帧(约 5~10 秒)才打出
    #   第一行统计, 在那之前日志里"最后一次出现"的帧号还是【旧进程】的值。
    #   这段时间内不做 L2 判定, 免得拿旧帧号误判成停滞。
    _last_start=$(state_get last_start_ts 0)
    if [ $((now - _last_start)) -lt "$L2_COOLDOWN_SEC" ]; then
        continue
    fi

    # -----------------------------------------------------------------------
    # 4.5 L1.5 静默卡死: 日志【还在写】, 但【业务帧号】全都不涨
    #
    #    ★ 为什么 L1(日志 mtime)不够 —— 这是本条存在的理由:
    #      rk_gate 正常运行时日志里一直有东西在写, mtime 几乎永远是"新鲜的"。
    #      哪怕 4 路分析线程全堵死在 futex 上、业务帧号一秒都不涨,
    #      L1 也发现不了 —— 它只回答"进程还在写日志吗", 不回答"业务还在跑吗"。
    #      真正的健康信号是【帧号】。
    #
    #    ★ 与 L2 的分工:
    #      L2 是【单路】判据(CH_STALL_SEC, 默认 180s), 且区分"这路慢"和"整机慢"。
    #      这里补一个【整机】判据(SILENT_STALL_SEC, 默认 60s):
    #      日志还在写 + 所有路的帧号都不涨 -> 静默卡死 -> 自愈。
    #      比 L2 的 180s 快, 因为"全都停"和"某一路慢"是两回事。
    #
    #    ★ 顺手兜住 L2 的一个盲区:
    #      如果日志尾部【一条帧号都解析不到】(帧号行被别的日志刷出去了 /
    #      日志被截断 / 统计行格式变了), L2 那个 for 循环等于什么都没查 ——
    #      也就是"看门狗以为正常"。这里把"解析不到帧号"也计入静默。
    #
    #    ★ 防误报:
    #      - 刚重启完有 L2_COOLDOWN_SEC 的冷却窗口(上面已经 continue 掉了)
    #      - 源是有限 mp4 时, 通道会 rewind 接着放, 帧号不会停
    #      - 日志 mtime 不新鲜时不算静默(那归 L1 管)
    # -----------------------------------------------------------------------
    _log_fresh=0
    [ "$age" -lt "$STALL_SEC" ] && _log_fresh=1
    if [ "$_log_fresh" -eq 1 ] && [ "$_alive" -eq 0 ]; then
        _sil=$(state_get silent_since 0)
        if [ "$_sil" -eq 0 ]; then
            _sil=$now
            state_set silent_since "$now"
            note "静默卡死观察开始: 日志在更新(mtime ${age}s 前), 但帧号没涨"
        fi
        _sil_age=$((now - _sil))
        if [ "$_sil_age" -gt "$SILENT_STALL_SEC" ]; then
            _nch=${#FRAME[@]}
            alert "L1.5 静默卡死: 日志仍在更新(mtime ${age}s 前), 但 ${_nch} 路帧号已 ${_sil_age}s 没有增长 (阈值 ${SILENT_STALL_SEC}s) —— 判定业务线程卡死, 重启服务"
            systemctl restart "$UNIT" 2>&1 | head -3 >&2
            state_set last_start_ts "$now"
            state_set silent_since 0
            continue
        fi
    else
        state_set silent_since 0
    fi

    for _ch in "${!FRAME[@]}"; do
        _fr="${FRAME[$_ch]}"
        _lf=$(state_get "ch${_ch}_frame" 0)
        _lt=$(state_get "ch${_ch}_ts" 0)
        if [ "$_fr" -lt "$_lf" ]; then
            # 帧号回退 = 进程重启过 -> 重新计时
            state_set "ch${_ch}_frame" "$_fr"
            state_set "ch${_ch}_ts" "$now"
            state_set "ch${_ch}_slow" 0
            continue
        fi
        if [ "$_fr" != "$_lf" ]; then
            state_set "ch${_ch}_frame" "$_fr"
            state_set "ch${_ch}_ts" "$now"
            state_set "ch${_ch}_slow" 0
            continue
        fi
        _stuck=$((now - _lt))
        if [ "$_stuck" -gt "$CH_STALL_SEC" ]; then
            if [ "$_alive" -ge 2 ]; then
                # 整机是活的 -> 这路只是慢/源断了 -> 先只告警, 连续多轮才动手
                _slow=$(state_get "ch${_ch}_slow" 0)
                _slow=$((_slow + 1))
                state_set "ch${_ch}_slow" "$_slow"
                if [ "$_slow" -ge "$CH_SLOW_ROUNDS" ]; then
                    alert "L2 通道 ch${_ch} 帧号停在 ${_fr} 超过 ${_stuck}s, 且连续 ${_slow} 轮没恢复 (阈值 ${CH_STALL_SEC}s x ${CH_SLOW_ROUNDS}) —— 判定该路确实卡死, 重启服务"
                    systemctl restart "$UNIT" 2>&1 | head -3 >&2
                    state_set last_start_ts "$now"
                    # ★ 不删状态文件(理由见 L1 分支的注释)
                    break
                fi
                alert "L2 通道 ch${_ch} 帧号停在 ${_fr} 超过 ${_stuck}s (阈值 ${CH_STALL_SEC}s) —— 其余 ${_alive} 路仍在推进, 判定为该路变慢/源断流, 暂不重启 (第 ${_slow}/${CH_SLOW_ROUNDS} 轮)"
            else
                alert "L2 通道 ch${_ch} 帧号停在 ${_fr} 超过 ${_stuck}s (阈值 ${CH_STALL_SEC}s), 且其它路也停了 —— 判定整机卡死, 重启服务"
                systemctl restart "$UNIT" 2>&1 | head -3 >&2
                state_set last_start_ts "$now"
                # ★ 不删状态文件(理由见 L1 分支的注释)
                break
            fi
        fi
    done

    # -----------------------------------------------------------------------
    # 5) 水位告警: RSS / 活堆 / 日志盘剩余空间
    # -----------------------------------------------------------------------
    rss_kb=$(awk '/^VmRSS:/{print $2}' "/proc/$pid/status" 2>/dev/null)
    if [ -n "${rss_kb:-}" ] && [ "$rss_kb" -gt $((RSS_WARN_MB * 1024)) ]; then
        alert "RSS $((rss_kb / 1024))MB 超过阈值 ${RSS_WARN_MB}MB"
    fi
    heap_kb=$({ tail -c "$LOG_TAIL_BYTES" "$OUT_LOG" 2>/dev/null
                tail -c "$LOG_TAIL_BYTES" "$ERR_LOG" 2>/dev/null; } \
              | grep -aoE 'heap=[0-9]+KB' | tail -1 | cut -d= -f2 | tr -d 'KB')
    if [ -n "${heap_kb:-}" ] && [ "$heap_kb" -gt $((HEAP_WARN_MB * 1024)) ]; then
        alert "活堆 $((heap_kb / 1024))MB 超过阈值 ${HEAP_WARN_MB}MB"
    fi
    free_kb=$(df -Pk "$LOG_DIR" 2>/dev/null | awk 'NR==2{print $4}')
    if [ -n "${free_kb:-}" ] && [ "$free_kb" -lt $((LOG_FREE_WARN_MB * 1024)) ]; then
        alert "日志盘 $LOG_DIR 只剩 $((free_kb / 1024))MB —— 写满会让应用 write() 失败, 请检查 logrotate"
    fi

    # ---- cgroup 内存水位 ----
    # ★ 为什么 RSS / 活堆之外还要看 cgroup(2026-11-23 实测踩出来的):
    #   2026-11-23 查活堆泄漏时发现, cgroup 顶满 1G 的主因【不是】活堆,
    #   而是【视频页缓存】—— 4 路 4.1GB 的 mp4 顺序读, 内核把读过的页全留在
    #   page cache 里, 以 ~7~10 MB/s 增长, 5 分钟就把 cgroup 顶到上限。
    #   那段时间 RSS 619MB / 活堆 489MB, 只看这两个指标【看不出】cgroup 快满了 ——
    #   真正撞墙的是 cgroup 里那个谁都不看的 file 分量。
    #   ⇒ 这条判据盯的是 cgroup 总量, 任何分量涨上来都能发现:
    #     活堆涨 -> 泄漏;  file 涨 -> 页缓存治理失效; shmem 涨 -> dma-buf 涨。
    # ★ 只告警不重启: cgroup 用量高本身不是"卡死", 重启解决不了,
    #   反而把现场证据(内存构成)冲掉了。
    _cg=$(systemctl show "$UNIT" -p ControlGroup --value 2>/dev/null)
    if [ -n "${_cg:-}" ] && [ -r "/sys/fs/cgroup${_cg}/memory.current" ]; then
        _cg_bytes=$(cat "/sys/fs/cgroup${_cg}/memory.current" 2>/dev/null)
        _cg_mb=$((_cg_bytes / 1048576))
        if [ "$_cg_mb" -gt "$CGROUP_WARN_MB" ]; then
            _cg_stat="/sys/fs/cgroup${_cg}/memory.stat"
            _cg_anon=$(awk '/^anon /{printf "%d", $2/1048576}' "$_cg_stat" 2>/dev/null)
            _cg_file=$(awk '/^file /{printf "%d", $2/1048576}' "$_cg_stat" 2>/dev/null)
            _cg_shmem=$(awk '/^shmem /{printf "%d", $2/1048576}' "$_cg_stat" 2>/dev/null)
            alert "cgroup 内存 ${_cg_mb}MB 超过阈值 ${CGROUP_WARN_MB}MB (anon=${_cg_anon}MB file=${_cg_file}MB shmem=${_cg_shmem}MB) —— 快撞到 MemoryMax 了, 只告警不重启, 请查内存构成"
        fi
    fi

    # -----------------------------------------------------------------------
    # 6) MQTT 探针(可选, 默认关)
    #    ★ 为什么默认关:
    #      ① broker 抖动/断网 会让一个【健康】的进程被误判卡死并重启;
    #      ② 探针本身要起一个 python 进程, 多一份开销;
    #      ③ 没有配 broker 时探针毫无意义。
    #      要开: 在 gate.env 里设 GATE_MQTT_PROBE=1
    # -----------------------------------------------------------------------
    if [ "${GATE_MQTT_PROBE:-0}" = "1" ] && [ -n "${GATE_MQTT_HOST:-}" ]; then
        _out=$(mktemp)
        timeout $((MQTT_PROBE_SEC + 10)) python3 "$PROJ/scripts/mqtt_sub.py" \
            --host "$GATE_MQTT_HOST" --port "${GATE_MQTT_PORT:-1883}" \
            --topic "${GATE_MQTT_TOPIC:-device/#}" \
            --out "$_out" --seconds "$MQTT_PROBE_SEC" >/dev/null 2>&1
        if [ ! -s "$_out" ]; then
            alert "MQTT 探针 ${MQTT_PROBE_SEC}s 内没收到任何消息 —— 疑似业务线程卡死"
        fi
        rm -f "$_out"
    fi
done
