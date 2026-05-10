#pragma once

#include "face_detector.hpp"
#include "face_types.hpp"
#include "jitter_buffer.hpp"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace rtsp {

class FaceAnalyzer {
public:
    FaceAnalyzer();
    ~FaceAnalyzer();

    FaceAnalyzer(const FaceAnalyzer&) = delete;
    FaceAnalyzer& operator=(const FaceAnalyzer&) = delete;

    bool initialize(const FaceDetectionOptions& options);
    void stop();
    bool isEnabled() const;

    void submitFrame(const std::shared_ptr<MediaFrame>& frame, size_t streamIndex);
    std::vector<FaceDetectionResult> latestResults() const;

private:
    struct PendingFrame {
        std::shared_ptr<MediaFrame> frame;
        size_t streamIndex = 0;
    };

    void workerLoop();
    bool takePendingFrameLocked(PendingFrame& work);

    FaceDetectionOptions options_;
    std::unique_ptr<IFaceDetector> detector_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    std::vector<PendingFrame> pendingFrames_;
    std::vector<FaceDetectionResult> latestResults_;
    std::vector<uint64_t> submittedFramesByStream_;
    size_t nextPendingStream_ = 0;
    bool enabled_ = false;
    bool stopping_ = false;
};

} // namespace rtsp
