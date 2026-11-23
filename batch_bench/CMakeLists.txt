cmake_minimum_required(VERSION 3.16)
project(batch_infer_bench CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

# ---------------------------------------------------------------- 依赖
# RKNN 运行库: 优先用主工程自带的(与生产完全同版本), 否则用板端的 /usr/lib
set(RKNN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../rk3588_zero_copy" CACHE PATH
    "主工程根目录(用于取 third_party/include 与 third_party/lib)")
set(RKNN_INCLUDE_DIR "${RKNN_ROOT}/third_party/include" CACHE PATH "rknn_api.h 所在目录")
set(RKNN_LIB_DIR    "${RKNN_ROOT}/third_party/lib"    CACHE PATH "librknnrt.so 所在目录")
set(RKNN_RUNTIME_LIB "/usr/lib/librknnrt.so" CACHE PATH "板端运行库(链接用)")

if(NOT EXISTS "${RKNN_INCLUDE_DIR}/rknn_api.h")
  message(FATAL_ERROR "找不到 rknn_api.h: ${RKNN_INCLUDE_DIR}/rknn_api.h")
endif()

add_executable(batch_infer_bench src/batch_infer_bench.cpp)
target_include_directories(batch_infer_bench PRIVATE "${RKNN_INCLUDE_DIR}")
target_link_libraries(batch_infer_bench PRIVATE pthread)

# 链接顺序: 工程自带库优先(与生产一致), 找不到就退回板端的
if(EXISTS "${RKNN_LIB_DIR}/librknnrt.so")
  target_link_libraries(batch_infer_bench PRIVATE "${RKNN_LIB_DIR}/librknnrt.so")
  message(STATUS "链接: ${RKNN_LIB_DIR}/librknnrt.so (工程自带)")
else()
  target_link_libraries(batch_infer_bench PRIVATE "${RKNN_RUNTIME_LIB}")
  message(STATUS "链接: ${RKNN_RUNTIME_LIB} (板端运行库)")
endif()

# 运行时把工程 third_party/lib 放在最前面, 保证用的是同一份 librknnrt.so
set_target_properties(batch_infer_bench PROPERTIES
  BUILD_RPATH "${RKNN_LIB_DIR}"
  INSTALL_RPATH "${RKNN_LIB_DIR}")

message(STATUS "rknn_api.h : ${RKNN_INCLUDE_DIR}")
message(STATUS "输出       : ${CMAKE_CURRENT_BINARY_DIR}/batch_infer_bench")
