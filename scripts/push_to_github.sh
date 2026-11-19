#!/usr/bin/env bash
# ============================================================================
# push_to_github.sh —— 带着"代理兜底"地 push
#
# 为什么需要这个脚本:
#   这台板子的 git 全局配了 http.proxy = http://192.168.8.21:7897,
#   但那台机器上的代理程序经常【没在跑】—— 于是 git 连 GitHub 直接失败:
#       Failed to connect to github.com port 443 via 192.168.8.21
#   而板子本身其实【能出网】(baidu / gitee / aliyun 都通), GitHub 直连也通
#   (实测 git -c http.proxy= -c https.proxy= ls-remote 正常返回)。
#   所以这里只做一件事: 【先探测代理通不通, 通就用, 不通就绕过】。
#
# 用法:
#   scripts/push_to_github.sh                 # push origin main
#   REMOTE=upstream BRANCH=dev scripts/push_to_github.sh
#
# ★ 认证仍需人工配置(安全红线: 不能代做账号操作):
#   二选一:
#   A) PAT   : git config --global credential.helper store
#               echo "https://<用户名>:<PAT>@github.com" > ~/.git-credentials
#               chmod 600 ~/.git-credentials
#   B) SSH   : ssh-keygen -t ed25519 -C "rk3588@board"
#               # 把 ~/.ssh/id_ed25519.pub 贴到 GitHub -> Settings -> SSH keys
#               git remote set-url origin git@github.com:azuleerin/RK3588-Ebike-Gate-System.git
#               # (实测 github.com:22 和 ssh.github.com:443 都通, 不依赖那个死代理)
# ============================================================================
set -u

cd "$(dirname "$0")/.." || exit 1

REMOTE="${REMOTE:-origin}"
BRANCH="${BRANCH:-main}"

PROXY="$(git config --get http.proxy 2>/dev/null || true)"
use_proxy=1

if [ -n "$PROXY" ]; then
    hostport="$(echo "$PROXY" | sed -E 's#^https?://##; s#/.*$##')"
    phost="${hostport%%:*}"
    pport="${hostport##*:}"
    [ "$pport" = "$phost" ] && pport=80
    if timeout 3 bash -c "echo >/dev/tcp/$phost/$pport" 2>/dev/null; then
        echo "[push] 代理可用: $PROXY —— 走代理"
    else
        echo "[push] 代理不可用 ($PROXY) —— 改走直连"
        use_proxy=0
    fi
else
    echo "[push] 未配置代理 —— 直连"
    use_proxy=0
fi

if [ "$use_proxy" = "1" ]; then
    exec git push "$REMOTE" "$BRANCH"
else
    exec git -c http.proxy= -c https.proxy= push "$REMOTE" "$BRANCH"
fi
