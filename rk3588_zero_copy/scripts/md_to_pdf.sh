#!/usr/bin/env bash
# ============================================================================
# md_to_pdf.sh —— 把 Markdown 交付文档转成 PDF(md_to_html.py + chromium)
#
# 用法:
#   ./scripts/md_to_pdf.sh <输出.pdf> <md文件> [md文件...]
#   ./scripts/md_to_pdf.sh out.pdf doc.md
#
# 为什么要有这个包装脚本:
#   手工敲两条命令时很容易漏掉 chromium 的 --no-pdf-header-footer,
#   结果 PDF 每一页页脚都印着一长串 percent-encode 的 file:/// 地址,
#   页眉还有打印日期 —— 给客户看非常难看。
#   这里把参数固定住, 保证"同一份 md 每次都能生成同样的、能直接交付的 PDF"。
#
# 说明:
#   * 图片用【相对路径】引用, 所以要在能解析到图片的目录下运行本脚本
#     (一般就是仓库根目录)。
#   * 中间产物 .html 会留在同目录, 方便出问题时排查排版。
# ============================================================================
set -eu

if [ $# -lt 2 ]; then
    echo "用法: $0 <输出.pdf> <md文件> [md文件...]"
    exit 2
fi

OUT_PDF="$1"; shift
HTML="${OUT_PDF%.pdf}.html"
HERE="$(cd "$(dirname "$0")" && pwd)"

# chromium 可能不在默认 PATH 里(某些板子环境)
CHROME=""
for c in chromium chromium-browser google-chrome; do
    if command -v "$c" >/dev/null 2>&1; then CHROME="$c"; break; fi
done
[ -n "$CHROME" ] || { echo "❌ 找不到 chromium"; exit 1; }

python3 "$HERE/md_to_html.py" "$HTML" "$@"

# --no-pdf-header-footer: 关掉页眉(日期)和页脚(文件 URL) —— 交付件不能带这些
"$CHROME" --headless --disable-gpu --no-sandbox --no-pdf-header-footer \
          --print-to-pdf="$OUT_PDF" "$HTML" 2>&1 | tail -1

[ -s "$OUT_PDF" ] || { echo "❌ PDF 没生成"; exit 1; }
echo "✅ PDF 已生成: $OUT_PDF ($(du -h "$OUT_PDF" | cut -f1))"
