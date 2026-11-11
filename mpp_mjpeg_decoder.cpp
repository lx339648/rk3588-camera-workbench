#define MODULE_TAG "mjpeg_decoder" // MPP 缓冲区宏使用的模块名称

#include "mpp_mjpeg_decoder.hpp" // 本模块的声明

#include <chrono>                 // 解码等待超时
#include <cstring>                // std::memcpy
#include <limits>                 // 防止输出容量计算溢出
#include <thread>                 // 短暂等待异步解码完成
#include <mpp_meta.h>             // KEY_OUTPUT_FRAME 和 MPP 元数据

MjpegDecoder::~MjpegDecoder() { stop(); } // 析构时自动清理

bool MjpegDecoder::initialize(int width, int height, std::string& error) {
    stop();                       // 重复初始化前先释放旧实例
    error.clear();                // 清除旧错误
    if (width <= 0 || height <= 0) { // 检查尺寸
        error = "解码尺寸必须大于 0";
        return false;
    }

    // MPP 的 JPEG 测试程序按 16 对齐，并给输出预留最多 4 字节/像素。
    const size_t aligned_width = (static_cast<size_t>(width) + 15) & ~size_t(15);
    const size_t aligned_height = (static_cast<size_t>(height) + 15) & ~size_t(15);
    if (aligned_width > std::numeric_limits<size_t>::max() / aligned_height / 4) {
        error = "解码图像尺寸过大";
        return false;
    }
    output_capacity_ = aligned_width * aligned_height * 4;

    MPP_RET ret = mpp_create(&context_, &api_); // 创建 MPP 上下文
    if (ret != MPP_OK || !context_ || !api_) { // 检查结果
        error = "mpp_create 失败，错误码 " + std::to_string(ret);
        stop();
        return false;
    }
    ret = mpp_init(context_, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG); // 选择 MJPEG 解码
    if (ret != MPP_OK) {
        error = "mpp_init(MJPEG) 失败，错误码 " + std::to_string(ret);
        stop();
        return false;
    }

    MppFrameFormat requested_format = MPP_FMT_YUV420SP; // 请求 NV12 输出
    ret = api_->control(context_, MPP_DEC_SET_OUTPUT_FORMAT, &requested_format);
    if (ret != MPP_OK) {       // 板子不接受 NV12 时明确报告
        error = "MPP 无法设置 NV12 输出，错误码 " + std::to_string(ret);
        stop();
        return false;
    }

    // 新内核优先 DMA-HEAP；若板级系统没有，再尝试 DRM 分配器。
    ret = mpp_buffer_group_get_internal(&group_, MPP_BUFFER_TYPE_DMA_HEAP);
    if (ret != MPP_OK) {
        ret = mpp_buffer_group_get_internal(&group_, MPP_BUFFER_TYPE_DRM);
    }
    if (ret != MPP_OK || !group_) {
        error = "MPP 无法创建硬件缓冲区组，错误码 " + std::to_string(ret);
        stop();
        return false;
    }
    return true;                // 解码器已准备好
}

DecodeStatus MjpegDecoder::decode(const CapturedFrame& input, DecodedFrame& output,
                                  std::string& error) {
    error.clear();              // 清除旧错误
    output.reset();             // 先释放上一次输出，避免一直占用硬件缓冲区
    if (!context_ || !api_ || !group_) { // 必须先初始化
        error = "MPP 解码器尚未初始化";
        return DecodeStatus::FatalError;
    }
    if (input.data.empty()) {   // 一次应输入一张完整的 MJPG 图像
        error = "输入的 MJPG 帧为空";
        return DecodeStatus::BadFrame;
    }

    MppBuffer input_buffer = nullptr; // 保存压缩 MJPG 的 MPP 输入缓冲区
    MppPacket packet = nullptr;       // 描述输入码流的 MPP 包
    MppFrame returned = nullptr;      // MPP 返回的解码图像

    // 统一释放本次输入资源；输出资源由 DecodedFrame 管理。
    auto release_input = [&] {
        if (packet) mpp_packet_deinit(&packet);
        if (input_buffer) mpp_buffer_put(input_buffer);
    };
    // 出错后先停止本次异步任务，防止硬件继续引用待释放的缓冲区。
    auto fail = [&](const std::string& message) {
        error = message;
        api_->reset(context_);
        if (returned && returned != output.frame_) mpp_frame_deinit(&returned);
        release_input();
        output.reset();
        return DecodeStatus::FatalError;
    };

    MPP_RET ret = mpp_buffer_get(group_, &input_buffer, input.data.size());
    if (ret != MPP_OK || !input_buffer) {
        return fail("MPP 输入缓冲区申请失败，错误码 " + std::to_string(ret));
    }
    void* input_address = mpp_buffer_get_ptr(input_buffer); // 可写的输入地址
    if (!input_address) return fail("MPP 输入缓冲区没有可写地址");
    std::memcpy(input_address, input.data.data(), input.data.size()); // 复制压缩数据
    mpp_buffer_sync_end(input_buffer); // 通知硬件读取 CPU 写入的数据

    ret = mpp_packet_init_with_buffer(&packet, input_buffer); // 包关联输入缓冲区
    if (ret != MPP_OK || !packet) {
        return fail("mpp_packet_init_with_buffer 失败，错误码 " + std::to_string(ret));
    }
    mpp_packet_set_length(packet, input.data.size()); // MJPG 有效字节数
    const RK_S64 pts_us = static_cast<RK_S64>(input.timestamp.tv_sec) * 1000000 +
                          input.timestamp.tv_usec; // V4L2 时间戳转微秒
    mpp_packet_set_pts(packet, pts_us); // 后续编码可以沿用这帧的时间

    ret = mpp_buffer_get(group_, &output.buffer_, output_capacity_); // 图像缓冲区
    if (ret != MPP_OK || !output.buffer_) {
        return fail("MPP 输出缓冲区申请失败，错误码 " + std::to_string(ret));
    }
    ret = mpp_frame_init(&output.frame_); // 创建输出图像描述对象
    if (ret != MPP_OK || !output.frame_) {
        return fail("mpp_frame_init 失败，错误码 " + std::to_string(ret));
    }
    mpp_frame_set_buffer(output.frame_, output.buffer_); // 给解码器提供像素缓冲区

    MppMeta meta = mpp_packet_get_meta(packet); // 获取输入包的元数据
    if (!meta) return fail("MJPG 输入包没有 MPP 元数据");
    ret = mpp_meta_set_frame(meta, KEY_OUTPUT_FRAME, output.frame_); // 绑定输出帧
    if (ret != MPP_OK) {
        return fail("绑定 MJPG 输出帧失败，错误码 " + std::to_string(ret));
    }

    ret = api_->decode_put_packet(context_, packet); // 将一整张 JPEG 送入硬件
    if (ret != MPP_OK) {
        return fail("decode_put_packet 失败，错误码 " + std::to_string(ret));
    }

    // MPP 内部异步解码；等待对应的输出帧，最多 2 秒。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        ret = api_->decode_get_frame(context_, &returned); // 尝试取出解码图像
        if (ret == MPP_OK && returned) break; // 已获得完整的返回帧
        if (ret != MPP_OK && ret != MPP_ERR_TIMEOUT) {
            return fail("decode_get_frame 失败，错误码 " + std::to_string(ret));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1)); // 避免空转
    } while (std::chrono::steady_clock::now() < deadline);

    if (!returned) return fail("等待 MJPG 解码输出超时");
    if (returned != output.frame_) return fail("MPP 返回了意外的输出帧对象");
    if (mpp_frame_get_info_change(returned)) {
        return fail("MPP 返回了格式变化事件，当前示例尚未处理该事件");
    }
    if (mpp_frame_get_errinfo(returned) || mpp_frame_get_discard(returned)) {
        error = "MPP 报告本帧解码失败"; // 可能是码流或硬件解码问题
        release_input();        // 输入包已处理完，释放它
        output.reset();         // 丢弃这一张不可用的像素帧
        return DecodeStatus::BadFrame; // JPEG 帧独立，继续尝试下一帧
    }
    if (!mpp_frame_get_buffer(returned)) {
        return fail("MPP 返回的图像没有像素缓冲区");
    }

    release_input();            // 解码完成后输入压缩数据可以释放
    return DecodeStatus::FrameReady; // output 继续持有解码后的像素缓冲区
}

void MjpegDecoder::stop() {      // 销毁解码器资源
    if (context_) {             // 若 MPP 上下文存在
        mpp_destroy(context_);  // 等待并销毁 MPP 实例
        context_ = nullptr;
        api_ = nullptr;
    }
    if (group_) {               // 若硬件缓冲区组存在
        mpp_buffer_group_put(group_); // 释放缓冲区组
        group_ = nullptr;
    }
    output_capacity_ = 0;       // 清除旧配置
}
