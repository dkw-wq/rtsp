#include "jitter_buffer.hpp"
#include <spdlog/spdlog.h>

namespace rtsp {

JitterBuffer::JitterBuffer(size_t maxSize, uint32_t latencyMs)
    : maxSize_(maxSize)
    , latencyMs_(latencyMs)
    , droppedFrames_(0)
    , totalFrames_(0)
    , lastPts_(0)
    , jitterSum_(0)
    , jitterCount_(0)
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

    // 如果缓冲区已满，丢弃最旧的帧
    if (buffer_.size() >= maxSize_) {
        buffer_.pop();
        droppedFrames_++;
    }

    buffer_.push(frame);
    totalFrames_++;

    // 计算抖动
    if (lastPts_ != 0 && frame->pts > lastPts_) {
        uint32_t jitter = static_cast<uint32_t>(frame->pts - lastPts_);
        jitterSum_ += jitter;
        jitterCount_++;
    }
    lastPts_ = static_cast<uint32_t>(frame->pts);

    cv_.notify_one();
    return true;
}

bool JitterBuffer::pop(std::shared_ptr<MediaFrame>& frame, uint32_t timeoutMs) {
    std::unique_lock<std::mutex> lock(mutex_);

    // 等待直到有数据或超时
    if (!cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), 
                     [this] { return !buffer_.empty(); })) {
        return false;
    }

    if (buffer_.empty()) {
        return false;
    }

    // 获取队首帧
    frame = buffer_.front();

    // 检查是否应该释放(基于延迟)
    if (!shouldRelease(frame)) {
        return false;
    }

    buffer_.pop();
    return true;
}

bool JitterBuffer::shouldRelease(const std::shared_ptr<MediaFrame>& frame) const {
    // 简单策略：关键帧立即释放，非关键帧需要累积一定数量
    if (frame->keyFrame) {
        return true;
    }

    const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
    const auto bufferedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - frame->recvTime).count();

    // 达到配置延迟后释放；缓冲积累过多时也释放，避免越积越慢
    return bufferedMs >= latencyMs_ || buffer_.size() >= (maxSize_ / 3);
}

void JitterBuffer::clear() {
    std::unique_lock<std::mutex> lock(mutex_);
    
    // 交换为空队列
    std::queue<std::shared_ptr<MediaFrame>> empty;
    buffer_.swap(empty);
    
    lastPts_ = 0;
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
    latencyMs_ = latencyMs;
}

JitterBuffer::Stats JitterBuffer::getStats() const {
    std::unique_lock<std::mutex> lock(mutex_);
    
    Stats stats;
    stats.bufferSize = buffer_.size();
    stats.droppedFrames = droppedFrames_;
    stats.totalFrames = totalFrames_;
    stats.avgJitter = (jitterCount_ > 0) ? (jitterSum_ / jitterCount_) : 0;
    
    return stats;
}

} // namespace rtsp
