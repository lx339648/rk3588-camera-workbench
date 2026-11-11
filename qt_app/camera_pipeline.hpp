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
    bool takeLatestImage(QImage& image, FrameLog& log);
    void noteUiSubmitted(FrameLog log);
    bool takeLogSummary(std::string& summary);
    std::string logPath();
    bool logEnabled() const { return logEnabled_.load(); }
    bool takeError(std::string& error);
    bool takeRecordError(std::string& error);
    bool streaming() const { return streaming_.load(); }
    bool recording() const { return recording_.load(); }
    bool alive() const { return alive_.load(); }
    bool finished() const { return finished_.load(); }
    uint64_t capturedFrames() const { return capturedFrames_.load(); }

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
        FrameLog log;
    };

    std::thread thread_;
    std::thread previewThread_;
    std::atomic<bool> requested_{false};
    std::atomic<bool> alive_{false};
    std::atomic<bool> finished_{false};
    std::atomic<uint64_t> capturedFrames_{0};
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
    QImage latestImage_;
    FrameLog latestLog_;
    std::string lastLogSummary_;
    std::string logPath_;
    std::string streamUrl_;
    std::string recordPath_;
    std::string lastError_;
    std::string lastRecordError_;
};
