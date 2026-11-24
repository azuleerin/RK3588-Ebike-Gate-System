#!/usr/bin/env bash
# ============================================================================
# watchdog_fail_alert_test.sh —— 看门狗「连续拉起失败」升级告警的行为验证 (P2-A3)
#
# 场景 A(必须不误报): 服务一直是 active -> 告警日志必须【零新增】
# 场景 B(必须能识别): 服务一直拉不起来(ActiveState=inactive, Result=exit-code)
#                     -> 告警日志必须出现【升级告警】(★ 连续拉起失败 N 次),
#                        而且必须带上 ActiveState/Result/fail_count
# 场景 C(必须留结局): 恢复健康 -> 必须出现「已恢复」记录, 且计数清零
#
# ★ 全程不碰生产:
#   - 看门狗脚本是【复制】出来的, LOG_DIR / RUN_DIR / UNIT 都指到沙盒
#   - systemctl 用一个【假的】挡在 PATH 前面, 只记录调用, 不真的重启任何东西
#   - CHECK_SEC / BACKOFF_START / HEALTH_RESET_SEC 都调小, 让测试秒级跑完
# ============================================================================
set -uo pipefail

REPO=/home/seeed/rk_deploy
PROJ=$REPO/rk3588_zero_copy
WD_SRC=$PROJ/scripts/gate_watchdog.sh
TMP=$(mktemp -d /tmp/wd_fail_test_XXXXXX)
mkdir -p "$TMP/bin" "$TMP/log" "$TMP/run"

cleanup() {
    [ -n "${WD_PID:-}" ] && kill "$WD_PID" 2>/dev/null
    [ -n "${TOUCH_PID:-}" ] && kill "$TOUCH_PID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

# ---- 假的 systemctl: 状态由 FAKE_STATE / FAKE_RESULT 控制, 只记录调用 ----
cat > "$TMP/bin/systemctl" <<'EOF'
#!/usr/bin/env bash
echo "$*" >> "$FAKE_SYSLOG"
case "$*" in
  *"-p MainPID"*)    echo "$FAKE_PID" ;;
  *"-p ActiveState"*) echo "$FAKE_STATE" ;;
  *"-p Result"*)     echo "$FAKE_RESULT" ;;
  *reset-failed*)    : ;;
  *start*)           echo "FAKE START $*" >> "$FAKE_SYSLOG" ;;
  *) : ;;
esac
exit 0
EOF
chmod +x "$TMP/bin/systemctl"

sleep 3600 &
FAKE_PID=$!

# ---- 复制看门狗脚本, 把路径指到沙盒 ----
sed -e "s#^LOG_DIR=.*#LOG_DIR=\"$TMP/log\"#" \
    -e "s#^RUN_DIR=.*#RUN_DIR=\"$TMP/run\"#" \
    -e "s#^UNIT=.*#UNIT=fake-gate.service#" \
    "$WD_SRC" > "$TMP/watchdog.sh"
chmod +x "$TMP/watchdog.sh"

export FAKE_SYSLOG="$TMP/systemctl_calls.log"
export FAKE_PID
export FAKE_STATE=active
export FAKE_RESULT=success
export PATH="$TMP/bin:$PATH"

# ---- 假的心跳日志: L1 判据看的是【日志文件的 mtime】 ----
#   沙盒里没有真日志, mtime 取不到 -> age 变成 epoch 级大数 -> L1 立刻误判"整机卡死"。
#   所以这里造两个日志文件, 并用后台循环持续 touch 保持 mtime 新鲜。
: > "$TMP/log/rk-gate.log"
: > "$TMP/log/rk-gate-error.log"
( while :; do touch "$TMP/log/rk-gate.log" "$TMP/log/rk-gate-error.log"; sleep 1; done ) \
    >/dev/null 2>&1 < /dev/null &
TOUCH_PID=$!

ALERT="$TMP/log/rk-gate-alerts.log"
rc=0

# ============================================================================
echo "=============================================================="
echo " 场景 A: 服务一直 active -> 告警日志必须零新增"
CHECK_SEC=1 BACKOFF_START=1 BACKOFF_MAX=2 HEALTH_RESET_SEC=2 \
    FAIL_ALERT_THRESHOLD=3 FAIL_ALERT_REPEAT=5 \
    bash "$TMP/watchdog.sh" > "$TMP/wd_a.log" 2>&1 &
WD_PID=$!
sleep 5
kill "$WD_PID" 2>/dev/null; wait "$WD_PID" 2>/dev/null
A_COUNT=$( [ -f "$ALERT" ] && wc -l < "$ALERT" || echo 0 )
echo "   告警日志行数 = ${A_COUNT} (期望 0)"
if [ "$A_COUNT" -ne 0 ]; then
    echo "   ✗ 正常态误报了:"; cat "$ALERT"; rc=1
else
    echo "   ✓ 正常态零误报"
fi

# ============================================================================
echo
echo "=============================================================="
echo " 场景 B: 服务一直拉不起来 -> 必须出现升级告警"
rm -f "$ALERT" "$TMP/run/fail_count" "$TMP/run/backoff" "$TMP/run/last_fail_ts"
export FAKE_STATE=inactive
export FAKE_RESULT=exit-code
CHECK_SEC=1 BACKOFF_START=1 BACKOFF_MAX=2 HEALTH_RESET_SEC=999 \
    FAIL_ALERT_THRESHOLD=3 FAIL_ALERT_REPEAT=5 \
    bash "$TMP/watchdog.sh" > "$TMP/wd_b.log" 2>&1 &
WD_PID=$!
sleep 14
kill "$WD_PID" 2>/dev/null; wait "$WD_PID" 2>/dev/null
if [ ! -f "$ALERT" ]; then
    echo "   ✗ 告警日志根本没生成"; rc=1
else
    echo "   告警日志内容:"; sed 's/^/      /' "$ALERT"
    echo
    if grep -q "连续拉起失败" "$ALERT"; then
        echo "   ✓ 有升级告警"
    else
        echo "   ✗ 缺少升级告警(★ 连续拉起失败 N 次)"; rc=1
    fi
    if grep -qE "ActiveState=.*Result=.*[0-9]+ 次" "$ALERT"; then
        echo "   ✓ 升级告警带上了 ActiveState/Result/fail_count"
    else
        echo "   ✗ 升级告警缺少现场信息(ActiveState/Result/fail_count)"; rc=1
    fi
    ROUTINE=$(grep -c "服务未在运行" "$ALERT" || true)
    echo "   流水账行数 = ${ROUTINE} (期望 >= 1)"
    if [ "$ROUTINE" -lt 1 ]; then echo "   ✗ 流水账没了"; rc=1; fi
    FC=$(cat "$TMP/run/fail_count" 2>/dev/null || echo 0)
    echo "   最终 fail_count = ${FC}"
fi

# ============================================================================
echo
echo "=============================================================="
echo " 场景 C: 恢复健康 -> 必须有「已恢复」记录且计数清零"
export FAKE_STATE=active
export FAKE_RESULT=success
CHECK_SEC=1 STALL_SEC=2 CH_STALL_SEC=999 SILENT_STALL_SEC=999 \
    BACKOFF_START=1 BACKOFF_MAX=2 HEALTH_RESET_SEC=2 \
    FAIL_ALERT_THRESHOLD=3 FAIL_ALERT_REPEAT=5 \
    bash "$TMP/watchdog.sh" > "$TMP/wd_c.log" 2>&1 &
WD_PID=$!
sleep 8
kill "$WD_PID" 2>/dev/null; wait "$WD_PID" 2>/dev/null
if grep -q "已恢复健康" "$TMP/wd_c.log" "$ALERT" 2>/dev/null; then
    echo "   ✓ 有「已恢复」记录"
else
    echo "   ✗ 缺少「已恢复」记录"; rc=1
    grep -iE "恢复|清零" "$TMP/wd_c.log" | tail -3 | sed 's/^/      /'
fi
FC=$(cat "$TMP/run/fail_count" 2>/dev/null || echo "?")
echo "   恢复后 fail_count = ${FC} (期望 0)"
[ "$FC" = "0" ] || { echo "   ✗ 计数没清零"; rc=1; }

echo
echo "=============================================================="
if [ "$rc" -eq 0 ]; then
    echo " 全部通过 ✓"
else
    echo " 有失败项 ✗"
fi
exit $rc
