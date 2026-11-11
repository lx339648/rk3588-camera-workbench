#include "capture_v4l2.hpp"     // 摄像头采集
#include "rga_yuyv_converter.hpp" // YUYV 硬件格式转换
#include "mpp_h264_encoder.hpp"  // H.264 硬件编码

#include <chrono>                 // 各阶段计时
#include <cstddef>                // size_t
#include <cstdint>                // int64_t
#include <fstream>                // 写出验证用的裸流文件
#include <iostream>               // 打印统计信息
#include <string>                 // 错误信息和输出路径

int main(int argc, char* argv[]) {
    const std::string output_path = argc > 1 ? argv[1] : "test.h264";
    CaptureConfig config;         // 默认 /dev/video84，YUYV 640x480@30
    V4L2Capture camera;           // 只负责采集 YUYV 数据
    RgaYuyvConverter converter;   // 只负责 YUYV -> NV12
    H264Encoder encoder;          // 只负责 NV12 -> H.264
    std::string error;            // 用于接收各模块错误

    if (!camera.initialize(config, error)) { // 打开摄像头并启动流
        std::cerr << "采集初始化失败：" << error << '\n';
        return 1;
    }
    if (!converter.initialize(config.width, config.height, error)) {
        std::cerr << "RGA 初始化失败：" << error << '\n';
        return 1;
    }

    CapturedFrame captured;      // 从 V4L2 返回的 YUYV 帧
    Nv12Frame decoded;           // 从 RGA 返回的 NV12 像素帧
    EncodedFrame encoded;        // 从 MPP 返回的 H.264 码流
    std::ofstream file;          // 第一次转换成功后才创建文件
    double convert_ms_sum = 0.0; // RGA 转换的累计耗时
    double encode_ms_sum = 0.0;  // 编码调用的累计耗时
    int encoded_frames = 0;     // 成功写出的 H.264 帧数
    size_t total_bytes = 0;     // 编码后的总字节数
    const auto begin = std::chrono::steady_clock::now(); // 整条链路起点

    for (int i = 0; i < 300; ++i) { // 处理约 10 秒的摄像头输入
        if (!camera.captureFrame(captured, error)) { // DQBUF、复制、QBUF
            std::cerr << "采集失败：" << error << '\n';
            return 1;
        }
        const auto convert_begin = std::chrono::steady_clock::now();
        if (!converter.convert(captured, decoded, error)) {
            std::cerr << "RGA 转换失败：" << error << '\n';
            return 1;
        }
        convert_ms_sum += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - convert_begin).count();

        if (encoded_frames == 0) { // 第一张 NV12 帧决定编码器的实际 stride
            if (!encoder.initialize(decoded, config.fps, 4000000, error)) {
                std::cerr << "编码器初始化失败：" << error << '\n';
                return 1;
            }
            std::vector<uint8_t> header; // H.264 解码所需的 SPS/PPS
            if (!encoder.getHeader(header, error)) {
                std::cerr << "获取 H.264 头失败：" << error << '\n';
                return 1;
            }
            file.open(output_path, std::ios::binary | std::ios::trunc);
            if (!file) {
                std::cerr << "无法创建输出文件：" << output_path << '\n';
                return 1;
            }
            file.write(reinterpret_cast<const char*>(header.data()), header.size());
            if (!file) {
                std::cerr << "写入 H.264 头失败\n";
                return 1;
            }
            std::cout << "NV12 " << decoded.width() << "x" << decoded.height()
                      << "，stride " << decoded.horizontalStride() << "x"
                      << decoded.verticalStride() << "，SPS/PPS " << header.size()
                      << " 字节\n";
        }

        const int64_t pts_us = static_cast<int64_t>(captured.timestamp.tv_sec) *
                               1000000 + captured.timestamp.tv_usec;
        const auto encode_begin = std::chrono::steady_clock::now();
        const bool ok = encoder.encode(decoded, pts_us, encoded, error);
        const auto encode_end = std::chrono::steady_clock::now();
        encode_ms_sum += std::chrono::duration<double, std::milli>(
            encode_end - encode_begin).count();
        decoded.reset();        // 编码器已输出，此时归还 NV12 硬件缓冲区
        if (!ok) {
            std::cerr << "编码失败：" << error << '\n';
            return 1;
        }
        file.write(reinterpret_cast<const char*>(encoded.data.data()),
                   encoded.data.size()); // 暂时只保存 H.264 裸流供验证
        if (!file) {
            std::cerr << "写入 H.264 文件失败\n";
            return 1;
        }
        ++encoded_frames;       // 成功保存一张编码帧
        total_bytes += encoded.data.size();
        if (encoded_frames == 1 || encoded_frames % 30 == 0) {
            std::cout << "已编码 " << encoded_frames << " 帧，当前包 "
                      << encoded.data.size() << " 字节，PTS " << encoded.pts_us
                      << " 微秒\n";
        }
    }

    file.close();                // 完成文件写入
    const auto end = std::chrono::steady_clock::now(); // 整条链路终点
    const double seconds = std::chrono::duration<double>(end - begin).count();
    std::cout << "总耗时 " << seconds << " 秒，采集 300 帧，编码 "
              << encoded_frames << " 帧\n"
              << "实际整链路输出 " << encoded_frames / seconds << " fps，"
              << "平均 RGA 转换 " << convert_ms_sum / 300.0 << " ms，"
              << "平均编码调用 "
              << (encoded_frames ? encode_ms_sum / encoded_frames : 0.0)
              << " ms，H.264 数据 " << total_bytes << " 字节\n"
              << "测试文件：" << output_path << '\n';
    return 0;                    // 对象析构时自动释放摄像头与 MPP 资源
}
