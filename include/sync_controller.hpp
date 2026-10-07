#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <deque>

struct AVPacket;

namespace rtsp {

class AudioPlayer;
class JitterBuffer;
struct AudioPlaybackOptions;
struct AudioPlaybackStats;
struct MediaFrame;
struct PlaybackStats;

enum class SyncMode { Timestamp, ReceiveTime };

struct SyncOptions {
    bool enabled = true;
    int maxWaitMs = 16;
    int lateDropMs = 250;
    int audioOffsetMs = 0;
    SyncMode mode = SyncMode::Timestamp;
};

struct AudioClockSnapshot {
    bool available = false;
    bool active = false;
    double ptsSeconds = 0.0;
    std::optional<double> referenceTimeSeconds;
};

// PRFT is FFmpeg's per-packet Unix time reconstructed from RTCP sender reports.
class SenderClockMapper {
public:
    void reset();
    bool observePacket(const AVPacket& packet, double secondsPerTick);
    bool hasMapping() const;
    std::optional<double> referenceTime(double sourcePtsSeconds) const;

private:
    std::optional<double> anchorPtsSeconds_;
    double anchorReferenceSeconds_ = 0.0;
};

struct SyncDecision {
    enum class Type {
        Render,
        Wait,
        WaitingForNewerFrame
    };

    Type type = Type::Render;
    int waitMs = 0;
    // Replacement frames consumed while catching up, for playback input statistics.
    uint64_t catchUpFrames = 0;
};

// Timing policies own no devices or locks; callers serialize access to their state.
AudioPlaybackOptions normalizeAudioPlaybackOptions(AudioPlaybackOptions options);

class AudioPlaybackTiming {
public:
    explicit AudioPlaybackTiming(const AudioPlaybackOptions& options);

    void reset();
    void resetForDevice();
    bool shouldResetQueue(uint32_t queuedMs) const;
    void resetQueue(double ptsSeconds);
    void noteQueuedFrame(double ptsSeconds, double durationSeconds,
                         std::optional<double> referenceTimeSeconds = std::nullopt);
    void trimPlayedFrames(double queuedSeconds);
    std::optional<double> referenceClockSeconds(double queuedSeconds) const;
    bool startIfReady(uint32_t queuedMs);
    bool active() const;
    bool hasClock() const;
    double clockSeconds(double queuedSeconds) const;

private:
    uint32_t targetLatencyMs_;
    uint32_t hardResetQueueMs_;
    double queuedAudioEndSeconds_ = 0.0;
    bool hasClock_ = false;
    bool playbackStarted_ = false;
    struct AudioSegment {
        double durationSeconds;
        std::optional<double> referenceTimeSeconds;
    };
    std::deque<AudioSegment> segments_;
};

class VideoTimestampTracker {
public:
    void reset();
    double stabilize(double candidateSeconds, bool hasCandidate, double frameDurationSeconds);

private:
    double nextSyntheticPtsSeconds_ = 0.0;
    double lastPtsSeconds_ = 0.0;
    bool hasLastPts_ = false;
};

class JitterBufferTiming {
public:
    JitterBufferTiming(uint32_t latencyMs, bool adaptive, uint32_t maxLatencyMs);

    void reset();
    bool observeArrival(const MediaFrame& frame);
    void noteReleased(const MediaFrame& frame);
    std::chrono::steady_clock::time_point releaseTime(const MediaFrame& frame) const;
    bool shouldRelease(const MediaFrame& frame) const;
    void setLatency(uint32_t latencyMs);
    uint32_t targetLatencyMs() const;
    double jitterMs() const;

private:
    void updateTargetLatency();

    uint32_t baseLatencyMs_;
    uint32_t targetLatencyMs_;
    uint32_t maxLatencyMs_;
    bool adaptive_;
    double lastPtsSeconds_ = 0.0;
    double lastReleasedPtsSeconds_ = 0.0;
    std::chrono::microseconds lastRecvTime_{0};
    double jitterEstimateMs_ = 0.0;
    bool hasLastArrival_ = false;
    bool hasLastRelease_ = false;
};

void updateFrameLatency(const std::shared_ptr<MediaFrame>& currentFrame,
                        PlaybackStats& stats);

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

// Timestamp mode aligns every mapped video stream to the actual audio queue head.
class MultiStreamSyncController {
public:
    MultiStreamSyncController(SyncOptions options, int audioTargetLatencyMs);

    SyncDecision synchronize(size_t streamIndex,
                             std::shared_ptr<MediaFrame>& pendingVideoFrame,
                             JitterBuffer& jitterBuffer,
                             const AudioClockSnapshot& audioClock,
                             PlaybackStats& playbackStats) const;

private:
    SyncOptions options_;
    int audioTargetLatencyMs_;
};

} // namespace rtsp
