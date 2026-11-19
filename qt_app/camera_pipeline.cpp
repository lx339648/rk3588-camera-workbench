#include "camera_pipeline.hpp"

#include "capture_v4l2.hpp"
#include "rga_yuyv_converter.hpp"
#include "mpp_h264_encoder.hpp"
#include "mkv_recorder.hpp"
#include "rtsp_publisher.hpp"
#include "ai/yolo_rknn.hpp"

#include <utility>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <chrono>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <im2d.hpp>
#include <rga.h>

namespace {
using Clock = std::chrono::steady_clock;
uint64_t elapsedNs(Clock::time_point start, Clock::time_point end) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

// AI 支路直接从相机 YUYV 再做一次 RGA 转换；不读取主链路的 NV12。
// 当前相机固定 640x480，模型固定 640x640：RGA 输出 RGB888 后居中补黑边。
bool yuyvToRknnRgb(const std::vector<uint8_t>& yuyv, uint32_t bytesPerLine,
                   std::vector<uint8_t>& rgb, std::vector<uint8_t>& modelInput,
                   std::string& error) {
    constexpr int width = 640, height = 480, side = vision::YoloRknn::side;
    if (bytesPerLine < width * 2 || bytesPerLine % 2 ||
        yuyv.size() < static_cast<size_t>(bytesPerLine) * height) {
        error = "AI YUYV 输入长度或行跨度不正确";
        return false;
    }
    rgb.resize(width * height * 3);
    modelInput.assign(vision::YoloRknn::inputBytes, 0);
    auto srcHandle = importbuffer_virtualaddr(const_cast<uint8_t*>(yuyv.data()),
                                              static_cast<int>(yuyv.size()));
    auto dstHandle = importbuffer_virtualaddr(rgb.data(), static_cast<int>(rgb.size()));
    if (!srcHandle || !dstHandle) {
        error = std::string("AI RGA 导入失败：") + imStrError();
        if (srcHandle) releasebuffer_handle(srcHandle);
        if (dstHandle) releasebuffer_handle(dstHandle);
        return false;
    }
    rga_buffer_t src = wrapbuffer_handle(srcHandle, width, height, RK_FORMAT_YUYV_422,
                                         static_cast<int>(bytesPerLine / 2), height);
    rga_buffer_t dst = wrapbuffer_handle(dstHandle, width, height, RK_FORMAT_RGB_888,
                                         width, height);
    IM_STATUS status = imcheck(src, dst, {}, {});
    if (status == IM_STATUS_NOERROR)
        status = imcvtcolor(src, dst, RK_FORMAT_YUYV_422, RK_FORMAT_RGB_888);
    if (status != IM_STATUS_SUCCESS)
        error = std::string("AI RGA RGB 转换失败：") + imStrError(status);
    releasebuffer_handle(srcHandle);
    releasebuffer_handle(dstHandle);
    if (status != IM_STATUS_SUCCESS) return false;
    constexpr size_t rowBytes = width * 3;
    constexpr size_t padTop = (side - height) / 2;
    for (int row = 0; row < height; ++row)
        std::memcpy(modelInput.data() + (padTop + row) * rowBytes,
                    rgb.data() + static_cast<size_t>(row) * rowBytes, rowBytes);
    return true;
}
}

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
        lastAiError_.clear();
        latestImage_ = {};
        latestLog_ = {};
        takenImageLog_ = {};
        lastDiagnostics_ = {};
        logPath_ = logReady ? logWriter_.path() : std::string();
        if (requestedLog && !logReady) lastError_ = logError;
    }
    requested_ = true;
    finished_ = false;
    captureFailed_ = false;
    capturedFrames_ = 0;
    pendingPreviewOverwrites_ = 0;
    readyImageOverwrites_ = 0;
    aiPendingOverwrites_ = 0;
    aiReadyOverwrites_ = 0;
    aiEnabled_ = false;
    ++aiGeneration_;
    previewThread_ = std::thread(&CameraPipeline::runPreview, this);
    aiThread_ = std::thread(&CameraPipeline::runAi, this);
    thread_ = std::thread(&CameraPipeline::run, this);
}

void CameraPipeline::stop() {
    requested_ = false;
    wantStream_ = false;
    wantRecord_ = false;
    preview_ = false;
    aiEnabled_ = false;
    previewReady_.notify_one();
    aiReady_.notify_one();
    if (thread_.joinable()) thread_.join();
    if (previewThread_.joinable()) previewThread_.join();
    if (aiThread_.joinable()) aiThread_.join();
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
    pendingAi_ = AiJob{};
    recycledYuyv_.clear();
}

void CameraPipeline::setPreviewEnabled(bool enabled) {
    preview_ = enabled;
    if (!enabled) {
        setAiEnabled(false);
        std::lock_guard<std::mutex> lock(mutex_);
        latestImage_ = {};
        latestLog_ = {};
        takenImageLog_ = {};
        pendingPreview_ = RawPreview{};
    }
}

void CameraPipeline::setAiEnabled(bool enabled) {
    aiEnabled_ = enabled;
    ++aiGeneration_;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingAi_ = {};
        latestImage_ = {};
        latestLog_ = {};
        takenImageLog_ = {};
        if (enabled) lastAiError_.clear();
    }
    aiReady_.notify_one();
}

bool CameraPipeline::takeAiError(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (lastAiError_.empty()) return false;
    error = std::move(lastAiError_);
    lastAiError_.clear();
    return true;
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
    if (latestImage_.nv12.empty()) return false;
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
    log.mark(FramePoint::UiSubmitted); // Qt 已请求 OpenGL 绘制，不等同于屏幕实际刷新。
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
    if (const char* device = std::getenv("CAMERA_DEVICE"); device && *device)
        config.device = device;
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
        captureFailed_ = true;
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
        if (captured.data.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!recycledYuyv_.empty()) captured.data.swap(recycledYuyv_);
        }
        if (!camera.captureFrame(captured, error)) {
            captureFailed_ = true;
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

        RawPreview aiPreview;
        bool submitAi = false;
        // 显示仍需 NV12 副本；AI 输入使用原始 YUYV，不从该副本再转 RGB。
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
            submitAi = aiEnabled_;
            if (submitAi) aiPreview = std::move(copy);
            else if (preview_) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!pendingPreview_.nv12.empty()) ++pendingPreviewOverwrites_;
                pendingPreview_ = std::move(copy); // 仅保留最新待转换帧。
            }
            if (!submitAi) previewReady_.notify_one();
        }
        if (submitAi && aiEnabled_ && preview_) {
            AiJob job;
            job.yuyv = std::move(captured.data); // RGA 主转换完成后立即交给 AI。
            job.bytesPerLine = captured.bytes_per_line;
            job.generation = aiGeneration_.load();
            job.queuedAt = Clock::now();
            job.preview = std::move(aiPreview);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!pendingAi_.yuyv.empty()) ++aiPendingOverwrites_;
                pendingAi_ = std::move(job);
            }
            aiReady_.notify_one();
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
    aiReady_.notify_one();
    finished_ = true;
}

void CameraPipeline::runPreview() {
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
        frame.log.mark(FramePoint::PreviewStart);
        frame.log.mark(FramePoint::PreviewReady);
        if (frame.log.enabled) logStats_.addPreview(frame.log);
        if (preview_ && !aiEnabled_) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!latestImage_.nv12.empty()) ++readyImageOverwrites_;
            latestImage_ = {std::move(frame.nv12), frame.width, frame.height,
                            frame.horizontalStride, frame.verticalStride,
                            frame.capturePtsUs, frame.sequence, frame.timestampFlags};
            latestLog_ = std::move(frame.log);
        }
    }
}

void CameraPipeline::runAi() {
    std::unique_ptr<vision::YoloRknn> model;
    std::vector<uint8_t> rgb;
    std::vector<uint8_t> input;
    while (requested_) {
        AiJob job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            aiReady_.wait(lock, [this] { return !requested_ || !pendingAi_.yuyv.empty(); });
            if (!requested_) break;
            job = std::move(pendingAi_);
            pendingAi_ = {};
        }
        auto recycleInput = [this, &job] {
            std::lock_guard<std::mutex> lock(mutex_);
            if (recycledYuyv_.empty()) recycledYuyv_ = std::move(job.yuyv);
        };
        if (!aiEnabled_ || job.generation != aiGeneration_.load()) {
            recycleInput();
            continue;
        }
        try {
            if (!model) {
                const char* path = std::getenv("CAMERA_RKNN_MODEL");
                std::string modelPath;
                if (path && *path) modelPath = path;
                else if (const char* home = std::getenv("HOME"))
                    modelPath = (std::filesystem::path(home) / "lx_ws/model_train/yolov8.rknn").string();
                else throw std::runtime_error("请设置 CAMERA_RKNN_MODEL 指向 yolov8.rknn");
                model = std::make_unique<vision::YoloRknn>(modelPath);
            }
            const auto start = Clock::now();
            job.preview.log.mark(FramePoint::PreviewStart);
            std::string error;
            if (!yuyvToRknnRgb(job.yuyv, job.bytesPerLine, rgb, input, error))
                throw std::runtime_error(error);
            const auto converted = Clock::now();
            std::vector<vision::Detection> detections;
            vision::InferTiming inferTiming;
            model->infer(input.data(), input.size(), &detections, &inferTiming);
            const auto inferred = Clock::now();
            PreviewImage ready;
            ready.nv12 = std::move(job.preview.nv12);
            ready.width = job.preview.width;
            ready.height = job.preview.height;
            ready.horizontalStride = job.preview.horizontalStride;
            ready.verticalStride = job.preview.verticalStride;
            ready.capturePtsUs = job.preview.capturePtsUs;
            ready.sequence = job.preview.sequence;
            ready.timestampFlags = job.preview.timestampFlags;
            ready.aiDetected = true;
            ready.aiQueueWaitNs = elapsedNs(job.queuedAt, start);
            ready.aiPreprocessNs = elapsedNs(start, converted);
            ready.aiInferNs = elapsedNs(converted, inferred);
            ready.aiInputNs = inferTiming.inputNs;
            ready.aiRunNs = inferTiming.runNs;
            ready.aiOutputNs = inferTiming.outputNs;
            ready.aiDecodeNs = inferTiming.decodeNs;
            ready.aiReleaseNs = inferTiming.releaseNs;
            ready.aiTotalNs = elapsedNs(start, inferred);
            // 640x480 输入在 640x640 模型中上下各补 80 像素。
            for (const auto& detection : detections) {
                if (detection.label != 0) continue;
                PersonBox box{detection.score,
                    std::clamp(detection.x1, 0.f, 640.f),
                    std::clamp(detection.y1 - 80.f, 0.f, 480.f),
                    std::clamp(detection.x2, 0.f, 640.f),
                    std::clamp(detection.y2 - 80.f, 0.f, 480.f)};
                if (box.x2 > box.x1 && box.y2 > box.y1) ready.people.push_back(box);
            }
            job.preview.log.mark(FramePoint::PreviewReady);
            if (job.preview.log.enabled) logStats_.addPreview(job.preview.log);
            if (logEnabled_ && detailLogEnabled_)
                logWriter_.push("branch=ai seq=" + std::to_string(ready.sequence) +
                    " preprocess_ms=" + std::to_string(ready.aiPreprocessNs / 1e6) +
                    " infer_ms=" + std::to_string(ready.aiInferNs / 1e6) +
                    " input_ms=" + std::to_string(ready.aiInputNs / 1e6) +
                    " run_ms=" + std::to_string(ready.aiRunNs / 1e6) +
                    " output_ms=" + std::to_string(ready.aiOutputNs / 1e6) +
                    " decode_ms=" + std::to_string(ready.aiDecodeNs / 1e6) +
                    " release_ms=" + std::to_string(ready.aiReleaseNs / 1e6) +
                    " people=" + std::to_string(ready.people.size()));
            if (aiEnabled_ && preview_ && job.generation == aiGeneration_.load()) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!latestImage_.nv12.empty()) ++aiReadyOverwrites_;
                latestImage_ = std::move(ready);
                latestLog_ = std::move(job.preview.log);
            }
            recycleInput();
        } catch (const std::exception& exception) {
            if (job.generation == aiGeneration_.load()) {
                aiEnabled_ = false;
                ++aiGeneration_;
                std::lock_guard<std::mutex> lock(mutex_);
                lastAiError_ = std::string("人员检测已关闭：") + exception.what();
                pendingAi_ = {};
            }
            model.reset();
            recycleInput();
        }
    }
}
