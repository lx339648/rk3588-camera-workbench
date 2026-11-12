#pragma once

#include <QImage>
#include "frame_log.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 界面只接触画面与采集元数据，不需要持有内部逐帧日志。
struct PreviewImage {
    QImage image;
    int64_t capturePtsUs = -1; // V4L2 时间戳；不一定是日历时间
    uint32_t sequence = 0;
    uint32_t timestampFlags = 0;
};

// 预览分支的累计覆盖次数；只提供数值，不依赖 Qt 日志实现。
struct PreviewDropStats {
    uint64_t pendingOverwrites = 0; // 尚未转换的 NV12 被新帧替换
    uint64_t readyOverwrites = 0;   // 尚未被界面取走的 QImage 被新帧替换
};

struct PipelineDiagnostics {
    double convertMs = -1;
    double encodeMs = -1;
    double rtspWriteMs = -1;
    double captureToUiMs = -1;
    std::string detail; // 文件日志同一统计窗口的完整文字，仅用于提示
};

// 工作线程独占摄像头、MPP 和 RTSP；Qt 主线程只读取已复制的最新画面。
class CameraPipeline {
public:
    CameraPipeline() = default;
    ~CameraPipeline();
    CameraPipeline(const CameraPipeline&) = delete;
    CameraPipeline& operator=(const CameraPipeline&) = delete;

    void start();
    void stop();
    void setPreviewEnabled(bool enabled);
    void setStreamingEnabled(bool enabled, std::string url);
    void setRecordingEnabled(bool enabled, std::string path);
    bool takeLatestImage(PreviewImage& frame);
    void noteUiSubmitted();
    bool takeDiagnostics(PipelineDiagnostics& diagnostics);
    std::string logPath();
    bool logEnabled() const { return logEnabled_.load(); }
    bool takeError(std::string& error);
    bool takeRecordError(std::string& error);
    bool streaming() const { return streaming_.load(); }
    bool recording() const { return recording_.load(); }
    bool alive() const { return alive_.load(); }
    bool finished() const { return finished_.load(); }
    uint64_t capturedFrames() const { return capturedFrames_.load(); }
    PreviewDropStats previewDropStats() const {
        return {pendingPreviewOverwrites_.load(), readyImageOverwrites_.load()};
    }

private:
    void run();
    void runPreview();
    void reportError(std::string message);
    void reportRecordError(std::string message);

    struct RawPreview {
        std::vector<uint8_t> nv12;
        int width = 0;
        int height = 0;
        int horizontalStride = 0;
        int verticalStride = 0;
        int64_t capturePtsUs = -1;
        uint32_t sequence = 0;
        uint32_t timestampFlags = 0;
        FrameLog log;
    };

    std::thread thread_;
    std::thread previewThread_;
    std::atomic<bool> requested_{false};
    std::atomic<bool> alive_{false};
    std::atomic<bool> finished_{false};
    std::atomic<uint64_t> capturedFrames_{0};
    std::atomic<uint64_t> pendingPreviewOverwrites_{0};
    std::atomic<uint64_t> readyImageOverwrites_{0};
    std::atomic<bool> preview_{false};
    std::atomic<bool> wantStream_{false};
    std::atomic<bool> streaming_{false};
    std::atomic<bool> wantRecord_{false};
    std::atomic<bool> recording_{false};
    std::atomic<bool> logEnabled_{false};
    std::atomic<bool> detailLogEnabled_{false};
    FrameLogStats logStats_;
    FrameLogWriter logWriter_;
    std::mutex mutex_;
    std::condition_variable previewReady_;
    RawPreview pendingPreview_;
    PreviewImage latestImage_;
    FrameLog latestLog_;
    FrameLog takenImageLog_;
    PipelineDiagnostics lastDiagnostics_;
    std::string logPath_;
    std::string streamUrl_;
    std::string recordPath_;
    std::string lastError_;
    std::string lastRecordError_;
};
