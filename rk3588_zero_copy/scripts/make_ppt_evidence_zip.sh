#!/usr/bin/env bash
# ============================================================================
# make_ppt_evidence_zip.sh —— 把汇报实证包打成 zip
#
# 为什么要有这个脚本:
#   .gitignore 里 *.zip 与 *.onnx/*.pth/*.whl 同属"二进制产物排除"(仓库里 0 个 zip 入库),
#   所以 zip 本身【不进版本库】, 只留在板端磁盘上。
#   但"怎么打出来的"必须留痕 —— 这个脚本就是那份留痕: 一条命令, 结果可复现。
#
# 用法:
#   ./scripts/make_ppt_evidence_zip.sh          # 打包 + 列出内容
#   ./scripts/make_ppt_evidence_zip.sh --list    # 只列内容不打包
# ============================================================================
set -uo pipefail

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
DOCS="$PROJ/docs"
SRC="$DOCS/ppt_evidence"
ZIP="$DOCS/ppt_evidence.zip"

if [ ! -d "$SRC" ]; then
    echo "[FATAL] 找不到 $SRC" >&2
    exit 1
fi

if [ "${1:-}" != "--list" ]; then
    cd "$DOCS" || exit 1
    rm -f "$ZIP"
    if command -v zip >/dev/null 2>&1; then
        zip -r -q "$ZIP" ppt_evidence
    else
        python3 -m zipfile -c "$ZIP" ppt_evidence
    fi
    echo "已生成: $ZIP  ($(du -h "$ZIP" | cut -f1))"
    echo
fi

echo "════ 压缩包内容清单 ════"
if command -v unzip >/dev/null 2>&1; then
    unzip -l "$ZIP" | sed -n '3,$p' | awk 'NF>=4 && $1>0 {printf "  %-58s %8s\n", $4, $1}'
else
    python3 - "$ZIP" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    for i in z.infolist():
        if i.file_size:
            print(f"  {i.filename:<58} {i.file_size:>8}")
PY
fi
echo
echo "包路径: $ZIP"
echo "文件数: $(find "$SRC" -type f | wc -l)   总大小: $(du -sh "$SRC" | cut -f1)"
