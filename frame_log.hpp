#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <sys/time.h>
#include <thread>

// 耗时使用 CLOCK_MONOTONIC；日期时间使用 CLOCK_REALTIME，仅供定位日志。
// -1 表示该环节尚未发生。
// WaitStart 到 Dequeued 包含等待下一张图的时间，不能算作图像处理耗时。
enum class FramePoint : std::size_t {
    WaitStart,
    Dequeued,
    Copied,
    Requeued,
    DecodeStart,
    Decoded,
    ConvertStart,
    Converted,
    EncodeStart,
    Encoded,
    RecordStart,
    Recorded,
    RtspStart,
    RtspWritten,
    PreviewStart,
    PreviewReady,
    UiSubmitted,
    UiPainted,
    Count
};

constexpr std::size_t kFramePointCount = static_cast<std::size_t>(FramePoint::Count);

struct FrameTimePoint {
    int64_t monotonic_ns = -1; // 计算环节耗时、与 V4L2 单调 PTS 比较
    int64_t realtime_us = -1;  // 机器墙上时间，便于按日期查日志
};

// 单张采集帧的诊断记录。后续可以随 CapturedFrame/NV12 帧/EncodedFrame 传递。
// 分叉到预览与推流时各复制一份，避免两个线程同时修改同一个对象。
struct FrameLog {
    bool enabled = false;                   // 关闭日志时跳过取时钟与统计
    uint32_t sequence = 0;                 // V4L2 帧序号，用于关联不同环节
    int64_t capture_pts_us = -1;           // 驱动给出的采集时间戳，单位微秒
    uint32_t capture_timestamp_flags = 0; // V4L2 时间戳时钟和取样点标志
    std::array<FrameTimePoint, kFramePointCount> points{};

    void setCaptureInfo(uint32_t seq, const timeval& timestamp, uint32_t flags);
    void mark(FramePoint point);                    // 在当前时刻打点
    void markAt(FramePoint point, int64_t monotonic_ns,
                int64_t realtime_us = -1);         // 使用已取得的时间打点
    FrameTimePoint time(FramePoint point) const;    // 未打点时两个值均为 -1
    std::optional<double> elapsedMs(FramePoint begin, FramePoint end) const;
    std::optional<double> captureToMs(FramePoint end) const;
    std::string detailLine() const;                 // 按需输出单帧各环节原始时间
};

// 和 V4L2 单调时间戳使用同一时钟；失败时返回 -1。
int64_t frameMonotonicNs();
// 机器当前 UTC 时间，单位微秒；不能用于计算处理耗时。
int64_t frameRealtimeUs();

// 每个完整分支使用一个统计器。add() 只累计数字，不做终端或 Qt 输出。
// takeSummaryIfDue() 每隔 interval_ms 至多生成一条汇总文本。
class FrameLogStats {
public:
    static constexpr std::size_t kStageCount = 11;
    static constexpr std::size_t kAgeCount = 3;
    explicit FrameLogStats(int64_t interval_ms = 1000);
    void add(const FrameLog& frame);
    void addPreview(const FrameLog& frame);
    void addUi(const FrameLog& frame);
    std::string takeSummaryIfDue();
    std::string takeSummary(); // 手动取出并清空当前统计窗口

private:
    struct DurationStat {
        uint64_t count = 0;
        double total_ms = 0;
        double max_ms = 0;
        void add(double value_ms);
    };

    static constexpr std::size_t kDurationCount = kStageCount + kAgeCount;
    std::string summaryLocked(int64_t now_ns);

    std::mutex mutex_;
    std::array<DurationStat, kDurationCount> durations_{};
    uint64_t frames_ = 0;
    int64_t interval_ns_ = 1000000000;
    int64_t window_start_ns_ = -1;
};

// 文件写入由独立线程完成，采集线程只把每秒一条的汇总放入有界队列。
class FrameLogWriter {
public:
    ~FrameLogWriter();
    bool start(const std::string& directory, std::string& error);
    void push(std::string line);
    void stop();
    const std::string& path() const { return path_; }
    bool healthy() const { return healthy_.load(); }

private:
    void run();
    static constexpr std::size_t kQueueLimit = 128;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::string> queue_;
    std::thread thread_;
    std::ofstream file_;
    std::string path_;
    bool stopping_ = false;
    uint64_t dropped_ = 0;
    std::atomic<bool> healthy_{true};
};
