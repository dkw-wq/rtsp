#include "face_analyzer.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <utility>

#include <spdlog/spdlog.h>

namespace rtsp {

namespace {

std::shared_ptr<MediaFrame> cloneFrameForDetection(const std::shared_ptr<MediaFrame>& frame) {
    if (!frame || frame->type != MediaFrame::Type::VIDEO ||
        frame->pixelFormat != MediaFrame::PixelFormat::NV12) {
        return nullptr;
    }

    auto copy = std::make_shared<MediaFrame>(*frame);
    copy->hardwareFrameRef.reset();
    copy->gpuData = {0, 0};
    copy->gpuLinesize = {0, 0};
    return copy;
}

} // namespace

FaceAnalyzer::FaceAnalyzer() = default;

FaceAnalyzer::~FaceAnalyzer() {
    stop();
}

bool FaceAnalyzer::initialize(const FaceDetectionOptions& options) {
    stop();
    options_ = options;
    options_.detectEveryNFrames = std::max(options_.detectEveryNFrames, 1);

    if (!options_.enabled) {
        return false;
    }

    detector_ = createFaceDetector(options_);
    if (!detector_) {
        SPDLOG_WARN("Face detection disabled: no detector available for backend '{}'",
                    options_.backend);
        return false;
    }
    if (!detector_->initialize(options_)) {
        SPDLOG_WARN("Face detection disabled: failed to initialize backend '{}'",
                    options_.backend);
        detector_.reset();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_ = true;
        stopping_ = false;
        submittedFramesByStream_.clear();
        pendingFrames_.clear();
        nextPendingStream_ = 0;
        latestResults_.clear();
    }
    worker_ = std::thread(&FaceAnalyzer::workerLoop, this);
    SPDLOG_INFO("Face detection enabled: backend={}, model={}, input={}x{}, every_n_frames={}",
                detector_->backendName(),
                options_.modelPath,
                options_.inputWidth,
                options_.inputHeight,
                options_.detectEveryNFrames);
    return true;
}

void FaceAnalyzer::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        enabled_ = false;
        pendingFrames_.clear();
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    detector_.reset();
}

bool FaceAnalyzer::isEnabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_;
}

void FaceAnalyzer::submitFrame(const std::shared_ptr<MediaFrame>& frame, size_t streamIndex) {
    if (!frame) {
        return;
    }

    std::shared_ptr<MediaFrame> copiedFrame;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_ || stopping_) {
            return;
        }

        if (streamIndex >= submittedFramesByStream_.size()) {
            submittedFramesByStream_.resize(streamIndex + 1, 0);
        }
        ++submittedFramesByStream_[streamIndex];
        if (submittedFramesByStream_[streamIndex] %
                static_cast<uint64_t>(options_.detectEveryNFrames) !=
            0) {
            return;
        }
    }

    copiedFrame = cloneFrameForDetection(frame);
    if (!copiedFrame) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_ || stopping_) {
            return;
        }
        if (streamIndex >= pendingFrames_.size()) {
            pendingFrames_.resize(streamIndex + 1);
        }
        pendingFrames_[streamIndex].frame = std::move(copiedFrame);
        pendingFrames_[streamIndex].streamIndex = streamIndex;
    }
    cv_.notify_one();
}

std::vector<FaceDetectionResult> FaceAnalyzer::latestResults() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latestResults_;
}

void FaceAnalyzer::workerLoop() {
    while (true) {
        PendingFrame work;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return stopping_ ||
                       std::any_of(pendingFrames_.begin(), pendingFrames_.end(),
                                   [](const PendingFrame& pending) {
                                       return pending.frame != nullptr;
                                   });
            });
            if (stopping_) {
                return;
            }
            if (!takePendingFrameLocked(work)) {
                continue;
            }
        }

        const auto started = std::chrono::steady_clock::now();
        auto faces = detector_->detect(*work.frame);
        const auto finished = std::chrono::steady_clock::now();

        FaceDetectionResult result;
        result.streamIndex = work.streamIndex;
        result.frameWidth = work.frame->width;
        result.frameHeight = work.frame->height;
        result.faces = std::move(faces);
        result.inferenceMs =
            std::chrono::duration<double, std::milli>(finished - started).count();
        result.timestamp = finished;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto existing = std::find_if(latestResults_.begin(), latestResults_.end(),
                                         [&result](const FaceDetectionResult& current) {
                                             return current.streamIndex == result.streamIndex;
                                         });
            if (existing == latestResults_.end()) {
                latestResults_.push_back(std::move(result));
            } else {
                *existing = std::move(result);
            }
        }
    }
}

bool FaceAnalyzer::takePendingFrameLocked(PendingFrame& work) {
    if (pendingFrames_.empty()) {
        return false;
    }

    for (size_t attempt = 0; attempt < pendingFrames_.size(); ++attempt) {
        const size_t index = (nextPendingStream_ + attempt) % pendingFrames_.size();
        if (!pendingFrames_[index].frame) {
            continue;
        }

        work = std::move(pendingFrames_[index]);
        pendingFrames_[index] = {};
        nextPendingStream_ = (index + 1) % pendingFrames_.size();
        return true;
    }

    return false;
}

} // namespace rtsp
