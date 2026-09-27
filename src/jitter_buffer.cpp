#include "jitter_buffer.hpp"

#include <algorithm>
#include <cmath>

namespace rtsp {

JitterBuffer::JitterBuffer(size_t maxSize, uint32_t latencyMs,
                           bool adaptive, uint32_t maxLatencyMs)
    : maxSize_(std::max<size_t>(maxSize, 1))
    , baseLatencyMs_(latencyMs)
    , targetLatencyMs_(latencyMs)
    , maxLatencyMs_(std::max(latencyMs, maxLatencyMs))
    , adaptive_(adaptive)
    , overflowDroppedFrames_(0)
    , lateDroppedFrames_(0)
    , totalFrames_(0)
    , lastPtsSeconds_(0.0)
    , lastReleasedPtsSeconds_(0.0)
    , lastRecvTime_(0)
    , jitterEstimateMs_(0.0)
    , hasLastArrival_(false)
    , hasLastRelease_(false)
{}

JitterBuffer::~JitterBuffer() = default;

bool JitterBuffer::push(const std::shared_ptr<MediaFrame>& frame) {
    if (!frame) {
        return false;
    }

    std::unique_lock<std::mutex> lock(mutex_);

    if (frame->recvTime.count() == 0) {
        frame->recvTime = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch());
    }

    ++totalFrames_;
    if (!std::isfinite(frame->ptsSeconds) ||
        (hasLastRelease_ && frame->ptsSeconds < lastReleasedPtsSeconds_)) {
        ++lateDroppedFrames_;
        return false;
    }

    if (hasLastArrival_ && frame->ptsSeconds > lastPtsSeconds_ &&
        frame->recvTime >= lastRecvTime_) {
        const double arrivalDeltaMs =
            static_cast<double>((frame->recvTime - lastRecvTime_).count()) / 1000.0;
        const double ptsDeltaMs = (frame->ptsSeconds - lastPtsSeconds_) * 1000.0;
        const double sampleMs = std::abs(arrivalDeltaMs - ptsDeltaMs);
        if (std::isfinite(sampleMs)) {
            jitterEstimateMs_ += (sampleMs - jitterEstimateMs_) / 16.0;
            updateTargetLatency();
        }
    }
    if (!hasLastArrival_ || frame->ptsSeconds > lastPtsSeconds_) {
        lastPtsSeconds_ = frame->ptsSeconds;
        lastRecvTime_ = frame->recvTime;
        hasLastArrival_ = true;
    }

    const auto position = std::upper_bound(
        buffer_.begin(), buffer_.end(), frame->ptsSeconds,
        [](double pts, const std::shared_ptr<MediaFrame>& queued) {
            return pts < queued->ptsSeconds;
        });
    buffer_.insert(position, frame);
    bool keptFrame = true;
    if (buffer_.size() > maxSize_) {
        keptFrame = buffer_.front() != frame;
        buffer_.pop_front();
        ++overflowDroppedFrames_;
    }

    cv_.notify_all();
    return keptFrame;
}

bool JitterBuffer::pop(std::shared_ptr<MediaFrame>& frame, uint32_t timeoutMs) {
    std::unique_lock<std::mutex> lock(mutex_);

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (true) {
        if (!buffer_.empty() && shouldRelease(buffer_.front())) {
            frame = buffer_.front();
            buffer_.pop_front();
            if (std::isfinite(frame->ptsSeconds)) {
                lastReleasedPtsSeconds_ = frame->ptsSeconds;
                hasLastRelease_ = true;
            }
            return true;
        }
        if (timeoutMs == 0 || std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        auto wakeTime = deadline;
        if (!buffer_.empty()) {
            const auto readyTime = std::chrono::steady_clock::time_point(
                buffer_.front()->recvTime + std::chrono::milliseconds(targetLatencyMs_));
            wakeTime = std::min(wakeTime, readyTime);
        }
        cv_.wait_until(lock, wakeTime);
    }
}

bool JitterBuffer::shouldRelease(const std::shared_ptr<MediaFrame>& frame) const {
    return std::chrono::steady_clock::now() >=
           std::chrono::steady_clock::time_point(
               frame->recvTime + std::chrono::milliseconds(targetLatencyMs_));
}

void JitterBuffer::updateTargetLatency() {
    if (!adaptive_ || baseLatencyMs_ == 0) {
        targetLatencyMs_ = baseLatencyMs_;
        return;
    }
    const double proposed = static_cast<double>(baseLatencyMs_) + 4.0 * jitterEstimateMs_;
    targetLatencyMs_ = proposed >= maxLatencyMs_
        ? maxLatencyMs_
        : static_cast<uint32_t>(std::ceil(proposed));
}

void JitterBuffer::clear() {
    std::unique_lock<std::mutex> lock(mutex_);
    
    buffer_.clear();
    overflowDroppedFrames_ = 0;
    lateDroppedFrames_ = 0;
    totalFrames_ = 0;
    lastPtsSeconds_ = 0.0;
    lastReleasedPtsSeconds_ = 0.0;
    lastRecvTime_ = std::chrono::microseconds(0);
    jitterEstimateMs_ = 0.0;
    targetLatencyMs_ = baseLatencyMs_;
    hasLastArrival_ = false;
    hasLastRelease_ = false;
    cv_.notify_all();
}

size_t JitterBuffer::size() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return buffer_.size();
}

bool JitterBuffer::empty() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return buffer_.empty();
}

void JitterBuffer::setLatency(uint32_t latencyMs) {
    std::unique_lock<std::mutex> lock(mutex_);
    baseLatencyMs_ = latencyMs;
    maxLatencyMs_ = std::max(maxLatencyMs_, baseLatencyMs_);
    updateTargetLatency();
    cv_.notify_all();
}

JitterBuffer::Stats JitterBuffer::getStats() const {
    std::unique_lock<std::mutex> lock(mutex_);
    
    Stats stats;
    stats.bufferSize = buffer_.size();
    stats.droppedFrames = overflowDroppedFrames_ + lateDroppedFrames_;
    stats.overflowDroppedFrames = overflowDroppedFrames_;
    stats.lateDroppedFrames = lateDroppedFrames_;
    stats.totalFrames = totalFrames_;
    stats.avgJitter = jitterEstimateMs_;
    stats.targetLatencyMs = targetLatencyMs_;
    
    return stats;
}

} // namespace rtsp
