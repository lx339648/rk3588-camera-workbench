# Qt 预览更新记录

## 2026-11-18：NV12 OpenGL 预览试验

- 保留主线向预览槽复制 NV12 和最新帧覆盖策略；预览线程不再用 sws_scale 转 RGB。
- 实时预览改用 QOpenGLWidget 上传 NV12 的 Y、UV 平面，通过着色器转换颜色并按比例绘制。录像文件回放仍使用原 Qt Multimedia 组件。
- 原 `preview_convert` 日志改名 `preview_handoff`；Qt 预览日志用 `gl_enqueue` 记录提交更新请求的耗时、`gl_paint_cpu` 记录纹理上传与绘制命令的 CPU 耗时。`capture_to_ui_submitted` 现在截止于请求 OpenGL 绘制，实际 GPU 绘制和屏幕刷新发生在此之后。
- 本版仍有 CPU NV12 复制及纹理上传，不属于零拷贝。
- 初始化时打印实际 OpenGL vendor、renderer、version 和 API，用于分辨板卡 GPU 渲染、远程 X11 渲染与软件渲染。
- 增加 `gl_compose_swap`，记录 Qt 顶层窗口开始合成到交换缓冲返回，定位远程 X11 环境中绘制函数返回之后的长时间卡顿。
- RK3588 本地 X11 测试发现默认 GLX 上下文使用 Mesa llvmpipe；通过 `QT_XCB_GL_INTEGRATION=xcb_egl` 启动后，程序打印 `ARM / Mali-G610 / OpenGL ES`。该选择属于启动环境配置，未写死在程序中。
- EGL/Mali 测试：`qt_preview_20261118_154849_328_132797.log` 的两个完整稳定窗口各提交 30 帧，`seq_skipped=0`、`preview_replace_ready=0`，`gl_compose_swap` 平均约 1.6 ms；`camera_20261118_154854.log` 同时显示采集约 30 fps、采集时间戳到 Qt 提交约 41–42 ms。此数值不含物理上屏及 VNC 传输。

## 2026-11-12：预览计时与帧覆盖统计

- 在独立的 `qt_preview_*.log` 中增加 Qt 定时器实际间隔和界面回调耗时。
- 每秒记录回调次数、成功提交的新画面数、无新画面的回调次数，以及相邻已显示采集序号之间跳过的帧数。
- 在预览分支两处“只保留最新帧”的槽位增加累计覆盖计数；Qt 日志仅读取数值，不介入采集、RGA、编码、录像或 RTSP 流程。
- 日志仍由后台线程写入；关闭 `CAMERA_QT_PREVIEW_LOG` 后不进行 Qt 计时统计。

这些数据用于区分定时器晚触发、预览转换跟不上和界面取图不及时。`setPixmap` 返回时间仍不是屏幕实际显示时间。
