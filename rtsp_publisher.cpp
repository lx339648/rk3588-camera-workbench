#include "rtsp_publisher.hpp"

#include <cstring>
#include <limits>

extern "C" { // FFmpeg 的函数和结构体由 C 接口提供
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
}

namespace {
std::string ffError(int code) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, message, sizeof(message));
    return message;
}
} // namespace

RtspPublisher::~RtspPublisher() {
    std::string ignored;
    close(ignored);
}

bool RtspPublisher::open(const std::string& url, int width, int height, int fps,
                         const std::vector<uint8_t>& header, std::string& error) {
    std::string ignored;
    close(ignored); // 允许同一对象重新连接
    error.clear();

    if (url.rfind("rtsp://", 0) != 0 || width <= 0 || height <= 0 || fps <= 0 ||
        header.empty() ||
        header.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        error = "RTSP 地址、画面参数或 SPS/PPS 无效";
        return false;
    }

    int ret = avformat_network_init(); // 初始化 FFmpeg 网络接口
    if (ret < 0) {
        error = "初始化 FFmpeg 网络接口失败：" + ffError(ret);
        return false;
    }
    ret = avformat_alloc_output_context2(&format_, nullptr, "rtsp", url.c_str());
    if (ret < 0 || !format_) {
        error = "创建 RTSP 输出失败：" + ffError(ret);
        close(ignored);
        return false;
    }
    // 对应 FFmpeg 命令行的 -muxdelay 0.1；单位是微秒。
    // 只限制 RTSP 封装器的等待，不改变摄像头帧率或 H.264 编码参数。
    format_->max_delay = 100000;

    stream_ = avformat_new_stream(format_, nullptr); // 只有一条 H.264 视频轨道
    if (!stream_) {
        error = "创建 RTSP 视频轨道失败";
        close(ignored);
        return false;
    }
    fps_ = fps;
    stream_->time_base = AVRational{1, 1000000}; // 输入 PTS 的单位是微秒
    stream_->avg_frame_rate = AVRational{fps, 1};

    AVCodecParameters* codec = stream_->codecpar;
    codec->codec_type = AVMEDIA_TYPE_VIDEO;
    codec->codec_id = AV_CODEC_ID_H264;
    codec->width = width;
    codec->height = height;
    codec->codec_tag = 0;

    // MPP 给出的 Annex B SPS/PPS 让 RTSP 服务生成 H.264 会话描述。
    codec->extradata = static_cast<uint8_t*>(
        av_mallocz(header.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!codec->extradata) {
        error = "分配 SPS/PPS 空间失败";
        close(ignored);
        return false;
    }
    std::memcpy(codec->extradata, header.data(), header.size());
    codec->extradata_size = static_cast<int>(header.size());

    AVDictionary* options = nullptr;
    av_dict_set(&options, "rtsp_transport", "tcp", 0); // 推流与 MediaMTX 走 TCP
    // RTSP 封装器在这里建立到服务端的连接，不需要单独 avio_open()。
    ret = avformat_write_header(format_, &options);
    av_dict_free(&options);
    if (ret < 0) {
        error = "连接 RTSP 服务或发布 /live 失败：" + ffError(ret);
        close(ignored);
        return false;
    }
    header_written_ = true;
    return true;
}

bool RtspPublisher::write(const EncodedFrame& frame, std::string& error) {
    error.clear();
    if (!format_ || !stream_ || !header_written_) {
        error = "RTSP 尚未连接";
        return false;
    }
    if (frame.data.empty() ||
        frame.data.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        error = "H.264 编码帧为空或过大";
        return false;
    }
    if (last_pts_us_ >= 0 && frame.pts_us <= last_pts_us_) {
        error = "采集时间戳未递增";
        return false;
    }
    if (first_pts_us_ < 0 && !frame.keyframe) {
        error = "RTSP 首帧需要 IDR 关键帧";
        return false;
    }
    if (first_pts_us_ < 0) first_pts_us_ = frame.pts_us;

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        error = "分配 FFmpeg 包失败";
        return false;
    }
    int ret = av_new_packet(packet, static_cast<int>(frame.data.size()));
    if (ret < 0) {
        error = "分配 H.264 包数据失败：" + ffError(ret);
        av_packet_free(&packet);
        return false;
    }
    std::memcpy(packet->data, frame.data.data(), frame.data.size());
    packet->stream_index = stream_->index;
    const AVRational microseconds{1, 1000000};
    packet->pts = av_rescale_q(frame.pts_us - first_pts_us_, microseconds,
                               stream_->time_base);
    packet->dts = packet->pts; // 当前 MPP 编码器按输入顺序输出，不使用 B 帧
    packet->duration = av_rescale_q(1000000 / fps_, microseconds,
                                    stream_->time_base);
    if (frame.keyframe) packet->flags |= AV_PKT_FLAG_KEY;

    ret = av_interleaved_write_frame(format_, packet); // 把编码帧交给 RTSP 封装器
    av_packet_free(&packet);
    if (ret < 0) {
        error = "RTSP 发送失败：" + ffError(ret);
        return false;
    }
    last_pts_us_ = frame.pts_us;
    return true;
}

bool RtspPublisher::close(std::string& error) {
    error.clear();
    if (!format_) return true;
    if (header_written_) {
        const int ret = av_write_trailer(format_); // 正常关闭 RTSP 发布会话
        if (ret < 0) error = "关闭 RTSP 会话失败：" + ffError(ret);
    }
    avformat_free_context(format_);
    format_ = nullptr;
    stream_ = nullptr;
    fps_ = 0;
    first_pts_us_ = -1;
    last_pts_us_ = -1;
    header_written_ = false;
    return error.empty();
}
