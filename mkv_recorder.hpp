#pragma once

#include "mpp_h264_encoder.hpp" // 使用编码器给出的 H.264 帧

#include <cstdint>                // int64_t
#include <string>                 // 文件名与错误说明
#include <vector>                 // SPS/PPS 数据

struct AVFormatContext;           // FFmpeg 的文件封装上下文
struct AVStream;                  // FFmpeg 的视频轨道

// 只负责把已编码的 H.264 帧封装到一个 MKV 文件，不负责采集或编码。
class MkvRecorder {
public:
    MkvRecorder() = default;
    ~MkvRecorder();

    MkvRecorder(const MkvRecorder&) = delete;
    MkvRecorder& operator=(const MkvRecorder&) = delete;

    // header 是编码器 getHeader() 返回的 Annex B SPS/PPS。
    bool open(const std::string& path, int width, int height, int fps,
              const std::vector<uint8_t>& header, std::string& error);
    // 一次写入一张编码帧；PTS 采用采集模块返回的微秒时间。
    bool write(const EncodedFrame& frame, std::string& error);
    // 正常结束时写入 MKV 尾部索引并关闭文件。
    bool close(std::string& error);

private:
    AVFormatContext* format_ = nullptr; // FFmpeg 输出文件
    AVStream* stream_ = nullptr;         // 唯一的视频流
    int fps_ = 0;                       // 名义帧率，用来标注单帧持续时间
    int64_t first_pts_us_ = -1;         // 第一张视频帧的采集时间
    int64_t last_pts_us_ = -1;          // 防止 PTS 倒退
    bool header_written_ = false;      // 是否已经写入 MKV 文件头
};
