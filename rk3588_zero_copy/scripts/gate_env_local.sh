#!/usr/bin/env bash
# ============================================================================
# gate_env_local.sh —— 【可选】外挂配置加载器 (仓库外)
#
# 干什么:
#   若 ${GATE_ENV_LOCAL:-/home/seeed/.config/rk-gate/gate.env.local} 存在,
#   就按与 config/gate.env 【完全相同的解析规则】加载它, 覆盖同名变量。
#   文件不存在时 —— 本脚本什么都不做, 调用方的行为与"没有这个机制"逐字节一致。
#
# 为什么需要它:
#   真实监控相机的 RTSP 地址带账号密码, 绝不能进 Git。
#   而 config/gate.env 是【被 Git 跟踪】的生产配置, 不能改(改了就把凭据带进仓库了)。
#   所以: 生产默认配置留在仓库里不动, 现场真实地址放在仓库外的私密文件,
#   由本脚本在运行时覆盖。
#
# ★ 与 config/gate.env 同一套解析规则(三条硬规矩):
#   1) 【不要写 export】—— 因为 systemd 的 EnvironmentFile= 会原样注入,
#      而脚本这边又要 source 一遍; 写了 export 两边的行为就一致不了。
#   2) 【不要写命令替换/管道】—— EnvironmentFile= 不做命令替换, 写了会不一致。
#   3) 值里不要出现未加引号的空格 —— 需要引号时用 "..." 或 '...'。
#
# ★ 为什么单独抽一个文件:
#   run_gate_service.sh / preflight_gate.sh / render_gate_args.sh 三处都要用它。
#   三份各写一遍必然漂移(本仓库已经吃过"两份逻辑不同步"的亏, 见
#   render_gate_args.sh 头部注释), 所以统一放这里。
#
# 用法:
#   . /path/to/gate_env_local.sh
# ============================================================================

# ★ 允许用环境变量指定外挂文件位置(测试/多实例场景), 默认在仓库外。
GATE_ENV_LOCAL="${GATE_ENV_LOCAL:-/home/seeed/.config/rk-gate/gate.env.local}"

if [ -r "$GATE_ENV_LOCAL" ]; then
    # ★ 故意不用 `set -a`: 本文件只负责【读值】, 由调用方决定要不要 export。
    #   已有的 GATE_* 变量在 systemd 那条路上本来就是 exported, 重新赋值不丢
    #   export 属性, 所以不需要在这里 export。
    # shellcheck disable=SC1090
    . "$GATE_ENV_LOCAL"
    echo "[gate] 已加载外挂配置: $GATE_ENV_LOCAL" >&2
fi
