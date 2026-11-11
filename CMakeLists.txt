cmake_minimum_required(VERSION 3.16)
project(v4l2_capture_demo LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 原采集阶段仍可单独编译，方便对比。
add_executable(capture_demo main.cpp capture_v4l2.cpp frame_log.cpp)
target_link_libraries(capture_demo PRIVATE pthread)

# 从已构建的 mpp 目录或系统安装目录查找头文件和动态库。
find_path(MPP_INCLUDE_DIR rk_mpi.h
    HINTS ../mpp/inc /usr/include/rockchip /usr/local/include/rockchip)
find_library(MPP_LIBRARY NAMES rockchip_mpp
    HINTS ../mpp-build/mpp ../mpp-build /usr/lib/aarch64-linux-gnu)
if(NOT MPP_INCLUDE_DIR OR NOT MPP_LIBRARY)
    message(FATAL_ERROR "找不到 MPP；请指定 -DMPP_INCLUDE_DIR=... -DMPP_LIBRARY=...")
endif()

add_executable(decode_demo decode_main.cpp capture_v4l2.cpp frame_log.cpp
    nv12_frame.cpp mpp_mjpeg_decoder.cpp)
target_include_directories(decode_demo PRIVATE ${MPP_INCLUDE_DIR})
target_link_libraries(decode_demo PRIVATE ${MPP_LIBRARY} pthread)

# D405C YUYV -> RGA NV12 -> MPP H.264。
set(RGA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../librga" CACHE PATH "librga 根目录")
find_path(RGA_INCLUDE_DIR im2d.hpp
    HINTS "${RGA_ROOT}/include" /usr/include/rga /usr/local/include/rga)
find_library(RGA_LIBRARY NAMES rga
    HINTS "${RGA_ROOT}/libs/Linux/gcc-aarch64")
if(NOT RGA_INCLUDE_DIR OR NOT RGA_LIBRARY)
    message(FATAL_ERROR "找不到 librga；请设置 -DRGA_ROOT=...，或分别指定 -DRGA_INCLUDE_DIR=... -DRGA_LIBRARY=...")
endif()
add_executable(encode_demo encode_main.cpp capture_v4l2.cpp frame_log.cpp
    nv12_frame.cpp rga_yuyv_converter.cpp mpp_h264_encoder.cpp)
target_include_directories(encode_demo PRIVATE ${MPP_INCLUDE_DIR} ${RGA_INCLUDE_DIR})
target_link_libraries(encode_demo PRIVATE ${MPP_LIBRARY} ${RGA_LIBRARY} pthread)

# 第四阶段仅在 FFmpeg 开发包齐全时编译，避免影响前面三个练习。
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBAV QUIET IMPORTED_TARGET libavformat libavcodec libavutil)
endif()
if(LIBAV_FOUND)
    add_executable(record_demo record_main.cpp capture_v4l2.cpp frame_log.cpp
        nv12_frame.cpp rga_yuyv_converter.cpp mpp_h264_encoder.cpp mkv_recorder.cpp)
    target_include_directories(record_demo PRIVATE ${MPP_INCLUDE_DIR} ${RGA_INCLUDE_DIR})
    target_link_libraries(record_demo PRIVATE ${MPP_LIBRARY} ${RGA_LIBRARY} PkgConfig::LIBAV pthread)

    # 第五阶段：真实摄像头持续采集、MPP 编码，发布到板上的 RTSP 服务。
    add_executable(stream_demo stream_main.cpp capture_v4l2.cpp frame_log.cpp
        nv12_frame.cpp rga_yuyv_converter.cpp mpp_h264_encoder.cpp rtsp_publisher.cpp)
    target_include_directories(stream_demo PRIVATE ${MPP_INCLUDE_DIR} ${RGA_INCLUDE_DIR})
    target_link_libraries(stream_demo PRIVATE ${MPP_LIBRARY} ${RGA_LIBRARY} PkgConfig::LIBAV pthread)
else()
    message(STATUS "未找到 FFmpeg 开发包：暂不编译 record_demo 和 stream_demo")
endif()
