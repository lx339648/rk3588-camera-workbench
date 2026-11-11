#include "nv12_frame.hpp"

Nv12Frame::~Nv12Frame() { reset(); }

void Nv12Frame::reset() {
    if (frame_) mpp_frame_deinit(&frame_);
    if (buffer_) {
        mpp_buffer_put(buffer_);
        buffer_ = nullptr;
    }
    log_ = {};
}

bool Nv12Frame::valid() const { return frame_ && buffer_; }
int Nv12Frame::width() const { return valid() ? mpp_frame_get_width(frame_) : 0; }
int Nv12Frame::height() const { return valid() ? mpp_frame_get_height(frame_) : 0; }
int Nv12Frame::horizontalStride() const {
    return valid() ? mpp_frame_get_hor_stride(frame_) : 0;
}
int Nv12Frame::verticalStride() const {
    return valid() ? mpp_frame_get_ver_stride(frame_) : 0;
}
MppFrameFormat Nv12Frame::format() const {
    return valid() ? mpp_frame_get_fmt(frame_) : MPP_FMT_BUTT;
}
MppFrame Nv12Frame::nativeFrame() const { return frame_; }
