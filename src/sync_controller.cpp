#include "sync_controller.hpp"

#include "jitter_buffer.hpp"

#include <algorithm>
#include <cmath>

#include <spdlog/spdlog.h>

namespace rtsp {

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

std::chrono::steady_clock::time_point frameTargetTimeByReceiveTime(
    const std::shared_ptr<MediaFrame>& frame,
    int targetDelayMs) {
    const auto frameRecvTime = std::chrono::steady_clock::time_point(frame->recvTime);
    return frameRecvTime + std::chrono::milliseconds(std::max(0, targetDelayMs));
}

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
                return {SyncDecision::Type::WaitingForNewerFrame, 0};
            }

            pendingVideoFrame = catchUpFrame;
            frame = pendingVideoFrame;
            updateFrameLatency(frame, playbackStats);
            avDiffMs = frameAudioDiffMs(frame);
        }

        playbackStats.avSyncDiffMs = static_cast<int32_t>(std::lround(avDiffMs));
        playbackStats.syncDroppedFrames = droppedFrames_;

        if (avDiffMs > 1.0 && options_.maxWaitMs > 0) {
            const int waitMs =
                std::clamp(static_cast<int>(std::ceil(avDiffMs)), 1, options_.maxWaitMs);
            return {SyncDecision::Type::Wait, waitMs};
        }

        return {};
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
            return {SyncDecision::Type::WaitingForNewerFrame, 0};
        }

        pendingVideoFrame = catchUpFrame;
        frame = pendingVideoFrame;
        updateFrameLatency(frame, playbackStats);

        const auto updatedWallElapsedSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - wallBaseTime_).count();
        videoDelayMs =
            ((frame->ptsSeconds - videoBaseSeconds_) - updatedWallElapsedSeconds) * 1000.0;
    }

    playbackStats.syncDroppedFrames = droppedFrames_;
    return {};
}

} // namespace rtsp
