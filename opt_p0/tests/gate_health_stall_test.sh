#!/usr/bin/env bash
# ============================================================================
# gate_health_stall_test.sh —— gate_health.sh 的【负向】测试: 确认它还抓得住真停滞
#
# 场景 1(必须报正常): 日志里的帧号在推进   -> 结论"正常"
# 场景 2(必须报可疑): 帧号【几秒内不动】但日志还在写 -> 结论"可疑"
# 场景 3(必须报卡死): 帧号很久不动 + 日志也不写了 -> 结论"卡死"
#
# ★ 为什么要这个测试: 2026-11-24 把 gate_health.sh 的判据从
#   "拿日志帧号跟看门狗快照比"改成"自己隔 6 s 取两次快照比" ——
#   修掉了【看门狗刚轮完、应用下一行还没写出来】时被误报成"停滞 2 秒"的假信号。
#   但改完必须证明: 它没变成"永远说正常"的橡皮章。
#
# 全程不碰生产: 日志/状态文件/ENV 全部指向沙盒, systemctl 用假的挡在 PATH 前面。
# ============================================================================
set -uo pipefail

REPO=/home/seeed/rk_deploy
PROJ=$REPO/rk3588_zero_copy
TMP=$(mktemp -d /tmp/gh_test_XXXXXX)
mkdir -p "$TMP/bin" "$TMP/log" "$TMP/run"

cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

# ---- 假 systemctl: 服务 active, MainPID 指到一个真进程 ----
cat > "$TMP/bin/systemctl" <<'EOF'
#!/usr/bin/env bash
case "$*" in
  *"-p MainPID"*)     echo "$FAKE_PID" ;;
  *"-p ActiveState"*) echo "active" ;;
  *is-active*)        echo "active" ;;
  *) : ;;
esac
exit 0
EOF
chmod +x "$TMP/bin/systemctl"

sleep 3600 </dev/null >/dev/null 2>&1 &
FAKE_PID=$!
export FAKE_PID PATH="$TMP/bin:$PATH"

# ---- 沙盒 gate.env: 只把日志目录指到沙盒 ----
cat > "$TMP/gate.env" <<EOF
GATE_LOG_DIR=$TMP/log
GATE_MQTT_HOST=
GATE_SN=test
EOF

# ---- 沙盒版 gate_health.sh: 只换 ENV_FILE ----
sed -e "s#^ENV_FILE=.*#ENV_FILE=\"$TMP/gate.env\"#" \
    "$PROJ/scripts/gate_health.sh" > "$TMP/gate_health.sh"
chmod +x "$TMP/gate_health.sh"

run() { bash "$TMP/gate_health.sh" 2>&1 | sed 's/\x1b\[[0-9;]*m//g'; }
verdict() { run | grep -a "结论" | sed 's/.*: *//'; }

rc=0
echo "=============================================================="
echo " 场景 1: 帧号持续推进 -> 必须'正常'"
cat > "$TMP/log/rk-gate-error.log" <<'EOF'
[ch1][frame 100] fps=30.0 objs=0 rss=232000KB heap=84000KB
[ch2][frame 100] fps=30.0 objs=0 rss=232000KB heap=84000KB
[ch3][frame 100] fps=30.0 objs=0 rss=232000KB heap=84000KB
[ch4][frame 100] fps=30.0 objs=0 rss=232000KB heap=84000KB
EOF
# 模拟"日志在写、帧号在涨": 每秒追加一行新帧号
( for i in $(seq 1 40); do
    sleep 1
    f=$((100 + i * 100))
    echo "[ch1][frame $f] fps=30.0 objs=0 rss=232000KB heap=84000KB" >> "$TMP/log/rk-gate-error.log"
    echo "[ch2][frame $f] fps=30.0 objs=0 rss=232000KB heap=84000KB" >> "$TMP/log/rk-gate-error.log"
    echo "[ch3][frame $f] fps=30.0 objs=0 rss=232000KB heap=84000KB" >> "$TMP/log/rk-gate-error.log"
    echo "[ch4][frame $f] fps=30.0 objs=0 rss=232000KB heap=84000KB" >> "$TMP/log/rk-gate-error.log"
  done ) &
WRITER=$!
sleep 3
V=$(verdict)
echo "   结论 = $V"
[ "$V" = "正常" ] && echo "   ✓" || { echo "   ✗ 应该是正常"; rc=1; }
kill $WRITER 2>/dev/null; wait $WRITER 2>/dev/null

echo
echo "=============================================================="
echo " 场景 2: 帧号不动但日志还在写 -> 必须'可疑'(不是正常)"
# 帧号固定, 但日志 mtime 保持新鲜(靠 touch)
( for i in $(seq 1 40); do sleep 1; touch "$TMP/log/rk-gate-error.log"; done ) &
TOUCHER=$!
V=$(verdict)
echo "   结论 = $V"
if [ "$V" = "可疑" ]; then
    echo "   ✓ 抓得住'帧号不动'"
else
    echo "   ✗ 应该报可疑, 实际 $V"; rc=1
fi
kill $TOUCHER 2>/dev/null; wait $TOUCHER 2>/dev/null

echo
echo "=============================================================="
echo " 场景 3: 帧号很久不动 + 日志也不写 -> 必须'卡死'"
# 日志 mtime 拉老(10 分钟前), 看门狗的 ch*_ts 也拉老
touch -d '10 minutes ago' "$TMP/log/rk-gate-error.log"
for c in 1 2 3 4; do
    echo 100 > "$TMP/run/ch${c}_frame"
    echo $(( $(date +%s) - 600 )) > "$TMP/run/ch${c}_ts"
done
V=$(verdict)
echo "   结论 = $V"
if [ "$V" = "卡死" ]; then
    echo "   ✓ 抓得住'真卡死'"
else
    echo "   ✗ 应该报卡死, 实际 $V"; rc=1
fi

echo
echo "=============================================================="
if [ "$rc" -eq 0 ]; then echo " 全部通过 ✓"; else echo " 有失败项 ✗"; fi
exit $rc
