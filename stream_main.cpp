#include "capture_v4l2.hpp"     // D405C YUYV 采集
#include "rga_yuyv_converter.hpp" // RGA 把 YUYV 转为 NV12
#include "mpp_h264_encoder.hpp"  // MPP 把 NV12 编为 H.264
#include "rtsp_publisher.hpp"    // FFmpeg 把 H.264 发布到 RTSP 服务

#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {
volatile std::sig_atomic_t running = 1;
void stopOnSignal(int) { running = 0; } // Ctrl+C 时让主循环正常退出
} // namespace

int main(int argc, char* argv[]) {
    // MediaMTX 运行在同一块板上，所以程序向本机发布；电脑用板卡 IP 拉流。
    const std::string url = argc > 1 ? argv[1] : "rtsp://127.0.0.1:8554/live";
    std::signal(SIGINT, stopOnSignal);
    std::signal(SIGTERM, stopOnSignal);

    CaptureConfig config; // 默认 /dev/video84、YUYV 640x480、30 fps
    V4L2Capture camera;
    RgaYuyvConverter converter;
    H264Encoder encoder;
    RtspPublisher publisher;
    std::string error;

    if (!camera.initialize(config, error)) {
        std::cerr << "采集初始化失败：" << error << '\n';
        return 1;
    }
    if (!converter.initialize(config.width, config.height, error)) {
        std::cerr << "RGA 初始化失败：" << error << '\n';
        return 1;
    }

    CapturedFrame captured; // 当前 YUYV 帧
    Nv12Frame decoded;      // 当前 NV12 帧
    EncodedFrame encoded;     // 当前 H.264 帧
    bool ready = false;       // 编码器和 RTSP 是否已经准备好
    unsigned long frames = 0;
    const auto started = std::chrono::steady_clock::now(); // 计算整链路实际帧率

    while (running) { // 每轮只处理一张图，不额外设 100 ms 定时器
        if (!camera.captureFrame(captured, error)) {
            std::cerr << "采集失败：" << error << '\n';
            return 1;
        }
        if (!converter.convert(captured, decoded, error)) {
            std::cerr << "RGA 转换失败：" << error << '\n';
            return 1;
        }

        if (!ready) { // 第一张 NV12 帧提供真实尺寸与 stride
            if (!encoder.initialize(decoded, config.fps, 4000000, error)) {
                std::cerr << "编码器初始化失败：" << error << '\n';
                return 1;
            }
            std::vector<uint8_t> header; // MPP 给出的 H.264 SPS/PPS
            if (!encoder.getHeader(header, error)) {
                std::cerr << "读取 SPS/PPS 失败：" << error << '\n';
                return 1;
            }
            if (!publisher.open(url, decoded.width(), decoded.height(),
                                config.fps, header, error)) {
                std::cerr << "RTSP 发布失败：" << error << '\n';
                return 1;
            }
            ready = true;
            std::cout << "已连接 " << url << "，开始发送 H.264 视频\n";
        }

        // 时间戳由 V4L2 随每张采集帧返回，单位从秒/微秒合并成微秒。
        const int64_t pts_us = static_cast<int64_t>(captured.timestamp.tv_sec) *
                               1000000 + captured.timestamp.tv_usec;
        const bool ok = encoder.encode(decoded, pts_us, encoded, error);
        decoded.reset(); // 编码完成后归还 NV12 硬件缓冲区
        if (!ok) {
            std::cerr << "H.264 编码失败：" << error << '\n';
            return 1;
        }
        if (!publisher.write(encoded, error)) {
            std::cerr << "RTSP 发送失败：" << error << '\n';
            return 1;
        }
        ++frames;
        if (frames == 1 || frames % 30 == 0) {
            const double seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started).count();
            std::cout << "已发送 " << frames << " 帧，PTS " << encoded.pts_us
                      << " 微秒，平均 " << frames / seconds
                      << " fps\n";
        }
    }

    if (!publisher.close(error)) {
        std::cerr << "关闭 RTSP 连接失败：" << error << '\n';
        return 1;
    }
    std::cout << "推流结束，共发送 " << frames << " 帧\n";
    return 0;
}
