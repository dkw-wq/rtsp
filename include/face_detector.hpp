#pragma once

#include "face_types.hpp"
#include "jitter_buffer.hpp"

#include <memory>
#include <string>
#include <vector>

namespace rtsp {

class IFaceDetector {
public:
    virtual ~IFaceDetector() = default;

    virtual bool initialize(const FaceDetectionOptions& options) = 0;
    virtual std::vector<FaceBox> detect(const MediaFrame& frame) = 0;
    virtual std::string backendName() const = 0;
};

std::unique_ptr<IFaceDetector> createFaceDetector(const FaceDetectionOptions& options);

} // namespace rtsp
