cmake_minimum_required(VERSION 3.16)
project(qt_window LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets Network OpenGL OpenGLWidgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil)
find_path(MPP_INCLUDE_DIR rk_mpi.h
    HINTS ${CMAKE_CURRENT_SOURCE_DIR}/../../mpp/inc
          /usr/include/rockchip /usr/local/include/rockchip)
find_library(MPP_LIBRARY NAMES rockchip_mpp
    HINTS ${CMAKE_CURRENT_SOURCE_DIR}/../../mpp-build/mpp
          ${CMAKE_CURRENT_SOURCE_DIR}/../../mpp-build /usr/lib/aarch64-linux-gnu)
if(NOT MPP_INCLUDE_DIR OR NOT MPP_LIBRARY)
    message(FATAL_ERROR "找不到 MPP，请设置 MPP_INCLUDE_DIR 和 MPP_LIBRARY")
endif()
set(RGA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../librga" CACHE PATH "librga 根目录")
find_path(RGA_INCLUDE_DIR im2d.hpp
    HINTS "${RGA_ROOT}/include" /usr/include/rga /usr/local/include/rga)
find_library(RGA_LIBRARY NAMES rga
    HINTS "${RGA_ROOT}/libs/Linux/gcc-aarch64")
if(NOT RGA_INCLUDE_DIR OR NOT RGA_LIBRARY)
    message(FATAL_ERROR "找不到 librga；请设置 -DRGA_ROOT=...，或分别指定 -DRGA_INCLUDE_DIR=... -DRGA_LIBRARY=...")
endif()
find_library(RKNN_LIBRARY NAMES rknnrt
    HINTS ${CMAKE_CURRENT_SOURCE_DIR}/../../model_train
          ${CMAKE_CURRENT_SOURCE_DIR}/../.. /usr/lib/aarch64-linux-gnu /usr/local/lib)
if(NOT RKNN_LIBRARY)
    message(FATAL_ERROR "找不到 librknnrt；请设置 -DRKNN_LIBRARY=/path/to/librknnrt.so")
endif()
find_path(RKNN_INCLUDE_DIR rknn_api.h
    HINTS ${CMAKE_CURRENT_SOURCE_DIR}/ai
          /usr/include/rockchip /usr/include/rknn /usr/local/include)
if(NOT RKNN_INCLUDE_DIR)
    message(FATAL_ERROR "找不到 RKNN SDK 头文件；请设置 -DRKNN_INCLUDE_DIR=/path/to/rknn/include")
endif()

add_executable(qt_window
    app_style.cpp
    app_style.hpp
    camera_pipeline.cpp
    camera_pipeline.hpp
    ai/yolo_rknn.cpp
    ai/yolo_rknn.hpp
    main.cpp
    main_window.cpp
    main_window.hpp
    nv12_preview_widget.cpp
    nv12_preview_widget.hpp
    qt_preview_log.cpp
    qt_preview_log.hpp
    ../capture_v4l2.cpp
    ../frame_log.cpp
    ../nv12_frame.cpp
    ../rga_yuyv_converter.cpp
    ../mpp_h264_encoder.cpp
    ../mkv_recorder.cpp
    ../rtsp_publisher.cpp
)

target_include_directories(qt_window PRIVATE .. ai ${MPP_INCLUDE_DIR} ${RGA_INCLUDE_DIR} ${RKNN_INCLUDE_DIR})
target_link_libraries(qt_window PRIVATE Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Multimedia
    Qt6::MultimediaWidgets Qt6::Network ${MPP_LIBRARY} ${RGA_LIBRARY} ${RKNN_LIBRARY}
    PkgConfig::LIBAV pthread)
