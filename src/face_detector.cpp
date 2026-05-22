#include "face_detector.hpp"

#include "config_loader.hpp"
#include "onnx_scrfd_detector.hpp"

#include <spdlog/spdlog.h>

namespace rtsp {

std::unique_ptr<IFaceDetector> createFaceDetector(const FaceDetectionOptions& options) {
    const std::string backend = toLower(options.backend);
    if (backend == "onnx_cpu" || backend == "onnx_cuda" || backend == "cuda" ||
        backend == "scrfd" || backend == "onnx") {
        return std::make_unique<OnnxScrfdDetector>();
    }

    SPDLOG_WARN("Unsupported face detection backend '{}'; use onnx_cpu or onnx_cuda", backend);
    return nullptr;
}

} // namespace rtsp
