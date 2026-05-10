#pragma once

#include "face_detector.hpp"

#include <memory>
#include <string>
#include <vector>

namespace rtsp {

class OnnxScrfdDetector final : public IFaceDetector {
public:
    OnnxScrfdDetector();
    ~OnnxScrfdDetector() override;

    bool initialize(const FaceDetectionOptions& options) override;
    std::vector<FaceBox> detect(const MediaFrame& frame) override;
    std::string backendName() const override;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl_;
};

} // namespace rtsp
