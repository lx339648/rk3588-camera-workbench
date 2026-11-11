# V4L2 相机工程

当前 D405C 主链路的构建和验证步骤见 [D405C_RGA.md](D405C_RGA.md)。下面保留原 MJPG 摄像头的学习记录；`decode_demo` 仍是旧摄像头测试程序。

这个目录按五个独立阶段练习：`capture_demo` 只采集 MJPG；`decode_demo` 继续用 MPP 解码为 NV12；`encode_demo` 再用 MPP 编码为 H.264；`record_demo` 将 H.264 连同采集 PTS 封装进 MKV；`stream_demo` 把实时 H.264 发布到板上的 RTSP 服务。Qt 尚未接入。

在 RK3588 Linux 板上，仅编译采集练习时执行：

```bash
g++ -std=c++17 -O2 -Wall -Wextra main.cpp capture_v4l2.cpp -o capture_demo
./capture_demo
```

如果设备节点不同，在 `main.cpp` 里修改 `config.device`；也可以改宽、高和帧率。当前采集格式固定为 MJPG，以适配这台摄像头的 1920×1080、30 fps 模式。

`main.cpp` 只用两个采集入口：`initialize()` 和 `captureFrame()`。每次 `captureFrame()` 返回时，驱动缓冲区已经通过 `VIDIOC_QBUF` 归还；`frame.data` 是程序自己的字节副本，可以安全地送往后续模块。`stop()` 负责停止采集、解除映射并关闭设备；不调用时，析构函数也会清理。

当前示例连续获取 90 帧并打印信息，**不会生成视频文件**。没有接上后续消费者时，这些字节只在内存中短暂存在。

## 下一阶段：MPP MJPG 解码

`mpp_mjpeg_decoder.hpp/.cpp` 是独立解码模块。输入 `CapturedFrame`，输出 `DecodedFrame`；后者拥有 MPP 像素缓冲区，使用完调用 `reset()`，或者等待对象析构。当前 `decode_main.cpp` 连续处理 300 帧并报告输出格式、stride 和耗时，不保存文件。

`decode()` 返回 `FrameReady`、`BadFrame` 或 `FatalError`。MPP 报告单帧解码失败时，示例会跳过并计数，继续处理下一张 JPEG；解码器初始化或输入输出接口出错时才终止。

这一路采用 MPP MJPEG 的预分配输出帧方式：每次给解码器提供一张 `MppFrame` 及其缓冲区，然后将它附在输入包的 `KEY_OUTPUT_FRAME` 元数据上。这个步骤是 MPP 的 MJPEG 解码要求，不能直接套用 H.264 解码的调用方式。

假设目录摆放为 `~/lx_ws/mpp`、`~/lx_ws/mpp-build`、`~/lx_ws/v4l2-capture-demo`，在板上执行：

```bash
cmake -S . -B build
cmake --build build -j2
./build/decode_demo
```

如果 CMake 没找到 MPP，先找实际库路径，再在配置时显式指定：

```bash
find ~/lx_ws/mpp-build -name 'librockchip_mpp.so*'
cmake -S . -B build -DMPP_INCLUDE_DIR="$HOME/lx_ws/mpp/inc" -DMPP_LIBRARY="实际找到的 .so 完整路径"
```

你之前的 `/dev/mpp_service` 只有 root 可访问；若运行时报设备权限错误，可先用 `sudo ./build/decode_demo` 验证。后续再为设备节点配置合适的用户权限。

## 第三阶段：MPP H.264 编码

`mpp_h264_encoder.hpp/.cpp` 接收解码器输出的 NV12 `DecodedFrame`，直接将 MPP 硬件缓冲区送到 H.264 编码器；此处不再经过 `sws_scale`，也不转成 `yuv420p`。独立的 `encode_main.cpp` 将真实摄像头的 300 帧依次采集、解码、编码，记录各阶段耗时，并生成验证用的 `test.h264` 裸流。`H264Encoder::encode()` 返回后，上游才释放本帧 NV12 缓冲区。

在 RK3588 板上将本目录新增的三个文件复制过去，然后重新配置并编译：

```bash
cmake -S . -B build
cmake --build build -j2
sudo ./build/encode_demo test.h264
```

验证文件确实包含可解码的 H.264 图像：

```bash
ffprobe -v error -select_streams v:0 -count_frames \
  -show_entries stream=codec_name,width,height,nb_read_frames \
  -of default=noprint_wrappers=1 test.h264
ffmpeg -hide_banner -loglevel error -i test.h264 -f null -
```

`test.h264` 是 H.264 裸流，没有 MKV/MP4 封装，也没有记录完整的逐帧时间信息；播放器用约定帧率播放时，时间可能不准确。这里先测**真实整链路**是否接近 30 fps。`EncodedFrame` 的字节和 `pts_us` 已可交给下一阶段的录像模块。若某帧 MJPG 损坏，该帧会被跳过；统计中的实际输出帧率会如实降低。

## 第四阶段：保存 MKV 文件

`mkv_recorder.hpp/.cpp` 只负责 MKV 封装，使用 FFmpeg 的 `libavformat`。它将编码器的 SPS/PPS 写入视频轨道信息，再把每个 `EncodedFrame` 写成一个视频包。`pts_us` 来自 V4L2 的采集时间戳，文件内从第一帧起算为 0；写入前按 MKV 实际的 `time_base` 换算。第一帧要求是 IDR 关键帧，之后每个关键帧都做标记，便于播放器跳转。

板上若没有 FFmpeg 开发包，先安装：

```bash
sudo apt install libavformat-dev libavcodec-dev libavutil-dev pkg-config
```

把新增文件放到同一工程目录后，重新配置并编译。CMake 输出里应出现 `record_demo`：

```bash
cmake -S . -B build
cmake --build build -j2
sudo ./build/record_demo /home/linux/lx_ws/test.mkv
```

检查文件里的帧数、时长和解码错误：

```bash
ffprobe -v error -select_streams v:0 -count_frames \
  -show_entries stream=codec_name,width,height,nb_read_frames:format=duration \
  -of default=noprint_wrappers=1 /home/linux/lx_ws/test.mkv
ffmpeg -hide_banner -loglevel error -i /home/linux/lx_ws/test.mkv -f null -
```

这一版只录约 300 帧，用来验证**封装和真实 PTS**。它尚未实现录像按钮、按次创建目录、SD 卡空间管理或 RTSP。这些功能将在基础录像文件确认可正常播放后继续接入。

## 第五阶段：实时 RTSP 推流

`stream_demo` 复用前面的采集、MJPG 解码和 H.264 硬编码模块。`rtsp_publisher.hpp/.cpp` 只负责把编码帧和采集时间戳交给 FFmpeg 的 RTSP 封装器；不会再次编码。当前不调用 `MkvRecorder`，录像模块保留给后续 Qt 录像按钮使用。程序是单线程串行验证版，网络发送慢时会影响采集速度；跑通后再按实际测试结果拆队列和线程。

RTSP 发布端设置 `max_delay = 100000` 微秒，对应 FFmpeg 命令行的 `-muxdelay 0.1`。与接收端 VLC 的网络缓存分开设置；实际端到端延迟还需用同一动作或计时画面对比测试。

先在板上启动 MediaMTX，保持该终端运行；再在另一个终端编译、启动推流：

```bash
cd ~/lx_ws/mediamtx
./mediamtx
```

```bash
cd ~/lx_ws/hard_camera
cmake -S . -B build
cmake --build build -j2
sudo ./build/stream_demo rtsp://127.0.0.1:8554/live
```

如果 MediaMTX 已在后台运行，不要再启动第二份。看到 `已发送 30 帧` 等输出后，电脑 VLC 打开 `rtsp://192.168.31.17:8554/live`；这里的 IP 是此前板卡 `uap0` 的地址，变化时要换成当前板卡地址。板上的程序始终向本机 MediaMTX 发布，电脑负责拉流。按 Ctrl+C 结束。若无法连接或发送中断，程序会报告错误并退出；当前阶段手动重启即可。
