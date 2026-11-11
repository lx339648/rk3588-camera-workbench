#include "capture_v4l2.hpp" // 使用封装好的采集类

#include <iostream>           // 打印采集结果和错误
#include <iomanip>            // 用 6 位数字显示微秒
#include <string>             // 保存错误信息

int main() {                   // 示例程序入口
    CaptureConfig config;      // 默认 /dev/video84、YUYV 640x480、30 fps
    V4L2Capture camera;        // 创建摄像头对象，此时还没有打开设备
    std::string error;         // 接收函数返回的错误说明

    if (!camera.initialize(config, error)) { // 一次完成设备配置、映射和启动
        std::cerr << "初始化失败：" << error << '\n'; // 显示具体原因
        return 1;              // 初始化失败，程序结束
    }

    CapturedFrame frame;       // 用来接收每次取到的 YUYV 数据
    for (int i = 0; i < 90; ++i) { // 连续取 90 帧，大约是 3 秒
        if (!camera.captureFrame(frame, error)) { // 内部完成 DQBUF、复制、QBUF
            std::cerr << "取帧失败：" << error << '\n'; // 显示原因
            camera.stop();     // 停止采集并解除映射
            return 1;          // 结束程序
        }

        std::cout << "序号=" << frame.sequence           // 驱动帧序号
                  << " 字节数=" << frame.data.size()     // 本帧 YUYV 数据的大小
                  << " 时间戳=" << frame.timestamp.tv_sec // 时间戳秒数
                  << "." << std::setw(6) << std::setfill('0')
                  << frame.timestamp.tv_usec             // 时间戳微秒数
                  << '\n';                              // 输出这一帧的信息

        // 下一步可以将 frame 交给 RGA 转为 NV12。
    }

    camera.stop();              // STREAMOFF、munmap、close
    return 0;                   // 全部 90 帧采集成功
}
