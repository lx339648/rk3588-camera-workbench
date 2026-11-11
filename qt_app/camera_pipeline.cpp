#include "camera_pipeline.hpp"

#include "capture_v4l2.hpp"
#include "rga_yuyv_converter.hpp"
#include "mpp_h264_encoder.hpp"
#include "mkv_recorder.hpp"
#include "rtsp_publisher.hpp"

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}

#include <utility>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

CameraPipeline::~CameraPipeline() { stop(); }

void CameraPipeline::start() {
    stop(); // 允许故障后重新启动，同时回收旧线程。
    const char* flag = std::getenv("CAMERA_FRAME_LOG");
    const bool requestedLog = !flag || std::strcmp(flag, "0") != 0;
    std::string logError;
    bool logReady = false;
    if (requestedLog) {
        const char* state = std::getenv("XDG_STATE_HOME");
        const char* home = std::getenv("HOME");
        std::filesystem::path base = std::filesystem::current_path();
        if (home && *home) base = std::filesystem::path(home) / ".local/state";
        if (state && *state) base = state;
        logReady = logWriter_.start((base / "CameraWorkbench/logs").string(), logError);
    }
    logEnabled_ = logReady;
    const char* detailFlag = std::getenv("CAMERA_FRAME_LOG_DETAIL");
    detailLogEnabled_ = logReady && detailFlag && std::strcmp(detailFlag, "1") == 0;
    if (logReady) logWriter_.push(detailLogEnabled_
        ? "session_start mode=summary_and_frame_detail"
        : "session_start mode=summary");
    logStats_.takeSummary(); // 清空上一次启动的统计窗口。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastError_.clear();
        lastRecordError_.clear();
        latestImage_ = {};
        latestLog_ = {};
        takenImageLog_ = {};
        lastDiagnostics_ = {};
        logPath_ = logReady ? logWriter_.path() : std::string();
        if (requestedLog && !logReady) lastError_ = logError;
    }
    requested_ = true;
    finished_ = false;
    capturedFrames_ = 0;
    previewThread_ = std::thread(&CameraPipeline::runPreview, this);
    thread_ = std::thread(&CameraPipeline::run, this);
}

void CameraPipeline::stop() {
    requested_ = false;
    wantStream_ = false;
    wantRecord_ = false;
    preview_ = false;
    previewReady_.notify_one();
    if (thread_.joinable()) thread_.join();
    if (previewThread_.joinable()) previewThread_.join();
    logWriter_.stop();
    logEnabled_ = false;
    detailLogEnabled_ = false;
    streaming_ = false;
    recording_ = false;
    alive_ = false;
    finished_ = true;
    std::lock_guard<std::mutex> lock(mutex_);
    latestImage_ = {};
    latestLog_ = {};
    takenImageLog_ = {};
    pendingPreview_ = RawPreview{};
}

void CameraPipeline::setPreviewEnabled(bool enabled) {
    preview_ = enabled;
    if (!enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        latestImage_ = {};
        latestLog_ = {};
        takenImageLog_ = {};
        pendingPreview_ = RawPreview{};
    }
}

void CameraPipeline::setStreamingEnabled(bool enabled, std::string url) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        streamUrl_ = std::move(url);
    }
    wantStream_ = enabled;
}

void CameraPipeline::setRecordingEnabled(bool enabled, std::string path) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        recordPath_ = std::move(path);
    }
    wantRecord_ = enabled;
}

bool CameraPipeline::takeLatestImage(PreviewImage& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (latestImage_.image.isNull()) return false;
    frame = std::move(latestImage_);
    takenImageLog_ = std::move(latestLog_);
    latestImage_ = {};
    latestLog_ = {};
    return true;
}

void CameraPipeline::noteUiSubmitted() {
    FrameLog log;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        log = std::move(takenImageLog_);
        takenImageLog_ = {};
    }
    if (!log.enabled) return;
    log.mark(FramePoint::UiSubmitted); // Qt 已调用 setPixmap，不等同于屏幕实际刷新。
    logStats_.addUi(log);
    if (detailLogEnabled_) logWriter_.push("branch=ui " + log.detailLine());
}

bool CameraPipeline::takeDiagnostics(PipelineDiagnostics& diagnostics) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (lastDiagnostics_.detail.empty()) return false;
    diagnostics = std::move(lastDiagnostics_);
    lastDiagnostics_ = {};
    return true;
}

std::string CameraPipeline::logPath() {
    std::lock_guard<std::mutex> lock(mutex_);
    return logPath_;
}

bool CameraPipeline::takeError(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (lastError_.empty()) return false;
    error = std::move(lastError_);
    lastError_.clear();
    return true;
}

bool CameraPipeline::takeRecordError(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (lastRecordError_.empty()) return false;
    error = std::move(lastRecordError_);
    lastRecordError_.clear();
    return true;
}

void CameraPipeline::reportError(std::string message) {
    std::lock_guard<std::mutex> lock(mutex_);
    lastError_ = std::move(message);
}

void CameraPipeline::reportRecordError(std::string message) {
    std::lock_guard<std::mutex> lock(mutex_);
    lastRecordError_ = std::move(message);
}

void CameraPipeline::run() {
    CaptureConfig config;
    config.enable_frame_log = logEnabled_;
    V4L2Capture camera;
    RgaYuyvConverter converter;
    H264Encoder encoder;
    RtspPublisher publisher;
    MkvRecorder recorder;
    std::string error;
    auto logFailure = [this](const char* stage, const std::string& message,
                             const FrameLog& log) {
        if (logEnabled_) logWriter_.push(
            std::string("error_stage=") + stage + " message=" + message +
            " " + log.detailLine());
    };
    if (!camera.initialize(config, error)) {
        reportError("启动摄像头失败：" + error);
        requested_ = false;
        finished_ = true;
        return;
    }
    if (!converter.initialize(config.width, config.height, error)) {
        reportError("RGA 初始化失败：" + error);
        requested_ = false;
        finished_ = true;
        return;
    }
    alive_ = true;

    CapturedFrame captured;
    Nv12Frame decoded;
    EncodedFrame encoded;
    bool encoderReady = false;
    bool awaitingKeyframe = false;
    bool recordIdrRequested = false;
    bool logWriteFailureReported = false;
    std::vector<uint8_t> header;

    while (requested_) {
        if (!camera.captureFrame(captured, error)) {
            logFailure("capture", error, captured.log);
            if (requested_) reportError("采集失败：" + error);
            break;
        }
        ++capturedFrames_;
        if (!converter.convert(captured, decoded, error)) {
            logFailure("convert", error, captured.log);
            reportError("YUYV 转 NV12 失败：" + error);
            break;
        }

        // 主线只复制 NV12；缩放和转 RGB 交给预览线程，避免编码等待转换。
        if (preview_) {
            MppBuffer buffer = mpp_frame_get_buffer(decoded.nativeFrame());
            auto* pixels = buffer ? static_cast<const uint8_t*>(mpp_buffer_get_ptr(buffer)) : nullptr;
            if (!pixels || decoded.format() != MPP_FMT_YUV420SP) {
                logFailure("preview_input", "无法读取 MPP NV12 画面", decoded.log());
                reportError("无法读取 MPP NV12 画面");
                break;
            }
            RawPreview copy;
            copy.width = decoded.width();
            copy.height = decoded.height();
            copy.horizontalStride = decoded.horizontalStride();
            copy.verticalStride = decoded.verticalStride();
            copy.capturePtsUs = static_cast<int64_t>(captured.timestamp.tv_sec) *
                                1000000 + captured.timestamp.tv_usec;
            copy.sequence = captured.sequence;
            copy.timestampFlags = captured.timestamp_flags;
            if (logEnabled_) copy.log = decoded.log();
            const size_t bytes = static_cast<size_t>(copy.horizontalStride) *
                                 copy.verticalStride * 3 / 2;
            if (bytes > mpp_buffer_get_size(buffer)) {
                logFailure("preview_buffer", "MPP NV12 缓冲区长度不足", decoded.log());
                reportError("MPP NV12 缓冲区长度不足");
                break;
            }
            copy.nv12.resize(bytes);
            mpp_buffer_sync_begin(buffer);
            std::memcpy(copy.nv12.data(), pixels, bytes);
            mpp_buffer_sync_end(buffer);
            if (preview_) {
                std::lock_guard<std::mutex> lock(mutex_);
                pendingPreview_ = std::move(copy); // 仅保留最新待转换帧。
            }
            previewReady_.notify_one();
        }

        if ((wantStream_ || wantRecord_) && !encoderReady) {
            if (!encoder.initialize(decoded, config.fps, 4000000, error) ||
                !encoder.getHeader(header, error)) {
                logFailure("encoder_init", error, decoded.log());
                if (wantStream_) reportError("编码器初始化失败：" + error);
                if (wantRecord_) reportRecordError("编码器初始化失败：" + error);
                wantStream_ = false;
                wantRecord_ = false;
            } else {
                encoderReady = true;
            }
        }
        if (encoderReady && wantStream_ && !streaming_) {
            std::string url;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                url = streamUrl_;
            }
            if (!publisher.open(url, decoded.width(), decoded.height(),
                                config.fps, header, error)) {
                logFailure("rtsp_open", error, decoded.log());
                reportError("RTSP 连接失败：" + error);
                wantStream_ = false;
            } else {
                streaming_ = true;
                awaitingKeyframe = true;
                if (!encoder.requestKeyframe(error)) {
                    reportError("RTSP 请求关键帧失败：" + error);
                    wantStream_ = false;
                }
            }
        }

        if (encoderReady && wantRecord_ && !recording_ && !recordIdrRequested) {
            if (!encoder.requestKeyframe(error)) {
                reportRecordError("录像请求关键帧失败：" + error);
                wantRecord_ = false;
            } else {
                recordIdrRequested = true;
            }
        }

        // 录像与推流只编码一次；两个封装器各自从下一个 IDR 开始。
        FrameLog frameLog;
        if (logEnabled_) frameLog = decoded.log();
        if (encoderReady && (wantStream_ || wantRecord_)) {
            const int64_t pts = static_cast<int64_t>(captured.timestamp.tv_sec) *
                                1000000 + captured.timestamp.tv_usec;
            if (!encoder.encode(decoded, pts, encoded, error)) {
                logFailure("encode", error, encoded.log);
                if (wantStream_) reportError("H.264 编码失败：" + error);
                if (wantRecord_) reportRecordError("H.264 编码失败：" + error);
                wantStream_ = false;
                wantRecord_ = false;
            } else {
                if (logEnabled_) frameLog = encoded.log;
                if (wantRecord_ && !recording_ && encoded.keyframe) {
                    std::string path;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        path = recordPath_;
                    }
                    if (!recorder.open(path, decoded.width(), decoded.height(),
                                       config.fps, header, error)) {
                        logFailure("record_open", error, frameLog);
                        std::remove(path.c_str()); // 清除建头失败留下的空文件。
                        reportRecordError("创建录像文件失败：" + error);
                        wantRecord_ = false;
                    } else {
                        recording_ = true;
                        recordIdrRequested = false;
                    }
                }
                if (recording_ && wantRecord_) {
                    frameLog.mark(FramePoint::RecordStart);
                    if (!recorder.write(encoded, error)) {
                        logFailure("record_write", error, frameLog);
                        reportRecordError("写入录像失败：" + error);
                        wantRecord_ = false;
                    } else frameLog.mark(FramePoint::Recorded);
                }
                if (streaming_ && wantStream_ &&
                    (!awaitingKeyframe || encoded.keyframe)) {
                    awaitingKeyframe = false;
                    frameLog.mark(FramePoint::RtspStart);
                    if (!publisher.write(encoded, error)) {
                        logFailure("rtsp_write", error, frameLog);
                        reportError("RTSP 发送失败：" + error);
                        wantStream_ = false;
                    } else frameLog.mark(FramePoint::RtspWritten);
                }
            }
        }
        if (logEnabled_) {
            logStats_.add(frameLog);
            if (detailLogEnabled_)
                logWriter_.push("branch=main " + frameLog.detailLine());
            if (auto summary = logStats_.takeSummaryIfDue()) {
                PipelineDiagnostics diagnostic;
                auto average = [](const FrameDurationSummary& stat) {
                    return stat.count ? stat.average_ms : -1.0;
                };
                diagnostic.convertMs = average(summary->stages[static_cast<size_t>(FrameStage::Convert)]);
                diagnostic.encodeMs = average(summary->stages[static_cast<size_t>(FrameStage::Encode)]);
                diagnostic.rtspWriteMs = average(summary->stages[static_cast<size_t>(FrameStage::RtspWrite)]);
                diagnostic.captureToUiMs = average(summary->ages[static_cast<size_t>(FrameAge::CaptureToUiSubmitted)]);
                diagnostic.detail = summary->toLine();
                logWriter_.push(diagnostic.detail);
                std::lock_guard<std::mutex> lock(mutex_);
                lastDiagnostics_ = std::move(diagnostic);
            }
            if (!logWriter_.healthy() && !logWriteFailureReported) {
                reportError("诊断日志写入失败：" + logWriter_.path());
                logWriteFailureReported = true;
            }
        }
        decoded.reset(); // 预览已复制、编码已完成，此时归还 NV12 缓冲区。

        if (!wantRecord_ && recording_) {
            if (!recorder.close(error)) reportRecordError("结束录像失败：" + error);
            recording_ = false;
        }
        if (!wantRecord_) recordIdrRequested = false;
        if (!wantStream_ && streaming_) {
            if (!publisher.close(error)) reportError("RTSP 关闭失败：" + error);
            streaming_ = false;
        }
    }

    if (logEnabled_) {
        if (auto summary = logStats_.takeSummary())
            logWriter_.push(summary->toLine());
    }

    decoded.reset();
    if (recording_) {
        if (!recorder.close(error)) reportRecordError("结束录像失败：" + error);
        recording_ = false;
    }
    if (streaming_) {
        if (!publisher.close(error)) reportError("RTSP 关闭失败：" + error);
        streaming_ = false;
    }
    alive_ = false;
    requested_ = false;
    previewReady_.notify_one();
    finished_ = true;
}

void CameraPipeline::runPreview() {
    SwsContext* scaler = nullptr;
    for (;;) {
        RawPreview frame;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            previewReady_.wait(lock, [this] {
                return !requested_ || !pendingPreview_.nv12.empty();
            });
            if (!requested_) break;
            frame = std::move(pendingPreview_);
            pendingPreview_ = RawPreview{};
        }
        const int outWidth = frame.width;
        const int outHeight = frame.height;
        scaler = sws_getCachedContext(scaler, frame.width, frame.height,
            AV_PIX_FMT_NV12, outWidth, outHeight, AV_PIX_FMT_RGB24,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        QImage image(outWidth, outHeight, QImage::Format_RGB888);
        if (!scaler || image.isNull()) {
            if (logEnabled_) logWriter_.push(
                "error_stage=preview_convert message=创建预览图像或颜色转换器失败 " +
                frame.log.detailLine());
            reportError("创建预览图像或颜色转换器失败");
            continue;
        }
        frame.log.mark(FramePoint::PreviewStart);
        const uint8_t* src[] = {
            frame.nv12.data(),
            frame.nv12.data() + frame.horizontalStride * frame.verticalStride
        };
        const int srcStride[] = {frame.horizontalStride, frame.horizontalStride};
        uint8_t* dst[] = {image.bits()};
        const int dstStride[] = {static_cast<int>(image.bytesPerLine())};
        if (sws_scale(scaler, src, srcStride, 0, frame.height,
                      dst, dstStride) != outHeight) {
            if (logEnabled_) logWriter_.push(
                "error_stage=preview_convert message=NV12 转 RGB 失败 " +
                frame.log.detailLine());
            reportError("NV12 转 RGB 失败");
            continue;
        }
        frame.log.mark(FramePoint::PreviewReady);
        if (frame.log.enabled) logStats_.addPreview(frame.log);
        if (preview_) {
            std::lock_guard<std::mutex> lock(mutex_);
            latestImage_ = {std::move(image), frame.capturePtsUs,
                            frame.sequence, frame.timestampFlags};
            latestLog_ = std::move(frame.log);
        }
    }
    if (scaler) sws_freeContext(scaler);
}
