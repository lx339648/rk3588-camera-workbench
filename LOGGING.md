# 相机工作台：逐帧诊断日志

## 时间戳与界面的边界

V4L2 返回的 `CapturedFrame::timestamp` 和 `sequence` 是采集帧的元数据，编码器使用采集时间戳作为 PTS。预览分支通过 `PreviewImage` 把画面、PTS、序号和时间戳标志一起交给 Qt；即使关闭日志，这些元数据仍然存在。V4L2 的 PTS 通常使用单调时钟，不能直接当作日期显示。

`FrameLog` 只记录各环节的诊断时间点，不决定录像或推流的时间戳。`MainWindow` 不持有 `FrameLog`，只接收 `PreviewImage` 和数值形式的 `PipelineDiagnostics`。日志文字仍写入文件，界面不会解析日志文本来计算耗时。界面调用 `noteUiSubmitted()` 标记已提交画面，但它不代表屏幕硬件已经显示该帧。

Qt 程序启动摄像头时默认开启诊断。每张帧只在内存中记时间点，采集线程每秒产生一条统计摘要；独立写入线程负责保存文件。故障帧另记一条带采集序号和时间戳的详情。普通运行不会逐帧写磁盘。

日志默认放在 `$XDG_STATE_HOME/CameraWorkbench/logs/`；未设置 `XDG_STATE_HOME` 时放在 `$HOME/.local/state/CameraWorkbench/logs/`。每次启动使用 `camera_YYYYMMDD_HHMMSS.log` 文件名。Qt「设置 → 运行诊断」会显示实际路径；录像仍保存在用户选择的录像目录。

## 开关与对比

从工程根目录重新构建 Qt 程序：

```bash
cmake -S qt_app -B qt_app/build
cmake --build qt_app/build -j2
```

使用同一份可执行文件对比，有助于排除不同编译版本的影响：

```bash
CAMERA_FRAME_LOG=0 ./qt_app/build/qt_window  # 不打点、不写日志
CAMERA_FRAME_LOG=1 ./qt_app/build/qt_window  # 默认模式：每秒一条摘要
CAMERA_FRAME_LOG=1 CAMERA_FRAME_LOG_DETAIL=1 ./qt_app/build/qt_window  # 调试模式：额外记录逐帧详情
```

如果设备节点需要提权，注意提权后 `$HOME` 可能变成 `/root`；以界面显示的日志路径为准。做性能对比时，保持同一分辨率、预览/录像/推流开关、网络和场景，先运行一小段时间让程序稳定，再分别记录至少 60 秒的采集 FPS、预览 FPS 和日志中的各环节平均值、最大值。详细逐帧模式会增加磁盘写入，不宜当作日常性能基线。

## 日志字段

- `frames`、`fps`：本统计窗口内走过主线的帧数及输出频率。
- `wait`：等待新帧和 DQBUF 的时间，含相机帧间隔，不是处理耗时。
- `copy`、`requeue`：V4L2 缓冲区复制、归还的时间。
- `convert`：RGA YUYV → NV12；`encode`：MPP NV12 → H.264。
- `record`、`rtsp_write`：分别为 MKV 写入、RTSP 发布接口调用耗时；没有开启的分支不会出现。
- `preview_convert`：预览线程的 NV12 → RGB；`preview_to_ui`：转换完成到 Qt 调用 `setPixmap` 的时间，包含主线程排队与画面缩放。
- `capture_to_encoded`、`capture_to_rtsp`、`capture_to_ui_submitted`：仅在 V4L2 时间戳标明为单调时钟时才计算，表示采集时间戳到对应阶段的本机耗时。`ui_submitted` 是 Qt 调用 `setPixmap` 完成，**不代表显示器已实际刷新，也不含接收端播放延迟**。
- `n` 是该阶段有有效起止时间的样本数；预览可丢旧帧，所以预览阶段的 `n` 可能小于 `frames`。

逐帧模式的 `branch=main` 和 `branch=ui` 可以用 `seq` 与 `capture_pts_us` 对齐。`*_mono_ns` 用于算间隔，`*_utc_us` 用于按日期检索；不要直接用墙上时间相减计算视频处理耗时。
