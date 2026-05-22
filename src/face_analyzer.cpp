#include "face_analyzer.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <utility>

#include <spdlog/spdlog.h>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

namespace rtsp {

namespace {

std::string ffmpegError(int errorCode) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errorCode, errbuf, sizeof(errbuf));
    return errbuf;
}

void copyPlane(uint8_t* dst, int dstStride, const uint8_t* src,
               int srcStride, int width, int height) {
    for (int row = 0; row < height; ++row) {
        std::memcpy(dst + row * dstStride, src + row * srcStride, width);
    }
}

std::shared_ptr<MediaFrame> transferCudaFrameForDetection(
    const std::shared_ptr<MediaFrame>& frame) {
    if (!frame || frame->pixelFormat != MediaFrame::PixelFormat::CUDA_NV12 ||
        !frame->hardwareFrameRef) {
        return nullptr;
    }

    AVFrame* hardwareFrame = static_cast<AVFrame*>(frame->hardwareFrameRef.get());
    if (!hardwareFrame) {
        return nullptr;
    }

    AVFrame* softwareFrame = av_frame_alloc();
    if (!softwareFrame) {
        SPDLOG_WARN("Face detection failed to allocate CPU AVFrame for CUDA readback");
        return nullptr;
    }

    const int transferResult = av_hwframe_transfer_data(softwareFrame, hardwareFrame, 0);
    if (transferResult < 0) {
        SPDLOG_WARN("Face detection failed to transfer CUDA frame to CPU: {}",
                    ffmpegError(transferResult));
        av_frame_free(&softwareFrame);
        return nullptr;
    }

    const auto cleanupFrame = std::unique_ptr<AVFrame, void (*)(AVFrame*)>(
        softwareFrame,
        [](AVFrame* framePtr) {
            AVFrame* frameToFree = framePtr;
            av_frame_free(&frameToFree);
        });

    if (softwareFrame->format != AV_PIX_FMT_NV12 ||
        softwareFrame->data[0] == nullptr ||
        softwareFrame->data[1] == nullptr ||
        softwareFrame->linesize[0] <= 0 ||
        softwareFrame->linesize[1] <= 0) {
        SPDLOG_WARN("Face detection CUDA readback produced unsupported pixel format {}",
                    softwareFrame->format);
        return nullptr;
    }

    auto copy = std::make_shared<MediaFrame>(*frame);
    copy->pixelFormat = MediaFrame::PixelFormat::NV12;
    copy->hardwareFrameRef.reset();
    copy->gpuData = {0, 0};
    copy->gpuLinesize = {0, 0};
    copy->width = softwareFrame->width;
    copy->height = softwareFrame->height;

    const int ySize = copy->width * copy->height;
    copy->data.resize(static_cast<size_t>(ySize) * 3U / 2U);

    copyPlane(copy->data.data(),
              copy->width,
              softwareFrame->data[0],
              softwareFrame->linesize[0],
              copy->width,
              copy->height);
    copyPlane(copy->data.data() + ySize,
              copy->width,
              softwareFrame->data[1],
              softwareFrame->linesize[1],
              copy->width,
              copy->height / 2);

    return copy;
}

std::shared_ptr<MediaFrame> cloneFrameForDetection(const std::shared_ptr<MediaFrame>& frame) {
    if (!frame || frame->type != MediaFrame::Type::VIDEO) {
        return nullptr;
    }

    if (frame->pixelFormat == MediaFrame::PixelFormat::CUDA_NV12) {
        auto copy = std::make_shared<MediaFrame>(*frame);
        return copy;
    }

    if (frame->pixelFormat != MediaFrame::PixelFormat::NV12) {
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

        auto detectionFrame = work.frame;
        if (detectionFrame &&
            detectionFrame->pixelFormat == MediaFrame::PixelFormat::CUDA_NV12) {
            detectionFrame = transferCudaFrameForDetection(detectionFrame);
        }
        if (!detectionFrame) {
            continue;
        }

        const auto started = std::chrono::steady_clock::now();
        auto faces = detector_->detect(*detectionFrame);
        const auto finished = std::chrono::steady_clock::now();

        FaceDetectionResult result;
        result.streamIndex = work.streamIndex;
        result.frameWidth = detectionFrame->width;
        result.frameHeight = detectionFrame->height;
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
