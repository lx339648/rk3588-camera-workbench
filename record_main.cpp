#include "capture_v4l2.hpp"     // D405C YUYV 采集
#include "rga_yuyv_converter.hpp" // RGA YUYV -> NV12
#include "mpp_h264_encoder.hpp"  // MPP NV12 -> H.264
#include "mkv_recorder.hpp"       // FFmpeg H.264 -> MKV 文件

#include <chrono>                 // 测量实际整链路速度
#include <cstdint>                // int64_t
#include <iostream>               // 状态与错误输出
#include <string>                 // 文件路径及错误说明
#include <vector>                 // SPS/PPS 头

int main(int argc, char* argv[]) {
    const std::string path = argc > 1 ? argv[1] : "test.mkv"; // 可指定保存位置
    CaptureConfig config;        // 默认 /dev/video84、640x480、30 fps
    V4L2Capture camera;          // 采集器
    RgaYuyvConverter converter;  // RGA 格式转换器
    H264Encoder encoder;         // H.264 编码器
    MkvRecorder recorder;        // MKV 录像器
    std::string error;           // 每一步的错误说明

    if (!camera.initialize(config, error)) {
        std::cerr << "采集初始化失败：" << error << '\n';
        return 1;
    }
    if (!converter.initialize(config.width, config.height, error)) {
        std::cerr << "RGA 初始化失败：" << error << '\n';
        return 1;
    }

    CapturedFrame captured;     // 一张 YUYV 原始图
    Nv12Frame decoded;          // 一张 NV12 像素图
    EncodedFrame encoded;       // 一张 H.264 编码图
    bool ready = false;         // 是否已创建编码器与 MKV 文件
    int frame_count = 0;        // 成功录入 MKV 的帧数
    const auto begin = std::chrono::steady_clock::now();

    for (int i = 0; i < 300; ++i) { // 先录约 10 秒，验证文件和 PTS
        if (!camera.captureFrame(captured, error)) {
            std::cerr << "采集失败：" << error << '\n';
            return 1;
        }
        if (!converter.convert(captured, decoded, error)) {
            std::cerr << "RGA 转换失败：" << error << '\n';
            return 1;
        }

        if (!ready) {           // 第一张 NV12 帧给出编码器所需的真实 stride
            if (!encoder.initialize(decoded, config.fps, 4000000, error)) {
                std::cerr << "编码器初始化失败：" << error << '\n';
                return 1;
            }
            std::vector<uint8_t> header; // 编码器生成的 H.264 SPS/PPS
            if (!encoder.getHeader(header, error)) {
                std::cerr << "读取 SPS/PPS 失败：" << error << '\n';
                return 1;
            }
            if (!recorder.open(path, decoded.width(), decoded.height(),
                               config.fps, header, error)) {
                std::cerr << "创建 MKV 文件失败：" << error << '\n';
                return 1;
            }
            ready = true;
            std::cout << "开始录制 " << path << "，视频 " << decoded.width()
                      << "x" << decoded.height() << " H.264\n";
        }

        // V4L2 在 DQBUF 时返回时间戳；转成微秒后沿着同一帧传递。
        const int64_t pts_us = static_cast<int64_t>(captured.timestamp.tv_sec) *
                               1000000 + captured.timestamp.tv_usec;
        if (!encoder.encode(decoded, pts_us, encoded, error)) {
            std::cerr << "H.264 编码失败：" << error << '\n';
            return 1;
        }
        decoded.reset();       // 本帧已经编码，归还 NV12 硬件缓冲区
        if (!recorder.write(encoded, error)) {
            std::cerr << "MKV 写帧失败：" << error << '\n';
            return 1;
        }
        ++frame_count;
        if (frame_count == 1 || frame_count % 30 == 0) {
            std::cout << "已录入 " << frame_count << " 帧，PTS "
                      << encoded.pts_us << " 微秒，关键帧 "
                      << (encoded.keyframe ? "是" : "否") << '\n';
        }
    }

    if (!recorder.close(error)) { // 正常结束时写入时长与索引
        std::cerr << "结束录像失败：" << error << '\n';
        return 1;
    }
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - begin).count();
    std::cout << "录像完成：" << path << "，帧数 " << frame_count
              << "，整链路 "
              << frame_count / seconds << " fps\n";
    return 0;                   // 各模块自动释放资源
}
