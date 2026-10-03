#include "jitter_buffer.hpp"

#include <algorithm>

namespace rtsp {

JitterBuffer::JitterBuffer(size_t maxSize, uint32_t latencyMs,
                           bool adaptive, uint32_t maxLatencyMs)
    : maxSize_(std::max<size_t>(maxSize, 1))
    , timing_(latencyMs, adaptive, maxLatencyMs)
    , overflowDroppedFrames_(0)
    , lateDroppedFrames_(0)
    , totalFrames_(0)
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
    if (!timing_.observeArrival(*frame)) {
        ++lateDroppedFrames_;
        return false;
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
        if (!buffer_.empty() && timing_.shouldRelease(*buffer_.front())) {
            frame = buffer_.front();
            buffer_.pop_front();
            timing_.noteReleased(*frame);
            return true;
        }
        if (timeoutMs == 0 || std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        auto wakeTime = deadline;
        if (!buffer_.empty()) {
            const auto readyTime = timing_.releaseTime(*buffer_.front());
            wakeTime = std::min(wakeTime, readyTime);
        }
        cv_.wait_until(lock, wakeTime);
    }
}

void JitterBuffer::clear() {
    std::unique_lock<std::mutex> lock(mutex_);
    
    buffer_.clear();
    overflowDroppedFrames_ = 0;
    lateDroppedFrames_ = 0;
    totalFrames_ = 0;
    timing_.reset();
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
    timing_.setLatency(latencyMs);
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
    stats.avgJitter = timing_.jitterMs();
    stats.targetLatencyMs = timing_.targetLatencyMs();
    
    return stats;
}

} // namespace rtsp
