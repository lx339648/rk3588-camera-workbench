#pragma once

#include "nv12_frame.hpp" // 编码器只依赖 NV12 帧，不依赖其来源

#include <cstdint>               // int64_t、uint8_t
#include <string>                // 错误信息
#include <vector>                // 独立持有编码结果
#include <rk_mpi.h>              // MppCtx 和 MppApi

// 一张输入图像对应的一组 H.264 NAL 字节，后续可交给录像或推流模块。
struct EncodedFrame {
    std::vector<uint8_t> data;   // H.264 Annex B 数据，不是完整 MP4/MKV 文件
    int64_t pts_us = 0;          // 采集时间戳，单位微秒
    bool keyframe = false;       // 包含 IDR 图像，录像模块据此建立可跳转位置
    FrameLog log;                // 与这帧 H.264 对应的采集时间线
};

// 一个实例管理一个 RK MPP H.264 硬件编码器。
class H264Encoder {
public:
    H264Encoder() = default;
    ~H264Encoder();

    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    // stride 必须采用实际 NV12 输出值，不能直接拿可见宽高代替。
    bool initialize(const Nv12Frame& first_frame, int fps, int bitrate,
                    std::string& error);
    // 取得 SPS/PPS；原始 .h264 文件应先写入它们。
    bool getHeader(std::vector<uint8_t>& header, std::string& error);
    // 请求下一张输入帧成为 IDR，供新录像或新 RTSP 会话立即起播。
    bool requestKeyframe(std::string& error);
    // 送入一帧 NV12，等待对应的编码输出；调用返回后可释放 decoded。
    bool encode(const Nv12Frame& decoded, int64_t pts_us,
                EncodedFrame& output, std::string& error);
    void stop();                 // 销毁编码器上下文

private:
    MppCtx context_ = nullptr;  // MPP 编码器实例
    MppApi* api_ = nullptr;     // MPP 操作函数表
    int width_ = 0;             // 初始化时固定的可见宽度
    int height_ = 0;            // 初始化时固定的可见高度
    int hor_stride_ = 0;        // 初始化时固定的水平 stride
    int ver_stride_ = 0;        // 初始化时固定的垂直 stride
};
