#!/bin/bash
# ============================================================================
# p0_mpp_verify.sh —— P0 验收脚本: 板端 MPP 硬解 + dma-buf 零拷贝
#
# 用法(在板子上, 工程根目录 $HOME/rk3588_zero_copy 下):
#     ./p0_mpp_verify.sh
#
# 它做什么:
#   1. 环境体检(架构/工具链/ffmpeg dev/opencv dev/MPP 头与库/VPU 内核驱动)
#   2. 只安装【MPP 那两个 .deb】—— 不碰 libc6 等核心库, 不会把板子搞坏
#   3. cmake 配置, 必须能看到 MPP 头/库路径
#   4. 编译
#   5. 跑 --decoder mpp --no-show --frames 300
#   6. 断言两个验收关键字 + dec 耗时, 给出 PASS/FAIL 结论
#   7. 失败时自动打印诊断包(dmesg / ldconfig / CMake 输出 / 运行日志)
#
# 环境变量(都有默认值, 一般不用改):
#   MPP_DEB_DIR=./mpp_debs     已下载好的 MPP .deb 目录(优先于网络下载)
#   DL_HOST=192.168.31.214    开发机地址(板子无外网, 从开发机拉包)
#   DL_PORT=8900              开发机 HTTP 端口
#   FRAMES=300                跑多少帧
#   SKIP_INSTALL=1            跳过装包
#   SKIP_BUILD=1              跳过编译
#   RTSP=rtsp://192.168.31.200:8557/h264
# ============================================================================
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

MPP_DEB_DIR="${MPP_DEB_DIR:-$HERE/mpp_debs}"
DL_HOST="${DL_HOST:-192.168.31.214}"
DL_PORT="${DL_PORT:-8900}"
FRAMES="${FRAMES:-300}"
RTSP="${RTSP:-rtsp://192.168.31.200:8557/h264}"
SKIP_INSTALL="${SKIP_INSTALL:-0}"
SKIP_BUILD="${SKIP_BUILD:-0}"

LOG="/tmp/p0_mpp_verify.log"
CM_LOG="/tmp/p0_cmake.log"
RUN_LOG="/tmp/p0_run.log"
: > "$LOG"

RED=$'\033[31m'; GRN=$'\033[32m'; YEL=$'\033[33m'; CYN=$'\033[36m'; OFF=$'\033[0m'
say() { echo "${CYN}==> $1${OFF}" | tee -a "$LOG"; }
ok()  { echo "  ${GRN}[OK]${OFF} $1"   | tee -a "$LOG"; }
warn(){ echo "  ${YEL}[!]${OFF}  $1"  | tee -a "$LOG"; }
err() { echo "  ${RED}[X]${OFF} $1"   | tee -a "$LOG"; }
step(){ echo "" | tee -a "$LOG"; say "$1"; }

# ---------------------------------------------------------------- 0. 体检
step "0) 板端环境体检"
echo "    架构      : $(uname -m)  |  内核 $(uname -r)" | tee -a "$LOG"
echo "    dpkg 架构 : $(dpkg --print-architecture 2>/dev/null)" | tee -a "$LOG"
echo "    时间      : $(date)" | tee -a "$LOG"
echo "" | tee -a "$LOG"

for t in g++ cmake make; do
  if command -v "$t" >/dev/null 2>&1; then
    ok "$t  $($t --version 2>/dev/null | head -1)"
  else
    err "$t 缺失 —— 编译不了(装 build-essential)"
  fi
done

for p in libavformat libavcodec libavutil libswscale; do
  v=$(pkg-config --modversion "$p" 2>/dev/null)
  if [ -n "$v" ]; then ok "ffmpeg-dev $p $v"; else warn "缺 ffmpeg-dev $p"; fi
done

if [ -f /usr/include/opencv4/opencv2/core.hpp ]; then
  ok "OpenCV 头文件 /usr/include/opencv4"
else
  warn "没有 OpenCV 头文件(板子上可能只有运行库)"
fi

if [ -f /usr/include/rockchip/rk_mpi.h ]; then
  ok "MPP 头文件已装 /usr/include/rockchip/rk_mpi.h"
  HAVE_MPP_HDR=1
else
  warn "MPP 头文件缺失"
  HAVE_MPP_HDR=0
fi
if ldconfig -p 2>/dev/null | grep -q librockchip_mpp; then
  ok "MPP 库: $(ldconfig -p | grep -m1 librockchip_mpp | sed 's/.*=> //')"
  HAVE_MPP_LIB=1
else
  warn "librockchip_mpp 不在 ldconfig 缓存里"
  HAVE_MPP_LIB=0
fi

if dmesg 2>/dev/null | grep -iE "rkvdec|vpu|mpp|rockchip.*video" | tail -5 | grep -q .; then
  ok "内核 VPU/解码驱动有痕迹:"
  dmesg 2>/dev/null | grep -iE "rkvdec|vpu|mpp|rockchip.*video" | tail -5 | sed 's/^/        /' | tee -a "$LOG"
else
  warn "dmesg 里没看到 VPU/解码驱动痕迹(可能没权限看 dmesg, 不一定是坏事)"
fi

# ---------------------------------------------------------------- 1. 装 MPP
step "1) 安装 Rockchip MPP (只装这 2 个包, 不碰核心库)"
if [ "$SKIP_INSTALL" = "1" ]; then
  warn "SKIP_INSTALL=1, 跳过安装"
elif [ "$HAVE_MPP_HDR" = "1" ] && [ "$HAVE_MPP_LIB" = "1" ]; then
  ok "MPP 已经装好, 跳过安装"
else
  mkdir -p "$MPP_DEB_DIR"
  DEBS=()
  # 1a) 本地目录里已有的
  for f in "$MPP_DEB_DIR"/librockchip-mpp1_*.deb \
           "$MPP_DEB_DIR"/librockchip-mpp-dev_*.deb \
           "$HERE"/librockchip-mpp1_*.deb \
           "$HERE"/librockchip-mpp-dev_*.deb \
           $HOME/librockchip-mpp1_*.deb \
           $HOME/librockchip-mpp-dev_*.deb; do
    [ -f "$f" ] && DEBS+=("$f")
  done
  # 1b) 否则从开发机 HTTP 拉(板子没外网, 开发机有)
  if [ ${#DEBS[@]} -lt 2 ]; then
    BASE="http://${DL_HOST}:${DL_PORT}"
    echo "    从 ${BASE} 下载 MPP 包..." | tee -a "$LOG"
    fetch() {
      if command -v curl >/dev/null 2>&1; then
        curl -sSfL --connect-timeout 10 --max-time 120 -o "$2" "$1"
      elif command -v wget >/dev/null 2>&1; then
        wget -q -T 10 -O "$2" "$1"
      else
        err "板子上既没有 curl 也没有 wget —— 请用 scp/FileZilla 把 board_mpp/ 整个目录传到板子"
        return 1
      fi
    }
    fetch "${BASE}/librockchip-mpp1_20190626-1_arm64.deb"    "$MPP_DEB_DIR/librockchip-mpp1_20190626-1_arm64.deb"    || true
    fetch "${BASE}/librockchip-mpp-dev_20190626-1_arm64.deb" "$MPP_DEB_DIR/librockchip-mpp-dev_20190626-1_arm64.deb" || true
    for f in "$MPP_DEB_DIR"/librockchip-mpp*.deb; do
      [ -f "$f" ] && DEBS+=("$f")
    done
  fi

  if [ ${#DEBS[@]} -eq 0 ]; then
    err "没拿到任何 MPP .deb —— 装不了, 后面编译出来也没有硬解"
  else
    echo "    待安装:" | tee -a "$LOG"
    for d in "${DEBS[@]}"; do
      echo "      $(basename "$d")  ($(stat -c%s "$d" 2>/dev/null) B)" | tee -a "$LOG"
      a=$(dpkg-deb -f "$d" Architecture 2>/dev/null)
      if [ "$a" != "arm64" ] && [ "$a" != "all" ]; then
        err "$(basename "$d") 架构是 '$a', 拒绝安装"
        DEBS=()
        break
      fi
    done
  fi

  if [ ${#DEBS[@]} -gt 0 ]; then
    # mpp1 运行时 + mpp-dev 头文件, 一次 dpkg -i 一起装(顺序自动处理)
    # ⚠️ 管道里 if 取的是最后一个命令(tee)的退出码, 必须看 PIPESTATUS[0]
    sudo dpkg -i "${DEBS[@]}" 2>&1 | tee -a "$LOG"
    DPKG_RC=${PIPESTATUS[0]}
    if [ "$DPKG_RC" = "0" ]; then
      ok "dpkg -i 完成"
    else
      warn "dpkg -i 返回 $DPKG_RC, 用 --force-depends 重试"
      sudo dpkg -i --force-depends "${DEBS[@]}" 2>&1 | tee -a "$LOG" || true
    fi
    sudo dpkg --configure -a 2>&1 | tee -a "$LOG" || true
    sudo ldconfig
    if [ -f /usr/include/rockchip/rk_mpi.h ]; then
      ok "MPP 头文件就位 /usr/include/rockchip/rk_mpi.h"
    else
      err "MPP 头文件仍然缺失"
    fi
    if ldconfig -p 2>/dev/null | grep -q librockchip_mpp; then
      ok "MPP 库就位"
    else
      err "MPP 库仍然不在 ldconfig 缓存里"
    fi
  fi
fi

# ---------------------------------------------------------------- 2. cmake
step "2) CMake 配置 (必须能看到 MPP 头/库)"
rm -rf build
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1 | tee "$CM_LOG" | grep -E "^-- (MPP|RKNN|RGA|FFMPEG|OpenCV)" | tee -a "$LOG"
if grep -q "Configuring done" "$CM_LOG" && grep -q "Generating done" "$CM_LOG"; then
  ok "CMake 配置成功"
else
  err "CMake 配置失败, 错误尾部:"
  tail -30 "$CM_LOG" | sed 's/^/        /' | tee -a "$LOG"
fi
if grep -qE "MPP +=" "$CM_LOG"; then
  ok "CMake 找到 MPP: $(grep -E 'MPP +=' "$CM_LOG" | head -1)"
else
  err "CMake 没找到 MPP -> 会编成 RK_HAVE_MPP=0, 只能软解"
  err "  看 CMakeLists 的 find_path(MPP_INC_DIR rk_mpi.h PATH_SUFFIXES rockchip)"
fi
if grep -q "找不到 librockchip_mpp" "$CM_LOG"; then
  err "CMake 明确警告: 找不到 librockchip_mpp"
fi
cd "$HERE"

# ---------------------------------------------------------------- 3. 编译
if [ "$SKIP_BUILD" = "1" ]; then
  warn "SKIP_BUILD=1, 跳过编译"
else
  step "3) 编译"
  cd build
  make -j"$(nproc)" 2>&1 | tee -a "$LOG" | tail -5
  MAKE_RC=${PIPESTATUS[0]}
  if [ "$MAKE_RC" = "0" ]; then
    if [ -x rk_zero_copy ]; then
      ok "编译成功 -> $(pwd)/rk_zero_copy ($(stat -c%s rk_zero_copy) B)"
    else
      err "make 返回 0 但没有可执行文件"
    fi
  else
    err "编译失败 (make rc=$MAKE_RC), 完整输出见 $LOG"
  fi
  cd "$HERE"
fi

# ---------------------------------------------------------------- 4. 运行
step "4) 运行 (--decoder mpp --no-show --frames ${FRAMES})"
if [ ! -x build/rk_zero_copy ]; then
  err "没有可执行文件 build/rk_zero_copy, 跳过运行"
  RUN_OK=0
else
  export LD_LIBRARY_PATH="${HERE}/third_party/lib:${HERE}/lib:/usr/lib:${LD_LIBRARY_PATH:-}"
  ./run_demo.sh --no-show --frames "$FRAMES" --decoder mpp 2>&1 | tee "$RUN_LOG" | tail -20 | tee -a "$LOG"
  RUN_OK=1
fi

# ---------------------------------------------------------------- 5. 验收
step "5) 验收断言"
PASS=1
if [ "$RUN_OK" = "1" ]; then
  if grep -q "后端 = Rockchip MPP 硬解" "$RUN_LOG"; then
    ok "关键字1: [解码] 后端 = Rockchip MPP 硬解 (H.264, VPU)"
  else
    err "关键字1 缺失: [解码] 后端 = Rockchip MPP 硬解"
    grep -E "\[解码\]|\[警告\] MPP" "$RUN_LOG" | head -5 | sed 's/^/        /' | tee -a "$LOG"
    PASS=0
  fi

  if grep -q "数据源: dma-buf fd" "$RUN_LOG"; then
    ok "关键字2: [NV12] 数据源: dma-buf fd (RGA import)"
  else
    err "关键字2 缺失: [NV12] 数据源: dma-buf fd (RGA import)"
    grep -E "\[NV12\]" "$RUN_LOG" | head -5 | sed 's/^/        /' | tee -a "$LOG"
    PASS=0
  fi

  LAST=$(grep -oE "\[frame [0-9]+\].*" "$RUN_LOG" | tail -1)
  if [ -n "$LAST" ]; then
    echo "        $LAST" | tee -a "$LOG"
    DEC=$(echo "$LAST" | grep -oE "dec=[0-9.]+ms" | head -1 | cut -d= -f2 | tr -d 'ms')
    if [ -n "$DEC" ]; then
      DECV=$(echo "$DEC" | awk '{printf "%.2f", $1}')
      if awk "BEGIN{exit !($DECV < 5.0)}"; then
        ok "解码耗时 dec=${DECV}ms (< 5ms, 软解一般 15~30ms)"
      else
        warn "解码耗时 dec=${DECV}ms 还是偏高 —— 硬解一般 1~2ms"
        PASS=0
      fi
    fi
  else
    warn "日志里没有 [frame N] 统计行, 可能一帧都没跑出来"
    PASS=0
  fi
else
  PASS=0
fi

# ---------------------------------------------------------------- 6. 结论
step "6) 结论"
if [ "$PASS" = "1" ]; then
  echo "${GRN}  ✅ P0 通过: MPP 硬解 + dma-buf 零拷贝 都已生效${OFF}" | tee -a "$LOG"
else
  echo "${RED}  ❌ P0 未通过${OFF}" | tee -a "$LOG"
fi

step "7) 诊断包 (失败时把这段贴回来)"
{
  echo "### uname"; uname -a
  echo "### dpkg arch"; dpkg --print-architecture
  echo "### MPP 包"; dpkg -l | grep -E "librockchip-mpp" || echo "(没装)"
  echo "### ★ dpkg 历史(装 MPP 之前板子有没有自带 MPP? 有没有被我们降级?)"
  grep -iE "rockchip-mpp" /var/log/dpkg.log 2>/dev/null | tail -20 || echo "(读不到 dpkg.log)"
  echo "### ★ 全盘找 librockchip_mpp.so (有几个候选?)"
  find / -name "librockchip_mpp.so*" 2>/dev/null | head -20 || echo "(没找到)"
  echo "### MPP 头文件"; ls -la /usr/include/rockchip/ 2>&1 | head -15
  echo "### ldconfig"; ldconfig -p | grep -E "rockchip_mpp|rknnrt|librga" || echo "(空)"
  echo "### ★ 内核 MPP/VPU 设备节点"
  ls -l /dev/mpp_service /dev/rkvdec /dev/dri 2>&1 | head -10
  echo "### cmake MPP 行"; grep -E "MPP" "$CM_LOG" | head -10
  echo "### 运行日志尾部"; tail -40 "$RUN_LOG" 2>/dev/null
  echo "### dmesg vpu"; dmesg 2>/dev/null | grep -iE "rkvdec|vpu|mpp" | tail -20 || echo "(读不到 dmesg)"
  echo "### 运行时实际加载的 .so (若 build 存在)"
  [ -x ./build/rk_zero_copy ] && ./build/rk_zero_copy --mpp-diag 2>&1 | head -30
} | tee -a "$LOG"

echo "" | tee -a "$LOG"
echo "完整日志: $LOG   (cmake: $CM_LOG   运行: $RUN_LOG)" | tee -a "$LOG"
exit $([ "$PASS" = "1" ] && echo 0 || echo 1)
