# D405C 彩色流接入

主链路：`/dev/video84` YUYV 640×480@30 → V4L2 MMAP 采集 → RGA 转 NV12 → MPP H.264 编码 → MKV 录像或 RTSP 发布。`/dev/video80` 是 Z16 深度流，不属于这条彩色视频链路。Qt 预览从 NV12 拷贝最新一帧，再转成 QImage；它不会再次打开摄像头。

## 先检查设备和依赖

在 RK3588 板上运行：

```bash
v4l2-ctl -d /dev/video84 --list-formats-ext
ls /usr/include/rga/im2d.hpp /usr/local/include/rga/im2d.hpp 2>/dev/null
ldconfig -p | grep librga
ls -l /dev/dma_heap/system /dev/mpp_service 2>/dev/null
```

找不到 `im2d.hpp` 或 `librga.so` 时，先安装板卡 SDK 配套的 librga 开发包，再配置 CMake。RGA 设备节点的名称依系统而异。

## 构建与逐步验证

进入包含本文件与 `CMakeLists.txt` 的项目目录：

```bash
cmake -S . -B build
cmake --build build -j2
./build/capture_demo
./build/encode_demo test.h264
ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,width,height -of default=noprint_wrappers=1 test.h264
./build/record_demo test.mkv
```

Qt 程序单独构建：

```bash
cmake -S qt_app -B qt_app/build
cmake --build qt_app/build -j2
./qt_app/build/qt_window
```

MediaMTX 启动后，可运行 `./build/stream_demo rtsp://127.0.0.1:8554/live`，再从另一台机器拉流。

## 数据所有权

`captureFrame()` 复制一张 YUYV 到 `CapturedFrame::data` 后归还 V4L2 缓冲区。RGA 导入这块内存进行同步转换，输出直接写入 `Nv12Frame` 持有的 MPP DMA 缓冲区。编码、录像、推流沿用 V4L2 时间戳。转换完成前不得修改输入 `data`；编码完成前不得 `reset()` 输出 NV12 帧。

这一版仍有 V4L2 MMAP 到 `std::vector` 的 CPU 复制。要做零拷贝时，应单独改成导出摄像头 DMA-BUF fd，再交给 RGA；本次没有改变采集缓冲区的归还流程。

若 RGA 转换报错，先看程序输出的 `imStrError()`，再核对 `bytesperline`、librga 版本、设备节点权限以及 DMA-HEAP 权限。旧版本 librga 可能不支持 YUYV 输入。
