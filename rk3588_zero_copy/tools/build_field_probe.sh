#!/usr/bin/env bash
# ============================================================================
# build_field_probe.sh —— 编译现场预检用的流信息探针 (tools/field_probe.c)
#
# ★ 为什么需要它:
#   现场板子上【不一定】装了 ffmpeg/ffprobe。本板就没装:
#       $ ffprobe -version
#       bash: ffprobe: command not found
#   而 apt install ffmpeg 需要 root(本板 sudo 要密码), 现场不一定给得上。
#   但本工程本来就依赖 libavformat/libavcodec/libswscale
#   (CMakeLists.txt:182-199 用 pkg-config 找的), 头文件和库都在板上 ——
#   所以用工程【自己已有的依赖】编一个探针, 零额外依赖、不需要 root。
#
# ★ 产物:
#   tools/field_probe            (可执行文件, 不入库, 由本脚本现场生成)
#
# ★ scripts/field_preflight.sh 会在需要时【自动调用本脚本】,
#   所以正常情况下现场工程师什么都不用做, 直接跑预检即可。
#
# 用法:
#   bash tools/build_field_probe.sh          # 编到 tools/field_probe
#   bash tools/build_field_probe.sh --clean  # 删掉产物
# ============================================================================
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/field_probe.c"
BIN="$HERE/field_probe"

if [ "${1:-}" = "--clean" ]; then
    rm -f "$BIN"
    echo "[build_field_probe] 已删除 $BIN"
    exit 0
fi

if [ ! -r "$SRC" ]; then
    echo "[build_field_probe] 找不到源文件: $SRC" >&2
    exit 1
fi

# ---- 工具链 ----
if ! command -v cc >/dev/null 2>&1 && ! command -v gcc >/dev/null 2>&1; then
    echo "[build_field_probe] 没有 C 编译器(gcc/cc) —— 装不了探针。" >&2
    echo "[build_field_probe] 退化方案: 装 ffmpeg 后用 ffprobe, 或手工跑 ffmpeg 取流信息。" >&2
    exit 1
fi
CC="${CC:-$(command -v cc || command -v gcc)}"

# ---- 依赖头文件/库 ----
# 优先用 pkg-config; 没有 pkg-config 或 pkg-config 找不到, 就退回直接链接
# /usr/lib/aarch64-linux-gnu 下的 .so —— 那正是 CMakeLists.txt:189-190 的兜底路径。
PKG_CFLAGS=""
PKG_LIBS=""
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libavformat libavcodec libavutil libswscale; then
    PKG_CFLAGS="$(pkg-config --cflags libavformat libavcodec libavutil libswscale)"
    PKG_LIBS="$(pkg-config --libs libavformat libavcodec libavutil libswscale)"
    HAVE_PKG=1
else
    HAVE_PKG=0
    for hdr in libavformat/avformat.h libavcodec/avcodec.h libavutil/imgutils.h libswscale/swscale.h; do
        if [ ! -r "/usr/include/$hdr" ]; then
            echo "[build_field_probe] 缺少头文件 /usr/include/$hdr" >&2
            echo "[build_field_probe] 装法(需 root): sudo apt install libavformat-dev libavcodec-dev libavutil-dev libswscale-dev" >&2
            exit 1
        fi
    done
    PKG_LIBS="-lavformat -lavcodec -lavutil -lswscale"
fi

echo "[build_field_probe] CC=$CC  pkg-config=$HAVE_PKG"
echo "[build_field_probe] 编译 $SRC -> $BIN"

# shellcheck disable=SC2086
"$CC" -O2 -std=c11 -Wall -o "$BIN" "$SRC" $PKG_CFLAGS $PKG_LIBS

if [ ! -x "$BIN" ]; then
    echo "[build_field_probe] 编译失败(产物不存在)" >&2
    exit 1
fi

"$BIN" version
echo "[build_field_probe] OK: $BIN"
