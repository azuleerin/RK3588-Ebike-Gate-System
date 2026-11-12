#!/bin/bash
# ============================================================================
# install_deps.sh —— RK3588 板端离线安装 board_deps/*.deb
#
#   用法:
#     sudo ./install_deps.sh board_deps
#     sudo ./install_deps.sh board_deps --allow-apt    # 允许用 apt 补依赖(需外网)
#     ./install_deps.sh board_deps --dry-run          # 只检查不安装
#     ./install_deps.sh board_deps --verify-only     # 只做安装后验证
#
# 设计要点:
#   * 板子【没有外网】, 所以不能用 `apt-get -f install` 去网上拉依赖。
#     board_deps 里已经包含完整依赖闭包, `dpkg -i` 一次装齐即可。
#   * 安装前逐个检查 .deb 的架构, 出现非 arm64 立刻中止(绝不可能装错架构)。
#   * dpkg -i 会先全部解包再统一配置, 所以命令顺序无关, 不用手工排序。
# ============================================================================
set -u

DIR="${1:-board_deps}"
ALLOW_APT=0
DRY_RUN=0
VERIFY_ONLY=0
for a in "$@"; do
  case "$a" in
    --allow-apt)   ALLOW_APT=1 ;;
    --dry-run)     DRY_RUN=1 ;;
    --verify-only) VERIFY_ONLY=1 ;;
    board_deps)    DIR="$a" ;;
  esac
done

RED=$'\033[31m'; GRN=$'\033[32m'; YEL=$'\033[33m'; CYN=$'\033[36m'; OFF=$'\033[0m'
say()  { echo "${CYN}==> $1${OFF}"; }
ok()   { echo "  ${GRN}[OK]${OFF} $1"; }
warn() { echo "  ${YEL}[!]${OFF}  $1"; }
err()  { echo "  ${RED}[X]${OFF}  $1"; }

# ---------------------------------------------------------------- 0. 环境
say "板端环境"
ARCH=$(dpkg --print-architecture 2>/dev/null || echo unknown)
echo "    dpkg 架构 : $ARCH"
echo "    内核      : $(uname -r)"
if [ "$ARCH" != "arm64" ] && [ "$ARCH" != "aarch64" ]; then
    err "这个脚本只适用于 arm64 板子(当前 $ARCH)"
    exit 1
fi

if [ ! -d "$DIR" ]; then
    err "找不到目录: $DIR"
    echo "    用法: sudo $0 <board_deps 目录>"
    exit 1
fi
cd "$DIR" || exit 1
DEBS=(*.deb)
if [ ! -e "${DEBS[0]}" ]; then
    err "$DIR 里没有 .deb 文件"
    exit 1
fi
COUNT=${#DEBS[@]}
echo "    待安装    : $COUNT 个 .deb ($(du -ch ./*.deb 2>/dev/null | tail -1 | cut -f1))"

# ---------------------------------------------------------------- 1. 架构检查
say "检查 .deb 架构 (必须是 arm64/all)"
BAD=0
for d in "${DEBS[@]}"; do
    a=$(dpkg-deb -f "$d" Architecture 2>/dev/null)
    case "$a" in
        arm64|all) ;;
        *) err "$d 的架构是 '$a' —— 绝对不能装"; BAD=1 ;;
    esac
done
if [ "$BAD" != "0" ]; then
    err "发现非 arm64 的包, 中止(重新下载 board_deps)"
    exit 1
fi
ok "全部 $COUNT 个包都是 arm64/all"

if [ "$DRY_RUN" = "1" ]; then
    say "Dry run, 不安装"
    for d in "${DEBS[@]}"; do
        printf "    %-58s %s\n" "$d" "$(dpkg-deb -f "$d" Package 2>/dev/null) $(dpkg-deb -f "$d" Version 2>/dev/null)"
    done
    exit 0
fi

# ---------------------------------------------------------------- 2. 安装
if [ "$VERIFY_ONLY" != "1" ]; then

    # ---- 2.1 核心库降级保护 ----------------------------------------------
    # Ubuntu Ports 的核心库(libc6 等)版本可能和板子上已装的不一样。
    # 盲目 dpkg -i 会把核心库【降级】, 板子直接起不来(systemd/动态链接全挂)。
    # 规则: 核心包如果板子上已有的版本 >= 包里的版本, 就跳过它。
    CORE_PKGS="libc6 libc6-dev libgcc-s1 libstdc++6 zlib1g"
    TO_INSTALL=()
    SKIPPED=0
    for d in "${DEBS[@]}"; do
        pkg=$(dpkg-deb -f "$d" Package 2>/dev/null)
        ver=$(dpkg-deb -f "$d" Version 2>/dev/null)
        inst=$(dpkg-query -W -f='${Version}' "$pkg" 2>/dev/null)
        skip=0
        case " $CORE_PKGS " in
            *" $pkg "*) ;;
            *) ;;
        esac
        if [ $skip = 0 ] && [ -n "$inst" ] && [ -n "$ver" ]; then
            if dpkg --compare-versions "$inst" ge "$ver"; then
                warn "$pkg: 板子上已有 $inst >= 包里 $ver -> 跳过(防止核心库降级)"
                SKIPPED=$((SKIPPED+1))
                skip=1
            fi
        fi
        [ $skip = 0 ] && TO_INSTALL+=("$d")
    done
    if [ "$SKIPPED" != "0" ]; then
        ok "跳过 $SKIPPED 个核心包(板子上版本更新, 降级会搞坏系统)"
    fi
    if [ ${#TO_INSTALL[@]} -eq 0 ]; then
        warn "没有需要安装的包"
    else
        say "dpkg -i ${#TO_INSTALL[@]} 个包 (先解包后统一配置, 顺序无关)"
        # dpkg -i 一次传全部: 它会先解包所有包再配置, 依赖顺序自动处理
        if sudo dpkg -i "${TO_INSTALL[@]}"; then
            ok "dpkg -i 完成"
        else
            warn "dpkg -i 返回非 0, 继续尝试 dpkg --configure -a"
            # MPP 那两个包是 Radxa bionic 版构建的, 依赖声明可能和 26.04 对不上,
            # 用 --force-depends 强行解包(它们只需要 glibc>=2.17, 26.04 完全满足)
            warn "重试 --force-depends"
            sudo dpkg -i --force-depends "${TO_INSTALL[@]}" || true
        fi
    fi

    say "dpkg --configure -a (配置刚才解包但没配好的包)"
    if sudo dpkg --configure -a; then
        ok "配置完成"
    else
        warn "仍有包没配置成功, 下面会列出"
    fi

    # 还有问题才考虑 apt(需要外网)
    UNCONF=$(dpkg -l 2>/dev/null | awk '$1 ~ /^i[UHF]/ {print $2" ("$1")"}')
    if [ -n "$UNCONF" ]; then
        warn "以下包处于未配置状态:"
        echo "$UNCONF" | sed 's/^/      /'
        if [ "$ALLOW_APT" = "1" ]; then
            warn "--allow-apt: 尝试用 apt 补依赖(需要外网)"
            sudo apt-get update && sudo apt-get -f install -y
        else
            warn "板子没有外网时不要用 apt-get -f install。"
            warn "缺什么就把对应的 .deb 再下载一份放进 board_deps 重跑本脚本。"
            warn "查看具体缺什么: dpkg -l | grep '^iU'"
        fi
    fi
fi

# ---------------------------------------------------------------- 3. ldconfig
say "刷新动态库缓存"
sudo ldconfig
ok "ldconfig 完成"

# ---------------------------------------------------------------- 4. 验证
say "验证"
FAIL=0

chk() {  # chk <描述> <命令...>
    local desc="$1"; shift
    if "$@" >/dev/null 2>&1; then ok "$desc"; else warn "$desc  —— 没找到"; FAIL=1; fi
}

echo "    -- 编译工具链"
chk "g++"      sh -c 'command -v g++'
chk "cmake"    sh -c 'command -v cmake'
chk "make"     sh -c 'command -v make'
if command -v g++ >/dev/null 2>&1; then
    echo "        $(g++ --version | head -1)"
fi

echo "    -- FFmpeg 开发包"
for p in libavformat libavcodec libavutil libswscale libswresample; do
    v=$(pkg-config --modversion "$p" 2>/dev/null)
    if [ -n "$v" ]; then ok "$p $v"; else warn "$p 的 pkg-config 缺失"; FAIL=1; fi
done
chk "头文件 libavformat/avformat.h" test -f /usr/include/x86_64-linux-gnu/libavformat/avformat.h
chk "ffmpeg 命令"                   sh -c 'command -v ffmpeg'

echo "    -- RGA / DRM"
chk "libdrm-dev"  sh -c 'pkg-config --modversion libdrm'
chk "drm 头文件"  sh -c 'ls /usr/include/libdrm/drm.h'

echo "    -- OpenCV"
v=$(pkg-config --modversion opencv4 2>/dev/null)
if [ -n "$v" ]; then
    ok "opencv4 $v"
else
    warn "没有 opencv4 的 pkg-config"
    if [ -f /usr/include/opencv4/opencv2/core.hpp ]; then
        ok "但头文件在 /usr/include/opencv4 (cmake 可能仍能找到)"
    else
        FAIL=1
    fi
fi

echo "    -- Rockchip MPP (可选, Ubuntu 源里没有)"
if [ -f /usr/include/rockchip/rk_mpi.h ]; then
    ok "MPP 头文件 /usr/include/rockchip/rk_mpi.h"
else
    warn "没有 MPP 头文件 -> 本工程会自动关掉 MPP 支持(仍可编译, 走 CPU 映射地址路径)"
fi
if ldconfig -p 2>/dev/null | grep -q librockchip_mpp; then
    ok "librockchip_mpp: $(ldconfig -p | grep -m1 librockchip_mpp | sed 's/.*=> //')"
else
    warn "librockchip_mpp 不在 ldconfig 缓存里"
fi

echo "    -- FFmpeg 有没有 rkmpp 硬解码器"
if command -v ffmpeg >/dev/null 2>&1; then
    if ffmpeg -hide_banner -decoders 2>/dev/null | grep -q rkmpp; then
        ok "有 rkmpp 解码器 -> 会走 MPP 硬解"
    else
        warn "没有 rkmpp 解码器 (Ubuntu 的 ffmpeg 不带 Rockchip MPP 支持)"
        warn "  -> 程序会自动退回【软解码】, 流水线照常跑, 但 CPU 占用高一些"
        warn "  -> 要硬解需从 Rockchip SDK 拿 ffmpeg-rockchip"
    fi
fi

echo ""
if [ "$FAIL" = "0" ]; then
    ok "依赖检查通过, 可以编译了"
else
    warn "有些依赖缺失, 编译可能失败 —— 见上面带 [!] 的行"
fi
echo ""
echo "下一步:"
echo "    cd <工程目录> && chmod +x *.sh && ./board_setup.sh"
