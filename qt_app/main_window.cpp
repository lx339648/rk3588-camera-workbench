#include "main_window.hpp"

#include <QButtonGroup>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHostAddress>
#include <QHBoxLayout>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProcess>
#include <QRegularExpression>
#include <QPushButton>
#include <QPixmap>
#include <QSlider>
#include <QSignalBlocker>
#include <QSettings>
#include <QStackedWidget>
#include <QTcpSocket>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoWidget>
#include <QWidget>

#include <algorithm>
#include <limits>

namespace {
QString playbackTime(qint64 ms) {
    const qint64 seconds = std::max<qint64>(0, ms) / 1000;
    return QString("%1:%2:%3")
        .arg(seconds / 3600, 2, 10, QLatin1Char('0'))
        .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("相机工作台");
    resize(1180, 720);
    setMinimumSize(900, 560);

    auto* rootWidget = new QWidget(this);
    rootWidget->setObjectName("windowRoot");
    auto* rootLayout = new QHBoxLayout(rootWidget);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);
    rootLayout->addWidget(createSidebar());

    auto* mainContent = new QWidget(rootWidget);
    mainContent->setObjectName("mainContent");
    auto* mainLayout = new QVBoxLayout(mainContent);
    mainLayout->setContentsMargins(28, 26, 28, 28);
    mainLayout->setSpacing(22);

    pageTitle_ = new QLabel(mainContent);
    pageTitle_->setObjectName("pageTitle");
    pageHint_ = new QLabel(mainContent);
    pageHint_->setObjectName("mutedText");
    auto* headerLayout = new QVBoxLayout;
    headerLayout->setSpacing(4);
    headerLayout->addWidget(pageTitle_);
    headerLayout->addWidget(pageHint_);
    mainLayout->addLayout(headerLayout);

    pages_ = new QStackedWidget(mainContent);
    pages_->addWidget(createMediaPage());
    pages_->addWidget(createSettingsPage());
    mainLayout->addWidget(pages_, 1);

    rootLayout->addWidget(mainContent, 1);
    setCentralWidget(rootWidget);

    mediaMtx_ = new QProcess(this);
    connect(mediaMtx_, &QProcess::readyReadStandardOutput, this, [this] {
        serverLog_ += QString::fromUtf8(mediaMtx_->readAllStandardOutput());
        if (serverLog_.size() > 2000) serverLog_ = serverLog_.right(2000);
    });
    serverReadyTimer_ = new QTimer(this);
    serverReadyTimer_->setInterval(100);
    connect(serverReadyTimer_, &QTimer::timeout, this, [this] {
        const QUrl url(rtspEdit_->text().trimmed());
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, url.port(8554));
        if (socket.waitForConnected(20)) {
            serverReadyTimer_->stop();
            pipeline_.setStreamingEnabled(true, url.toString().toStdString());
        } else if (mediaMtx_->state() == QProcess::NotRunning ||
                   serverStartClock_.elapsed() > 5000) {
            serverReadyTimer_->stop();
            streaming_ = false;
            stopOwnedServer();
            pageHint_->setText("MediaMTX 未启动：" + serverLog_.right(180));
            refreshUi();
        }
    });
    showSection(0);
    viewTimer_ = new QTimer(this);
    viewTimer_->setTimerType(Qt::PreciseTimer);
    viewTimer_->setInterval(16); // 仅检查最新画面，不控制摄像头采集节奏。
    connect(viewTimer_, &QTimer::timeout, this, [this] { updateCameraView(); });
    viewTimer_->start();
}

MainWindow::~MainWindow() {
    viewTimer_->stop();
    serverReadyTimer_->stop();
    if (player_) player_->stop();
    pipeline_.stop(); // MkvRecorder::close() 在工作线程退出前写完 MKV 索引。
    stopOwnedServer();
}

QWidget* MainWindow::createSidebar() {
    auto* sidebar = new QFrame(this);
    sidebar->setObjectName("sidebar");
    sidebar->setFixedWidth(208);
    auto* layout = new QVBoxLayout(sidebar);
    layout->setContentsMargins(14, 26, 14, 18);
    layout->setSpacing(8);

    auto* name = new QLabel("相机工作台", sidebar);
    name->setObjectName("appName");
    layout->addWidget(name);
    layout->addSpacing(28);

    auto* group = new QButtonGroup(sidebar);
    group->setExclusive(true);
    auto addNavigation = [&](const QString& text, int index) {
        auto* button = new QPushButton(text, sidebar);
        button->setObjectName("navButton");
        button->setCheckable(true);
        group->addButton(button, index);
        layout->addWidget(button);
        connect(button, &QPushButton::clicked, this,
                [this, index] { showSection(index); });
        return button;
    };

    auto* liveButton = addNavigation("实时监控", 0);
    addNavigation("录像文件", 1);
    addNavigation("设置", 2);
    liveButton->setChecked(true);
    layout->addStretch();

    powerStatus_ = new QLabel("界面演示 · 未启动", sidebar);
    powerStatus_->setObjectName("powerStatus");
    layout->addWidget(powerStatus_);

    powerButton_ = new QPushButton("启动系统", sidebar);
    powerButton_->setObjectName("powerButton");
    powerButton_->setCheckable(true);
    connect(powerButton_, &QPushButton::clicked, this, [this](bool on) {
        if (on) {
            pipeline_.start(); // 摄像头与解码器在工作线程初始化。
            powerOn_ = true;
            logPathLabel_->setText(pipeline_.logEnabled()
                ? "日志文件：" + QString::fromStdString(pipeline_.logPath())
                : "帧日志已关闭（设置 CAMERA_FRAME_LOG=1 可开启）");
        } else {
            serverReadyTimer_->stop();
            if (player_) player_->stop();
            pipeline_.stop(); // 等工作线程退出，再清除按钮状态。
            stopOwnedServer();
            powerOn_ = false;
            recording_ = false;
            streaming_ = false;
            previewOn_ = false;
            playing_ = false;
            videoLabel_->clear();
            fpsClock_.invalidate();
            fpsLabel_->setText("采集 -- / 预览 -- fps");
            diagnosticLabel_->setText("诊断：等待采集");
        }
        refreshUi();
    });
    layout->addWidget(powerButton_);
    return sidebar;
}

QWidget* MainWindow::createMediaPage() {
    auto* page = new QWidget(this);
    page->setObjectName("contentPage");
    auto* layout = new QHBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(18);

    auto* videoCard = new QFrame(page);
    videoCard->setObjectName("card");
    auto* videoCardLayout = new QVBoxLayout(videoCard);
    videoCardLayout->setContentsMargins(18, 18, 18, 18);
    videoCardLayout->setSpacing(14);
    auto* videoHeading = new QLabel("画面", videoCard);
    videoHeading->setObjectName("sectionTitle");
    fpsLabel_ = new QLabel("采集 -- / 预览 -- fps", videoCard);
    fpsLabel_->setObjectName("fpsLabel");
    auto* videoHeader = new QHBoxLayout;
    videoHeader->addWidget(videoHeading);
    videoHeader->addStretch();
    videoHeader->addWidget(fpsLabel_);
    videoCardLayout->addLayout(videoHeader);
    diagnosticLabel_ = new QLabel("诊断：等待采集", videoCard);
    diagnosticLabel_->setObjectName("mutedText");
    diagnosticLabel_->setWordWrap(true);
    videoCardLayout->addWidget(diagnosticLabel_);

    auto* videoSurface = new QFrame(videoCard);
    videoSurface->setObjectName("videoSurface");
    videoSurface->setMinimumSize(320, 240);
    auto* surfaceLayout = new QVBoxLayout(videoSurface);
    surfaceLayout->setContentsMargins(0, 0, 0, 0);
    videoViews_ = new QStackedWidget(videoSurface);
    videoLabel_ = new QLabel(videoSurface);
    videoLabel_->setObjectName("videoHint");
    videoLabel_->setAlignment(Qt::AlignCenter);
    videoViews_->addWidget(videoLabel_);
    surfaceLayout->addWidget(videoViews_);
    videoCardLayout->addWidget(videoSurface, 1);

    playbackControls_ = new QWidget(videoCard);
    auto* controls = new QHBoxLayout(playbackControls_);
    controls->setContentsMargins(0, 0, 0, 0);
    playerToggle_ = new QPushButton("▶", playbackControls_);
    playerToggle_->setObjectName("secondaryButton");
    controls->addWidget(playerToggle_);
    connect(playerToggle_, &QPushButton::clicked, this, [this] {
        if (playButton_->isEnabled()) playButton_->click();
    });
    seekSlider_ = new QSlider(Qt::Horizontal, playbackControls_);
    seekSlider_->setRange(0, 0);
    controls->addWidget(seekSlider_, 1);
    connect(seekSlider_, &QSlider::sliderReleased, this, [this] {
        if (player_) player_->setPosition(seekSlider_->value());
    });
    playbackTime_ = new QLabel("00:00:00 / 00:00:00", playbackControls_);
    controls->addWidget(playbackTime_);
    videoCardLayout->addWidget(playbackControls_);
    playbackControls_->hide();
    layout->addWidget(videoCard, 7);

    sidePanels_ = new QStackedWidget(page);
    sidePanels_->setMinimumWidth(245);
    sidePanels_->addWidget(createLivePanel());
    sidePanels_->addWidget(createFilesPanel());
    layout->addWidget(sidePanels_, 3);
    return page;
}

QWidget* MainWindow::createLivePanel() {
    auto* panel = new QFrame(this);
    panel->setObjectName("card");
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(12);

    auto* heading = new QLabel("实时控制", panel);
    heading->setObjectName("sectionTitle");
    layout->addWidget(heading);
    auto* hint = new QLabel("画面与录制操作", panel);
    hint->setObjectName("mutedText");
    layout->addWidget(hint);
    layout->addSpacing(18);

    auto* previewLabel = new QLabel("实时监控", panel);
    previewLabel->setObjectName("eyebrow");
    layout->addWidget(previewLabel);
    previewButton_ = new QPushButton("开启实时画面", panel);
    previewButton_->setObjectName("secondaryButton");
    previewButton_->setCheckable(true);
    connect(previewButton_, &QPushButton::clicked, this, [this](bool on) {
        if (!powerOn_) return;
        previewOn_ = on;
        pipeline_.setPreviewEnabled(on);
        previewFramesInWindow_ = 0;
        lastCaptureCount_ = pipeline_.capturedFrames();
        if (on) fpsClock_.start();
        else fpsClock_.invalidate();
        fpsLabel_->setText("采集 -- / 预览 -- fps");
        if (!on) {
            if (recording_) {
                pipeline_.setRecordingEnabled(false, recordingPath_.toStdString());
                recording_ = false;
            }
            videoLabel_->clear();
        }
        refreshUi();
    });
    layout->addWidget(previewButton_);

    layout->addSpacing(22);
    auto* recordLabel = new QLabel("录像", panel);
    recordLabel->setObjectName("eyebrow");
    layout->addWidget(recordLabel);
    recordButton_ = new QPushButton("开始录制", panel);
    recordButton_->setObjectName("primaryButton");
    recordButton_->setCheckable(true);
    connect(recordButton_, &QPushButton::clicked, this, [this](bool on) {
        if (!powerOn_ || !previewOn_) return;
        if (on) {
            const QString folder = directoryEdit_->text().trimmed();
            const QFileInfo directory(folder);
            if (!directory.isDir() || !directory.isWritable()) {
                pageHint_->setText("请先在设置中选择可写的录像文件夹");
                refreshUi();
                return;
            }
            const QString stem = QDateTime::currentDateTime().toString(
                "yyyy-MM-dd_HH-mm-ss-zzz");
            recordingPath_ = QDir(folder).filePath(stem + ".mkv");
            for (int suffix = 2; QFileInfo::exists(recordingPath_); ++suffix)
                recordingPath_ = QDir(folder).filePath(
                    stem + "_" + QString::number(suffix) + ".mkv");
            recordFailed_ = false;
        }
        pipeline_.setRecordingEnabled(on, recordingPath_.toStdString());
        recording_ = on;
        if (!on) pageHint_->setText("正在结束录像并写入文件索引…");
        refreshUi();
    });
    layout->addWidget(recordButton_);
    layout->addStretch();
    return panel;
}

QWidget* MainWindow::createFilesPanel() {
    auto* panel = new QFrame(this);
    panel->setObjectName("card");
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(12);

    auto* heading = new QLabel("录像文件", panel);
    heading->setObjectName("sectionTitle");
    layout->addWidget(heading);
    auto* hint = new QLabel("选择文件后在左侧播放", panel);
    hint->setObjectName("mutedText");
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addSpacing(8);

    fileList_ = new QListWidget(panel);
    connect(fileList_, &QListWidget::itemSelectionChanged, this, [this] {
        if (player_) player_->stop();
        playing_ = false;
        playbackPath_.clear();
        videoViews_->setCurrentIndex(0);
        refreshUi();
    });
    layout->addWidget(fileList_, 1);

    openFileButton_ = new QPushButton("选择视频文件", panel);
    openFileButton_->setObjectName("secondaryButton");
    connect(openFileButton_, &QPushButton::clicked, this, [this] {
        if (!powerOn_) return;
        const QString path = QFileDialog::getOpenFileName(
            this, "选择视频文件", QDir::homePath(),
            "视频文件 (*.mkv *.mp4);;所有文件 (*)");
        if (path.isEmpty()) return;
        extraFiles_.insert(path);
        refreshFileList();
        for (int i = 0; i < fileList_->count(); ++i) {
            auto* item = fileList_->item(i);
            if (item->data(Qt::UserRole).toString() == path) {
                fileList_->setCurrentItem(item);
                break;
            }
        }
    });
    layout->addWidget(openFileButton_);

    playButton_ = new QPushButton("播放所选文件", panel);
    playButton_->setObjectName("secondaryButton");
    playButton_->setCheckable(true);
    connect(playButton_, &QPushButton::clicked, this, [this](bool on) {
        if (!powerOn_ || !fileList_->currentItem()) return;
        const QString path = fileList_->currentItem()->data(Qt::UserRole).toString();
        if (!QFileInfo::exists(path) || (pipeline_.recording() && path == recordingPath_)) {
            pageHint_->setText("文件不存在，或录像尚未结束");
            refreshUi();
            return;
        }
        if (on) {
            ensurePlayer();
            if (playbackPath_ != path) {
                playbackPath_ = path;
                player_->setSource(QUrl::fromLocalFile(path));
            }
            videoViews_->setCurrentIndex(1);
            player_->play();
        } else {
            if (player_) player_->pause();
        }
        playing_ = on;
        refreshUi();
    });
    layout->addWidget(playButton_);
    return panel;
}

QWidget* MainWindow::createSettingsPage() {
    auto* page = new QWidget(this);
    page->setObjectName("contentPage");
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(18);

    auto* storageCard = new QFrame(page);
    storageCard->setObjectName("card");
    auto* storageLayout = new QVBoxLayout(storageCard);
    storageLayout->setContentsMargins(22, 22, 22, 22);
    storageLayout->setSpacing(12);
    auto* storageHeading = new QLabel("存储位置", storageCard);
    storageHeading->setObjectName("sectionTitle");
    storageLayout->addWidget(storageHeading);
    auto* storageHint = new QLabel("选择录像文件的保存文件夹", storageCard);
    storageHint->setObjectName("mutedText");
    storageLayout->addWidget(storageHint);
    auto* directoryRow = new QHBoxLayout;
    directoryEdit_ = new QLineEdit(storageCard);
    directoryEdit_->setText(QSettings("RobotCamera", "CameraWorkbench")
        .value("record_directory").toString());
    directoryEdit_->setPlaceholderText("尚未选择文件夹");
    browseButton_ = new QPushButton("选择文件夹", storageCard);
    browseButton_->setObjectName("secondaryButton");
    directoryRow->addWidget(directoryEdit_, 1);
    directoryRow->addWidget(browseButton_);
    storageLayout->addLayout(directoryRow);
    connect(directoryEdit_, &QLineEdit::textChanged, this, [this] {
        QSettings("RobotCamera", "CameraWorkbench").setValue(
            "record_directory", directoryEdit_->text().trimmed());
        refreshUi();
    });
    connect(browseButton_, &QPushButton::clicked, this,
            [this] {
                const QString path = QFileDialog::getExistingDirectory(
                    this, "选择保存文件夹", QDir::homePath());
                if (!path.isEmpty()) directoryEdit_->setText(path);
            });
    logPathLabel_ = new QLabel("诊断日志：启动系统后显示路径", storageCard);
    logPathLabel_->setObjectName("mutedText");
    logPathLabel_->setWordWrap(true);
    logPathLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    storageLayout->addWidget(logPathLabel_);
    layout->addWidget(storageCard);

    auto* streamCard = new QFrame(page);
    streamCard->setObjectName("card");
    auto* streamLayout = new QVBoxLayout(streamCard);
    streamLayout->setContentsMargins(22, 22, 22, 22);
    streamLayout->setSpacing(12);
    auto* streamHeading = new QLabel("网络传输", streamCard);
    streamHeading->setObjectName("sectionTitle");
    streamLayout->addWidget(streamHeading);
    auto* streamHint = new QLabel("设置 RTSP 发布地址", streamCard);
    streamHint->setObjectName("mutedText");
    streamLayout->addWidget(streamHint);
    rtspEdit_ = new QLineEdit(QSettings("RobotCamera", "CameraWorkbench")
        .value("rtsp_url", "rtsp://127.0.0.1:8554/live").toString(), streamCard);
    streamLayout->addWidget(rtspEdit_);
    connect(rtspEdit_, &QLineEdit::textChanged, this, [this] {
        QSettings("RobotCamera", "CameraWorkbench").setValue(
            "rtsp_url", rtspEdit_->text().trimmed());
    });
    auto* serverHint = new QLabel("本机服务程序（使用板卡上已有的 MediaMTX）", streamCard);
    serverHint->setObjectName("mutedText");
    streamLayout->addWidget(serverHint);
    auto* serverRow = new QHBoxLayout;
    serverPathEdit_ = new QLineEdit(
        QSettings("RobotCamera", "CameraWorkbench")
            .value("mediamtx_path", QDir::homePath() + "/lx_ws/mediamtx/mediamtx")
            .toString(), streamCard);
    connect(serverPathEdit_, &QLineEdit::textChanged, this, [this] {
        QSettings("RobotCamera", "CameraWorkbench").setValue(
            "mediamtx_path", serverPathEdit_->text().trimmed());
    });
    auto* browseServer = new QPushButton("选择程序", streamCard);
    browseServer->setObjectName("secondaryButton");
    serverRow->addWidget(serverPathEdit_, 1);
    serverRow->addWidget(browseServer);
    streamLayout->addLayout(serverRow);
    connect(browseServer, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, "选择 MediaMTX 程序", QFileInfo(serverPathEdit_->text()).absolutePath());
        if (!path.isEmpty()) serverPathEdit_->setText(path);
    });
    streamButton_ = new QPushButton("开启 RTSP 推流", streamCard);
    streamButton_->setObjectName("primaryButton");
    streamButton_->setCheckable(true);
    connect(streamButton_, &QPushButton::clicked, this, [this](bool on) {
        if (!powerOn_) return;
        if (on) beginStream();
        else {
            serverReadyTimer_->stop();
            pipeline_.setStreamingEnabled(false, {});
            streaming_ = false;
        }
        refreshUi();
    });
    streamLayout->addWidget(streamButton_, 0, Qt::AlignLeft);
    layout->addWidget(streamCard);
    layout->addStretch();
    return page;
}

void MainWindow::showSection(int index) {
    const QString titles[] = {"实时监控", "录像文件", "设置"};
    const QString hints[] = {
        "查看画面并管理录制",
        "浏览已保存的视频",
        "调整保存位置与网络地址"
    };
    if (index < 0 || index > 2) return;
    if (index != currentPage_) {
        previewFramesInWindow_ = 0;
        lastCaptureCount_ = pipeline_.capturedFrames();
        fpsLabel_->setText("采集 -- / 预览 -- fps");
        if (index == 0 && previewOn_) fpsClock_.start();
        else fpsClock_.invalidate();
    }
    if (currentPage_ == 1 && index != 1) {
        if (player_) player_->stop();
        playbackPath_.clear();
        playing_ = false;
    }
    currentPage_ = index;
    pageTitle_->setText(titles[index]);
    pageHint_->setText(hints[index]);

    if (index == 2) {
        pages_->setCurrentIndex(1);
    } else {
        pages_->setCurrentIndex(0);
        sidePanels_->setCurrentIndex(index);
    }
    videoLabel_->clear();
    videoViews_->setCurrentIndex(index == 1 && playing_ ? 1 : 0);
    playbackControls_->setVisible(index == 1);
    fpsLabel_->setVisible(index == 0);
    diagnosticLabel_->setVisible(index == 0);
    if (index == 1) refreshFileList();
    refreshUi();
}

void MainWindow::refreshUi() {
    powerButton_->setChecked(powerOn_);
    powerButton_->setText(powerOn_ ? "关闭系统" : "启动系统");
    powerStatus_->setText(powerOn_ ? "摄像头启动中 / 运行中" : "摄像头未启动");

    previewButton_->setEnabled(powerOn_);
    previewButton_->setChecked(previewOn_);
    previewButton_->setText(previewOn_ ? "关闭实时画面" : "开启实时画面");

    const QFileInfo directory(directoryEdit_->text().trimmed());
    const bool validDirectory = !directoryEdit_->text().trimmed().isEmpty() &&
                                directory.isDir() && directory.isWritable();
    recordButton_->setEnabled(powerOn_ && previewOn_ &&
        (recording_ || (recordingPath_.isEmpty() && !pipeline_.recording() &&
                        validDirectory)));
    recordButton_->setChecked(recording_);
    recordButton_->setText(recording_ ?
        (pipeline_.recording() ? "停止录制" : "等待关键帧，准备录制…") : "开始录制");
    recordButton_->setToolTip(validDirectory
        ? "" : "请先在设置中选择可写的录像文件夹");

    streamButton_->setEnabled(powerOn_ && (streaming_ || !pipeline_.streaming()));
    streamButton_->setChecked(streaming_);
    streamButton_->setText(streaming_ ? "关闭 RTSP 推流" : "开启 RTSP 推流");
    rtspEdit_->setEnabled(!streaming_);
    serverPathEdit_->setEnabled(!streaming_);

    openFileButton_->setEnabled(powerOn_);
    playButton_->setEnabled(powerOn_ && fileList_->currentItem() != nullptr &&
        fileList_->currentItem()->data(Qt::UserRole).toString() != recordingPath_);
    playButton_->setChecked(playing_);
    playButton_->setText(playing_ ? "停止播放" : "播放所选文件");

    directoryEdit_->setEnabled(!recording_ && !pipeline_.recording());
    browseButton_->setEnabled(!recording_ && !pipeline_.recording());

    if (currentPage_ == 0) {
        if (!previewOn_) videoLabel_->setText("点击“开启实时画面”显示摄像头");
        else if (videoLabel_->pixmap().isNull()) videoLabel_->setText("正在等待摄像头画面…");
    } else if (currentPage_ == 1 && !playing_) {
        videoLabel_->setText("选择录像后在这里播放");
    }
    updatePlaybackControls();
}

void MainWindow::addFileItem(const QString& path) {
    auto* item = new QListWidgetItem(fileList_);
    item->setData(Qt::UserRole, path);
    auto* row = new QWidget(fileList_);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(6, 2, 6, 2);
    auto* name = new QPushButton(QFileInfo(path).fileName(), row);
    name->setFlat(true);
    name->setToolTip(path);
    name->setStyleSheet("text-align:left; border:none; background:transparent;");
    connect(name, &QPushButton::clicked, this, [this, item] {
        fileList_->setCurrentItem(item);
    });
    auto* remove = new QToolButton(row);
    remove->setText("✕");
    remove->setObjectName("deleteFileButton");
    remove->setToolTip("删除此视频文件");
    connect(remove, &QToolButton::clicked, this, [this, path] {
        if (path == recordingPath_ && (recording_ || pipeline_.recording())) {
            pageHint_->setText("正在录制的文件不能删除");
            return;
        }
        if (QMessageBox::question(this, "删除录像",
                "确定从磁盘删除“" + QFileInfo(path).fileName() + "”吗？")
            != QMessageBox::Yes) return;
        if (playbackPath_ == path) {
            if (player_) player_->stop();
            playing_ = false;
            playbackPath_.clear();
            videoViews_->setCurrentIndex(0);
        }
        if (!QFile::remove(path)) {
            pageHint_->setText("删除失败：" + path);
            return;
        }
        extraFiles_.remove(path);
        refreshFileList();
    });
    layout->addWidget(name, 1);
    layout->addWidget(remove);
    item->setSizeHint(row->sizeHint());
    fileList_->setItemWidget(item, row);
}

void MainWindow::refreshFileList() {
    const QString selected = fileList_->currentItem()
        ? fileList_->currentItem()->data(Qt::UserRole).toString() : QString();
    QSignalBlocker blocked(fileList_);
    fileList_->clear();
    QSet<QString> listed;
    const QDir folder(directoryEdit_->text().trimmed());
    if (!directoryEdit_->text().trimmed().isEmpty() && folder.exists()) {
        const auto entries = folder.entryInfoList({"*.mkv", "*.mp4"},
                                                   QDir::Files, QDir::Time);
        for (const QFileInfo& file : entries) {
            const QString path = file.absoluteFilePath();
            if (path == recordingPath_ && (recording_ || pipeline_.recording())) continue;
            addFileItem(path);
            listed.insert(path);
        }
    }
    for (const QString& path : extraFiles_) {
        if (path == recordingPath_ && (recording_ || pipeline_.recording())) continue;
        if (QFileInfo::exists(path) && !listed.contains(path)) addFileItem(path);
    }
    for (int i = 0; i < fileList_->count(); ++i) {
        auto* item = fileList_->item(i);
        if (item->data(Qt::UserRole).toString() == selected) {
            fileList_->setCurrentItem(item);
            break;
        }
    }
    refreshUi();
}

void MainWindow::updatePlaybackControls() {
    playerToggle_->setText(playing_ ? "Ⅱ" : "▶");
    playerToggle_->setEnabled(playButton_->isEnabled());
    if (!player_) {
        playbackTime_->setText("00:00:00 / 00:00:00");
        return;
    }
    playbackTime_->setText(playbackTime(player_->position()) + " / " +
                           playbackTime(player_->duration()));
}

void MainWindow::ensurePlayer() {
    if (player_) return;
    // 仅在用户第一次播放文件时创建 Qt Multimedia/GStreamer 对象。
    playbackVideo_ = new QVideoWidget(videoViews_);
    videoViews_->addWidget(playbackVideo_);
    player_ = new QMediaPlayer(this);
    player_->setVideoOutput(playbackVideo_);
    connect(player_, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
        if (!seekSlider_->isSliderDown()) seekSlider_->setValue(
            static_cast<int>(std::min<qint64>(position, std::numeric_limits<int>::max())));
        updatePlaybackControls();
    });
    connect(player_, &QMediaPlayer::durationChanged, this, [this](qint64 duration) {
        seekSlider_->setMaximum(static_cast<int>(std::min<qint64>(
            duration, std::numeric_limits<int>::max())));
        updatePlaybackControls();
    });
    connect(player_, &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia) {
            playing_ = false;
            refreshUi();
        }
    });
    connect(player_, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error, const QString& message) {
        pageHint_->setText("播放失败：" + message);
        playing_ = false;
        refreshUi();
    });
}

void MainWindow::beginStream() {
    const QUrl url(rtspEdit_->text().trimmed());
    if (!url.isValid() || url.scheme() != "rtsp" || url.host().isEmpty() ||
        url.port(8554) <= 0 || url.port(8554) > 65535) {
        streaming_ = false;
        pageHint_->setText("请填写有效的 rtsp://主机:端口/路径");
        return;
    }
    streaming_ = true;
    const QString host = url.host();
    const bool local = host == "localhost" || host == "127.0.0.1" ||
        host == "::1" || QNetworkInterface::allAddresses().contains(QHostAddress(host));
    if (!local) { // 外部服务器由对方维护，本程序只发布视频。
        pipeline_.setStreamingEnabled(true, url.toString().toStdString());
        return;
    }

    QTcpSocket probe;
    probe.connectToHost(QHostAddress::LocalHost, url.port(8554));
    if (probe.waitForConnected(20)) { // 已有服务，直接复用，不启动第二个实例。
        pipeline_.setStreamingEnabled(true, url.toString().toStdString());
        return;
    }
    if (mediaMtx_->state() == QProcess::NotRunning) {
        const QFileInfo executable(serverPathEdit_->text().trimmed());
        if (!executable.isFile() || !executable.isExecutable()) {
            streaming_ = false;
            pageHint_->setText("找不到 MediaMTX 程序，请在设置页选择它的路径");
            return;
        }
        mediaMtx_->setWorkingDirectory(executable.absolutePath());
        mediaMtx_->setProcessChannelMode(QProcess::MergedChannels);
        serverLog_.clear();
        mediaMtx_->start(executable.absoluteFilePath());
    }
    serverStartClock_.start();
    serverReadyTimer_->start();
}

void MainWindow::stopOwnedServer() {
    if (mediaMtx_->state() == QProcess::NotRunning) return;
    mediaMtx_->terminate();
    if (!mediaMtx_->waitForFinished(1000)) {
        mediaMtx_->kill();
        mediaMtx_->waitForFinished(1000);
    }
}

void MainWindow::updateCameraView() {
    std::string error;
    std::string logSummary;
    if (pipeline_.logEnabled() && pipeline_.takeLogSummary(logSummary)) {
        const QString full = QString::fromStdString(logSummary);
        auto average = [&full](const QString& stage) {
            const QRegularExpression pattern(
                QRegularExpression::escape(stage) +
                QStringLiteral("\\[n=\\d+,avg=([0-9.]+)ms"));
            const auto match = pattern.match(full);
            return match.hasMatch() ? match.captured(1) : QStringLiteral("--");
        };
        diagnosticLabel_->setText(QString("诊断  转换 %1 ms · 编码 %2 ms · 推流 %3 ms · 采集→界面 %4 ms")
            .arg(average("convert"))
            .arg(average("encode"))
            .arg(average("rtsp_write"))
            .arg(average("capture_to_ui_submitted")));
        diagnosticLabel_->setToolTip(full);
    }
    if (pipeline_.takeError(error)) {
        pageHint_->setText(QString::fromStdString(error));
        if (pipeline_.alive() &&
            (error.find("RTSP") != std::string::npos ||
             error.find("编码器") != std::string::npos ||
             error.find("H.264") != std::string::npos)) {
            streaming_ = false;
        }
        refreshUi();
    }
    if (pipeline_.takeRecordError(error)) {
        pageHint_->setText(QString::fromStdString(error));
        recordFailed_ = true;
        recording_ = false;
        refreshUi();
    }
    if (powerOn_ && pipeline_.finished()) {
        pipeline_.stop();
        stopOwnedServer();
        serverReadyTimer_->stop();
        if (player_) player_->stop();
        powerOn_ = previewOn_ = streaming_ = recording_ = false;
        videoLabel_->clear();
        fpsClock_.invalidate();
        fpsLabel_->setText("采集 -- / 预览 -- fps");
        refreshUi();
    }
    if (!recording_ && !pipeline_.recording() && !recordingPath_.isEmpty()) {
        const QString finishedFile = recordingPath_;
        recordingPath_.clear();
        refreshFileList();
        if (!recordFailed_ && QFileInfo::exists(finishedFile))
            pageHint_->setText("录像已保存：" + QFileInfo(finishedFile).fileName());
    }
    powerStatus_->setText(!powerOn_ ? "摄像头未启动" :
                          pipeline_.alive() ? "摄像头运行中" : "摄像头启动中");
    if (streaming_) streamButton_->setText(
        pipeline_.streaming() ? "关闭 RTSP 推流" : "正在连接 RTSP…");
    else streamButton_->setEnabled(powerOn_ && !pipeline_.streaming());
    if (!streaming_ && !pipeline_.streaming() &&
        !serverReadyTimer_->isActive() &&
        mediaMtx_->state() != QProcess::NotRunning) stopOwnedServer();
    if (recording_) recordButton_->setText(
        pipeline_.recording() ? "停止录制" : "等待关键帧，准备录制…");

    if (!powerOn_ || !previewOn_ || currentPage_ != 0) return;
    QImage image;
    FrameLog imageLog;
    if (pipeline_.takeLatestImage(image, imageLog)) {
        const QPixmap pixmap = QPixmap::fromImage(std::move(image));
        videoLabel_->setPixmap(pixmap.scaled(videoLabel_->size(),
            Qt::KeepAspectRatio, Qt::FastTransformation));
        pipeline_.noteUiSubmitted(std::move(imageLog));
        ++previewFramesInWindow_;
    }
    if (fpsClock_.isValid() && fpsClock_.elapsed() >= 1000) {
        const qint64 elapsedMs = fpsClock_.restart();
        const uint64_t captureCount = pipeline_.capturedFrames();
        const double captureFps = (captureCount - lastCaptureCount_) * 1000.0 / elapsedMs;
        const double previewFps = previewFramesInWindow_ * 1000.0 / elapsedMs;
        fpsLabel_->setText(QString("采集 %1 / 预览 %2 fps")
            .arg(captureFps, 0, 'f', 1).arg(previewFps, 0, 'f', 1));
        lastCaptureCount_ = captureCount;
        previewFramesInWindow_ = 0;
    }
}
