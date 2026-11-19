# Qt 相机工作台

本目录仍使用独立的 CMake 工程，但编译时会引用上级目录已有的
`capture_v4l2`、RGA 格式转换、MPP 编码和 `rtsp_publisher`，不另开摄像头。

```bash
cd ~/lx_ws/C405_Log_log
cmake -S qt_app -B qt_app/build
cmake --build qt_app/build -j2
./qt_app/build/qt_window
```

在 RK3588 的板卡本地 X11 桌面运行 GPU 预览时，使用：

```bash
DISPLAY=:0 XAUTHORITY="$HOME/.Xauthority" \
QT_QPA_PLATFORM=xcb QT_XCB_GL_INTEGRATION=xcb_egl \
./qt_app/build/qt_window
```

打开实时画面后，确认终端中的 `NV12 preview OpenGL` 显示
`renderer=Mali-G610`、`api=OpenGL ES`。板卡当前默认 GLX 路径可能落到
Mesa llvmpipe 软件渲染；仅看到 OpenGL 窗口不代表使用了 GPU。
`DISPLAY=:0` 指向板卡本地桌面，x11vnc 可以共享同一桌面供远端查看。

USB 摄像头断开后，程序每 2 秒重试采集；重新插入后恢复此前的预览、人员检测和
RTSP 推流。断线时当前 MKV 会封闭，录像需重新点击开始。默认打开 `/dev/video84`；
若重插后设备编号变化，启动前可设置 `CAMERA_DEVICE` 为相机的稳定
`/dev/v4l/by-id/...-video-index...` 路径（先用 `ls -l /dev/v4l/by-id/` 确认
它指向 D405C 的彩色采集节点），再运行程序。

需要 Qt6 Widgets、OpenGL/OpenGLWidgets、Qt6 Multimedia/MultimediaWidgets、FFmpeg 开发包
（libavformat、libavcodec、libavutil）、MPP 和带 IM2D 接口的 librga 头文件及库。
新增 AI 检测还需要 RKNN 运行库 `librknnrt.so`。默认模型路径为
`~/lx_ws/model_train/yolov8.rknn`，也可设置 `CAMERA_RKNN_MODEL` 指向模型文件。
若运行库不在 `~/lx_ws/model_train` 或系统库目录，在 CMake 配置时指定
`-DRKNN_LIBRARY=/绝对路径/librknnrt.so`。
RKNN SDK 的 `rknn_api.h` 由本机 SDK 提供，不包含在仓库中；若不在系统头文件目录，
配置时指定 `-DRKNN_INCLUDE_DIR=/绝对路径/SDK/include`。
MPP 若未安装在常见位置，可以在
`cmake` 配置命令后加 `-DMPP_INCLUDE_DIR=... -DMPP_LIBRARY=...`。

按钮对应关系：

- **启动系统**：工作线程打开 `/dev/video84`，按 YUYV 640×480@30 采集并由 RGA
  转换成 NV12；关闭时等线程退出并释放设备。
- **开启实时画面**：采集线程复制一份 NV12 到单帧待处理槽；预览线程
  交接 NV12，Qt 每 16 ms 检查一次最新图像。OpenGL 上传 Y、UV 纹理并绘制。
  两处槽位都直接替换旧画面，不积压；该定时器不控制摄像头帧率。
  画面上方每秒更新一次“采集 / 预览”帧率；预览数值统计的是 Qt 收到并
  提交显示的新帧数，不等同于屏幕物理刷新率或端到端延迟。
- **开启人员检测**：仅在实时画面开启后可用。AI 线程接收最新 YUYV 帧，
  另用 RGA 转 RGB888 并补边至 640×640，交给 RKNN YOLOv8n 推理；后处理只保留
  person 类别。检测结果按相机序号与该帧 NV12 画面一起提交 OpenGL，图像等待
  自己的检测框；AI 慢时覆盖待处理旧帧，不阻塞采集、编码、录像或 RTSP。
  本版 AI 输入仍使用 `rknn_inputs_set`，预处理后有一次 RGB 补边拷贝，
  尚未使用 RKNN 零拷贝接口。模型载入或推理出错时自动关闭 AI，普通预览恢复。
- **开启 RTSP 推流**：同一个工作线程使用已有 MPP H.264 编码器和 FFmpeg
  发布器。地址指向本机时，先检查端口；若没有服务，就自动启动设置页
  指定的 `mediamtx` 程序。已存在的服务不会重复启动，也不会被程序关闭。
  由本程序启动的服务在推流关闭、发布器退出后停止。
- **开始录制**：必须先在设置页选择存在且可写的文件夹。按启动时间生成
  `yyyy-MM-dd_HH-mm-ss-zzz.mkv`；与 RTSP 共用一次 H.264 编码。
  点击后请求编码器把下一帧设为 IDR；停止录制或退出时正常写完 MKV 文件尾。
- **录像文件**：自动列出保存目录里的 MKV/MP4；还可以手动加入视频。
  每行右侧的 `✕` 在确认后从磁盘删除文件，正在录制的文件不会出现在列表里。
  回放使用 Qt Multimedia，带播放/暂停、进度拖动和时间显示。
  Qt Multimedia 播放器在第一次点击播放时才创建，实时预览不初始化回放后端。

运行诊断默认开启：设置页可复制日志路径，实时页显示转换、编码、推流与
采集到 Qt 提交画面的平均耗时。日志按秒写入独立文件夹；可用环境变量
`CAMERA_FRAME_LOG=0` 关闭，以同一版本比较性能。逐帧明细及字段解释见
[`../LOGGING.md`](../LOGGING.md)。

Qt 预览另有独立日志 `qt_preview_*.log`，设置页会显示路径。它每秒统计
`gl_enqueue`（交给 OpenGL 预览控件并请求更新）和 `gl_paint_cpu`
（纹理上传与绘制命令的 CPU 耗时）、`gl_compose_swap`（窗口开始合成到交换缓冲返回）
的平均值与最大值，包含最后一帧的采集序号和 PTS。
另外统计 `tick_interval`（Qt 定时器的实际回调间隔）、`ui_callback`
（整个 `updateCameraView()` 回调耗时）、`ticks` / `frames` / `empty_ticks`
（回调次数、提交新画面次数、没有新画面的次数）。`seq_skipped` 是相邻已显示
采集序号之间跳过的帧数；`preview_replace_pending` 是待交接 NV12 槽被覆盖
的次数，`preview_replace_ready` 是待界面取走的 NV12 槽被覆盖的次数。
AI 开启时另有 `ai_frames`、`ai_people`、`ai_replace_pending`、
`ai_replace_ready`、`ai_queue_wait`、`ai_preprocess`、`ai_infer_post` 和 `ai_total`；
这些耗时基于已提交给 Qt 的检测帧，其中 `ai_infer_post` 包含 RKNN 输入输出
传递及 YOLO 后处理，不能直接当作纯 NPU 执行时间。
现另分出 `rknn_inputs_set`、`rknn_run`、`rknn_outputs_get`、
`yolo_decode_nms`、`rknn_outputs_release` 的墙钟耗时；
`rknn_run` 的调用耗时不一定等于 NPU 硬件计算时间，等待也可能落在输出获取阶段。
这些数值只在实时预览页开启时按秒汇总，不在每帧写盘；切走页面会结束当前
统计窗口。每个耗时项的 `n` 是该窗口内的样本数，首个回调没有间隔样本。
定时器约每秒检查 60 次而相机每秒只出约 30 帧，所以 `empty_ticks` 不为零
是正常现象，不能直接当成丢帧数。`seq_skipped` 说明已显示画面序号有缺口，
但单凭它不能判断缺口发生在哪个环节。
写文件由单独线程完成。设置 `CAMERA_QT_PREVIEW_LOG=0` 可单独关闭此日志，
不会影响主链路诊断；`gl_paint_cpu` 不等于 GPU 完成或屏幕刷新耗时。
`capture_to_ui_submitted` 到请求绘制为止，也不是物理上屏延迟。

RTSP 发布仍在采集工作线程内执行，如果网络调用阻塞，会影响采集。
录像从按钮后的下一张可编码帧开始，帧边界通常约 33 ms；这一版还没有
实现你之前提出的、以按键时间戳精确选择录像起点的 1 秒环形缓冲。
Qt 回放能否解码板卡上的 MKV/H.264，取决于板卡 Qt Multimedia 后端。
若仅点击回放后出现 GStreamer 报错，先用 `ffprobe` 检查 MKV，再检查板卡
GStreamer 的 `playbin`、`matroskademux` 和 `avdec_h264` 插件。

手工验证顺序：

1. 在设置中选一个已挂载、可写的目录；启动系统并开启实时画面。
2. 录制数秒后点击停止，进入录像文件页，确认出现以时间命名的 MKV。
3. 选中文件，试播放/暂停和拖动进度；关掉回放页，确认采集仍在运行。
4. 在设置页开启 RTSP。如果本机 8554 端口已有服务则直接复用；否则
   程序启动设置页指向的 `mediamtx`，电脑用板卡 IP 访问 `/live`。
5. 停止推流、关闭总开关，检查已录 MKV 可以再次播放，最后试单条删除。
