#!/usr/bin/env bash
# ============================================================================
# publish_release.sh —— 推代码 + 建 Release + 上传白皮书 PDF 附件
#
# 需要一个 GitHub Token (classic 勾 repo 权限, 或 fine-grained 给到本仓库
# Contents: Read and write)。通过环境变量传进来:
#
#   GITHUB_TOKEN=ghp_xxx ./scripts/publish_release.sh
#
# 做的事:
#   1. git push main + --tags
#   2. 用 GitHub API 创建 Release (tag / 标题 / 正文)
#   3. 把 docs/RK3588边缘AI系统交付白皮书.pdf 作为附件上传
#
# 幂等: Release 已存在会跳过创建, 直接复用; 附件同名会覆盖。
# ============================================================================
set -Eeuo pipefail

TAG="${TAG:-v2.0-gate-4ch}"
REPO_SLUG="${REPO_SLUG:-azuleerin/RK3588-Ebike-Gate-System}"
API="https://api.github.com"
UPLOAD_API="https://uploads.github.com"

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
cd "$ROOT"

NOTES_FILE="docs/RELEASE_NOTES_${TAG}.md"
ASSET_FILE="docs/RK3588边缘AI系统交付白皮书.pdf"

# ---------------------------------------------------------------- 凭据
TOKEN="${GITHUB_TOKEN:-${GH_TOKEN:-}}"
if [ -z "$TOKEN" ]; then
    echo "❌ 没有找到 Token。请这样调用:" >&2
    echo "    GITHUB_TOKEN=ghp_xxx ./scripts/publish_release.sh" >&2
    echo "   (classic token 勾选 repo 权限; fine-grained 需要本仓库 Contents: Read and write)" >&2
    exit 1
fi

auth=(-H "Authorization: Bearer ${TOKEN}" -H "Accept: application/vnd.github+json"
      -H "X-GitHub-Api-Version: 2022-11-28" -H "User-Agent: rk3588-publish")

for f in "$NOTES_FILE" "$ASSET_FILE"; do
    [ -f "$f" ] || { echo "❌ 找不到文件: $f" >&2; exit 1; }
done

# ---------------------------------------------------------------- 1. push
if [ "${SKIP_PUSH:-0}" = "1" ]; then
    echo "==> [1/3] 跳过 push (SKIP_PUSH=1)"
else
    echo "==> [1/3] 推送 main 与 tag ${TAG}"
    # 用临时 credential.helper 供 token, 不写进 .git/config, 也不进 remote URL
    helper="!f() { echo username=x-access-token; echo password=${TOKEN}; }; f"
    if ! git -c "credential.helper=${helper}" push origin main; then
        echo "❌ push 失败。通常是 Token 无效或缺少 repo / Contents:Write 权限。" >&2
        exit 1
    fi
    if ! git -c "credential.helper=${helper}" push origin "refs/tags/${TAG}"; then
        echo "❌ 推送 tag ${TAG} 失败。" >&2
        exit 1
    fi
fi

# ---------------------------------------------------------------- 2. 建 Release
echo "==> [2/3] 创建 Release ${TAG}"
existing="$(curl -sS "${auth[@]}" "${API}/repos/${REPO_SLUG}/releases/tags/${TAG}" \
           | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("id") or "")' 2>/dev/null || true)"

if [ -n "$existing" ]; then
    RELEASE_ID="$existing"
    echo "    Release 已存在 (id=${RELEASE_ID}), 复用"
else
    payload="$(python3 - "$NOTES_FILE" "$TAG" <<'PY'
import json, sys
notes = open(sys.argv[1], encoding='utf-8').read()
tag = sys.argv[2]
print(json.dumps({
    "tag_name": tag,
    "name": f"{tag} —— 4 路大门架构 / NPU 三核全局调度 / Web 控制台 3.0",
    "body": notes,
    "draft": False,
    "prerelease": False,
}, ensure_ascii=False))
PY
)"
    resp="$(curl -sS "${auth[@]}" -X POST "${API}/repos/${REPO_SLUG}/releases" -d "$payload")"
    RELEASE_ID="$(printf '%s' "$resp" \
        | python3 -c 'import json,sys
try:
    d=json.load(sys.stdin)
except Exception:
    print(""); raise SystemExit
print(d.get("id") or "")' 2>/dev/null || true)"
    if [ -z "$RELEASE_ID" ]; then
        echo "❌ 创建 Release 失败, GitHub 返回:" >&2
        printf '%s' "$resp" | python3 -c 'import json,sys
try:
    d=json.load(sys.stdin)
    print("   ", d.get("message", d))
    if "documentation_url" in d: print("   ", d["documentation_url"])
except Exception:
    print("   ", sys.stdin.read()[:400])' >&2
        exit 1
    fi
    echo "    创建成功 (id=${RELEASE_ID})"
fi

# ---------------------------------------------------------------- 3. 上传附件
echo "==> [3/3] 上传附件: $(basename "$ASSET_FILE")"
asset_name="$(basename "$ASSET_FILE")"
# 文件名含中文, 查询串要 percent-encode
asset_name_enc="$(python3 -c 'import sys,urllib.parse; print(urllib.parse.quote(sys.argv[1]))' "$asset_name")"

# 同名附件先删掉再传 (DELETE 要的是 asset id, 不是 release id)
old_asset_id="$(curl -sS "${auth[@]}" "${API}/repos/${REPO_SLUG}/releases/${RELEASE_ID}/assets" \
    | python3 -c "
import json,sys
try:
    for a in json.load(sys.stdin):
        if a.get('name') == sys.argv[1]:
            print(a['id']); break
except Exception:
    pass
" "$asset_name")"
if [ -n "$old_asset_id" ]; then
    curl -sS "${auth[@]}" -X DELETE \
        "${API}/repos/${REPO_SLUG}/releases/assets/${old_asset_id}" >/dev/null
    echo "    删除同名旧附件 (id=${old_asset_id})"
fi

curl -sS "${auth[@]}" -X POST \
    "${UPLOAD_API}/repos/${REPO_SLUG}/releases/${RELEASE_ID}/assets?name=${asset_name_enc}" \
    -H "Content-Type: application/octet-stream" \
    --data-binary "@${ASSET_FILE}" \
    | python3 -c 'import json,sys; d=json.load(sys.stdin); print("    上传完成:", d.get("name"), d.get("size"), "bytes")'

echo ""
echo "✅ 完成 —— https://github.com/${REPO_SLUG}/releases/tag/${TAG}"
