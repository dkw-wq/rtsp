#include "stream_session.hpp"

#include "config_loader.hpp"
#include "sync_controller.hpp"

#include <utility>

#include <spdlog/spdlog.h>

namespace rtsp {

StreamSession::StreamSession(StreamSessionOptions options)
    : options_(std::move(options))
    , client_(std::make_unique<RtspClient>())
    , jitterBuffer_(std::make_unique<JitterBuffer>(
          options_.jitterMaxSize, options_.jitterLatencyMs)) {
    client_->setConnectionOptions(options_.connectionOptions);
    client_->setAudioEnabled(options_.audioEnabled);
    client_->setVideoEnabled(options_.videoEnabled);
    client_->setHardwareDecode(options_.hardwareDecodeBackend);
    client_->setHardwareFrameOutput(options_.hardwareFrameOutput);

    stats.decoderBackend = "CPU";
    client_->setFrameCallback([this](const std::shared_ptr<MediaFrame>& frame) {
        if (!frame) {
            return;
        }

        if (frame->type == MediaFrame::Type::AUDIO) {
            if (options_.forwardAudioToPlayer && options_.audioPlayer) {
                options_.audioPlayer->pushFrame(frame);
            }
            return;
        }

        jitterBuffer_->push(frame);
    });

    client_->setErrorCallback([this](const std::string& error) {
        SPDLOG_ERROR("RTSP stream {} error: {}", options_.streamIndex + 1, error);
    });
}

StreamSession::~StreamSession() {
    stopAndDisconnect();
}

bool StreamSession::connect() {
    if (!client_->connect(options_.url)) {
        return false;
    }

    SPDLOG_INFO("RTSP stream {} resolution: {}x{}",
                options_.streamIndex + 1,
                client_->getWidth(),
                client_->getHeight());
    resetBufferedFrames();
    refreshStats();
    return true;
}

void StreamSession::start() {
    client_->start();
}

void StreamSession::stopAndDisconnect() {
    client_->stop();
    client_->disconnect();
}

bool StreamSession::isRunning() const {
    return client_->isRunning();
}

void StreamSession::setForwardAudioToPlayer(bool enabled) {
    options_.forwardAudioToPlayer = enabled;
}

RtspClient& StreamSession::client() {
    return *client_;
}

const RtspClient& StreamSession::client() const {
    return *client_;
}

JitterBuffer& StreamSession::jitterBuffer() {
    return *jitterBuffer_;
}

const std::string& StreamSession::url() const {
    return options_.url;
}

size_t StreamSession::streamIndex() const {
    return options_.streamIndex;
}

void StreamSession::resetBufferedFrames() {
    jitterBuffer_->clear();
    pendingFrame.reset();
    latestFrame.reset();
}

void StreamSession::resetStats() {
    stats = {};
    stats.decoderBackend = "CPU";
    stats.hardwareDecodeStatus = client_->getHardwareDecodeStatus();
    receivedFramesSinceFpsUpdate_ = 0;
    lastFpsUpdateTime_ = std::chrono::steady_clock::now();
}

void StreamSession::refreshStats() {
    const auto jitterStats = jitterBuffer_->getStats();
    stats.decoderBackend = displayDecodeBackend(client_->getDecodeBackend());
    stats.hardwareDecodeStatus = client_->getHardwareDecodeStatus();
    stats.decodedFrames = jitterStats.totalFrames;
    stats.droppedFrames = jitterStats.droppedFrames;
    stats.jitterBufferSize = jitterStats.bufferSize;
    updateFrameLatency(latestFrame, stats);
}

void StreamSession::noteInputFrame() {
    ++receivedFramesSinceFpsUpdate_;
}

void StreamSession::updateInputFpsIfDue() {
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = now - lastFpsUpdateTime_;
    if (elapsed < std::chrono::seconds(1)) {
        return;
    }

    const double elapsedSeconds = std::chrono::duration<double>(elapsed).count();
    stats.fps = static_cast<double>(receivedFramesSinceFpsUpdate_) / elapsedSeconds;
    SPDLOG_INFO("RTSP stream {} input fps={:.1f}, jitter_buffer={}, dropped={}, latency_ms={}",
                options_.streamIndex + 1,
                stats.fps,
                stats.jitterBufferSize,
                stats.droppedFrames,
                stats.latencyMs);
    receivedFramesSinceFpsUpdate_ = 0;
    lastFpsUpdateTime_ = now;
}

} // namespace rtsp
