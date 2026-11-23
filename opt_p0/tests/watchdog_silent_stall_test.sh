#!/usr/bin/env bash
# ============================================================================
# watchdog_silent_stall_test.sh —— 看门狗「静默卡死」判据的行为验证
#
# 场景 A(必须不误报): 日志里帧号持续增长 -> 看门狗不该动
# 场景 B(必须能识别): 日志【还在写】, 但业务帧号【不涨】 -> 看门狗必须自愈
#
# ★ 全程不碰生产:
#   - 看门狗脚本是【复制】出来的, LOG_DIR / RUN_DIR / UNIT 都指到沙盒
#   - systemctl 用一个【假的】挡在 PATH 前面, 只记录调用, 不真的重启任何东西
# ============================================================================
set -uo pipefail

REPO=/home/seeed/rk_deploy
PROJ=$REPO/rk3588_zero_copy
WD_SRC=$PROJ/scripts/gate_watchdog.sh
TMP=$(mktemp -d /tmp/wd_test_XXXXXX)
mkdir -p "$TMP/bin" "$TMP/log" "$TMP/run"

cleanup() {
    [ -n "${FAKE_PID:-}" ] && kill "$FAKE_PID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

# ---- 假的 systemctl: 只记录调用, 按 -p 返回假值 ----
cat > "$TMP/bin/systemctl" <<'EOF'
#!/usr/bin/env bash
echo "$*" >> "$FAKE_SYSLOG"
case "$*" in
  *"-p MainPID"*) echo "$FAKE_PID" ;;
  *"-p ActiveState"*) echo "active" ;;
  *"-p Result"*) echo "success" ;;
  *) : ;;
esac
exit 0
EOF
chmod +x "$TMP/bin/systemctl"

# ---- 假的常驻进程(让 kill -0 pid 成功) ----
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
export PATH="$TMP/bin:$PATH"

# ★ grep -c 无匹配时会打印 0 并且 exit 1, 所以不能加 "|| echo 0"(会变成两行 0)
restart_calls() { grep -c "restart fake-gate.service" "$FAKE_SYSLOG" 2>/dev/null | head -1; }

# ============================================================================
echo "=============================================================="
echo " 场景 A: 帧号持续增长 -> 看门狗【不该】误报"
echo "=============================================================="
: > "$FAKE_SYSLOG"
rm -f "$TMP/log/rk-gate-error.log"

# 模拟 rk_gate: 每秒写一行统计, 帧号持续增长
(
  f=100
  while :; do
    printf '[ch1][frame %d] fps=20.0 deliv=20.0 npu=27.0ms\n' "$f" >> "$TMP/log/rk-gate-error.log"
    printf '[ch2][frame %d] fps=20.0 deliv=20.0 npu=27.0ms\n' "$f" >> "$TMP/log/rk-gate-error.log"
    f=$((f+20))
    sleep 1
  done
) &
WRITER=$!

CHECK_SEC=2 STALL_SEC=30 SILENT_STALL_SEC=10 CH_STALL_SEC=60 L2_COOLDOWN_SEC=5 \
  timeout 25 "$TMP/watchdog.sh" > "$TMP/wdA.log" 2>&1
kill $WRITER 2>/dev/null

A_RESTART=$(restart_calls)
echo "  看门狗跑了 25s, 期间 systemctl restart 调用次数 = $A_RESTART"
if [ "$A_RESTART" -eq 0 ] && ! grep -q "静默卡死观察开始" "$TMP/wdA.log"; then
    echo "  判定: ✅ 帧号在涨, 看门狗没有误报(且从未进入静默观察状态)"
else
    echo "  判定: ❌ 误报了"
    grep -E "L1|L1.5|L2" "$TMP/wdA.log" | tail -3
fi

# ============================================================================
echo ""
echo "=============================================================="
echo " 场景 B: 日志【还在写】但业务帧号【不涨】 -> 看门狗必须自愈"
echo "=============================================================="
: > "$FAKE_SYSLOG"
rm -f "$TMP/log/rk-gate-error.log"

# 先写一行帧号(之后永远不再更新帧号), 然后持续写【非帧号】的日志行
(
  printf '[ch1][frame 100] fps=20.0 deliv=20.0 npu=27.0ms\n' > "$TMP/log/rk-gate-error.log"
  while :; do
    printf '[ch1][plate] track=9 cls=1 score=0.88 text=A339058\n' >> "$TMP/log/rk-gate-error.log"
    printf '[ch2][plate] track=11 cls=1 score=0.91 text=A339058\n' >> "$TMP/log/rk-gate-error.log"
    sleep 1
  done
) &
WRITER=$!

START=$(date +%s)
CHECK_SEC=2 STALL_SEC=30 SILENT_STALL_SEC=10 CH_STALL_SEC=60 L2_COOLDOWN_SEC=5 \
  timeout 40 "$TMP/watchdog.sh" > "$TMP/wdB.log" 2>&1
END=$(date +%s)
kill $WRITER 2>/dev/null

B_RESTART=$(restart_calls)
echo "  看门狗跑了 $((END-START))s, 期间 systemctl restart 调用次数 = $B_RESTART"
grep -E "静默|L1.5|L2" "$TMP/wdB.log" | tail -3 | sed 's/^/    /'
if [ "$B_RESTART" -ge 1 ]; then
    echo "  判定: ✅ 识别出「日志在写但帧号不涨」, 触发了自愈"
else
    echo "  判定: ❌ 没识别出来"
fi

echo ""
echo "=============================================================="
echo " 场景 C: 日志在写, 但一条业务帧号都【解析不到】(L2 的盲区)"
echo "=============================================================="
: > "$FAKE_SYSLOG"
rm -f "$TMP/log/rk-gate-error.log"

# 只写【非帧号】的日志行 —— L2 的 for 循环里没有任何 channel 可查, 等于什么都没查
(
  while :; do
    printf '[ch1][plate] track=9 cls=1 score=0.88 text=A339058\n' >> "$TMP/log/rk-gate-error.log"
    printf 'some other log line without a frame counter\n' >> "$TMP/log/rk-gate-error.log"
    sleep 1
  done
) &
WRITER=$!

START=$(date +%s)
CHECK_SEC=2 STALL_SEC=30 SILENT_STALL_SEC=10 CH_STALL_SEC=60 L2_COOLDOWN_SEC=5 \
  timeout 30 "$TMP/watchdog.sh" > "$TMP/wdC.log" 2>&1
END=$(date +%s)
kill $WRITER 2>/dev/null

C_RESTART=$(restart_calls)
echo "  看门狗跑了 $((END-START))s, 期间 systemctl restart 调用次数 = $C_RESTART"
grep -E "静默|L1.5" "$TMP/wdC.log" | tail -2 | sed 's/^/    /'
if [ "$C_RESTART" -ge 1 ]; then
    echo "  判定: ✅ 兜住了 L2 的盲区(解析不到帧号 != 看门狗以为正常)"
else
    echo "  判定: ❌ 漏了"
fi

echo ""
echo "=== 调用记录 ==="
cat "$FAKE_SYSLOG" 2>/dev/null | sed 's/^/  /' | head -10
