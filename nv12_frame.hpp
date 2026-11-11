#pragma once

#include <mpp_buffer.h>
#include <rk_mpi.h>
#include "frame_log.hpp"

// 一张供 MPP 编码器使用的 NV12 图像，负责释放 MPP 帧和像素缓冲区。
class Nv12Frame {
public:
    Nv12Frame() = default;
    ~Nv12Frame();
    Nv12Frame(const Nv12Frame&) = delete;
    Nv12Frame& operator=(const Nv12Frame&) = delete;

    void reset();
    bool valid() const;
    int width() const;
    int height() const;
    int horizontalStride() const;
    int verticalStride() const;
    MppFrameFormat format() const;
    MppFrame nativeFrame() const;
    const FrameLog& log() const { return log_; }

private:
    friend class MjpegDecoder;
    friend class RgaYuyvConverter;
    MppFrame frame_ = nullptr;
    MppBuffer buffer_ = nullptr;
    FrameLog log_;
};
