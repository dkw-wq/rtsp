#pragma once

#include "audio_player.hpp"
#include "jitter_buffer.hpp"
#include "rtsp_client.hpp"
#include "video_renderer.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace rtsp {

struct StreamSessionOptions {
    std::string url;
    RtspConnectionOptions connectionOptions;
    std::string hardwareDecodeBackend = "none";
    bool audioEnabled = false;
    bool videoEnabled = true;
    bool hardwareFrameOutput = false;
    bool forwardAudioToPlayer = false;
    AudioPlayer* audioPlayer = nullptr;
    size_t jitterMaxSize = 12;
    uint32_t jitterLatencyMs = 30;
    size_t streamIndex = 0;
};

class StreamSession {
public:
    explicit StreamSession(StreamSessionOptions options);
    ~StreamSession();

    StreamSession(const StreamSession&) = delete;
    StreamSession& operator=(const StreamSession&) = delete;

    bool connect();
    void start();
    void stopAndDisconnect();
    bool isRunning() const;
    void setForwardAudioToPlayer(bool enabled);

    RtspClient& client();
    const RtspClient& client() const;
    JitterBuffer& jitterBuffer();
    const std::string& url() const;
    size_t streamIndex() const;

    void resetBufferedFrames();
    void resetStats();
    void refreshStats();
    void noteInputFrame();
    void updateInputFpsIfDue();

    std::shared_ptr<MediaFrame> pendingFrame;
    std::shared_ptr<MediaFrame> latestFrame;
    PlaybackStats stats;

private:
    StreamSessionOptions options_;
    std::unique_ptr<RtspClient> client_;
    std::unique_ptr<JitterBuffer> jitterBuffer_;
    uint64_t receivedFramesSinceFpsUpdate_ = 0;
    std::chrono::steady_clock::time_point lastFpsUpdateTime_ =
        std::chrono::steady_clock::now();
};

} // namespace rtsp
