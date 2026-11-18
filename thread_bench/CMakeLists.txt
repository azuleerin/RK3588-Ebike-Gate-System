# ===========================================================================
# thread_bench —— RK3588 三核 NPU 极限并发测试台
#
# ★ 独立构建: 这个目录【不挂到】rk3588_zero_copy/CMakeLists.txt 上,
#   也不共用它的任何 .cc/.h。它只依赖板上的 librknnrt.so + rknn_api.h。
#   目的就是"在隔离环境里"单独测 NPU 并发能力, 不影响主工程。
#
#   构建:
#     cd thread_bench && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j8
#
#   rknn_api.h 的查找顺序:
#     1) -DRKNN_INC=<目录>
#     2) 主工程 third_party/include (只读引用, 不修改)
#     3) 系统目录 /usr/include 等
#   librknnrt.so 同理。
# ===========================================================================
cmake_minimum_required(VERSION 3.16)
project(thread_bench LANGUAGES CXX)

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

if(NOT RKNN_INC)
  message(FATAL_ERROR "找不到 rknn_api.h。请显式指定: -DRKNN_INC=<含 rknn_api.h 的目录>")
endif()
if(NOT RKNN_RT_LIB)
  message(FATAL_ERROR "找不到 librknnrt.so。请显式指定: -DRKNN_RT_LIB=<librknnrt.so 全路径>")
endif()

# ⚠️ CMake 只有把【basename 以 lib 开头】的 .so 当文件路径, 其余一律当成 -lxxx。
#    所以覆盖 RKNN_RT_LIB 时, 文件名必须保持 lib*.so 的形式。
get_filename_component(_rk "${RKNN_RT_LIB}" NAME)
if(NOT _rk MATCHES "^lib")
  message(WARNING "库文件名 '${_rk}' 不以 lib 开头, CMake 会当成 -l${_rk} 处理, 链接会失败")
endif()

message(STATUS "RKNN_INC    = ${RKNN_INC}")
message(STATUS "RKNN_RT_LIB = ${RKNN_RT_LIB}")

add_executable(thread_bench
  src/main.cpp
  src/frame_queue.cpp
  src/latency_rec.cpp
  src/npu_worker.cpp
  src/sys_probe.cpp
)

target_include_directories(thread_bench PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${RKNN_INC}
)

target_link_libraries(thread_bench PRIVATE
  ${RKNN_RT_LIB}
  pthread
)

target_compile_options(thread_bench PRIVATE -Wall -Wextra -Wno-unused-parameter)
