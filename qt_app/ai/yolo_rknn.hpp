#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <rknn_api.h>

namespace vision {
struct Detection {
    int label;
    float score, x1, y1, x2, y2;  // Coordinates in the padded 640x640 image.
};

struct InferTiming {
    std::uint64_t inputNs = 0;
    std::uint64_t runNs = 0;
    std::uint64_t outputNs = 0;
    std::uint64_t decodeNs = 0;
    std::uint64_t releaseNs = 0;
};

// One synchronous context. Supply tightly packed HWC RGB bytes.
class YoloRknn {
public:
    static constexpr int side = 640;
    static constexpr std::size_t inputBytes = side * side * 3;
    explicit YoloRknn(const std::string& modelPath);
    ~YoloRknn();
    YoloRknn(const YoloRknn&) = delete;
    YoloRknn& operator=(const YoloRknn&) = delete;

    // When detections is null, still retrieve all nine float outputs for benchmarking.
    void infer(const std::uint8_t* rgb, std::size_t bytes,
               std::vector<Detection>* detections = nullptr,
               InferTiming* timing = nullptr);
private:
    rknn_context context_{};
};
} // namespace vision
