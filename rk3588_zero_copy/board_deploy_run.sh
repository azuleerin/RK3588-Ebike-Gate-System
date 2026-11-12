#!/bin/bash
# ============================================================================
# board_deploy_run.sh —— 板端一键: 解压 -> 编译 -> 运行
#
# 放在 $HOME 下执行(和 rk3588_zero_copy.tar.gz 同级):
#     cd $HOME
#     tar xzf rk3588_zero_copy.tar.gz
#     ./rk3588_zero_copy/board_deploy_run.sh
#
# 或直接一行(等价):
#     cd $HOME && rm -rf rk3588_zero_copy && \
#       tar xzf rk3588_zero_copy.tar.gz && cd rk3588_zero_copy && \
#       chmod +x *.sh && ./board_setup.sh && ./run_demo.sh --no-show --frames 300
#
# 参数:
#     --setup-only    只编译, 不运行
#     --no-build      跳过编译(用已有的二进制)
#     --skip-setup    跳过 board_setup.sh 的依赖安装(只做编译)
#     其余参数原样传给 run_demo.sh
# ============================================================================
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
PARENT="$(cd "${HERE}/.." && pwd)"
NAME="$(basename "${HERE}")"

RUN_ARGS=()
DO_SETUP=1
DO_BUILD=1
DO_RUN=1

for a in "$@"; do
  case "$a" in
    --setup-only) DO_RUN=0 ;;
    --no-build)   DO_BUILD=0 ;;
    --skip-setup) DO_SETUP=0 ;;
    *)            RUN_ARGS+=("$a") ;;
  esac
done

# ---- 1) 解包(如果同级有压缩包, 就地解开) --------------------------------
cd "${PARENT}"
for f in "${NAME}.tar.gz" "${NAME}.tgz" "${NAME}.zip"; do
  if [ -f "$f" ]; then
    echo "==> 解压 ${f}"
    rm -rf "${NAME}"
    case "$f" in
      *.zip) (command -v unzip >/dev/null 2>&1 && unzip -q -o "$f" \
              || python3 -m zipfile -e "$f" .) ;;
      *)    tar xzf "$f" ;;
    esac
    break
  fi
done

cd "${HERE}"
chmod +x *.sh 2>/dev/null || true

# ---- 2) 环境自检 + 依赖 + 编译 -------------------------------------------
if [ "${DO_SETUP}" = "1" ]; then
  if [ "${DO_BUILD}" = "1" ]; then
    echo "==> board_setup.sh (自检 + 装依赖 + 编译)"
    ./board_setup.sh
  else
    echo "==> 跳过 board_setup.sh (--no-build)"
  fi
elif [ "${DO_BUILD}" = "1" ]; then
  echo "==> 只编译 (跳过依赖安装)"
  mkdir -p build && cd build
  cmake .. -DCMAKE_BUILD_TYPE=Release
  make -j"$(nproc)"
  cd "${HERE}"
fi

# ---- 3) 运行 -------------------------------------------------------------
if [ "${DO_RUN}" = "1" ]; then
  echo "==> run_demo.sh ${RUN_ARGS[*]}"
  ./run_demo.sh ${RUN_ARGS[@]+"${RUN_ARGS[@]}"}
else
  echo "==> 只编译不运行 (--setup-only)"
  echo "    之后手动运行: cd ${HERE} && ./run_demo.sh --no-show --frames 300"
fi
