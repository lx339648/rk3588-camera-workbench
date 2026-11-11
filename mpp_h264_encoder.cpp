#include "mpp_h264_encoder.hpp" // 本模块接口

#include <chrono>                 // 编码输出等待期限
#include <cstddef>                // size_t
#include <thread>                 // 短暂等待异步编码输出
#include <rk_venc_cfg.h>          // MppEncCfg 和配置键
#include <mpp_packet.h>          // 编码后的 MppPacket

namespace {
// 在 Annex B 码流中寻找 H.264 的 IDR NAL（类型 5）。
bool containsIdr(const std::vector<uint8_t>& bytes) {
    for (size_t i = 0; i + 3 < bytes.size(); ++i) {
        // 同时接受 00 00 01 和 00 00 00 01 两种起始码。
        if (bytes[i] != 0 || bytes[i + 1] != 0) continue;
        size_t nal = i + 3;
        if (bytes[i + 2] == 1) {
            // 三字节起始码后面直接是 NAL 头。
        } else if (bytes[i + 2] == 0 && bytes[i + 3] == 1) {
            nal = i + 4;
        } else {
            continue;
        }
        if (nal < bytes.size() && (bytes[nal] & 0x1f) == 5) return true;
    }
    return false;
}
} // namespace

H264Encoder::~H264Encoder() { stop(); } // 自动销毁 MPP 实例

bool H264Encoder::initialize(const Nv12Frame& first_frame, int fps,
                             int bitrate, std::string& error) {
    stop();                        // 可重复初始化
    error.clear();                 // 清除调用方上次留下的错误
    if (!first_frame.valid() || first_frame.format() != MPP_FMT_YUV420SP) {
        error = "编码器需要有效的 NV12 输入帧";
        return false;
    }
    if (fps <= 0 || bitrate <= 0) { // 避免无效码率与帧率配置
        error = "fps 和 bitrate 必须大于 0";
        return false;
    }

    width_ = first_frame.width();   // 画面可见尺寸
    height_ = first_frame.height();
    hor_stride_ = first_frame.horizontalStride(); // 实际内存跨度
    ver_stride_ = first_frame.verticalStride();
    if (width_ <= 0 || height_ <= 0 || hor_stride_ < width_ ||
        ver_stride_ < height_) {
        error = "NV12 帧的尺寸或 stride 不合法";
        stop();
        return false;
    }

    MPP_RET ret = mpp_create(&context_, &api_); // 创建编码器上下文
    if (ret != MPP_OK || !context_ || !api_) {
        error = "mpp_create(encoder) 失败，错误码 " + std::to_string(ret);
        stop();
        return false;
    }
    MppPollType non_block = MPP_POLL_NON_BLOCK; // 由程序自己控制最长等待时间
    ret = api_->control(context_, MPP_SET_OUTPUT_TIMEOUT, &non_block);
    if (ret != MPP_OK) {
        error = "设置编码输出为非阻塞失败，错误码 " + std::to_string(ret);
        stop();
        return false;
    }
    MppPollType block = MPP_POLL_BLOCK; // 送帧时等待空闲输入槽
    ret = api_->control(context_, MPP_SET_INPUT_TIMEOUT, &block);
    if (ret != MPP_OK) {
        error = "设置编码输入等待方式失败，错误码 " + std::to_string(ret);
        stop();
        return false;
    }
    ret = mpp_init(context_, MPP_CTX_ENC, MPP_VIDEO_CodingAVC); // H.264 编码
    if (ret != MPP_OK) {
        error = "mpp_init(H264) 失败，错误码 " + std::to_string(ret);
        stop();
        return false;
    }

    MppEncCfg cfg = nullptr;       // 一次性的编码参数对象
    ret = mpp_enc_cfg_init(&cfg);  // 创建参数对象
    if (ret != MPP_OK || !cfg) {
        error = "mpp_enc_cfg_init 失败，错误码 " + std::to_string(ret);
        stop();
        return false;
    }
    auto fail_cfg = [&](const std::string& message) { // 统一释放配置资源
        error = message;
        mpp_enc_cfg_deinit(cfg);
        stop();
        return false;
    };
    ret = api_->control(context_, MPP_ENC_GET_CFG, cfg); // 先读默认配置
    if (ret != MPP_OK) {
        return fail_cfg("MPP_ENC_GET_CFG 失败，错误码 " + std::to_string(ret));
    }
    // 每个参数都检查返回值，避免拼错配置键时仍以为已经生效。
    auto set = [&](const char* key, RK_S32 value) {
        const MPP_RET result = mpp_enc_cfg_set_s32(cfg, key, value);
        if (result != MPP_OK) {
            error = std::string("设置 ") + key + " 失败，错误码 " + std::to_string(result);
            return false;
        }
        return true;
    };
    if (!set("prep:width", width_) || !set("prep:height", height_) ||
        !set("prep:hor_stride", hor_stride_) ||
        !set("prep:ver_stride", ver_stride_) ||
        !set("prep:format", MPP_FMT_YUV420SP) ||
        !set("codec:type", MPP_VIDEO_CodingAVC) ||
        !set("rc:mode", MPP_ENC_RC_MODE_CBR) ||
        !set("rc:fps_in_flex", 0) || !set("rc:fps_in_num", fps) ||
        !set("rc:fps_in_denom", 1) || !set("rc:fps_out_flex", 0) ||
        !set("rc:fps_out_num", fps) || !set("rc:fps_out_denom", 1) ||
        !set("rc:gop", fps) || !set("rc:bps_target", bitrate) ||
        !set("rc:bps_max", bitrate * 17 / 16) ||
        !set("rc:bps_min", bitrate * 15 / 16) ||
        !set("h264:profile", 100) || !set("h264:level", 40)) {
        return fail_cfg(error);    // 任意配置失败都停止初始化
    }
    ret = api_->control(context_, MPP_ENC_SET_CFG, cfg); // 让配置生效
    mpp_enc_cfg_deinit(cfg);      // 参数已经由 MPP 接收，可以释放对象
    if (ret != MPP_OK) {
        error = "MPP_ENC_SET_CFG 失败，错误码 " + std::to_string(ret);
        stop();
        return false;
    }
    return true;                  // 编码器准备完毕
}

bool H264Encoder::getHeader(std::vector<uint8_t>& header, std::string& error) {
    header.clear();               // 不保留旧 SPS/PPS
    error.clear();
    if (!context_ || !api_) {
        error = "编码器尚未初始化";
        return false;
    }
    std::vector<uint8_t> storage(4096); // SPS/PPS 的临时接收空间
    MppPacket packet = nullptr;   // 指向临时空间的 MPP 包对象
    MPP_RET ret = mpp_packet_init(&packet, storage.data(), storage.size());
    if (ret != MPP_OK || !packet) {
        error = "创建 SPS/PPS 接收包失败，错误码 " + std::to_string(ret);
        return false;
    }
    mpp_packet_set_length(packet, 0); // 官方示例要求取头前清零有效长度
    ret = api_->control(context_, MPP_ENC_GET_HDR_SYNC, packet);
    if (ret == MPP_OK) {
        const auto* bytes = static_cast<const uint8_t*>(mpp_packet_get_pos(packet));
        const size_t count = mpp_packet_get_length(packet);
        if (bytes && count > 0 && count <= storage.size()) {
            header.assign(bytes, bytes + count); // 复制后不依赖临时内存
        } else {
            error = "MPP 返回的 SPS/PPS 长度异常";
        }
    } else {
        error = "MPP_ENC_GET_HDR_SYNC 失败，错误码 " + std::to_string(ret);
    }
    mpp_packet_deinit(&packet);   // 包对象不再使用
    return error.empty();         // true 表示取得了有效 SPS/PPS
}

bool H264Encoder::requestKeyframe(std::string& error) {
    error.clear();
    if (!context_ || !api_) {
        error = "编码器尚未初始化";
        return false;
    }
    const MPP_RET ret = api_->control(context_, MPP_ENC_SET_IDR_FRAME, nullptr);
    if (ret != MPP_OK) {
        error = "请求 IDR 失败，错误码 " + std::to_string(ret);
        return false;
    }
    return true;
}

bool H264Encoder::encode(const Nv12Frame& decoded, int64_t pts_us,
                         EncodedFrame& output, std::string& error) {
    output = {};                  // 每次都输出新的完整访问单元
    error.clear();
    if (!context_ || !api_ || !decoded.valid()) {
        error = "编码器或输入帧尚未准备好";
        return false;
    }
    if (decoded.format() != MPP_FMT_YUV420SP ||
        decoded.width() != width_ || decoded.height() != height_ ||
        decoded.horizontalStride() != hor_stride_ ||
        decoded.verticalStride() != ver_stride_) {
        error = "输入帧不是初始化时约定的 NV12 尺寸和 stride";
        return false;
    }

    if (decoded.log().enabled) {
        output.log = decoded.log();
        output.log.mark(FramePoint::EncodeStart);
    }

    // 这一帧的采集时间跟着 MppFrame 一起进入硬件编码器。
    mpp_frame_set_pts(decoded.nativeFrame(), static_cast<RK_S64>(pts_us));
    MPP_RET ret = api_->encode_put_frame(context_, decoded.nativeFrame());
    if (ret != MPP_OK) {
        error = "encode_put_frame 失败，错误码 " + std::to_string(ret);
        return false;
    }

    // MPP 异步产出压缩包；同一线程等到本帧完成，再允许上游释放 decoded。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;) {
        MppPacket packet = nullptr; // MPP 返回的包由调用方释放
        ret = api_->encode_get_packet(context_, &packet);
        if (ret != MPP_OK && ret != MPP_ERR_TIMEOUT) {
            error = "encode_get_packet 失败，错误码 " + std::to_string(ret);
            return false;
        }
        if (packet) {
            const auto* bytes = static_cast<const uint8_t*>(mpp_packet_get_pos(packet));
            const size_t count = mpp_packet_get_length(packet);
            const bool last_part = !mpp_packet_is_partition(packet) ||
                                   mpp_packet_is_eoi(packet);
            if (!bytes || count == 0) {
                mpp_packet_deinit(&packet);
                error = "MPP 返回了空的 H.264 编码包";
                return false;
            }
            // 默认未启用分片；若出现分片，就把各分片按顺序拼成一帧。
            output.data.insert(output.data.end(), bytes, bytes + count);
            output.pts_us = pts_us; // 保存本次输入的 V4L2 时间戳
            mpp_packet_deinit(&packet);
            if (last_part) {
                output.keyframe = containsIdr(output.data);
                output.log.mark(FramePoint::Encoded);
                return true;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            error = "等待 H.264 编码输出超时";
            api_->reset(context_); // 丢弃可能迟到的旧包，避免下帧错配
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void H264Encoder::stop() {        // 清理编码器资源
    if (context_) {
        mpp_destroy(context_);
        context_ = nullptr;
        api_ = nullptr;
    }
    width_ = height_ = hor_stride_ = ver_stride_ = 0;
}
