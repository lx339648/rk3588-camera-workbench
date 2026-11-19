#pragma once

#include "camera_pipeline.hpp"
#include "qt_preview_log.hpp"
#include <QElapsedTimer>
#include <QMainWindow>
#include <QSet>
#include <QString>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QMediaPlayer;
class QProcess;
class QSlider;
class QStackedWidget;
class QTimer;
class QVideoWidget;
class QWidget;
class Nv12PreviewWidget;

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private:
    QWidget* createSidebar();
    QWidget* createMediaPage();
    QWidget* createLivePanel();
    QWidget* createFilesPanel();
    QWidget* createSettingsPage();
    void showSection(int index);
    void refreshUi();
    void updateCameraView();
    void refreshFileList();
    void addFileItem(const QString& path);
    void beginStream();
    void stopOwnedServer();
    void updatePlaybackControls();
    void ensurePlayer();

    QStackedWidget* pages_ = nullptr;      // 媒体区 / 设置页
    QStackedWidget* sidePanels_ = nullptr; // 实时控制 / 文件列表
    QLabel* videoLabel_ = nullptr;         // 共用画面区的占位文字
    Nv12PreviewWidget* glPreview_ = nullptr;
    QLabel* playbackTime_ = nullptr;
    QLabel* fpsLabel_ = nullptr;           // 采集及预览更新帧率
    QLabel* diagnosticLabel_ = nullptr;    // 每秒汇总的环节耗时
    QLabel* logPathLabel_ = nullptr;       // 日志文件实际位置
    QStackedWidget* videoViews_ = nullptr;
    QVideoWidget* playbackVideo_ = nullptr;
    QWidget* playbackControls_ = nullptr;
    QMediaPlayer* player_ = nullptr;
    QSlider* seekSlider_ = nullptr;
    QPushButton* playerToggle_ = nullptr;
    QLabel* pageTitle_ = nullptr;          // 当前页面标题
    QLabel* pageHint_ = nullptr;           // 当前页面说明
    QLabel* powerStatus_ = nullptr;
    QPushButton* powerButton_ = nullptr;
    QPushButton* previewButton_ = nullptr;
    QPushButton* aiButton_ = nullptr;
    QPushButton* recordButton_ = nullptr;
    QPushButton* streamButton_ = nullptr;
    QPushButton* openFileButton_ = nullptr;
    QPushButton* playButton_ = nullptr;
    QPushButton* browseButton_ = nullptr;
    QLineEdit* directoryEdit_ = nullptr;
    QLineEdit* rtspEdit_ = nullptr;
    QLineEdit* serverPathEdit_ = nullptr;
    QListWidget* fileList_ = nullptr;
    QTimer* viewTimer_ = nullptr;
    QTimer* reconnectTimer_ = nullptr;
    QTimer* serverReadyTimer_ = nullptr;
    QProcess* mediaMtx_ = nullptr;
    CameraPipeline pipeline_;
    QtPreviewLog qtPreviewLog_;

    int currentPage_ = 0;
    bool powerOn_ = false;
    bool reconnecting_ = false;
    bool reconnectAttemptActive_ = false;
    bool previewOn_ = false;
    bool aiOn_ = false;
    bool recording_ = false;
    bool streaming_ = false;
    bool playing_ = false;
    bool recordFailed_ = false;
    QString recordingPath_;
    QString playbackPath_;
    QSet<QString> extraFiles_;
    QString serverLog_;
    QElapsedTimer serverStartClock_;
    QElapsedTimer fpsClock_;
    QElapsedTimer glComposeClock_;
    int previewFramesInWindow_ = 0;
    uint64_t lastCaptureCount_ = 0;
};
