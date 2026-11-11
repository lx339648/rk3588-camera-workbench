#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QString>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

class QLabel;

// 只负责 Qt 预览的显示与计时；不接触 V4L2、MPP 或主链路 FrameLog。
// present() 只能在 Qt 主线程调用，文件写入由自己的后台线程完成。
class QtPreviewLog {
public:
    QtPreviewLog();
    ~QtPreviewLog();
    QtPreviewLog(const QtPreviewLog&) = delete;
    QtPreviewLog& operator=(const QtPreviewLog&) = delete;

    void present(QLabel* label, QImage&& image, uint32_t sequence,
                 int64_t capture_pts_us);
    bool enabled() const { return enabled_; }
    const QString& path() const { return path_; }

private:
    struct Stage {
        uint64_t total_ns = 0;
        uint64_t max_ns = 0;
        void add(uint64_t ns);
    };

    void flushSummary();
    void enqueue(std::string line);
    void writeLoop();

    bool enabled_ = false;
    QString path_;
    QElapsedTimer summaryClock_;
    uint64_t frames_ = 0;
    uint32_t lastSequence_ = 0;
    int64_t lastPtsUs_ = -1;
    Stage fromImage_;
    Stage scale_;
    Stage setPixmap_;

    std::ofstream file_;
    std::thread writer_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::string> lines_;
    bool stopping_ = false;
    uint64_t droppedLines_ = 0;
};
