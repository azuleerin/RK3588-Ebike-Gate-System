#!/usr/bin/env bash
# ============================================================================
# sync_model_links.sh —— 重建 rk3588_zero_copy/model/ 指向 ../models/ 的镜像
#
# 背景:
#   models/ 是所有 .rknn 与标签文件的【单一真源】, 但 git 不保留硬链接,
#   所以 rk3588_zero_copy/model/ 这个"工程内快捷路径"被 .gitignore 排除在
#   版本库之外 —— 克隆下来的新机器上这个目录是空的, ./run_demo.sh 找不到模型。
#   本脚本就是把那层镜像重新建出来。
#
# 做法:
#   优先硬链接(同一文件系统, 零额外磁盘占用, 两边永远是同一份文件);
#   跨文件系统时退化成相对符号链接(仍然零拷贝、仍然单一真源)。
#
# 用法:
#   tools/sync_model_links.sh          # 建镜像
#   tools/sync_model_links.sh --check  # 只检查, 不写任何东西
# ============================================================================
set -Eeuo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SRC_DIR="$ROOT/models"
DST_DIR="$ROOT/rk3588_zero_copy/model"

CHECK_ONLY=0
[ "${1:-}" = "--check" ] && CHECK_ONLY=1

[ -d "$SRC_DIR" ] || { echo "❌ 找不到模型真源目录: $SRC_DIR" >&2; exit 1; }

mkdir -p "$DST_DIR"

linked=0
copied=0
missing=0
mismatch=0

shopt -s nullglob
for src in "$SRC_DIR"/*; do
    base="$(basename "$src")"
    [ -f "$src" ] || continue
    dst="$DST_DIR/$base"

    # --- 校验模式: 只报告状态 ---
    if [ "$CHECK_ONLY" = "1" ]; then
        if [ ! -e "$dst" ]; then
            echo "  ✗ 缺失: $base"
            missing=$((missing + 1))
            continue
        fi
        src_inode="$(stat -c '%i' "$src")"
        dst_inode="$(stat -c '%i' "$dst" 2>/dev/null || echo 0)"
        if [ "$src_inode" = "$dst_inode" ]; then
            echo "  ✓ 硬链接: $base"
            linked=$((linked + 1))
        elif [ -L "$dst" ] && [ -e "$dst" ]; then
            echo "  ✓ 符号链接: $base -> $(readlink "$dst")"
            linked=$((linked + 1))
        else
            echo "  ! 内容不一致(需重建): $base"
            mismatch=$((mismatch + 1))
        fi
        continue
    fi

    # --- 重建模式 ---
    rm -f "$dst"
    if ln -f -- "$src" "$dst" 2>/dev/null; then
        echo "  ✓ 硬链接: $base"
        linked=$((linked + 1))
    elif ln -sf -- "../../models/$base" "$dst" 2>/dev/null; then
        echo "  ✓ 符号链接(跨文件系统回退): $base"
        copied=$((copied + 1))
    else
        echo "  ✗ 失败: $base" >&2
        mismatch=$((mismatch + 1))
    fi
done

echo "------------------------------------------------------------"
if [ "$CHECK_ONLY" = "1" ]; then
    echo "检查完成: 正常 $linked, 缺失 $missing, 不一致 $mismatch"
    [ "$missing" -eq 0 ] && [ "$mismatch" -eq 0 ] || exit 1
else
    echo "镜像完成: 硬链接 $linked, 符号链接 $copied, 失败 $mismatch"
    echo "目标: $DST_DIR  <-  真源: $SRC_DIR"
fi
