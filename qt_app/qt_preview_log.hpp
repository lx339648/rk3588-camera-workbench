#pragma once

#include <QElapsedTimer>
#include <QString>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

// 只负责 Qt 预览的显示与计时；不接触 V4L2、MPP 或主链路 FrameLog。
// present() 只能在 Qt 主线程调用，文件写入由自己的后台线程完成。
class QtPreviewLog {
public:
    QtPreviewLog();
    ~QtPreviewLog();
    QtPreviewLog(const QtPreviewLog&) = delete;
    QtPreviewLog& operator=(const QtPreviewLog&) = delete;

    // OpenGL 更新请求返回时记录；绘制和窗口交换发生在此之后。
    void noteGlSubmitted(uint32_t sequence, int64_t capture_pts_us, uint64_t enqueue_ns);
    void noteGlPaint(uint64_t paint_ns);
    void noteGlCompose(uint64_t compose_ns);
    void noteAiFrame(uint64_t queue_wait_ns, uint64_t preprocess_ns,
                     uint64_t infer_ns, uint64_t total_ns, size_t people,
                     uint64_t input_ns, uint64_t run_ns, uint64_t output_ns,
                     uint64_t decode_ns, uint64_t release_ns);
    // 一次 Qt 定时器回调的入口/出口；覆盖数由预览分支累计后传入。
    void beginTick(bool live, uint64_t pending_overwrites,
                   uint64_t ready_overwrites, uint64_t ai_pending_overwrites,
                   uint64_t ai_ready_overwrites);
    void endTick();
    bool enabled() const { return enabled_; }
    const QString& path() const { return path_; }

private:
    struct Stage {
        uint64_t samples = 0;
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
    QElapsedTimer tickClock_;
    QElapsedTimer callbackClock_;
    bool tickActive_ = false;
    bool dropBaselineValid_ = false;
    bool lastSequenceValid_ = false;
    uint64_t previousPendingOverwrites_ = 0;
    uint64_t previousReadyOverwrites_ = 0;
    uint64_t previousAiPendingOverwrites_ = 0;
    uint64_t previousAiReadyOverwrites_ = 0;
    uint64_t pendingOverwrites_ = 0;
    uint64_t readyOverwrites_ = 0;
    uint64_t aiPendingOverwrites_ = 0;
    uint64_t aiReadyOverwrites_ = 0;
    uint64_t aiFrames_ = 0;
    uint64_t aiPeople_ = 0;
    uint64_t skippedSequences_ = 0;
    uint64_t sequenceResets_ = 0;
    uint64_t ticks_ = 0;
    uint64_t frames_ = 0;
    uint32_t lastSequence_ = 0;
    int64_t lastPtsUs_ = -1;
    Stage glEnqueue_;
    Stage glPaint_;
    Stage glCompose_;
    Stage aiPreprocess_;
    Stage aiQueueWait_;
    Stage aiInfer_;
    Stage aiTotal_;
    Stage aiInput_;
    Stage aiRun_;
    Stage aiOutput_;
    Stage aiDecode_;
    Stage aiRelease_;
    Stage tickInterval_;
    Stage callbackTime_;

    std::ofstream file_;
    std::thread writer_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::string> lines_;
    bool stopping_ = false;
    uint64_t droppedLines_ = 0;
};
