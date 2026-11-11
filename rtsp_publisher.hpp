#pragma once

#include "mpp_h264_encoder.hpp" // EncodedFrame 保存 H.264 数据和采集时间戳

#include <cstdint>
#include <string>
#include <vector>

struct AVFormatContext; // 避免在头文件里引入 FFmpeg 的 C 头文件
struct AVStream;

// 把已经编码好的 H.264 视频包发布到 RTSP 服务，不负责采集或编码。
class RtspPublisher {
public:
    RtspPublisher() = default;
    ~RtspPublisher();

    RtspPublisher(const RtspPublisher&) = delete;
    RtspPublisher& operator=(const RtspPublisher&) = delete;

    // url 指向正在运行的 MediaMTX，例如 rtsp://127.0.0.1:8554/live。
    bool open(const std::string& url, int width, int height, int fps,
              const std::vector<uint8_t>& header, std::string& error);
    // 一次发送一张完整的 H.264 编码帧，直接复制码流，不重新编码。
    bool write(const EncodedFrame& frame, std::string& error);
    // 结束本次 RTSP 发布并释放 FFmpeg 资源。
    bool close(std::string& error);

private:
    AVFormatContext* format_ = nullptr;
    AVStream* stream_ = nullptr;
    int fps_ = 0;
    int64_t first_pts_us_ = -1;
    int64_t last_pts_us_ = -1;
    bool header_written_ = false;
};
