#include "capture_v4l2.hpp"     // V4L2 摄像头模块
#include "mpp_mjpeg_decoder.hpp" // MPP MJPG 解码模块

#include <chrono>               // 统计实际耗时
#include <iostream>             // 输出测试结果
#include <string>               // 保存错误信息

int main() {
    CaptureConfig config;        // 旧 MJPG 摄像头的独立对照测试
    config.device = "/dev/video80";
    config.width = 1920;
    config.height = 1080;
    config.pixel_format = CapturePixelFormat::Mjpeg;
    V4L2Capture camera;          // 摄像头对象
    MjpegDecoder decoder;        // MPP MJPG 解码器对象
    std::string error;           // 两个模块返回的错误说明

    if (!camera.initialize(config, error)) { // 先启动 V4L2 采集
        std::cerr << "采集初始化失败：" << error << '\n';
        return 1;
    }
    if (!decoder.initialize(config.width, config.height, error)) { // 启动解码器
        std::cerr << "解码初始化失败：" << error << '\n';
        return 1;
    }

    CapturedFrame compressed;    // 每次从摄像头取得 MJPG 字节
    DecodedFrame decoded;        // 每次从 MPP 取得未压缩像素帧
    double decode_ms_sum = 0.0;  // 累计每次 decode() 调用耗时
    int decoded_count = 0;       // 成功解码的帧数
    int bad_count = 0;           // MPP 标记为失败的帧数
    const auto begin = std::chrono::steady_clock::now(); // 开始统计整段运行时间

    for (int i = 0; i < 300; ++i) { // 连续处理 300 帧，约 10 秒
        if (!camera.captureFrame(compressed, error)) { // DQBUF、复制、QBUF
            std::cerr << "采集失败：" << error << '\n';
            return 1;
        }
        const auto decode_begin = std::chrono::steady_clock::now(); // 开始计时
        const DecodeStatus status = decoder.decode(compressed, decoded, error);
        const auto decode_end = std::chrono::steady_clock::now(); // 结束计时
        decode_ms_sum += std::chrono::duration<double, std::milli>(
            decode_end - decode_begin).count(); // 累加本次解码耗时
        if (status == DecodeStatus::FatalError) { // 解码器本身故障
            std::cerr << "解码器错误：" << error << '\n';
            return 1;
        }
        if (status == DecodeStatus::BadFrame) { // 单帧失败不终止整次采集
            ++bad_count;
            std::cerr << "跳过序号 " << compressed.sequence << "：" << error << '\n';
            continue;
        }
        ++decoded_count;         // 统计成功取得的像素帧

        if (i == 0 || (i + 1) % 30 == 0) { // 第一帧及每 30 帧打印一次
            std::cout << "已采集 " << i + 1 << " 帧，成功解码 " << decoded_count
                      << " 帧，尺寸 "
                      << decoded.width() << "x" << decoded.height()
                      << "，stride " << decoded.horizontalStride()
                      << "x" << decoded.verticalStride()
                      << "，格式值 " << static_cast<int>(decoded.format())
                      << (decoded.format() == MPP_FMT_YUV420SP ? " (NV12)" : " (需检查)")
                      << '\n';
        }
        decoded.reset();         // 本次只测解码，暂不送编码器
    }

    const auto end = std::chrono::steady_clock::now(); // 结束统计
    const double elapsed_seconds = std::chrono::duration<double>(end - begin).count();
    std::cout << "采集 300 帧，成功解码 " << decoded_count
              << " 帧，跳过 " << bad_count << " 帧，总耗时 "
              << elapsed_seconds << " 秒，实际解码输出 "
              << decoded_count / elapsed_seconds << " 帧/秒，平均 decode() 调用 "
              << decode_ms_sum / 300.0 << " 毫秒\n";
    return 0;                    // 对象析构时自动释放 MPP 和摄像头资源
}
