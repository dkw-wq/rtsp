#pragma once

#include "jitter_buffer.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace rtsp {

struct AudioPlaybackOptions {
    bool enabled = true;
    int targetLatencyMs = 30;
    int maxQueueMs = 300;
    int hardResetQueueMs = 1500;
};

struct AudioPlaybackStats {
    bool enabled = false;
    bool active = false;
    int sampleRate = 0;
    int channels = 0;
    uint32_t queuedMs = 0;
    uint32_t targetLatencyMs = 0;
    uint64_t playedFrames = 0;
    uint64_t droppedFrames = 0;
};

class AudioPlayer {
public:
    explicit AudioPlayer(AudioPlaybackOptions options = {});
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    bool pushFrame(const std::shared_ptr<MediaFrame>& frame);
    void reset();

    bool hasClock() const;
    double clockSeconds() const;
    uint32_t queuedMs() const;
    AudioPlaybackStats getStats() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl_;
};

} // namespace rtsp
