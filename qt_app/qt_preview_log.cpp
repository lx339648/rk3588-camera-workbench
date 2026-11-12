#include "qt_preview_log.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QLabel>
#include <QPixmap>

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
                              uint64_t ready_overwrites) {
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
    }
    previousPendingOverwrites_ = pending_overwrites;
    previousReadyOverwrites_ = ready_overwrites;
    dropBaselineValid_ = true;
}

void QtPreviewLog::endTick() {
    if (!enabled_ || !tickActive_) return;
    callbackTime_.add(static_cast<uint64_t>(callbackClock_.nsecsElapsed()));
    tickActive_ = false;
    if (summaryClock_.elapsed() >= 1000) flushSummary();
}

void QtPreviewLog::present(QLabel* label, QImage&& image,
                           uint32_t sequence, int64_t capture_pts_us) {
    if (!label) return;
    if (!enabled_) {
        const QPixmap pixmap = QPixmap::fromImage(std::move(image));
        label->setPixmap(pixmap.scaled(label->size(),
            Qt::KeepAspectRatio, Qt::FastTransformation));
        return;
    }
    QElapsedTimer timer;
    timer.start();
    QPixmap pixmap = QPixmap::fromImage(std::move(image));
    const qint64 convertedNs = timer.nsecsElapsed();
    QPixmap scaled = pixmap.scaled(label->size(),
        Qt::KeepAspectRatio, Qt::FastTransformation);
    const qint64 scaledNs = timer.nsecsElapsed();
    label->setPixmap(scaled);
    const qint64 submittedNs = timer.nsecsElapsed();

    ++frames_;
    if (lastSequenceValid_) {
        const uint32_t advance = sequence - lastSequence_; // 允许 uint32_t 正常回绕
        if (advance > 1 && advance < 10000) skippedSequences_ += advance - 1;
        else if (advance >= 10000) ++sequenceResets_;
    }
    lastSequenceValid_ = true;
    lastSequence_ = sequence;
    lastPtsUs_ = capture_pts_us;
    fromImage_.add(static_cast<uint64_t>(convertedNs));
    scale_.add(static_cast<uint64_t>(scaledNs - convertedNs));
    setPixmap_.add(static_cast<uint64_t>(submittedNs - scaledNs));
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
    append("from_image", fromImage_);
    append("scale", scale_);
    append("set_pixmap", setPixmap_);
    enqueue(line.str());
    ticks_ = 0;
    frames_ = 0;
    skippedSequences_ = 0;
    sequenceResets_ = 0;
    pendingOverwrites_ = 0;
    readyOverwrites_ = 0;
    tickInterval_ = {};
    callbackTime_ = {};
    fromImage_ = {};
    scale_ = {};
    setPixmap_ = {};
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
