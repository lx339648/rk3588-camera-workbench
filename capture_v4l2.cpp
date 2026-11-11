#include "capture_v4l2.hpp"     // 本类的声明

#include <cerrno>                 // errno、EINTR
#include <cstring>                // std::strerror()

#include <fcntl.h>                // open()、O_RDWR、O_NONBLOCK
#include <linux/videodev2.h>      // V4L2 的结构体和 ioctl 命令
#include <poll.h>                 // poll()，等待新帧
#include <sys/ioctl.h>            // ioctl()
#include <sys/mman.h>             // mmap()、munmap()
#include <unistd.h>               // close()

namespace {                       // 这些辅助函数只在本 cpp 中使用

// ioctl 被信号打断时重试；其他错误交给调用者处理。
int cameraIoctl(int fd, unsigned long command, void* argument) {
    int result = -1;              // 保存 ioctl 的返回值
    do {                          // 至少调用一次 ioctl
        result = ioctl(fd, command, argument); // 把命令发给驱动
    } while (result < 0 && errno == EINTR);    // 被信号打断才重试
    return result;                // 返回成功或最终的错误
}

// 拼接当前系统错误，便于 main 打印准确原因。
std::string systemError(const char* operation) {
    return std::string(operation) + ": " + std::strerror(errno);
}

} // namespace

V4L2Capture::~V4L2Capture() {      // 对象离开作用域时自动清理
    stop();                        // 即使 main 忘记显式调用，也会释放资源
}

bool V4L2Capture::initialize(const CaptureConfig& config, std::string& error) {
    stop();                        // 若此前打开过，先关闭旧设备
    error.clear();                 // 清掉上一次留下的错误信息
    log_enabled_ = config.enable_frame_log;

    if (config.width == 0 || config.height == 0 || config.fps == 0 ||
        config.buffer_count < 2) { // 拒绝明显无效的参数
        error = "宽、高、帧率必须大于 0，缓冲区至少请求 2 块";
        return false;              // 初始化失败
    }

    // 统一的失败出口：写下原因，再清理已经申请的资源。
    auto fail = [&](const std::string& message) {
        error = message;           // 把错误交给 main 显示
        stop();                    // 包括停止队列、munmap 和 close
        return false;              // 返回初始化失败
    };

    fd_ = open(config.device.c_str(), O_RDWR | O_NONBLOCK); // 打开视频节点
    if (fd_ < 0) {                 // 检查设备是否打开成功
        return fail(systemError("open")); // 例如权限不足或节点不存在
    }

    v4l2_capability capability{};  // 清零设备能力结构体
    if (cameraIoctl(fd_, VIDIOC_QUERYCAP, &capability) < 0) { // 查询能力
        return fail(systemError("VIDIOC_QUERYCAP"));         // 查询失败
    }

    const uint32_t caps =          // 取当前 /dev/videoX 节点的能力
        (capability.capabilities & V4L2_CAP_DEVICE_CAPS)
            ? capability.device_caps // 驱动区分多节点时用 device_caps
            : capability.capabilities; // 否则使用总能力
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
        return fail("该节点不支持流式视频采集"); // 避免打开元数据节点
    }

    v4l2_format format{};          // 清零采集格式结构体
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; // 单平面视频采集
    format.fmt.pix.width = config.width;        // 请求画面宽度
    format.fmt.pix.height = config.height;      // 请求画面高度
    const uint32_t requested_format = config.pixel_format == CapturePixelFormat::Yuyv
        ? V4L2_PIX_FMT_YUYV : V4L2_PIX_FMT_MJPEG;
    format.fmt.pix.pixelformat = requested_format;
    format.fmt.pix.field = V4L2_FIELD_ANY;      // 由驱动选择场格式
    if (cameraIoctl(fd_, VIDIOC_S_FMT, &format) < 0) { // 设置采集格式
        return fail(systemError("VIDIOC_S_FMT"));     // 设置失败
    }
    if (format.fmt.pix.width != config.width ||       // 检查实际宽度
        format.fmt.pix.height != config.height ||     // 检查实际高度
        format.fmt.pix.pixelformat != requested_format) { // 检查实际格式
        return fail("驱动没有采用请求的分辨率或像素格式");
    }
    width_ = format.fmt.pix.width;
    height_ = format.fmt.pix.height;
    bytes_per_line_ = format.fmt.pix.bytesperline;
    pixel_format_ = config.pixel_format;
    if (pixel_format_ == CapturePixelFormat::Yuyv &&
        (bytes_per_line_ < width_ * 2 || bytes_per_line_ % 2 != 0)) {
        return fail("驱动返回的 YUYV 行跨度无效");
    }

    v4l2_streamparm rate{};       // 清零帧率参数结构体
    rate.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; // 采集队列的参数
    rate.parm.capture.timeperframe.numerator = 1;  // 每帧时间分子
    rate.parm.capture.timeperframe.denominator = config.fps; // 每帧时间分母
    if (cameraIoctl(fd_, VIDIOC_S_PARM, &rate) < 0) { // 请求帧率
        return fail(systemError("VIDIOC_S_PARM"));    // 无法设置则报告
    }
    const auto& interval = rate.parm.capture.timeperframe; // 驱动返回值
    if (interval.numerator == 0 || interval.denominator == 0) {
        return fail("驱动返回了无效的帧率"); // 防止除以 0
    }
    if (static_cast<double>(interval.denominator) / interval.numerator + 0.01 <
        config.fps) {              // 驱动若降到更低帧率，明确报告
        return fail("驱动没有采用请求的帧率");
    }

    v4l2_requestbuffers request{}; // 清零申请缓冲区的结构体
    request.count = config.buffer_count; // 请求的缓冲区数量
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; // 视频采集队列
    request.memory = V4L2_MEMORY_MMAP; // 使用 MMAP 方式
    if (cameraIoctl(fd_, VIDIOC_REQBUFS, &request) < 0) { // 请求分配
        return fail(systemError("VIDIOC_REQBUFS"));        // 申请失败
    }
    buffers_allocated_ = true;    // 从这里开始，清理时要清空驱动队列
    if (request.count < 2) {      // 驱动可能分配得比请求少
        return fail("驱动实际分配的缓冲区少于 2 块");
    }
    buffers_.resize(request.count); // 为每块驱动缓冲区留一条记录

    for (uint32_t i = 0; i < request.count; ++i) { // 逐块查询并映射
        v4l2_buffer buffer{};     // 清零缓冲区描述结构体
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; // 视频采集类型
        buffer.memory = V4L2_MEMORY_MMAP; // MMAP 内存类型
        buffer.index = i;         // 查询第 i 块缓冲区
        if (cameraIoctl(fd_, VIDIOC_QUERYBUF, &buffer) < 0) { // 查询长度/偏移
            return fail(systemError("VIDIOC_QUERYBUF"));      // 查询失败
        }
        void* address = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd_, buffer.m.offset); // 建立映射
        if (address == MAP_FAILED) { // 检查映射是否成功
            return fail(systemError("mmap")); // 之前映射的缓冲区也会清理
        }
        buffers_[i].address = address; // 记住映射地址
        buffers_[i].length = buffer.length; // 记住映射长度
    }

    for (uint32_t i = 0; i < request.count; ++i) { // 逐块把空缓冲区交给驱动
        v4l2_buffer buffer{};     // 清零缓冲区描述结构体
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; // 视频采集类型
        buffer.memory = V4L2_MEMORY_MMAP; // MMAP 内存类型
        buffer.index = i;         // 指定要入队的缓冲区
        if (cameraIoctl(fd_, VIDIOC_QBUF, &buffer) < 0) { // 缓冲区入队
            return fail(systemError("VIDIOC_QBUF"));    // 入队失败
        }
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE; // 指定要启动的队列
    if (cameraIoctl(fd_, VIDIOC_STREAMON, &type) < 0) { // 开始连续采集
        return fail(systemError("VIDIOC_STREAMON")); // 启动失败
    }
    return true;                 // 摄像头已经准备好，可以反复取帧
}

bool V4L2Capture::captureFrame(CapturedFrame& frame, std::string& error) {
    error.clear();               // 清掉上次取帧的错误
    frame.data.clear();          // 本次失败时不保留上一次成功帧的数据
    if (log_enabled_) {
        frame.log = {};
        frame.log.enabled = true;
    } else frame.log.enabled = false;
    if (fd_ < 0 || !buffers_allocated_) { // 必须先初始化
        error = "摄像头尚未初始化";
        return false;            // 无法取帧
    }

    pollfd wait_fd{};            // 清零等待结构体
    frame.log.mark(FramePoint::WaitStart);
    wait_fd.fd = fd_;            // 等待摄像头节点
    wait_fd.events = POLLIN;     // 等待有完整帧可取
    int ready = -1;              // 保存 poll 的结果
    do {                         // 被信号打断时重试等待
        ready = poll(&wait_fd, 1, 2000); // 最多等待 2 秒
    } while (ready < 0 && errno == EINTR);
    if (ready < 0) {             // 检查 poll 错误
        error = systemError("poll"); // 保存错误原因
        return false;            // 取帧失败
    }
    if (ready == 0) {            // 超时仍没有帧
        error = "等待摄像头新帧超时";
        return false;            // 本次取帧失败
    }
    if (!(wait_fd.revents & POLLIN)) { // 可能是设备断开等异常
        error = "摄像头没有返回可读取的帧";
        return false;            // 不执行可能阻塞的 DQBUF
    }

    v4l2_buffer buffer{};        // 清零接收帧的结构体
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; // 单平面视频采集
    buffer.memory = V4L2_MEMORY_MMAP; // MMAP 内存类型
    if (cameraIoctl(fd_, VIDIOC_DQBUF, &buffer) < 0) { // 取出一帧
        error = systemError("VIDIOC_DQBUF"); // 保存错误原因
        return false;            // 没有拿到有效帧
    }
    frame.log.mark(FramePoint::Dequeued);

    if (buffer.index >= buffers_.size()) { // 驱动返回无效 index
        error = "驱动返回了超出范围的缓冲区 index";
        return false;            // 无法安全访问或归还这块缓冲区
    }

    // 无论数据是否有效，已 DQBUF 的缓冲区都要尽快 QBUF 归还。
    const bool valid_data = !(buffer.flags & V4L2_BUF_FLAG_ERROR) &&
                            buffer.bytesused > 0 &&
                            buffer.bytesused <= buffers_[buffer.index].length &&
                            (pixel_format_ != CapturePixelFormat::Yuyv ||
                             static_cast<size_t>(buffer.bytesused) >=
                                 static_cast<size_t>(bytes_per_line_) * height_);
    if (valid_data) {            // 只复制有效帧
        const auto* first = static_cast<const uint8_t*>(buffers_[buffer.index].address);
        frame.data.assign(first, first + buffer.bytesused); // 复制到自己的内存
        frame.timestamp = buffer.timestamp; // 同时记住采集时间
        frame.timestamp_flags = buffer.flags & // 保留时间戳的解释方式
            (V4L2_BUF_FLAG_TIMESTAMP_MASK | V4L2_BUF_FLAG_TSTAMP_SRC_MASK);
        frame.sequence = buffer.sequence;   // 同时记住帧序号
        frame.width = width_;
        frame.height = height_;
        frame.bytes_per_line = bytes_per_line_;
        frame.pixel_format = pixel_format_;
        if (log_enabled_) frame.log.setCaptureInfo(
            frame.sequence, frame.timestamp, frame.timestamp_flags);
        frame.log.mark(FramePoint::Copied);
    } else {                     // 本帧损坏或长度不合理
        frame.data.clear();      // 不让调用方误用上一帧数据
    }

    v4l2_buffer returned{};      // 新建结构体，避免带回 DQBUF 的状态标志
    returned.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; // 采集队列
    returned.memory = V4L2_MEMORY_MMAP; // MMAP 类型
    returned.index = buffer.index; // 归还刚取出的那一块
    if (cameraIoctl(fd_, VIDIOC_QBUF, &returned) < 0) { // 放回驱动队列
        frame.data.clear();      // 设备状态异常，不交付这帧
        error = systemError("VIDIOC_QBUF"); // 报告归还失败
        return false;            // 本次取帧失败
    }
    frame.log.mark(FramePoint::Requeued);

    if (!valid_data) {           // 缓冲区已安全归还
        error = "摄像头返回了空帧、损坏帧或长度不匹配的帧";
        return false;            // 调用方可以决定是否重试
    }
    return true;                 // frame.data 现在由调用方安全持有
}

void V4L2Capture::stop() {        // 可以显式调用，也会被析构函数调用
    if (fd_ < 0) {                // 未打开时无需访问驱动
        return;                   // 直接结束清理
    }
    if (buffers_allocated_) {    // 已经申请过驱动缓冲区
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE; // 指定采集队列
        cameraIoctl(fd_, VIDIOC_STREAMOFF, &type); // 停止并清空队列
    }
    for (const MappedBuffer& buffer : buffers_) { // 遍历所有映射记录
        if (buffer.address != nullptr) { // 仅处理映射成功的缓冲区
            munmap(buffer.address, buffer.length); // 解除这块映射
        }
    }
    buffers_.clear();            // 清空程序保存的映射记录
    close(fd_);                  // 关闭摄像头节点
    fd_ = -1;                    // 标记为已关闭
    buffers_allocated_ = false; // 标记为未分配缓冲区
    width_ = height_ = bytes_per_line_ = 0;
}
