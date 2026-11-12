#!/bin/bash
# ============================================================================
# board_setup.sh —— 在 RK3588 板子上直接执行: 环境自检 + 装依赖 + 编译
#
#   cd /path/to/rk3588_zero_copy && chmod +x board_setup.sh && ./board_setup.sh
#
# 这个工程是自包含的: 头文件(rknn_api.h / im2d.h)和运行期库(librknnrt.so /
# librga.so)都在 third_party/ 里, 模型和标签在 model/ 里, 整个目录拷过来就能编。
# ============================================================================

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

echo "==================== 1) 环境自检 ===================="
echo "-- 系统"; (cat /etc/os-release 2>/dev/null | head -3); uname -a
echo "-- 架构"; uname -m
echo
echo "-- NPU / RGA / MPP / DRM 设备节点"
for d in /dev/rknpu /dev/rga /dev/mpp_service /dev/dri /dev/dma_heap; do
  if [ -e "$d" ]; then ls -ld "$d"; else echo "  缺失: $d"; fi
done
ls /dev/dri 2>/dev/null | sed 's/^/    \/dev\/dri\//'
ls /dev/dma_heap 2>/dev/null | sed 's/^/    \/dev\/dma_heap\//'
echo
echo "-- 系统里的 Rockchip 库"
for l in librknnrt.so librga.so librockchip_mpp.so; do
  printf "  %-22s " "$l"
  hit=$(ls /usr/lib/$l /usr/lib/aarch64-linux-gnu/$l /usr/local/lib/$l 2>/dev/null | head -1)
  if [ -n "$hit" ]; then echo "$hit"; else echo "(没有, 用 third_party/lib 里的)"; fi
done
echo "  MPP 头文件"; (ls /usr/include/rockchip/rk_mpi.h 2>/dev/null || echo "    (没有 -> 自动关掉 MPP 支持)")
echo
echo "-- 工具链"
for t in g++ gcc make cmake pkg-config; do
  printf "  %-12s " "$t"; (command -v $t) || echo "缺失"
done
g++ --version 2>/dev/null | head -1
cmake --version 2>/dev/null | head -1
echo
echo "-- FFmpeg: 有没有 rkmpp 硬解码器"
if command -v ffmpeg >/dev/null 2>&1; then
  ffmpeg -version 2>/dev/null | head -1
  if ffmpeg -decoders 2>/dev/null | grep -i rkmpp; then
    echo "  ==> 有 rkmpp 解码器, 会走硬解"
  else
    echo "  ==> 没有 rkmpp 解码器(系统自带 FFmpeg 都没有)"
    echo "      程序会自动退回【软解码】, 流水线仍然能跑通,"
    echo "      只是 NV12 源变成 CPU 内存(日志会打印 'NV12:cpu-ptr'),"
    echo "      且 CPU 占用会高一些。要硬解需从 Rockchip SDK 拷"
    echo "      librockchip_mpp.so + 带 rkmpp 的 FFmpeg。"
  fi
else
  echo "  没有 ffmpeg 命令(库可能在但没装命令行)"
fi
echo
echo "-- OpenCV"
(pkg-config --modversion opencv4 2>/dev/null && echo "  pkg-config: opencv4 OK") || echo "  未安装"
echo
echo "-- 磁盘空间"; df -h . | tail -1

echo
echo "==================== 2) 安装依赖 ===================="
APT_PACKAGES="build-essential cmake pkg-config libopencv-dev \
libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
ffmpeg libdrm-dev"

echo "sudo apt install -y ${APT_PACKAGES}"
if sudo apt-get update -qq; then
  # shellcheck disable=SC2086
  sudo apt-get install -y ${APT_PACKAGES} || echo "[警告] 有包没装上, 见上面的报错"
else
  echo "[警告] apt update 失败(可能没网), 跳过安装"
fi

echo
echo "==================== 3) 编译 ===================="
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release || exit 1
make -j"$(nproc 2>/dev/null || echo 4)" || exit 1

echo
echo "编译完成: $(pwd)/rk_zero_copy"
echo
echo "下一步:"
echo "  cd $HERE && ./run_demo.sh            # 跑检测"
echo "  cd $HERE && ./run_demo.sh --no-show --frames 300   # 只跑性能不显示窗口"
echo "  cd $HERE && ./run_parity.sh          # 与 Python 基线对拍(需要 docker, 可跳过)"
