# ===========================================================================
# npu_layer_bench —— RK3588 NPU 分层压测台
#
# ★ 独立构建: 这个目录【不挂到】rk3588_zero_copy/CMakeLists.txt 上,
#   也不共用它的任何 .cc/.h。它只依赖板上的 librknnrt.so + librga.so +
#   rknn_api.h + im2d.h。目的是在【隔离环境】里逐层回答:
#
#   L1  纯 NPU 算力天花板(裸推理, 无前后处理)
#   L2  检测 + OCR 双模型纯推理
#   L3  RGA 预处理 + 检测推理
#   L4  单路全链路(解码 + 预处理 + 检测 + OCR + 后处理)
#   L5  4 路全链路极限(复现生产 93/95 FPS)
#   EX  分辨率扫描(640/576/512) + 算子回落统计
#
#   构建:
#     cd npu_layer_bench && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j8
#
#   头文件/库的查找顺序:
#     1) -DRKNN_INC=<目录> / -DRKNN_RT_LIB=<路径> / -DRGA_INC=<目录> / -DRGA_LIB=<路径>
#     2) 主工程 third_party/include (只读引用, 不修改)
#     3) 系统目录
# ===========================================================================
cmake_minimum_required(VERSION 3.16)
project(npu_layer_bench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- rknn_api.h -----------------------------------------------------------
if(NOT RKNN_INC)
  foreach(_c
      "${CMAKE_CURRENT_SOURCE_DIR}/../rk3588_zero_copy/third_party/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/third_party/include"
      "/usr/include" "/usr/local/include" "/usr/include/rknn_api" "/opt/rknn/include")
    if(EXISTS "${_c}/rknn_api.h")
      set(RKNN_INC "${_c}")
      break()
    endif()
  endforeach()
endif()

# ---- librknnrt.so ---------------------------------------------------------
if(NOT RKNN_RT_LIB)
  foreach(_c
      "${CMAKE_CURRENT_SOURCE_DIR}/../rk3588_zero_copy/third_party/lib/librknnrt.so"
      "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lib/librknnrt.so"
      "/usr/lib/librknnrt.so" "/usr/local/lib/librknnrt.so" "/opt/rknn/lib/librknnrt.so")
    if(EXISTS "${_c}")
      set(RKNN_RT_LIB "${_c}")
      break()
    endif()
  endforeach()
endif()

# ---- librga.so / im2d.h ---------------------------------------------------
if(NOT RGA_INC)
  foreach(_c
      "${CMAKE_CURRENT_SOURCE_DIR}/../rk3588_zero_copy/third_party/include/rga"
      "${CMAKE_CURRENT_SOURCE_DIR}/third_party/include/rga"
      "/usr/include/rga" "/usr/local/include/rga" "/opt/rga/include")
    if(EXISTS "${_c}/im2d.h")
      set(RGA_INC "${_c}")
      break()
    endif()
  endforeach()
endif()

if(NOT RGA_LIB)
  foreach(_c
      "${CMAKE_CURRENT_SOURCE_DIR}/../rk3588_zero_copy/third_party/lib/librga.so"
      "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lib/librga.so"
      "/usr/lib/librga.so" "/usr/local/lib/librga.so" "/opt/rga/lib/librga.so")
    if(EXISTS "${_c}")
      set(RGA_LIB "${_c}")
      break()
    endif()
  endforeach()
endif()

if(NOT RKNN_INC)
  message(FATAL_ERROR "找不到 rknn_api.h。请显式指定: -DRKNN_INC=<含 rknn_api.h 的目录>")
endif()
if(NOT RKNN_RT_LIB)
  message(FATAL_ERROR "找不到 librknnrt.so。请显式指定: -DRKNN_RT_LIB=<librknnrt.so 全路径>")
endif()
if(NOT RGA_INC)
  message(FATAL_ERROR "找不到 im2d.h。请显式指定: -DRGA_INC=<含 im2d.h 的目录>")
endif()
if(NOT RGA_LIB)
  message(FATAL_ERROR "找不到 librga.so。请显式指定: -DRGA_LIB=<librga.so 全路径>")
endif()

# ⚠️ CMake 只有把【basename 以 lib 开头】的 .so 当文件路径, 其余一律当成 -lxxx。
#    所以覆盖 RKNN_RT_LIB / RGA_LIB 时, 文件名必须保持 lib*.so 的形式。
foreach(_var RKNN_RT_LIB RGA_LIB)
  get_filename_component(_bn "${${_var}}" NAME)
  if(NOT _bn MATCHES "^lib")
    message(WARNING "库文件名 '${_bn}' 不以 lib 开头, CMake 会当成 -l${_bn} 处理, 链接会失败")
  endif()
endforeach()

message(STATUS "RKNN_INC    = ${RKNN_INC}")
message(STATUS "RKNN_RT_LIB = ${RKNN_RT_LIB}")
message(STATUS "RGA_INC     = ${RGA_INC}")
message(STATUS "RGA_LIB     = ${RGA_LIB}")

add_executable(npu_layer_bench
  src/main.cpp
  src/stat.cpp
  src/dma_buf.cpp
  src/npu_model.cpp
  src/rga_pre.cpp
  src/sys_probe.cpp
  src/harness.cpp
  src/layer1_npu.cpp
  src/layer2_dual.cpp
  src/layer3_rga.cpp
  src/layer_full.cpp
  src/layer_extra.cpp
  src/json.cpp
)

target_include_directories(npu_layer_bench PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${RKNN_INC}
  ${RGA_INC}
)

target_link_libraries(npu_layer_bench PRIVATE
  ${RKNN_RT_LIB}
  ${RGA_LIB}
  pthread
  dl
)

target_compile_options(npu_layer_bench PRIVATE -Wall -Wextra -Wno-unused-parameter)
