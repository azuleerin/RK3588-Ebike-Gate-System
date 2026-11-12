cmake_minimum_required(VERSION 3.16)
project(rk_zero_copy LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ===========================================================================
# 依赖查找
#   下面每一项都支持"多个候选路径自动探测", 这样同一份 CMakeLists 既能在
#   PC/SDK 交叉编译, 也能直接在 RK3588 板子上原生编译。
#   也可以全部用 -D 显式指定:
#     -DRKNN_INC=... -DRKNN_RT_LIB=... -DRGA_INC=... -DRGA_LIB=...
#     -DMPP_INC_DIR=... -DMPP_LIB=...
# ===========================================================================

# ---- rknn_api.h -----------------------------------------------------------
# 优先用工程自带的 third_party(整个目录拷到板子上就能直接编)
if(NOT RKNN_INC)
  foreach(_c "${CMAKE_SOURCE_DIR}/third_party/include"
            "${RK_DEMO_ROOT}/include" "/usr/include" "/usr/local/include"
            "/usr/include/rknn_api" "/opt/rknn/include")
    if(EXISTS "${_c}/rknn_api.h")
      set(RKNN_INC "${_c}")
      break()
    endif()
  endforeach()
endif()

# ---- librknnrt.so ---------------------------------------------------------
if(NOT RKNN_RT_LIB)
  foreach(_c "${CMAKE_SOURCE_DIR}/third_party/lib/librknnrt.so"
            "${RK_DEMO_ROOT}/include/librknnrt.so"
            "/usr/lib/librknnrt.so" "/usr/local/lib/librknnrt.so"
            "/opt/rknn/lib/librknnrt.so")
    if(EXISTS "${_c}")
      set(RKNN_RT_LIB "${_c}")
      break()
    endif()
  endforeach()
endif()

# ---- RGA 头文件 (im2d.h / rga.h) ------------------------------------------
if(NOT RGA_INC)
  foreach(_c "${CMAKE_SOURCE_DIR}/third_party/include/rga"
            "${RK_DEMO_ROOT}/include/3rdparty/rga/RK3588/include"
            "/usr/include/rga" "/usr/local/include/rga"
            "/usr/include" "/opt/rga/include")
    if(EXISTS "${_c}/im2d.h")
      set(RGA_INC "${_c}")
      break()
    endif()
  endforeach()
endif()

# ---- librga.so ------------------------------------------------------------
if(NOT RGA_LIB)
  foreach(_c "${CMAKE_SOURCE_DIR}/third_party/lib/librga.so"
            "${RK_DEMO_ROOT}/include/3rdparty/rga/RK3588/lib/Linux/aarch64/librga.so"
            "/usr/lib/librga.so" "/usr/local/lib/librga.so" "/opt/rga/lib/librga.so")
    if(EXISTS "${_c}")
      set(RGA_LIB "${_c}")
      break()
    endif()
  endforeach()
endif()

foreach(_v RKNN_INC RKNN_RT_LIB RGA_INC RGA_LIB)
  if(NOT ${_v})
    message(FATAL_ERROR
      "缺少 ${_v}。请显式指定, 例如:\n"
      "  -DRKNN_INC=<含 rknn_api.h 的目录> -DRKNN_RT_LIB=<librknnrt.so 全路径>\n"
      "  -DRGA_INC=<含 im2d.h 的目录>        -DRGA_LIB=<librga.so 全路径>")
  endif()
endforeach()

# ⚠️ CMake 的链接项分类规则: 只有【basename 以 lib 开头】的 .so 才会被当成
#    "文件路径"直接交给链接器; 否则一律转成 -l<去掉扩展名的名字>。
#    例如 -DRKNN_RT_LIB=/work/stub_librknnrt.so 会变成 "-lstub_librknnrt" -> 链接失败。
#    所以覆盖这两个变量时, 文件名请保持 lib*.so 的形式。
get_filename_component(_rk "${RKNN_RT_LIB}" NAME)
get_filename_component(_rg "${RGA_LIB}" NAME)
foreach(_n "${_rk}" "${_rg}")
  if(NOT _n MATCHES "^lib")
    message(WARNING "库文件名 '${_n}' 不以 lib 开头, CMake 会当成 -l${_n} 处理, 链接会失败")
  endif()
endforeach()
message(STATUS "RKNN_INC    = ${RKNN_INC}")
message(STATUS "RKNN_RT_LIB = ${RKNN_RT_LIB}")
message(STATUS "RGA_INC     = ${RGA_INC}")
message(STATUS "RGA_LIB     = ${RGA_LIB}")

# ---- librockchip_mpp ------------------------------------------------------
# ⚠️ MPP 的【头文件 / 用户态 .so / 内核驱动】三者版本必须对齐。
#    头文件里定死了结构体布局和 MppApi 函数指针表的顺序; 一旦编译时用的是
#    旧版头(例如 20190626), 而运行时加载的是板子自带的 BSP 版 .so(1.5.0),
#    mpp_create() 之后的第一次调用就会按错偏移跳进野地址 -> SIGSEGV。
#    (板子自带的是 BSP 版 MPP; 工程里如果还留着旧版 MPP 头文件, 必崩。)
#
# 所以这里把搜索范围【锁死】在系统目录, 并用 NO_DEFAULT_PATH 断掉
# "从工程目录 / 工具链 sysroot 里捡到旧 MPP"的路:
#      头文件 : /usr/include/rockchip/          (板子自带的 BSP 版)
#      库     : /usr/lib/aarch64-linux-gnu/librockchip_mpp.so
# MPP 的头是【扁平】装在 <prefix>/include/rockchip/ 下的(rk_mpi.h / mpp_buffer.h ...),
# 不是 rk_mpp/rk_mpp.h —— 写错这个名字会让 MPP 支持被静默关掉。
if(NOT MPP_INC_DIR)
  find_path(MPP_INC_DIR rk_mpi.h
            PATHS /usr/include/rockchip /usr/local/include/rockchip
            NO_DEFAULT_PATH)
endif()
if(NOT MPP_LIB)
  find_library(MPP_LIB rockchip_mpp
               PATHS /usr/lib/aarch64-linux-gnu /usr/lib /usr/local/lib
               NO_DEFAULT_PATH)
endif()

# find_path/find_library 找不到时会把变量设成 "<VAR>-NOTFOUND",
# 直接喂给 target_include_directories()/target_link_libraries() 会让
# "CMake Generate step failed"。这里统一清成空串, 走 RK_HAVE_MPP=0 分支。
foreach(_v MPP_INC_DIR MPP_LIB)
  if(${_v} MATCHES "NOTFOUND")
    set(${_v} "")
  endif()
endforeach()

# ---- MPP 目录卫生检查 -----------------------------------------------------
# ① 工程目录里如果还躺着 MPP 头文件, 它会抢在系统头文件前面被
#    #include <rockchip/rk_mpi.h> 命中 -> 结构体布局和运行时 .so 对不上 -> 必崩。
# ② MPP 库如果来自工程目录(自带旧版 .so), 运行时加载的就是旧版 -> 同样必崩。
# 这两条一旦命中就直接报错, 别等到运行时再拿 SIGSEGV 去定位。
foreach(_d "${CMAKE_CURRENT_SOURCE_DIR}/include"
          "${CMAKE_CURRENT_SOURCE_DIR}/third_party/include"
          "${RKNN_INC}" "${RGA_INC}")
  if(_d AND (EXISTS "${_d}/rockchip/rk_mpi.h" OR EXISTS "${_d}/rk_mpi.h"))
    message(FATAL_ERROR
      "工程目录 ${_d}/ 下有旧版 MPP 头文件。\n"
      "  它会被 #include <rockchip/rk_mpi.h> 抢先命中, 结构体布局与板子自带的\n"
      "  ${MPP_INC_DIR} 不一致, mpp_create() 必然 SIGSEGV。\n"
      "  请删除该目录下的旧 MPP 头文件后重新编译。")
  endif()
endforeach()

if(MPP_LIB)
  file(REAL_PATH "${MPP_LIB}" _mpp_lib_real)
  file(REAL_PATH "${CMAKE_SOURCE_DIR}" _src_real)
  string(FIND "${_mpp_lib_real}" "${_src_real}/" _mpp_lib_in_src)
  if(_mpp_lib_in_src EQUAL 0)
    message(FATAL_ERROR
      "MPP 库不能来自工程目录: ${MPP_LIB}\n"
      "  请使用板子系统自带的 /usr/lib/aarch64-linux-gnu/librockchip_mpp.so")
  endif()
endif()

# ---- FFmpeg (必须带 rkmpp 解码器) ----------------------------------------
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(FFMPEG QUIET libavformat libavcodec libavutil)
endif()
if(NOT FFMPEG_FOUND)
  find_path(FFMPEG_INC_DIR libavformat/avformat.h
            PATHS /usr/include /usr/local/include /opt/ffmpeg/include)
  find_library(AVFORMAT_LIB avformat PATHS /usr/lib /usr/local/lib /opt/ffmpeg/lib)
  find_library(AVCODEC_LIB  avcodec  PATHS /usr/lib /usr/local/lib /opt/ffmpeg/lib)
  find_library(AVUTIL_LIB   avutil   PATHS /usr/lib /usr/local/lib /opt/ffmpeg/lib)
  set(FFMPEG_INCLUDE_DIRS ${FFMPEG_INC_DIR})
  set(FFMPEG_LIBRARIES ${AVFORMAT_LIB} ${AVCODEC_LIB} ${AVUTIL_LIB})
endif()
if(NOT FFMPEG_LIBRARIES OR NOT FFMPEG_INCLUDE_DIRS)
  message(FATAL_ERROR
    "找不到 FFmpeg 的头文件/库。板子上通常只装了运行库没装 dev 包。\n"
    "解决:  -DFFMPEG_INCLUDE_DIRS=<含 libavformat/avformat.h 的目录> \\\n"
    "        -DFFMPEG_LIBRARIES='<libavformat.so全路径>;<libavcodec.so全路径>;<libavutil.so全路径>'")
endif()
message(STATUS "FFMPEG      = ${FFMPEG_INCLUDE_DIRS}")

# ---- OpenCV ---------------------------------------------------------------
find_package(OpenCV QUIET)
if(NOT OpenCV_FOUND)
  message(STATUS "OpenCV cmake config 未找到, 改为手工查找")
  find_path(OpenCV_INC_DIR
            NAMES opencv2/core.hpp
            HINTS ${CMAKE_SYSROOT}/usr/include/opencv4
                  ${CMAKE_SYSROOT}/usr/local/include/opencv4
                  /usr/include/opencv4 /usr/local/include/opencv4 /usr/include)
  find_library(OPENCV_CORE_LIB    opencv_core    HINTS ${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu ${CMAKE_SYSROOT}/usr/local/lib /usr/lib /usr/local/lib)
  find_library(OPENCV_IMGPROC_LIB opencv_imgproc HINTS ${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu ${CMAKE_SYSROOT}/usr/local/lib /usr/lib /usr/local/lib)
  find_library(OPENCV_HIGHGUI_LIB opencv_highgui HINTS ${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu ${CMAKE_SYSROOT}/usr/local/lib /usr/lib /usr/local/lib)
  find_library(OPENCV_IMGCODECS_LIB opencv_imgcodecs HINTS ${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu ${CMAKE_SYSROOT}/usr/local/lib /usr/lib /usr/local/lib)
  if(NOT OpenCV_INC_DIR OR NOT OPENCV_CORE_LIB)
    message(FATAL_ERROR
      "找不到 OpenCV。板子上通常只有 opencv 运行库没有头文件。\n"
      "解决:  -DOpenCV_INC_DIR=<含 opencv2/core.hpp 的目录> \\\n"
      "        -DOpenCV_CORE_LIB=<libopencv_core.so> -DOpenCV_IMGPROC_LIB=<...> \\\n"
      "        -DOpenCV_HIGHGUI_LIB=<...> -DOpenCV_IMGCODECS_LIB=<...>")
  endif()
  set(OpenCV_INCLUDE_DIRS ${OpenCV_INC_DIR})
  set(OpenCV_LIBS ${OPENCV_CORE_LIB} ${OPENCV_IMGPROC_LIB} ${OPENCV_HIGHGUI_LIB}
                   ${OPENCV_IMGCODECS_LIB})
endif()
message(STATUS "OpenCV      = ${OpenCV_INCLUDE_DIRS}")

# ===========================================================================
add_executable(rk_zero_copy
  src/main.cc
  src/dmabuf.cc
  src/ipm.cc
  src/led_serial.cc
  src/license_plate_rec.cc
  src/lprnet.cc
  src/mpp_decoder.cc
  src/mqtt_pub.cc
  src/rga_convert.cc
  src/result_sink.cc
  src/safe_probe.cc
  src/speed_track.cc
  src/yolo_post.cc
)

target_include_directories(rk_zero_copy PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${RKNN_INC}
  ${RGA_INC}
  ${MPP_INC_DIR}
  ${FFMPEG_INCLUDE_DIRS}
  ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(rk_zero_copy PRIVATE
  ${RKNN_RT_LIB}          # librknnrt.so   (全路径, 不依赖 -dev 软链接)
  ${RGA_LIB}              # librga.so
  ${MPP_LIB}              # librockchip_mpp.so
  ${FFMPEG_LIBRARIES}     # libavformat / libavcodec / libavutil
  ${OpenCV_LIBS}
  dl
  pthread
)

# ===========================================================================
# 单图车牌识别测试工具 (lpr_test)
#
# 独立的小程序: 读一张图 -> 预处理 -> 送 rknn 模型 -> 打印识别结果。
# 跟主程序解耦(不共用 main.cc 那套流水线), 所以模型换了只改命令行参数。
#   ./lpr_test --model model/lprnet_finetuned_int8.rknn --image test_plate.jpg
# ===========================================================================
add_executable(lpr_test src/lpr_test.cc)

target_include_directories(lpr_test PRIVATE
  ${RKNN_INC}
  ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(lpr_test PRIVATE
  ${RKNN_RT_LIB}          # librknnrt.so
  ${OpenCV_LIBS}
  dl
  pthread
)

# 静态背景过滤(模块 F)的单元测试
#   ./build/tests/test_static_filter
# 不需要 RKNN / OpenCV / 摄像头, 纯逻辑, 随时能跑。
# ===========================================================================
add_executable(test_static_filter
  tests/test_static_filter.cpp
  src/speed_track.cc
  src/ipm.cc
  src/yolo_post.cc
)
target_include_directories(test_static_filter PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ===========================================================================
# PP-OCRv4_mobile_rec 车牌识别类的单元测试
#   ./build/tests/test_license_plate_rec
#
# 覆盖:
#   1) CTC 贪心解码(对齐官方 rec_postprocess 的语义)
#   2) 车牌字符白名单过滤
#   3) 字典加载 + 类数对齐
#   4) RGA 硬件预处理(用【本板子真实 dma-buf】跑 裁剪+缩放+颜色空间)
# 带 --model <rknn> 参数时再跑一遍真实端到端识别。
# ===========================================================================
add_executable(test_license_plate_rec
  tests/test_license_plate_rec.cpp
  src/license_plate_rec.cc
  src/rga_convert.cc
  src/safe_probe.cc
)
target_include_directories(test_license_plate_rec PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${RKNN_INC}
  ${RGA_INC}
  ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(test_license_plate_rec PRIVATE
  ${RKNN_RT_LIB}
  ${RGA_LIB}
  ${OpenCV_LIBS}
  dl
  pthread
)

# UTF-8 -> GBK (串口控制卡语音/临显用)。glibc 已把 iconv 放进 libc,
# 只有装了独立 libiconv 时才需要显式链接。
find_library(ICONV_LIB NAMES iconv libiconv)
if(ICONV_LIB)
  target_link_libraries(rk_zero_copy PRIVATE ${ICONV_LIB})
endif()

if(MPP_LIB AND MPP_INC_DIR)
  # 把"编译期用的是哪份头文件 / 哪个库"烙进二进制, 运行时直接打印出来。
  # 一眼就能看出【编译期头文件】和【运行时真正加载的 .so】是不是同一份版本 ——
  # 这正是 mpp_create 段错误最常见的根因。
  target_compile_definitions(rk_zero_copy PRIVATE
    RK_HAVE_MPP=1
    RK_MPP_HEADER_DIR="${MPP_INC_DIR}"
    RK_MPP_LINK_LIB="${MPP_LIB}")
  message(STATUS "MPP        = ${MPP_LIB} (header ${MPP_INC_DIR})")
else()
  target_compile_definitions(rk_zero_copy PRIVATE RK_HAVE_MPP=0)
  message(WARNING "找不到 librockchip_mpp, 将只走 CPU 映射地址路径(仍无 CPU 拷贝)")
endif()

install(TARGETS rk_zero_copy DESTINATION .)
# 运行期需要的 .so 装到产物目录的 lib/ 下, 运行时 LD_LIBRARY_PATH=./lib 即可
install(FILES ${RKNN_RT_LIB} ${RGA_LIB} DESTINATION lib OPTIONAL)

# ===========================================================================
# rk_base —— 崩溃排查基础库 (step/01)
#
# 只做"观测", 不改变任何硬件调用时序:
#   log        : 分级日志落盘(INFO/WARN/ERROR, 带线程ID/模块/函数)
#   mem_guard  : 受控分配 + canary 越界哨兵
#   fd_ledger  : fd 台账 + 与 /proc/self/fd 对账 + 交给硬件前的尺寸校验
#   stack_guard: 线程栈水位 + 主循环心跳看门狗
#
# 规则依据: AGENT_RULES_FOR_CRASH_DEBUGGING.md 第二/四节
# ===========================================================================
add_library(rk_base STATIC
  src/rk_base/log.cc
  src/rk_base/mem_guard.cc
  src/rk_base/fd_ledger.cc
  src/rk_base/stack_guard.cc
)

target_include_directories(rk_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(rk_base PUBLIC pthread dl)
# 静态库也用 -O2 + 去掉帧指针影响不到正确性, 但关掉 -fno-strict-aliasing
# 会让 canary 的 reinterpret_cast 更安全
target_compile_options(rk_base PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ===========================================================================
# rk_minimal —— 最小化测试单元 (step/02 起)
#
# 每个单元只允许触碰一个硬件模块, 入口处强制模式互斥校验:
#   --decoder-only (MPP)   --rga-only (RGA)   --npu-only (RKNN)
#
# 单元内部复用业务代码的 dma-buf 通路(src/dmabuf.cc), 保证输入路径与
# 线上运行的那份完全一致 —— 这是"带病复现"能成立的前提。
# ===========================================================================
add_executable(rk_minimal
  src/minimal/main_minimal.cc
  src/minimal/unit_common.cc
  src/minimal/unit_npu.cc
  src/minimal/unit_decoder.cc
  src/minimal/unit_rga.cc
  src/dmabuf.cc
  src/mpp_decoder.cc
  src/safe_probe.cc
)

target_include_directories(rk_minimal PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${RKNN_INC}
  ${RGA_INC}
  ${MPP_INC_DIR}
  ${FFMPEG_INCLUDE_DIRS}
)

target_link_libraries(rk_minimal PRIVATE
  rk_base
  ${RKNN_RT_LIB}          # librknnrt.so
  ${RGA_LIB}              # librga.so
  ${MPP_LIB}              # librockchip_mpp.so
  ${FFMPEG_LIBRARIES}     # libavformat / libavcodec / libavutil
  dl
  pthread
)

if(MPP_LIB AND MPP_INC_DIR)
  target_compile_definitions(rk_minimal PRIVATE RK_HAVE_MPP=1)
else()
  target_compile_definitions(rk_minimal PRIVATE RK_HAVE_MPP=0)
  message(WARNING "rk_minimal: 找不到 librockchip_mpp, decoder-only 单元不可用")
endif()

target_compile_options(rk_minimal PRIVATE -Wall -Wextra -Wno-unused-parameter)

install(TARGETS rk_minimal DESTINATION .)
