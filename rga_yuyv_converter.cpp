#define MODULE_TAG "rga_yuyv_converter"

#include "rga_yuyv_converter.hpp"

#include <cstring> // librga 的 imcheck 宏会调用全局 memset
#include <im2d.hpp>
#include <rga.h>
#include <limits>

namespace {
int align16(int value) { return (value + 15) & ~15; }
}

RgaYuyvConverter::~RgaYuyvConverter() { stop(); }

bool RgaYuyvConverter::initialize(int width, int height, std::string& error) {
    stop();
    error.clear();
    if (width <= 0 || height <= 0 || width % 2 || height % 2 ||
        width > std::numeric_limits<int>::max() - 15 ||
        height > std::numeric_limits<int>::max() - 15) {
        error = "RGA 需要正数且为偶数的 YUYV 宽高";
        return false;
    }
    width_ = width;
    height_ = height;
    horizontal_stride_ = align16(width);
    vertical_stride_ = align16(height);
    output_bytes_ = static_cast<size_t>(horizontal_stride_) * vertical_stride_ * 3 / 2;
    if (output_bytes_ > static_cast<size_t>(std::numeric_limits<int>::max())) {
        error = "NV12 图像大小超出 RGA 接口范围";
        stop();
        return false;
    }

    MPP_RET ret = mpp_buffer_group_get_internal(&group_, MPP_BUFFER_TYPE_DMA_HEAP);
    if (ret != MPP_OK) ret = mpp_buffer_group_get_internal(&group_, MPP_BUFFER_TYPE_DRM);
    if (ret != MPP_OK || !group_) {
        error = "无法建立 MPP DMA 缓冲区组，错误码 " + std::to_string(ret);
        stop();
        return false;
    }
    return true;
}

bool RgaYuyvConverter::convert(const CapturedFrame& input, Nv12Frame& output,
                               std::string& error) {
    output.reset();
    error.clear();
    if (!group_ || input.pixel_format != CapturePixelFormat::Yuyv ||
        input.width != static_cast<uint32_t>(width_) ||
        input.height != static_cast<uint32_t>(height_) ||
        input.bytes_per_line < input.width * 2 || input.bytes_per_line % 2 ||
        input.data.size() < static_cast<size_t>(input.bytes_per_line) * input.height ||
        input.data.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        error = "RGA 输入不是完整的预期 YUYV 帧";
        return false;
    }

    if (input.log.enabled) {
        output.log_ = input.log;
        output.log_.mark(FramePoint::ConvertStart);
    }

    const MPP_RET buffer_ret = mpp_buffer_get(group_, &output.buffer_, output_bytes_);
    if (buffer_ret != MPP_OK || !output.buffer_) {
        error = "申请 NV12 DMA 缓冲区失败，错误码 " + std::to_string(buffer_ret);
        output.reset();
        return false;
    }
    const MPP_RET frame_ret = mpp_frame_init(&output.frame_);
    if (frame_ret != MPP_OK || !output.frame_) {
        error = "创建 NV12 MPP 帧失败，错误码 " + std::to_string(frame_ret);
        output.reset();
        return false;
    }
    mpp_frame_set_width(output.frame_, width_);
    mpp_frame_set_height(output.frame_, height_);
    mpp_frame_set_hor_stride(output.frame_, horizontal_stride_);
    mpp_frame_set_ver_stride(output.frame_, vertical_stride_);
    mpp_frame_set_fmt(output.frame_, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(output.frame_, output.buffer_);

    const int output_fd = mpp_buffer_get_fd(output.buffer_);
    if (output_fd < 0) {
        error = "NV12 缓冲区没有可供 RGA 导入的 DMA fd";
        output.reset();
        return false;
    }

    // 当前 V4L2 采集模块已经把 MMAP 帧复制到 vector；此处只借用它的地址。
    // RGA 调用同步完成后才释放导入句柄和归还本帧的输入数据。
    auto* input_address = const_cast<uint8_t*>(input.data.data());
    rga_buffer_handle_t src_handle = importbuffer_virtualaddr(
        input_address, static_cast<int>(input.data.size()));
    rga_buffer_handle_t dst_handle = importbuffer_fd(
        output_fd, static_cast<int>(output_bytes_));
    if (!src_handle || !dst_handle) {
        error = std::string("RGA 导入图像缓冲区失败：") + imStrError();
        if (src_handle) releasebuffer_handle(src_handle);
        if (dst_handle) releasebuffer_handle(dst_handle);
        output.reset();
        return false;
    }

    const int source_stride_pixels = static_cast<int>(input.bytes_per_line / 2);
    rga_buffer_t src = wrapbuffer_handle(src_handle, width_, height_,
        RK_FORMAT_YUYV_422, source_stride_pixels, height_);
    rga_buffer_t dst = wrapbuffer_handle(dst_handle, width_, height_,
        RK_FORMAT_YCbCr_420_SP, horizontal_stride_, vertical_stride_);
    IM_STATUS status = imcheck(src, dst, {}, {});
    if (status == IM_STATUS_NOERROR) {
        status = imcvtcolor(src, dst, RK_FORMAT_YUYV_422, RK_FORMAT_YCbCr_420_SP);
    }
    if (status != IM_STATUS_SUCCESS) {
        error = std::string("RGA YUYV 转 NV12 失败：") + imStrError(status);
    }
    releasebuffer_handle(src_handle);
    releasebuffer_handle(dst_handle);
    if (status != IM_STATUS_SUCCESS) {
        output.reset();
        return false;
    }
    output.log_.mark(FramePoint::Converted);
    return true;
}

void RgaYuyvConverter::stop() {
    if (group_) {
        mpp_buffer_group_put(group_);
        group_ = nullptr;
    }
    width_ = height_ = horizontal_stride_ = vertical_stride_ = 0;
    output_bytes_ = 0;
}
