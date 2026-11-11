#pragma once

#include "capture_v4l2.hpp"
#include "nv12_frame.hpp"

#include <mpp_buffer.h>
#include <string>

// D405C YUYV -> NV12。RGA 负责像素转换，输出直接放在 MPP 可用的 DMA 缓冲区。
class RgaYuyvConverter {
public:
    RgaYuyvConverter() = default;
    ~RgaYuyvConverter();
    RgaYuyvConverter(const RgaYuyvConverter&) = delete;
    RgaYuyvConverter& operator=(const RgaYuyvConverter&) = delete;

    bool initialize(int width, int height, std::string& error);
    bool convert(const CapturedFrame& input, Nv12Frame& output, std::string& error);
    void stop();

private:
    MppBufferGroup group_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int horizontal_stride_ = 0;
    int vertical_stride_ = 0;
    size_t output_bytes_ = 0;
};
