#!/usr/bin/env bash
# ============================================================================
# lock_dir_selfcheck_test.sh —— 锁目录应用层自检的行为验证
#
# 场景 A: 目录存在且可写 -> 自检【通过】(脚本继续往下走)
# 场景 B: 目录不存在       -> 明确报错 + 退出(非零码), 不静默失败
# 场景 C: 目录存在但不可写 -> 明确报错 + 退出(非零码), 不静默失败
#
# ★ 为什么不能直接用环境变量 GATE_LOCK_FILE 覆盖:
#   run_gate_service.sh 是【先】读环境变量、然后再 `. config/gate.env`,
#   gate.env 里写了 GATE_LOCK_FILE 就会【盖掉】环境变量(gate.env 是配置真源)。
#   所以测试改成: 复制一份 gate.env + 复制一份脚本, 把配置指到沙盒。
#
# ★ 不会启动真正的 rk_gate: 自检在第 1 段, 后面紧跟 systemd 托管标记检查
#   (本机 /etc/rk-gate/systemd_managed 存在 -> 脚本 exit 78), 所以
#   自检通过时脚本会在启动业务之前就退出, 不会碰 NPU。
# ============================================================================
set -uo pipefail


# ---- 需要 root: 以 root 身份运行, 或先执行 `sudo -v` 缓存凭据(凭据不落盘) ----
if [ "$(id -u)" -ne 0 ] && ! sudo -n true 2>/dev/null; then
    echo "需要 root 权限，请使用 sudo 运行或先执行 sudo -v" >&2
    exit 1
fi
REPO=/home/seeed/rk_deploy
PROJ=$REPO/rk3588_zero_copy
SCRIPT=$PROJ/scripts/run_gate_service.sh
TMP=$(mktemp -d /tmp/lockchk_XXXXXX)
trap 'rm -rf "$TMP"' EXIT

# ---- 复制脚本, 把配置文件指到沙盒 ----
sed -e "s#^ENV_FILE=.*#ENV_FILE=\"$TMP/gate.env\"#" "$SCRIPT" > "$TMP/script.sh"
chmod +x "$TMP/script.sh"

make_env() {
    local lockfile="$1"
    sed -e "s#^GATE_LOCK_FILE=.*#GATE_LOCK_FILE=$lockfile#" \
        "$PROJ/config/gate.env" > "$TMP/gate.env"
    # 告警日志也指到沙盒 (原路径 /var/log 对服务用户可能不可写)
    echo "GATE_LOCK_CHECK_LOG=$TMP/lock-check.log" >> "$TMP/gate.env"
}

run_case() {
    local label="$1" lockfile="$2" expect="$3"
    make_env "$lockfile"
    local out rc
    out=$(timeout 20 "$TMP/script.sh" 2>&1)
    rc=$?
    echo "  --- $label ---"
    echo "    退出码 = $rc"
    echo "$out" | grep -E "\[自检失败\]|打不开锁文件|systemd 模式|拒绝手动启动" | head -3 | sed 's/^/    /'
    if echo "$out" | grep -q "$expect"; then
        if [ "$rc" -ne 0 ]; then
            echo "    判定: ✅ 命中预期提示, 且退出码非零($rc)"
            return 0
        fi
        echo "    判定: ❌ 命中提示但退出码是 0"
        return 1
    fi
    echo "    判定: ❌ 没有命中预期提示"
    return 1
}

echo "=============================================================="
echo " 场景 A: 锁目录存在且可写 -> 自检通过"
echo "=============================================================="
mkdir -p "$TMP/ok"
make_env "$TMP/ok/rk-gate.lock"
A_RC=$(timeout 20 "$TMP/script.sh" >/dev/null 2>&1; echo $?)
A_OUT=$(timeout 20 "$TMP/script.sh" 2>&1 | grep -E "自检失败|systemd 模式" | head -1)
echo "    退出码 = $A_RC   (自检通过后, 因 systemd 托管标记而退出, 不启动业务)"
echo "    锁文件已建立 = $([ -f "$TMP/ok/rk-gate.lock" ] && echo yes || echo no)"
if [ "$A_RC" -ne 0 ] && [ -f "$TMP/ok/rk-gate.lock" ] && ! echo "$A_OUT" | grep -q "自检失败"; then
    echo "    判定: ✅ 自检通过, 锁文件已建立, 且未误报"
    A_OK=1
else
    echo "    判定: ❌"
    A_OK=0
fi

echo ""
echo "=============================================================="
echo " 场景 B: 锁目录不存在 -> 明确报错 + 非零退出"
echo "=============================================================="
run_case "目录不存在" "$TMP/no_such_dir/rk-gate.lock" "锁目录不存在"
B_OK=$?

echo ""
echo "=============================================================="
echo " 场景 C: 锁目录存在但不可写 -> 明确报错 + 非零退出"
echo "=============================================================="
mkdir -p "$TMP/ro"
sudo chown root:root "$TMP/ro" 2>/dev/null
sudo chmod 0555 "$TMP/ro" 2>/dev/null
run_case "目录不可写" "$TMP/ro/rk-gate.lock" "锁目录不可写"
C_OK=$?

echo ""
echo "=============================================================="
echo " 汇总"
echo "=============================================================="
echo "  A 目录正常 -> 自检通过   : $([ "$A_OK" = 1 ] && echo ✅ || echo ❌)"
echo "  B 目录不存在 -> 明确报错 : $([ "$B_OK" = 0 ] && echo ✅ || echo ❌)"
echo "  C 目录不可写 -> 明确报错 : $([ "$C_OK" = 0 ] && echo ✅ || echo ❌)"
[ "$A_OK" = 1 ] && [ "$B_OK" = 0 ] && [ "$C_OK" = 0 ]
