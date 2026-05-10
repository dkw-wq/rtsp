#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace rtsp {

struct FaceDetectionOptions {
    bool enabled = false;
    std::string backend = "onnx_cpu";
    std::string modelPath = "models/scrfd_500m.onnx";
    int inputWidth = 640;
    int inputHeight = 640;
    int detectEveryNFrames = 5;
    float scoreThreshold = 0.5F;
    float nmsThreshold = 0.4F;
};

struct FaceBox {
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
    float score = 0.0F;
    float landmarks[10] = {};
};

struct FaceDetectionResult {
    size_t streamIndex = 0;
    int frameWidth = 0;
    int frameHeight = 0;
    std::vector<FaceBox> faces;
    double inferenceMs = 0.0;
    std::chrono::steady_clock::time_point timestamp{};
};

} // namespace rtsp
