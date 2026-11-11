#include "mkv_recorder.hpp"       // 本模块接口

#include <cstring>                  // std::memcpy
#include <limits>                   // 防止数据长度超出 FFmpeg 的 int 范围
extern "C" {                      // FFmpeg 是 C 库，避免 C++ 改写函数符号名
#include <libavcodec/avcodec.h>    // AV_INPUT_BUFFER_PADDING_SIZE
#include <libavcodec/packet.h>     // AVPacket、av_new_packet
#include <libavformat/avformat.h>  // MKV 封装接口
#include <libavutil/error.h>       // av_strerror
#include <libavutil/mathematics.h> // av_rescale_q
#include <libavutil/mem.h>         // av_mallocz
}

namespace {
std::string ffError(int code) {    // 把 FFmpeg 的错误码变成可读文字
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, message, sizeof(message));
    return message;
}
} // namespace

MkvRecorder::~MkvRecorder() {      // 即使调用者忘记 close，也释放资源
    std::string ignored;
    close(ignored);
}

bool MkvRecorder::open(const std::string& path, int width, int height, int fps,
                       const std::vector<uint8_t>& header, std::string& error) {
    std::string ignored;
    close(ignored);                 // 重复 open 前关闭旧文件
    error.clear();
    if (path.empty() || width <= 0 || height <= 0 || fps <= 0 ||
        header.empty() || header.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        error = "MKV 文件名、画面参数或 SPS/PPS 无效";
        return false;
    }

    int ret = avformat_alloc_output_context2(&format_, nullptr, "matroska",
                                              path.c_str()); // 创建 MKV 封装器
    if (ret < 0 || !format_) {
        error = "创建 MKV 封装器失败：" + ffError(ret);
        close(ignored);
        return false;
    }
    stream_ = avformat_new_stream(format_, nullptr); // 文件里只建一个视频轨道
    if (!stream_) {
        error = "创建 MKV 视频轨道失败";
        close(ignored);
        return false;
    }
    fps_ = fps;
    stream_->time_base = AVRational{1, 1000000}; // 采集 PTS 以微秒为单位
    stream_->avg_frame_rate = AVRational{fps, 1}; // 名义播放帧率
    AVCodecParameters* codec = stream_->codecpar;
    codec->codec_type = AVMEDIA_TYPE_VIDEO; // 这条轨道是视频
    codec->codec_id = AV_CODEC_ID_H264;     // 视频编码格式为 H.264
    codec->width = width;
    codec->height = height;
    codec->codec_tag = 0;                   // 由 MKV 封装器自行选择编码标识

    // MKV 需要 SPS/PPS 作为 CodecPrivate；FFmpeg 会把 Annex B 转为 avcC。
    codec->extradata = static_cast<uint8_t*>(
        av_mallocz(header.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!codec->extradata) {
        error = "分配 SPS/PPS 存储空间失败";
        close(ignored);
        return false;
    }
    std::memcpy(codec->extradata, header.data(), header.size());
    codec->extradata_size = static_cast<int>(header.size());

    ret = avio_open(&format_->pb, path.c_str(), AVIO_FLAG_WRITE); // 打开本地文件
    if (ret < 0) {
        error = "打开 MKV 文件失败：" + ffError(ret);
        close(ignored);
        return false;
    }
    ret = avformat_write_header(format_, nullptr); // 写入 EBML、轨道和编码信息
    if (ret < 0) {
        error = "写入 MKV 文件头失败：" + ffError(ret);
        close(ignored);
        return false;
    }
    header_written_ = true;
    return true;
}

bool MkvRecorder::write(const EncodedFrame& frame, std::string& error) {
    error.clear();
    if (!format_ || !header_written_ || !stream_) {
        error = "MKV 文件尚未打开";
        return false;
    }
    if (frame.data.empty() ||
        frame.data.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        error = "H.264 编码帧为空或过大";
        return false;
    }
    if (last_pts_us_ >= 0 && frame.pts_us <= last_pts_us_) {
        error = "采集 PTS 没有递增，不能写入 MKV";
        return false;
    }
    if (first_pts_us_ < 0 && !frame.keyframe) {
        error = "MKV 第一帧必须是 H.264 IDR 关键帧";
        return false;
    }
    if (first_pts_us_ < 0) first_pts_us_ = frame.pts_us; // 文件从 0 开始计时

    AVPacket* packet = av_packet_alloc(); // 创建 FFmpeg 视频包
    if (!packet) {
        error = "创建 FFmpeg 视频包失败";
        return false;
    }
    int ret = av_new_packet(packet, static_cast<int>(frame.data.size()));
    if (ret < 0) {
        error = "分配 FFmpeg 视频包失败：" + ffError(ret);
        av_packet_free(&packet);
        return false;
    }
    std::memcpy(packet->data, frame.data.data(), frame.data.size());
    packet->stream_index = stream_->index; // 写入唯一的视频轨道
    const AVRational microseconds{1, 1000000};
    const int64_t relative_pts = frame.pts_us - first_pts_us_;
    // avformat_write_header() 可能把 MKV time_base 改为 1/1000，这里按实际值换算。
    packet->pts = av_rescale_q(relative_pts, microseconds, stream_->time_base);
    packet->dts = packet->pts;      // 当前 MPP 配置按输入顺序输出帧
    packet->duration = av_rescale_q(1000000 / fps_, microseconds,
                                    stream_->time_base);
    if (frame.keyframe) packet->flags |= AV_PKT_FLAG_KEY;

    ret = av_interleaved_write_frame(format_, packet); // 真正写入 MKV
    av_packet_free(&packet);      // FFmpeg 已接管或复制包内容
    if (ret < 0) {
        error = "写入 MKV 视频帧失败：" + ffError(ret);
        return false;
    }
    last_pts_us_ = frame.pts_us;
    return true;
}

bool MkvRecorder::close(std::string& error) {
    error.clear();
    if (!format_) return true;    // 已关闭时可重复调用
    if (header_written_) {
        const int ret = av_write_trailer(format_); // 写入时长及索引
        if (ret < 0) error = "结束 MKV 文件失败：" + ffError(ret);
    }
    if (format_->pb) {
        const int ret = avio_closep(&format_->pb); // 关闭文件描述符
        if (ret < 0 && error.empty()) error = "关闭 MKV 文件失败：" + ffError(ret);
    }
    avformat_free_context(format_); // 释放轨道及 SPS/PPS 内存
    format_ = nullptr;
    stream_ = nullptr;
    fps_ = 0;
    first_pts_us_ = -1;
    last_pts_us_ = -1;
    header_written_ = false;
    return error.empty();
}
