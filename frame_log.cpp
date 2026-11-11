#include "frame_log.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <linux/videodev2.h>
#include <sstream>
#include <system_error>
#include <time.h>

namespace {
constexpr std::array<const char*, kFramePointCount> kPointNames = {
    "wait_start", "dequeued", "copied", "requeued", "decode_start",
    "decoded", "convert_start", "converted", "encode_start", "encoded",
    "record_start", "recorded", "rtsp_start", "rtsp_written",
    "preview_start", "preview_ready", "ui_submitted", "ui_painted"
};

struct DurationDefinition {
    const char* name;
    FramePoint begin;
    FramePoint end;
};

constexpr std::array<DurationDefinition, FrameLogStats::kStageCount> kStageDurations = {{
    {"wait", FramePoint::WaitStart, FramePoint::Dequeued},
    {"copy", FramePoint::Dequeued, FramePoint::Copied},
    {"requeue", FramePoint::Copied, FramePoint::Requeued},
    {"decode", FramePoint::DecodeStart, FramePoint::Decoded},
    {"convert", FramePoint::ConvertStart, FramePoint::Converted},
    {"encode", FramePoint::EncodeStart, FramePoint::Encoded},
    {"record", FramePoint::RecordStart, FramePoint::Recorded},
    {"rtsp_write", FramePoint::RtspStart, FramePoint::RtspWritten},
    {"preview_convert", FramePoint::PreviewStart, FramePoint::PreviewReady},
    {"preview_to_ui", FramePoint::PreviewReady, FramePoint::UiSubmitted},
    {"ui_queue", FramePoint::UiSubmitted, FramePoint::UiPainted}
}};

constexpr std::array<const char*, 3> kAgeNames = {
    "capture_to_encoded", "capture_to_rtsp", "capture_to_ui_submitted"
};
} // namespace

int64_t frameMonotonicNs() {
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return static_cast<int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
}

int64_t frameRealtimeUs() {
    timespec now{};
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) return -1;
    return static_cast<int64_t>(now.tv_sec) * 1000000LL + now.tv_nsec / 1000;
}

void FrameLog::setCaptureInfo(uint32_t seq, const timeval& timestamp, uint32_t flags) {
    sequence = seq;
    capture_pts_us = static_cast<int64_t>(timestamp.tv_sec) * 1000000LL +
                     timestamp.tv_usec;
    capture_timestamp_flags = flags;
}

void FrameLog::mark(FramePoint point) {
    if (!enabled) return;
    const int64_t monotonic_ns = frameMonotonicNs();
    markAt(point, monotonic_ns, frameRealtimeUs());
}

void FrameLog::markAt(FramePoint point, int64_t monotonic_ns, int64_t realtime_us) {
    const auto index = static_cast<std::size_t>(point);
    if (index < points.size() && monotonic_ns >= 0)
        points[index] = {monotonic_ns, realtime_us};
}

FrameTimePoint FrameLog::time(FramePoint point) const {
    const auto index = static_cast<std::size_t>(point);
    return index < points.size() ? points[index] : FrameTimePoint{};
}

std::optional<double> FrameLog::elapsedMs(FramePoint begin, FramePoint end) const {
    const int64_t start_ns = time(begin).monotonic_ns;
    const int64_t end_ns = time(end).monotonic_ns;
    if (start_ns < 0 || end_ns < start_ns) return std::nullopt;
    return static_cast<double>(end_ns - start_ns) / 1000000.0;
}

std::optional<double> FrameLog::captureToMs(FramePoint end) const {
    if (capture_pts_us < 0 ||
        (capture_timestamp_flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) !=
            V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) return std::nullopt;
    const int64_t end_ns = time(end).monotonic_ns;
    const int64_t capture_ns = capture_pts_us * 1000;
    if (end_ns < capture_ns) return std::nullopt;
    return static_cast<double>(end_ns - capture_ns) / 1000000.0;
}

std::string FrameLog::detailLine() const {
    std::ostringstream out;
    out << "seq=" << sequence << " capture_pts_us=" << capture_pts_us
        << " timestamp_flags=0x" << std::hex << capture_timestamp_flags << std::dec;
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (points[i].monotonic_ns < 0) continue;
        out << ' ' << kPointNames[i] << "_mono_ns=" << points[i].monotonic_ns;
        if (points[i].realtime_us >= 0)
            out << ' ' << kPointNames[i] << "_utc_us=" << points[i].realtime_us;
    }
    return out.str();
}

FrameLogStats::FrameLogStats(int64_t interval_ms)
    : interval_ns_(std::max<int64_t>(1, interval_ms) * 1000000LL),
      window_start_ns_(frameMonotonicNs()) {}

void FrameLogStats::DurationStat::add(double value_ms) {
    ++count;
    total_ms += value_ms;
    max_ms = std::max(max_ms, value_ms);
}

void FrameLogStats::add(const FrameLog& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++frames_;
    for (std::size_t i = 0; i < 8; ++i) {
        const auto& stage = kStageDurations[i];
        if (auto ms = frame.elapsedMs(stage.begin, stage.end)) durations_[i].add(*ms);
    }
    constexpr std::array<FramePoint, 2> kAgeEnds = {
        FramePoint::Encoded, FramePoint::RtspWritten
    };
    for (std::size_t i = 0; i < kAgeEnds.size(); ++i) {
        if (auto ms = frame.captureToMs(kAgeEnds[i]))
            durations_[kStageDurations.size() + i].add(*ms);
    }
}

void FrameLogStats::addPreview(const FrameLog& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto& stage = kStageDurations[8];
    if (auto ms = frame.elapsedMs(stage.begin, stage.end)) durations_[8].add(*ms);
}

void FrameLogStats::addUi(const FrameLog& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto& stage = kStageDurations[9];
    if (auto ms = frame.elapsedMs(stage.begin, stage.end)) durations_[9].add(*ms);
    if (auto ms = frame.captureToMs(FramePoint::UiSubmitted))
        durations_[kStageDurations.size() + 2].add(*ms);
}

std::string FrameLogStats::summaryLocked(int64_t now_ns) {
    if (frames_ == 0) {
        window_start_ns_ = now_ns;
        return {};
    }
    std::ostringstream out;
    const double elapsed_seconds = window_start_ns_ >= 0 && now_ns > window_start_ns_
        ? static_cast<double>(now_ns - window_start_ns_) / 1000000000.0 : 0.0;
    out << std::fixed << std::setprecision(2) << "frames=" << frames_;
    // 手动立即取出统计时，窗口可能只有几微秒；这种 FPS 没有参考价值。
    if (elapsed_seconds >= 0.1) out << " fps=" << frames_ / elapsed_seconds;
    for (std::size_t i = 0; i < durations_.size(); ++i) {
        const auto& stat = durations_[i];
        if (stat.count == 0) continue;
        const char* name = i < kStageDurations.size()
            ? kStageDurations[i].name : kAgeNames[i - kStageDurations.size()];
        out << ' ' << name << "[n=" << stat.count
            << ",avg=" << stat.total_ms / stat.count
            << "ms,max=" << stat.max_ms << "ms]";
    }
    durations_ = {};
    frames_ = 0;
    window_start_ns_ = now_ns;
    return out.str();
}

std::string FrameLogStats::takeSummaryIfDue() {
    const int64_t now_ns = frameMonotonicNs();
    if (now_ns < 0) return {};
    std::lock_guard<std::mutex> lock(mutex_);
    if (window_start_ns_ < 0) window_start_ns_ = now_ns;
    if (now_ns - window_start_ns_ < interval_ns_) return {};
    return summaryLocked(now_ns);
}

std::string FrameLogStats::takeSummary() {
    const int64_t now_ns = frameMonotonicNs();
    std::lock_guard<std::mutex> lock(mutex_);
    return summaryLocked(now_ns);
}

FrameLogWriter::~FrameLogWriter() { stop(); }

bool FrameLogWriter::start(const std::string& directory, std::string& error) {
    stop();
    error.clear();
    healthy_ = false;
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        error = "创建日志目录失败：" + ec.message();
        return false;
    }
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_r(&seconds, &local);
    char name[64]{};
    std::strftime(name, sizeof(name), "camera_%Y%m%d_%H%M%S.log", &local);
    path_ = (std::filesystem::path(directory) / name).string();
    file_.open(path_, std::ios::out | std::ios::app);
    if (!file_) {
        error = "打开日志文件失败：" + path_;
        path_.clear();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = false;
        dropped_ = 0;
        queue_.clear();
    }
    try {
        thread_ = std::thread(&FrameLogWriter::run, this);
    } catch (const std::system_error& ex) {
        error = "创建日志写入线程失败：" + std::string(ex.what());
        file_.close();
        path_.clear();
        return false;
    }
    healthy_ = true;
    return true;
}

void FrameLogWriter::push(std::string line) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!thread_.joinable() || stopping_) return;
    if (queue_.size() >= kQueueLimit) {
        ++dropped_;
        return;
    }
    queue_.push_back(std::move(line));
    ready_.notify_one();
}

void FrameLogWriter::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_one();
    if (thread_.joinable()) thread_.join();
    if (file_.is_open()) file_.close();
}

void FrameLogWriter::run() {
    for (;;) {
        std::string line;
        uint64_t dropped = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) break;
            line = std::move(queue_.front());
            queue_.pop_front();
            dropped = dropped_;
            dropped_ = 0;
        }
        file_ << "utc_us=" << frameRealtimeUs() << ' ' << line;
        if (dropped) file_ << " dropped_log_lines=" << dropped;
        file_ << '\n';
        file_.flush();
        if (!file_) healthy_ = false;
    }
}
