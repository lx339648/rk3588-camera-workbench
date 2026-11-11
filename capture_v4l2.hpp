#pragma once

#include <cstddef>   // size_t
#include <cstdint>   // uint8_t、uint32_t
#include <string>    // std::string
#include <vector>    // std::vector
#include <sys/time.h> // timeval，保存 V4L2 给出的采集时间戳
#include "frame_log.hpp"

enum class CapturePixelFormat { Mjpeg, Yuyv };

// 初始化时向摄像头请求的参数。
struct CaptureConfig {
    std::string device = "/dev/video84"; // D405C 彩色视频节点
    uint32_t width = 640;             // 请求的画面宽度
    uint32_t height = 480;            // 请求的画面高度
    uint32_t fps = 30;                 // 请求的每秒帧数
    uint32_t buffer_count = 4;        // 请求驱动分配的缓冲区数量
    CapturePixelFormat pixel_format = CapturePixelFormat::Yuyv;
    bool enable_frame_log = false;       // Qt 诊断按需开启，不改变旧示例默认行为
};

// 一帧摄像头数据；data 由程序持有，归还驱动缓冲区后仍有效。
struct CapturedFrame {
    std::vector<uint8_t> data;         // MJPG 压缩字节，或 YUYV 原始像素
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bytes_per_line = 0;       // 驱动实际返回的行跨度（字节）
    CapturePixelFormat pixel_format = CapturePixelFormat::Yuyv;
    timeval timestamp{};              // 驱动在 DQBUF 时返回的时间戳
    uint32_t timestamp_flags = 0;     // 驱动标记的时钟类型和取时间点
    uint32_t sequence = 0;            // 驱动返回的帧序号，和缓冲区 index 不同
    FrameLog log;                      // 本帧时间线，随图像传递
};

// 只管理一台 V4L2 摄像头；不负责解码、录像、Qt 或 RTSP。
class V4L2Capture {
public:
    V4L2Capture() = default;          // 创建对象时暂不打开摄像头
    ~V4L2Capture();                   // 对象销毁时停止采集并解除映射

    V4L2Capture(const V4L2Capture&) = delete;            // 禁止复制设备所有权
    V4L2Capture& operator=(const V4L2Capture&) = delete; // 禁止复制设备所有权

    bool initialize(const CaptureConfig& config, std::string& error); // 打开、配置、映射并启动
    bool captureFrame(CapturedFrame& frame, std::string& error);      // DQBUF、复制、QBUF
    void stop();                       // 停止采集、解除映射并关闭设备

private:
    struct MappedBuffer {             // 记录一块驱动缓冲区的映射信息
        void* address = nullptr;      // 映射到程序地址空间后的起始地址
        size_t length = 0;            // 映射长度，解除映射时还要用
    };

    int fd_ = -1;                     // 摄像头文件描述符，-1 表示未打开
    bool buffers_allocated_ = false; // 是否已经执行成功 VIDIOC_REQBUFS
    std::vector<MappedBuffer> buffers_; // 所有已映射的驱动缓冲区
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t bytes_per_line_ = 0;
    CapturePixelFormat pixel_format_ = CapturePixelFormat::Yuyv;
    bool log_enabled_ = false;
};
