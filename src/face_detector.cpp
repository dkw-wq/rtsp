#include "face_detector.hpp"

#include "config_loader.hpp"

#include <spdlog/spdlog.h>

namespace rtsp {

std::unique_ptr<IFaceDetector> createFaceDetector(const FaceDetectionOptions& options) {
    const std::string backend = toLower(options.backend);
    SPDLOG_WARN("Face detection backend '{}' is not built yet; install ONNX Runtime/OpenCV "
                "then add OnnxScrfdDetector",
                backend);
    return nullptr;
}

} // namespace rtsp
