# Rockchip RK3588 aarch64 交叉编译工具链
#
# 用法:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-rockchip.cmake \
#         -DTOOLCHAIN_ROOT=/path/to/aarch64-rockchip-linux-gcc
#
# 也可以用环境变量: export TOOLCHAIN=/path/to/aarch64-rockchip-linux-gcc
#
# 工具链来源(Rockchip SDK):
#   ${SDK}/rk-linux/toolchain/aarch64-rockchip-linux-gcc/
#       bin/aarch64-rockchip-linux-gcc
#       bin/aarch64-rockchip-linux-g++
#       sysroot/                      <- 头文件和库都在这里
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT TOOLCHAIN_ROOT)
  if(DEFINED ENV{TOOLCHAIN})
    set(TOOLCHAIN_ROOT "$ENV{TOOLCHAIN}")
  else()
    set(TOOLCHAIN_ROOT "/opt/rk-linux/toolchain/aarch64-rockchip-linux-gcc")
  endif()
endif()
message(STATUS "TOOLCHAIN_ROOT = ${TOOLCHAIN_ROOT}")

set(CMAKE_C_COMPILER   "${TOOLCHAIN_ROOT}/bin/aarch64-rockchip-linux-gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_ROOT}/bin/aarch64-rockchip-linux-g++")

set(CMAKE_SYSROOT "${TOOLCHAIN_ROOT}/sysroot")
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# 交叉编译时不要去试运行编译出来的程序
set(CMAKE_CROSSCOMPILING TRUE)
