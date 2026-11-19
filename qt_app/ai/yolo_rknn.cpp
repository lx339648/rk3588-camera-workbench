#include "yolo_rknn.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace vision {
namespace {
void check(int rc, const char* operation) {
    if (rc != 0) throw std::runtime_error(std::string(operation) + ": " + std::to_string(rc));
}

void validateModel(rknn_context ctx) {
    rknn_input_output_num io{};
    check(rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)), "query IO");
    if (io.n_input != 1 || io.n_output != 9)
        throw std::runtime_error("Expected Rockchip YOLOv8n with 1 input and 9 outputs");
    rknn_tensor_attr in{};
    check(rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &in, sizeof(in)), "query input");
    bool nhwc = in.fmt == RKNN_TENSOR_NHWC && in.dims[1] == 640 && in.dims[2] == 640 && in.dims[3] == 3;
    bool nchw = in.fmt == RKNN_TENSOR_NCHW && in.dims[1] == 3 && in.dims[2] == 640 && in.dims[3] == 640;
    if (in.n_dims != 4 || in.dims[0] != 1 || !(nhwc || nchw))
        throw std::runtime_error("Expected one 640x640 RGB input");
    constexpr unsigned channels[]{64, 80, 1};
    for (unsigned i = 0; i < 9; ++i) {
        rknn_tensor_attr out{};
        out.index = i;
        check(rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &out, sizeof(out)), "query output");
        unsigned grid = 80u >> (i / 3);
        if (out.fmt != RKNN_TENSOR_NCHW || out.n_dims != 4 || out.dims[0] != 1 ||
            out.dims[1] != channels[i % 3] || out.dims[2] != grid || out.dims[3] != grid)
            throw std::runtime_error("Unexpected YOLO head layout at output " + std::to_string(i));
    }
}

float distance(const float* logits, int cells) {
    float peak = logits[0];
    for (int k = 1; k < 16; ++k) peak = std::max(peak, logits[k*cells]);
    float mass = 0, weighted = 0;
    for (int k = 0; k < 16; ++k) {
        const float p = std::exp(logits[k*cells] - peak);
        mass += p;
        weighted += k*p;
    }
    return weighted/mass;
}

float iou(const Detection& a, const Detection& b) {
    float w = std::max(0.f, std::min(a.x2,b.x2) - std::max(a.x1,b.x1));
    float h = std::max(0.f, std::min(a.y2,b.y2) - std::max(a.y1,b.y1));
    float area = (a.x2-a.x1)*(a.y2-a.y1) + (b.x2-b.x1)*(b.y2-b.y1) - w*h;
    return area > 0 ? w*h/area : 0.f;
}

std::vector<Detection> decode(const std::array<rknn_output,9>& out) {
    std::vector<Detection> candidates;
    for (int head = 0; head < 3; ++head) {
        const int grid = 80 >> head, cells = grid*grid, stride = 640/grid;
        const auto* boxes = static_cast<const float*>(out[3*head].buf);
        const auto* scores = static_cast<const float*>(out[3*head+1].buf);
        for (int cell = 0; cell < cells; ++cell) {
            // 监控支路只输出 COCO 类别 0：person。
            constexpr int label = 0;
            const float score = scores[cell];
            if (score < 0.25f) continue;
            float d[4];
            for (int k = 0; k < 4; ++k) d[k] = distance(boxes + k*16*cells + cell, cells);
            float x = cell%grid + 0.5f, y = cell/grid + 0.5f;
            candidates.push_back({label, score, (x-d[0])*stride, (y-d[1])*stride,
                                  (x+d[2])*stride, (y+d[3])*stride});
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(),
        [](const Detection& a,const Detection& b){ return a.score > b.score; });
    std::vector<Detection> kept;
    for (const auto& candidate : candidates) {
        if (std::none_of(kept.begin(), kept.end(), [&](const Detection& other){
                return candidate.label == other.label && iou(candidate,other) > 0.45f;
            })) kept.push_back(candidate);
    }
    return kept;
}
} // namespace

YoloRknn::YoloRknn(const std::string& modelPath) {
    std::ifstream file(modelPath, std::ios::binary | std::ios::ate);
    const std::streamoff size = file.tellg();
    if (size <= 0 || size > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("Cannot read RKNN model: " + modelPath);
    std::vector<char> data(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!file.read(data.data(), size)) throw std::runtime_error("Incomplete RKNN file");
    check(rknn_init(&context_,data.data(),static_cast<std::uint32_t>(size),0,nullptr), "rknn_init");
    try {
        check(rknn_set_core_mask(context_,RKNN_NPU_CORE_0), "set NPU core 0");
        validateModel(context_);
    } catch (...) {
        rknn_destroy(context_);
        throw;
    }
}

YoloRknn::~YoloRknn() { rknn_destroy(context_); }

void YoloRknn::infer(const std::uint8_t* rgb, std::size_t bytes,
                     std::vector<Detection>* detections, InferTiming* timing) {
    if (!rgb || bytes != inputBytes) throw std::invalid_argument("Expected 640x640x3 RGB bytes");
    using Clock = std::chrono::steady_clock;
    auto durationNs = [](Clock::time_point from, Clock::time_point to) {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(to - from).count());
    };
    if (timing) *timing = {};
    rknn_input input{};
    input.buf = const_cast<std::uint8_t*>(rgb);
    input.size = static_cast<std::uint32_t>(bytes);
    input.type = RKNN_TENSOR_UINT8;
    input.fmt = RKNN_TENSOR_NHWC;
    const auto t0 = Clock::now();
    check(rknn_inputs_set(context_,1,&input), "rknn_inputs_set");
    const auto t1 = Clock::now();
    check(rknn_run(context_,nullptr), "rknn_run");
    const auto t2 = Clock::now();
    std::array<rknn_output,9> outputs{};
    for (unsigned i = 0; i < outputs.size(); ++i) {
        outputs[i].index = i;
        outputs[i].want_float = 1;
    }
    const auto tGetStart = Clock::now();
    check(rknn_outputs_get(context_,outputs.size(),outputs.data(),nullptr), "rknn_outputs_get");
    const auto t3 = Clock::now();
    try {
        if (detections) *detections = decode(outputs);
    } catch (...) {
        rknn_outputs_release(context_,outputs.size(),outputs.data());
        throw;
    }
    const auto t4 = Clock::now();
    check(rknn_outputs_release(context_,outputs.size(),outputs.data()), "rknn_outputs_release");
    const auto t5 = Clock::now();
    if (timing) {
        timing->inputNs = durationNs(t0, t1);
        timing->runNs = durationNs(t1, t2);
        timing->outputNs = durationNs(tGetStart, t3);
        timing->decodeNs = durationNs(t3, t4);
        timing->releaseNs = durationNs(t4, t5);
    }
}
} // namespace vision
