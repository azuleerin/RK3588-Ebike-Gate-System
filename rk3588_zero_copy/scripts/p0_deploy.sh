#!/usr/bin/env bash
# ============================================================================
# p0_deploy.sh —— P0 收尾的一键 root 部署(2026-11-23)
#
# 为什么单独一个脚本: 本次 P0 有两件事【必须】动到 /etc 和 systemd,
# 而工程里所有改 /etc 的动作都收敛在 install_gate_service.sh 里 ——
# 所以这里只做"调它 + 补一步它不管的", 不重复实现安装逻辑。
#
# 用法:
#   sudo scripts/p0_deploy.sh
#
# 做了 4 件事:
#   1) 校验 gate.env 里的 GATE_LOG_DIR 已经改到真实磁盘(不是 zram)
#   2) 跑 install_gate_service.sh
#      —— 它会把 unit 的 StandardOutput/StandardError、rk-collect-crash 的
#         取证路径、/etc/logrotate.d/rk-gate 规则里的路径【一起】改到新目录,
#         并把 MemoryHigh/MemoryMax 按实测稳态校准好的值装进 unit
#   3) logrotate -f 强制轮转一次, 并【验证真的产出了 .1 文件】
#      (历史上这个安全网从来没成功过 —— 见下)
#   4) 打印部署后的实际状态供人工核对
# ============================================================================
set -u

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
ENV_FILE="$PROJ/config/gate.env"
LOGROTATE_RULE=/etc/logrotate.d/rk-gate

if [ "$(id -u)" != "0" ]; then
    echo "需要 root, 请用: sudo $0" >&2
    exit 1
fi

echo "==========================================================="
echo " rk_gate P0 收尾部署   $(date '+%F %T')"
echo "==========================================================="

# ---- 1) 校验日志目录 ----
# shellcheck disable=SC1090
. "$ENV_FILE"
LOG_DIR="${GATE_LOG_DIR:-/var/log}"
echo ""
echo "[1/4] 日志目录 = $LOG_DIR"
if [ "$LOG_DIR" = "/var/log" ]; then
    echo "  ★★ 仍然是 /var/log —— 本板 /var/log 是 47MB 的 zram 内存盘。" >&2
    echo "     业务日志写满会触发 write() ENOSPC, 应用直接崩(R19)。" >&2
    echo "     请先把 config/gate.env 里的 GATE_LOG_DIR 改成 /var/log.hdd。" >&2
    exit 1
fi
df -hT "$LOG_DIR" | tail -1 | sed 's/^/  /'

# ---- 2) 安装 ----
# ★ 先把看门狗【闭嘴】: 安装过程里主服务会重启, 而此时日志目标正从
#   /var/log 切到 /var/log.hdd —— 老看门狗还在盯 /var/log, 会看到
#   "日志不再更新" -> 判 L1 整机心跳停 -> 反过来把服务重启了。
#   停看门狗只需要写一个标记文件, 不需要停它自己。
echo ""
echo "[2/4] install_gate_service.sh"
touch /run/rk-gate/paused
trap 'rm -f /run/rk-gate/paused' EXIT
"$PROJ/scripts/install_gate_service.sh" 2>&1 | sed 's/^/  /'
_rc=${PIPESTATUS[0]}
if [ "$_rc" != "0" ]; then
    echo "  ★ 安装脚本返回 $_rc, 后续步骤中止" >&2
    exit "$_rc"
fi
rm -f /run/rk-gate/paused
trap - EXIT
echo "  看门狗已恢复看护"

# ---- 3) 强制轮转 + 验证 ----
# ★ 为什么必须验证: 历史上"logrotate 已修"这句话只对了一半 ——
#   规则里写的是 "su seeed root", su 的目标身份必须是【能在日志目录建文件】
#   的那个(root 可以, seeed 不在 syslog 组), 于是 logrotate 每次都报
#   "error creating output file ...: Permission denied",
#   而这个错只在 logrotate 自己的 stderr 里, 不进 syslog/journal ——
#   于是轮转【从来没成功过】, 目录里连一个 .1/.gz 都没有, 安全网是空的。
#   所以这里不能只看命令退出码, 必须【看产物】。
echo ""
echo "[3/4] logrotate -f 强制轮转并验证产物"
if ! logrotate -f "$LOGROTATE_RULE" 2>&1 | sed 's/^/  /'; then
    echo "  ★ logrotate 执行失败" >&2
fi
sleep 1
_fail=0
for base in rk-gate.log rk-gate-error.log rk-gate-watchdog.log; do
    if [ -f "$LOG_DIR/$base.1" ]; then
        sz=$(stat -c %s "$LOG_DIR/$base.1")
        echo "  ✔ $LOG_DIR/$base.1  ($sz 字节)"
    else
        echo "  ✘ $LOG_DIR/$base.1  【没生成】—— 轮转没生效" >&2
        _fail=1
    fi
done
if [ "$_fail" != "0" ]; then
    echo "" >&2
    echo "  ★ 轮转没生效。最可能的原因: logrotate 规则里的 su 身份不对。" >&2
    echo "    本板日志目录是 root:syslog 775(组可写), 必须写 'su root root';" >&2
    echo "    写 'su seeed root' 会报 Permission denied 且【错误不进日志】。" >&2
    echo "    查: sudo logrotate -v $LOGROTATE_RULE 2>&1 | tail -20" >&2
    exit 1
fi

# ---- 4) 收尾状态 ----
echo ""
echo "[4/4] 部署后状态"
systemctl show rk-gate.service -p MainPID -p ActiveState -p NRestarts \
    -p MemoryCurrent -p MemoryHigh -p MemoryMax 2>/dev/null | sed 's/^/  /'
echo ""
echo "  日志目录:"
ls -la "$LOG_DIR"/rk-gate*.log* 2>/dev/null | sed 's/^/    /'
echo ""
echo "  zram 占用(应该比之前小很多):"
df -h /var/log | tail -1 | sed 's/^/    /'
echo ""
echo "==========================================================="
echo " 完成。服务已用新二进制 + 新日志目录 + 校准后的 MemoryMax 跑起来。"
echo "==========================================================="
