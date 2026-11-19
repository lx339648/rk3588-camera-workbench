#include "qt_preview_log.hpp"

#include <QCoreApplication>
#include <QDateTime>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <utility>

void QtPreviewLog::Stage::add(uint64_t ns) {
    ++samples;
    total_ns += ns;
    max_ns = std::max(max_ns, ns);
}

QtPreviewLog::QtPreviewLog() {
    const char* flag = std::getenv("CAMERA_QT_PREVIEW_LOG");
    if (flag && std::strcmp(flag, "0") == 0) return;

    const char* state = std::getenv("XDG_STATE_HOME");
    const char* home = std::getenv("HOME");
    std::filesystem::path base = std::filesystem::current_path();
    if (home && *home) base = std::filesystem::path(home) / ".local/state";
    if (state && *state) base = state;
    const auto directory = base / "CameraWorkbench/logs";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return;

    const QString name = QString("qt_preview_%1_%2.log")
        .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz"))
        .arg(QCoreApplication::applicationPid());
    const auto filePath = directory / name.toStdString();
    file_.open(filePath, std::ios::out | std::ios::app);
    if (!file_) return;
    try {
        writer_ = std::thread(&QtPreviewLog::writeLoop, this);
    } catch (...) {
        file_.close();
        return;
    }
    path_ = QString::fromStdString(filePath.string());
    enabled_ = true;
}

QtPreviewLog::~QtPreviewLog() {
    if (!enabled_) return;
    flushSummary();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_one();
    writer_.join();
    file_.close();
}

void QtPreviewLog::beginTick(bool live, uint64_t pending_overwrites,
                              uint64_t ready_overwrites, uint64_t ai_pending_overwrites,
                              uint64_t ai_ready_overwrites) {
    if (!enabled_) return;
    if (!live) {
        if (ticks_ > 0) flushSummary();
        summaryClock_.invalidate();
        tickClock_.invalidate();
        callbackClock_.invalidate();
        tickActive_ = false;
        dropBaselineValid_ = false;
        lastSequenceValid_ = false;
        return;
    }

    if (!summaryClock_.isValid()) summaryClock_.start();
    if (tickClock_.isValid())
        tickInterval_.add(static_cast<uint64_t>(tickClock_.nsecsElapsed()));
    tickClock_.restart();
    callbackClock_.start();
    tickActive_ = true;
    ++ticks_;

    if (dropBaselineValid_) {
        // 计数器在重新启动摄像头时清零；新的基线从本次开始。
        if (pending_overwrites >= previousPendingOverwrites_)
            pendingOverwrites_ += pending_overwrites - previousPendingOverwrites_;
        if (ready_overwrites >= previousReadyOverwrites_)
            readyOverwrites_ += ready_overwrites - previousReadyOverwrites_;
        if (ai_pending_overwrites >= previousAiPendingOverwrites_)
            aiPendingOverwrites_ += ai_pending_overwrites - previousAiPendingOverwrites_;
        if (ai_ready_overwrites >= previousAiReadyOverwrites_)
            aiReadyOverwrites_ += ai_ready_overwrites - previousAiReadyOverwrites_;
    }
    previousPendingOverwrites_ = pending_overwrites;
    previousReadyOverwrites_ = ready_overwrites;
    previousAiPendingOverwrites_ = ai_pending_overwrites;
    previousAiReadyOverwrites_ = ai_ready_overwrites;
    dropBaselineValid_ = true;
}

void QtPreviewLog::endTick() {
    if (!enabled_ || !tickActive_) return;
    callbackTime_.add(static_cast<uint64_t>(callbackClock_.nsecsElapsed()));
    tickActive_ = false;
    if (summaryClock_.elapsed() >= 1000) flushSummary();
}

void QtPreviewLog::noteGlSubmitted(uint32_t sequence, int64_t capture_pts_us,
                                   uint64_t enqueue_ns) {
    if (!enabled_) return;
    ++frames_;
    if (lastSequenceValid_) {
        const uint32_t advance = sequence - lastSequence_; // 允许 uint32_t 正常回绕
        if (advance > 1 && advance < 10000) skippedSequences_ += advance - 1;
        else if (advance >= 10000) ++sequenceResets_;
    }
    lastSequenceValid_ = true;
    lastSequence_ = sequence;
    lastPtsUs_ = capture_pts_us;
    glEnqueue_.add(enqueue_ns);
}

void QtPreviewLog::noteGlPaint(uint64_t paint_ns) {
    if (enabled_) glPaint_.add(paint_ns);
}

void QtPreviewLog::noteGlCompose(uint64_t compose_ns) {
    if (enabled_) glCompose_.add(compose_ns);
}

void QtPreviewLog::noteAiFrame(uint64_t queue_wait_ns, uint64_t preprocess_ns,
                               uint64_t infer_ns, uint64_t total_ns, size_t people,
                               uint64_t input_ns, uint64_t run_ns, uint64_t output_ns,
                               uint64_t decode_ns, uint64_t release_ns) {
    if (!enabled_) return;
    ++aiFrames_;
    aiPeople_ += people;
    aiQueueWait_.add(queue_wait_ns);
    aiPreprocess_.add(preprocess_ns);
    aiInfer_.add(infer_ns);
    aiTotal_.add(total_ns);
    aiInput_.add(input_ns);
    aiRun_.add(run_ns);
    aiOutput_.add(output_ns);
    aiDecode_.add(decode_ns);
    aiRelease_.add(release_ns);
}

void QtPreviewLog::flushSummary() {
    if (ticks_ == 0) return;
    std::ostringstream line;
    line << std::fixed << std::setprecision(2)
         << "utc=" << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString()
         << " ticks=" << ticks_
         << " frames=" << frames_
         << " empty_ticks=" << (ticks_ - frames_)
         << " seq_skipped=" << skippedSequences_
         << " seq_resets=" << sequenceResets_
         << " preview_replace_pending=" << pendingOverwrites_
         << " preview_replace_ready=" << readyOverwrites_
         << " ai_frames=" << aiFrames_
         << " ai_people=" << aiPeople_
         << " ai_replace_pending=" << aiPendingOverwrites_
         << " ai_replace_ready=" << aiReadyOverwrites_
         << " last_seq=" << lastSequence_
         << " last_pts_us=" << lastPtsUs_;
    auto append = [&line](const char* name, const Stage& stage) {
        line << ' ' << name << "[avg="
             << (stage.samples ? static_cast<double>(stage.total_ns) / stage.samples / 1000000.0 : 0.0)
             << "ms,max=" << static_cast<double>(stage.max_ns) / 1000000.0
             << "ms,n=" << stage.samples << ']';
    };
    append("tick_interval", tickInterval_);
    append("ui_callback", callbackTime_);
    append("gl_enqueue", glEnqueue_);
    append("gl_paint_cpu", glPaint_);
    append("gl_compose_swap", glCompose_);
    append("ai_queue_wait", aiQueueWait_);
    append("ai_preprocess", aiPreprocess_);
    append("ai_infer_post", aiInfer_);
    append("rknn_inputs_set", aiInput_);
    append("rknn_run", aiRun_);
    append("rknn_outputs_get", aiOutput_);
    append("yolo_decode_nms", aiDecode_);
    append("rknn_outputs_release", aiRelease_);
    append("ai_total", aiTotal_);
    enqueue(line.str());
    ticks_ = 0;
    frames_ = 0;
    skippedSequences_ = 0;
    sequenceResets_ = 0;
    pendingOverwrites_ = 0;
    readyOverwrites_ = 0;
    aiPendingOverwrites_ = 0;
    aiReadyOverwrites_ = 0;
    aiFrames_ = 0;
    aiPeople_ = 0;
    tickInterval_ = {};
    callbackTime_ = {};
    glEnqueue_ = {};
    glPaint_ = {};
    glCompose_ = {};
    aiPreprocess_ = {};
    aiQueueWait_ = {};
    aiInfer_ = {};
    aiTotal_ = {};
    aiInput_ = {};
    aiRun_ = {};
    aiOutput_ = {};
    aiDecode_ = {};
    aiRelease_ = {};
    summaryClock_.restart();
}

void QtPreviewLog::enqueue(std::string line) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (lines_.size() >= 128) {
            lines_.pop_front();
            ++droppedLines_;
        }
        lines_.push_back(std::move(line));
    }
    ready_.notify_one();
}

void QtPreviewLog::writeLoop() {
    for (;;) {
        std::string line;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !lines_.empty(); });
            if (lines_.empty() && stopping_) break;
            line = std::move(lines_.front());
            lines_.pop_front();
        }
        file_ << line << '\n';
        file_.flush(); // 后台线程写盘，运行中也能实时查看日志。
    }
    if (droppedLines_) file_ << "dropped_log_lines=" << droppedLines_ << '\n';
    file_.flush();
}
