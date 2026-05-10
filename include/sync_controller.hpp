#pragma once

#include "audio_player.hpp"
#include "video_renderer.hpp"

#include <chrono>
#include <cstdint>
#include <memory>

namespace rtsp {

class JitterBuffer;
struct MediaFrame;

struct SyncOptions {
    bool enabled = true;
    int maxWaitMs = 16;
    int lateDropMs = 250;
    int audioOffsetMs = 0;
};

struct SyncDecision {
    enum class Type {
        Render,
        Wait,
        WaitingForNewerFrame
    };

    Type type = Type::Render;
    int waitMs = 0;
};

void updateFrameLatency(const std::shared_ptr<MediaFrame>& currentFrame,
                        PlaybackStats& stats);

std::chrono::steady_clock::time_point frameTargetTimeByReceiveTime(
    const std::shared_ptr<MediaFrame>& frame,
    int targetDelayMs);

class SingleStreamSyncController {
public:
    explicit SingleStreamSyncController(SyncOptions options = {});

    void reset();
    uint64_t droppedFrames() const;

    SyncDecision synchronize(std::shared_ptr<MediaFrame>& frame,
                             std::shared_ptr<MediaFrame>& pendingVideoFrame,
                             JitterBuffer& jitterBuffer,
                             const AudioPlayer& audioPlayer,
                             PlaybackStats& playbackStats);

private:
    bool shouldHoldForAudioStartup(const AudioPlaybackStats& audioStats) const;

    SyncOptions options_;
    uint64_t droppedFrames_ = 0;
    bool initialized_ = false;
    double videoBaseSeconds_ = 0.0;
    double audioBaseSeconds_ = 0.0;
    bool audioBaseInitialized_ = false;
    std::chrono::steady_clock::time_point wallBaseTime_{};
};

} // namespace rtsp
