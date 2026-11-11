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

    if (!summaryClock_.isValid()) summaryClock_.start();
    ++frames_;
    lastSequence_ = sequence;
    lastPtsUs_ = capture_pts_us;
    fromImage_.add(static_cast<uint64_t>(convertedNs));
    scale_.add(static_cast<uint64_t>(scaledNs - convertedNs));
    setPixmap_.add(static_cast<uint64_t>(submittedNs - scaledNs));
    if (summaryClock_.elapsed() >= 1000) flushSummary();
}

void QtPreviewLog::flushSummary() {
    if (frames_ == 0) return;
    std::ostringstream line;
    line << std::fixed << std::setprecision(2)
         << "utc=" << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString()
         << " frames=" << frames_
         << " last_seq=" << lastSequence_
         << " last_pts_us=" << lastPtsUs_;
    auto append = [&line, this](const char* name, const Stage& stage) {
        line << ' ' << name << "[avg="
             << static_cast<double>(stage.total_ns) / frames_ / 1000000.0
             << "ms,max=" << static_cast<double>(stage.max_ns) / 1000000.0
             << "ms]";
    };
    append("from_image", fromImage_);
    append("scale", scale_);
    append("set_pixmap", setPixmap_);
    enqueue(line.str());
    frames_ = 0;
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
