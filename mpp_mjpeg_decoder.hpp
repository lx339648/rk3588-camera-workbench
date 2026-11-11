#pragma once

#include "capture_v4l2.hpp" // 输入是采集模块返回的 MJPG 帧
#include "nv12_frame.hpp"

#include <string>             // 错误信息
#include <rk_mpi.h>           // MPP 的解码上下文和图像帧类型
#include <mpp_buffer.h>       // MPP 的图像缓冲区类型

// 区分正常输出、单帧解码失败和解码器本身故障。
enum class DecodeStatus {
    FrameReady,                // 得到可用的解码帧
    BadFrame,                  // 这一帧解码失败，可继续处理下一帧
    FatalError                 // 解码器状态异常，需要停止
};

using DecodedFrame = Nv12Frame; // 保留旧 MJPG 测试程序的接口名称

// 一个解码器实例对应一个 MPP MJPEG 解码上下文。
class MjpegDecoder {
public:
    MjpegDecoder() = default;
    ~MjpegDecoder();

    MjpegDecoder(const MjpegDecoder&) = delete;
    MjpegDecoder& operator=(const MjpegDecoder&) = delete;

    bool initialize(int width, int height, std::string& error); // 建立 MPP 解码器
    DecodeStatus decode(const CapturedFrame& input, DecodedFrame& output,
                        std::string& error); // 输入 MJPG 字节，输出像素帧
    void stop();                  // 销毁 MPP 解码器和缓冲区组

private:
    MppCtx context_ = nullptr;    // MPP 解码上下文
    MppApi* api_ = nullptr;       // MPP 函数表
    MppBufferGroup group_ = nullptr; // 输入和输出硬件缓冲区的分配组
    size_t output_capacity_ = 0;  // 单张输出图像预留的缓冲区字节数
};
