#include "sync_controller.hpp"

#include "audio_player.hpp"
#include "jitter_buffer.hpp"
#include "video_renderer.hpp"

#include <algorithm>
#include <cmath>

#include <spdlog/spdlog.h>

namespace rtsp {

AudioPlaybackOptions normalizeAudioPlaybackOptions(AudioPlaybackOptions options) {
    options.targetLatencyMs = std::clamp(options.targetLatencyMs, 0, 5000);
    options.maxQueueMs = std::max(options.maxQueueMs, 500);
    options.maxQueueMs = std::max(options.maxQueueMs, options.targetLatencyMs + 500);
    options.hardResetQueueMs = std::max(options.hardResetQueueMs, options.maxQueueMs + 500);
    return options;
}

AudioPlaybackTiming::AudioPlaybackTiming(const AudioPlaybackOptions& options) {
    const auto normalized = normalizeAudioPlaybackOptions(options);
    targetLatencyMs_ = static_cast<uint32_t>(normalized.targetLatencyMs);
    hardResetQueueMs_ = static_cast<uint32_t>(normalized.hardResetQueueMs);
}

void AudioPlaybackTiming::reset() {
    queuedAudioEndSeconds_ = 0.0;
    hasClock_ = false;
    playbackStarted_ = false;
}

void AudioPlaybackTiming::resetForDevice() {
    reset();
    playbackStarted_ = targetLatencyMs_ == 0;
}

bool AudioPlaybackTiming::shouldResetQueue(uint32_t queuedMs) const {
    return queuedMs > hardResetQueueMs_;
}

void AudioPlaybackTiming::resetQueue(double ptsSeconds) {
    queuedAudioEndSeconds_ = ptsSeconds;
    playbackStarted_ = true;
}

void AudioPlaybackTiming::noteQueuedFrame(double ptsSeconds, double durationSeconds) {
    queuedAudioEndSeconds_ = std::max(queuedAudioEndSeconds_, ptsSeconds + durationSeconds);
    hasClock_ = true;
}

bool AudioPlaybackTiming::startIfReady(uint32_t queuedMs) {
    if (playbackStarted_ || queuedMs < targetLatencyMs_) {
        return false;
    }
    playbackStarted_ = true;
    return true;
}

bool AudioPlaybackTiming::active() const {
    return playbackStarted_;
}

bool AudioPlaybackTiming::hasClock() const {
    return hasClock_;
}

double AudioPlaybackTiming::clockSeconds(double queuedSeconds) const {
    return hasClock_ ? queuedAudioEndSeconds_ - queuedSeconds : 0.0;
}

void VideoTimestampTracker::reset() {
    nextSyntheticPtsSeconds_ = 0.0;
    lastPtsSeconds_ = 0.0;
    hasLastPts_ = false;
}

double VideoTimestampTracker::stabilize(double candidateSeconds, bool hasCandidate,
                                       double frameDurationSeconds) {
    bool useCandidate = hasCandidate;
    if (hasLastPts_ && hasCandidate) {
        const double delta = candidateSeconds - lastPtsSeconds_;
        const double maxReasonableDelta = std::max(0.20, frameDurationSeconds * 4.0);
        if (delta <= 0.0 || delta > maxReasonableDelta) {
            useCandidate = false;
        }
    }

    const double ptsSeconds = useCandidate ? candidateSeconds : nextSyntheticPtsSeconds_;
    hasLastPts_ = true;
    lastPtsSeconds_ = ptsSeconds;
    nextSyntheticPtsSeconds_ = ptsSeconds + frameDurationSeconds;
    return ptsSeconds;
}

JitterBufferTiming::JitterBufferTiming(uint32_t latencyMs, bool adaptive, uint32_t maxLatencyMs)
    : baseLatencyMs_(latencyMs)
    , targetLatencyMs_(latencyMs)
    , maxLatencyMs_(std::max(latencyMs, maxLatencyMs))
    , adaptive_(adaptive) {}

void JitterBufferTiming::reset() {
    lastPtsSeconds_ = 0.0;
    lastReleasedPtsSeconds_ = 0.0;
    lastRecvTime_ = std::chrono::microseconds(0);
    jitterEstimateMs_ = 0.0;
    targetLatencyMs_ = baseLatencyMs_;
    hasLastArrival_ = false;
    hasLastRelease_ = false;
}

bool JitterBufferTiming::observeArrival(const MediaFrame& frame) {
    if (!std::isfinite(frame.ptsSeconds) ||
        (hasLastRelease_ && frame.ptsSeconds < lastReleasedPtsSeconds_)) {
        return false;
    }

    if (hasLastArrival_ && frame.ptsSeconds > lastPtsSeconds_ &&
        frame.recvTime >= lastRecvTime_) {
        const double arrivalDeltaMs =
            static_cast<double>((frame.recvTime - lastRecvTime_).count()) / 1000.0;
        const double ptsDeltaMs = (frame.ptsSeconds - lastPtsSeconds_) * 1000.0;
        const double sampleMs = std::abs(arrivalDeltaMs - ptsDeltaMs);
        if (std::isfinite(sampleMs)) {
            jitterEstimateMs_ += (sampleMs - jitterEstimateMs_) / 16.0;
            updateTargetLatency();
        }
    }
    if (!hasLastArrival_ || frame.ptsSeconds > lastPtsSeconds_) {
        lastPtsSeconds_ = frame.ptsSeconds;
        lastRecvTime_ = frame.recvTime;
        hasLastArrival_ = true;
    }
    return true;
}

void JitterBufferTiming::noteReleased(const MediaFrame& frame) {
    if (std::isfinite(frame.ptsSeconds)) {
        lastReleasedPtsSeconds_ = frame.ptsSeconds;
        hasLastRelease_ = true;
    }
}

std::chrono::steady_clock::time_point JitterBufferTiming::releaseTime(
    const MediaFrame& frame) const {
    return std::chrono::steady_clock::time_point(
        frame.recvTime + std::chrono::milliseconds(targetLatencyMs_));
}

bool JitterBufferTiming::shouldRelease(const MediaFrame& frame) const {
    return std::chrono::steady_clock::now() >= releaseTime(frame);
}

void JitterBufferTiming::setLatency(uint32_t latencyMs) {
    baseLatencyMs_ = latencyMs;
    maxLatencyMs_ = std::max(maxLatencyMs_, baseLatencyMs_);
    updateTargetLatency();
}

uint32_t JitterBufferTiming::targetLatencyMs() const {
    return targetLatencyMs_;
}

double JitterBufferTiming::jitterMs() const {
    return jitterEstimateMs_;
}

void JitterBufferTiming::updateTargetLatency() {
    if (!adaptive_ || baseLatencyMs_ == 0) {
        targetLatencyMs_ = baseLatencyMs_;
        return;
    }
    const double proposed = static_cast<double>(baseLatencyMs_) + 4.0 * jitterEstimateMs_;
    targetLatencyMs_ = proposed >= maxLatencyMs_
        ? maxLatencyMs_
        : static_cast<uint32_t>(std::ceil(proposed));
}

void updateFrameLatency(const std::shared_ptr<MediaFrame>& currentFrame,
                        PlaybackStats& stats) {
    if (!currentFrame || currentFrame->recvTime.count() <= 0) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto frameRecvTime = std::chrono::steady_clock::time_point(currentFrame->recvTime);
    stats.latencyMs = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - frameRecvTime).count());
}

namespace {

std::chrono::steady_clock::time_point frameTargetTimeByReceiveTime(
    const std::shared_ptr<MediaFrame>& frame,
    int targetDelayMs) {
    const auto frameRecvTime = std::chrono::steady_clock::time_point(frame->recvTime);
    return frameRecvTime + std::chrono::milliseconds(std::max(0, targetDelayMs));
}

} // namespace

SingleStreamSyncController::SingleStreamSyncController(SyncOptions options)
    : options_(options) {}

void SingleStreamSyncController::reset() {
    droppedFrames_ = 0;
    initialized_ = false;
    videoBaseSeconds_ = 0.0;
    audioBaseSeconds_ = 0.0;
    audioBaseInitialized_ = false;
    wallBaseTime_ = {};
}

uint64_t SingleStreamSyncController::droppedFrames() const {
    return droppedFrames_;
}

bool SingleStreamSyncController::shouldHoldForAudioStartup(
    const AudioPlaybackStats& audioStats) const {
    return options_.enabled &&
           audioStats.sampleRate > 0 &&
           !audioStats.active &&
           audioStats.queuedMs < audioStats.targetLatencyMs;
}

SyncDecision SingleStreamSyncController::synchronize(
    std::shared_ptr<MediaFrame>& frame,
    std::shared_ptr<MediaFrame>& pendingVideoFrame,
    JitterBuffer& jitterBuffer,
    const AudioPlayer& audioPlayer,
    PlaybackStats& playbackStats) {
    const auto audioStats = audioPlayer.getStats();
    playbackStats.audioActive = audioStats.active;
    playbackStats.audioQueueMs = audioStats.queuedMs;
    playbackStats.audioDroppedFrames = audioStats.droppedFrames;
    uint64_t catchUpFrames = 0;

    if (shouldHoldForAudioStartup(audioStats)) {
        ++droppedFrames_;
        std::shared_ptr<MediaFrame> staleFrame;
        while (jitterBuffer.pop(staleFrame, 0)) {
            ++droppedFrames_;
        }
        playbackStats.syncDroppedFrames = droppedFrames_;
        pendingVideoFrame.reset();
        return {SyncDecision::Type::Wait, 5};
    }

    if (!options_.enabled || frame->ptsSeconds <= 0.0) {
        playbackStats.avSyncDiffMs = 0;
        if (!options_.enabled || !audioPlayer.hasClock()) {
            initialized_ = false;
            audioBaseInitialized_ = false;
        }
        playbackStats.syncDroppedFrames = droppedFrames_;
        return {};
    }

    if (!initialized_) {
        initialized_ = true;
        videoBaseSeconds_ = frame->ptsSeconds;
        wallBaseTime_ = std::chrono::steady_clock::now();
        SPDLOG_INFO("Video sync base: video={:.3f}s", videoBaseSeconds_);
    }

    const auto wallElapsedSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - wallBaseTime_).count();
    double videoDelayMs =
        ((frame->ptsSeconds - videoBaseSeconds_) - wallElapsedSeconds) * 1000.0;

    if (audioPlayer.hasClock()) {
        const double audioClock = audioPlayer.clockSeconds();
        if (!audioBaseInitialized_) {
            audioBaseInitialized_ = true;
            audioBaseSeconds_ = audioClock;
            SPDLOG_INFO("A/V sync base: video={:.3f}s, audio={:.3f}s",
                        videoBaseSeconds_, audioBaseSeconds_);
        }

        auto frameAudioDiffMs = [&](const std::shared_ptr<MediaFrame>& currentFrame) {
            return ((currentFrame->ptsSeconds - videoBaseSeconds_) -
                    (audioClock - audioBaseSeconds_)) * 1000.0;
        };

        double avDiffMs = frameAudioDiffMs(frame);
        while (options_.lateDropMs > 0 &&
               avDiffMs < -static_cast<double>(options_.lateDropMs)) {
            ++droppedFrames_;

            std::shared_ptr<MediaFrame> catchUpFrame;
            if (!jitterBuffer.pop(catchUpFrame, 0)) {
                pendingVideoFrame.reset();
                playbackStats.syncDroppedFrames = droppedFrames_;
                return {SyncDecision::Type::WaitingForNewerFrame, 0, catchUpFrames};
            }

            pendingVideoFrame = catchUpFrame;
            frame = pendingVideoFrame;
            ++catchUpFrames;
            updateFrameLatency(frame, playbackStats);
            avDiffMs = frameAudioDiffMs(frame);
        }

        playbackStats.avSyncDiffMs = static_cast<int32_t>(std::lround(avDiffMs));
        playbackStats.syncDroppedFrames = droppedFrames_;

        if (avDiffMs > 1.0 && options_.maxWaitMs > 0) {
            const int waitMs =
                std::clamp(static_cast<int>(std::ceil(avDiffMs)), 1, options_.maxWaitMs);
            return {SyncDecision::Type::Wait, waitMs, catchUpFrames};
        }

        return {SyncDecision::Type::Render, 0, catchUpFrames};
    }

    playbackStats.avSyncDiffMs = 0;
    audioBaseInitialized_ = false;

    if (videoDelayMs > 1.0 && options_.maxWaitMs > 0) {
        const int waitMs =
            std::clamp(static_cast<int>(std::ceil(videoDelayMs)), 1, options_.maxWaitMs);
        playbackStats.syncDroppedFrames = droppedFrames_;
        return {SyncDecision::Type::Wait, waitMs};
    }

    while (options_.lateDropMs > 0 &&
           videoDelayMs < -static_cast<double>(options_.lateDropMs)) {
        ++droppedFrames_;

        std::shared_ptr<MediaFrame> catchUpFrame;
        if (!jitterBuffer.pop(catchUpFrame, 0)) {
            pendingVideoFrame.reset();
            playbackStats.syncDroppedFrames = droppedFrames_;
            return {SyncDecision::Type::WaitingForNewerFrame, 0, catchUpFrames};
        }

        pendingVideoFrame = catchUpFrame;
        frame = pendingVideoFrame;
        ++catchUpFrames;
        updateFrameLatency(frame, playbackStats);

        const auto updatedWallElapsedSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - wallBaseTime_).count();
        videoDelayMs =
            ((frame->ptsSeconds - videoBaseSeconds_) - updatedWallElapsedSeconds) * 1000.0;
    }

    playbackStats.syncDroppedFrames = droppedFrames_;
    return {SyncDecision::Type::Render, 0, catchUpFrames};
}

MultiStreamSyncController::MultiStreamSyncController(SyncOptions options, int audioTargetLatencyMs)
    : options_(options)
    , audioTargetLatencyMs_(audioTargetLatencyMs) {}

SyncDecision MultiStreamSyncController::synchronize(
    size_t streamIndex,
    std::shared_ptr<MediaFrame>& pendingVideoFrame,
    JitterBuffer& jitterBuffer,
    bool hasAudioClock,
    PlaybackStats& playbackStats) const {
    if (streamIndex != 0 || !options_.enabled || !hasAudioClock) {
        return {};
    }

    const int targetDelayMs = audioTargetLatencyMs_ + options_.audioOffsetMs;
    const auto now = std::chrono::steady_clock::now();
    const auto targetTime = frameTargetTimeByReceiveTime(pendingVideoFrame, targetDelayMs);
    if (now < targetTime) {
        const int waitMs = std::clamp(
            static_cast<int>(std::ceil(
                std::chrono::duration<double, std::milli>(targetTime - now).count())),
            1, std::max(options_.maxWaitMs, 1));
        playbackStats.avSyncDiffMs = waitMs;
        return {SyncDecision::Type::Wait, waitMs};
    }

    uint64_t catchUpFrames = 0;
    auto lateMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - targetTime).count();
    // Consume another ready frame only after the current frame misses its deadline.
    while (options_.lateDropMs > 0 && lateMs > static_cast<int64_t>(options_.lateDropMs)) {
        ++playbackStats.syncDroppedFrames;
        std::shared_ptr<MediaFrame> nextFrame;
        if (!jitterBuffer.pop(nextFrame, 0)) {
            pendingVideoFrame.reset();
            return {SyncDecision::Type::WaitingForNewerFrame, 0, catchUpFrames};
        }
        pendingVideoFrame = nextFrame;
        ++catchUpFrames;
        const auto updatedTargetTime = frameTargetTimeByReceiveTime(pendingVideoFrame, targetDelayMs);
        lateMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - updatedTargetTime).count();
    }

    return {SyncDecision::Type::Render, 0, catchUpFrames};
}

} // namespace rtsp
