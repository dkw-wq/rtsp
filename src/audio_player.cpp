#include "audio_player.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>

extern "C" {
#include <SDL2/SDL.h>
}

namespace rtsp {

namespace {

uint32_t bytesToMs(uint32_t bytes, int sampleRate, int channels, int bytesPerSample) {
    const int bytesPerSecond = sampleRate * channels * bytesPerSample;
    if (bytesPerSecond <= 0) {
        return 0;
    }

    return static_cast<uint32_t>((static_cast<uint64_t>(bytes) * 1000ULL) /
                                 static_cast<uint64_t>(bytesPerSecond));
}

double bytesToSeconds(uint32_t bytes, int sampleRate, int channels, int bytesPerSample) {
    const int bytesPerSecond = sampleRate * channels * bytesPerSample;
    if (bytesPerSecond <= 0) {
        return 0.0;
    }

    return static_cast<double>(bytes) / static_cast<double>(bytesPerSecond);
}

} // namespace

class AudioPlayer::Impl {
public:
    explicit Impl(AudioPlaybackOptions options)
        : options_(options)
    {
        options_.targetLatencyMs = std::clamp(options_.targetLatencyMs, 0, 5000);
        options_.maxQueueMs = std::max(options_.maxQueueMs, 20);
        options_.maxQueueMs = std::max(options_.maxQueueMs, options_.targetLatencyMs + 300);
        options_.hardResetQueueMs =
            std::max(options_.hardResetQueueMs, options_.maxQueueMs + 500);
        stats_.enabled = options_.enabled;
    }

    ~Impl() {
        reset();
    }

    bool pushFrame(const std::shared_ptr<MediaFrame>& frame) {
        if (!options_.enabled || !frame || frame->type != MediaFrame::Type::AUDIO ||
            frame->data.empty()) {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (!ensureDevice(frame->sampleRate, frame->channels, frame->bytesPerSample)) {
            ++stats_.droppedFrames;
            return false;
        }

        const uint32_t queuedBefore = SDL_GetQueuedAudioSize(device_);
        const uint32_t queuedBeforeMs =
            bytesToMs(queuedBefore, sampleRate_, channels_, bytesPerSample_);
        if (queuedBeforeMs > static_cast<uint32_t>(options_.hardResetQueueMs)) {
            SDL_ClearQueuedAudio(device_);
            queuedAudioEndSeconds_ = frame->ptsSeconds;
            playbackStarted_ = options_.targetLatencyMs <= 0;
            SDL_PauseAudioDevice(device_, playbackStarted_ ? 0 : 1);
            ++stats_.droppedFrames;
            warnQueueLimited("Audio queue exceeded hard limit; resetting queued audio",
                             queuedBeforeMs);
        } else if (queuedBeforeMs > static_cast<uint32_t>(options_.maxQueueMs)) {
            warnQueueLimited("Audio queue exceeded soft limit; keeping audio continuous",
                             queuedBeforeMs);
        }

        if (SDL_QueueAudio(device_, frame->data.data(),
                           static_cast<uint32_t>(frame->data.size())) < 0) {
            SPDLOG_WARN("SDL_QueueAudio failed: {}", SDL_GetError());
            ++stats_.droppedFrames;
            return false;
        }

        const double duration =
            frame->durationSeconds > 0.0
                ? frame->durationSeconds
                : bytesToSeconds(static_cast<uint32_t>(frame->data.size()),
                                 sampleRate_, channels_, bytesPerSample_);
        queuedAudioEndSeconds_ = std::max(queuedAudioEndSeconds_,
                                          frame->ptsSeconds + duration);
        hasClock_ = true;
        ++stats_.playedFrames;
        stats_.queuedMs = queuedMsLocked();
        maybeStartPlayback();
        return true;
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (device_ != 0) {
            SDL_ClearQueuedAudio(device_);
            SDL_CloseAudioDevice(device_);
            device_ = 0;
        }

        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        sampleRate_ = 0;
        channels_ = 0;
        bytesPerSample_ = 0;
        queuedAudioEndSeconds_ = 0.0;
        hasClock_ = false;
        playbackStarted_ = false;
        stats_ = {};
        stats_.enabled = options_.enabled;
    }

    bool hasClock() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return hasClock_ && device_ != 0;
    }

    double clockSeconds() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!hasClock_ || device_ == 0) {
            return 0.0;
        }

        const uint32_t queuedBytes = SDL_GetQueuedAudioSize(device_);
        return queuedAudioEndSeconds_ -
               bytesToSeconds(queuedBytes, sampleRate_, channels_, bytesPerSample_);
    }

    uint32_t queuedMs() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queuedMsLocked();
    }

    AudioPlaybackStats getStats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        AudioPlaybackStats stats = stats_;
        stats.enabled = options_.enabled;
        stats.active = playbackStarted_;
        stats.sampleRate = sampleRate_;
        stats.channels = channels_;
        stats.queuedMs = queuedMsLocked();
        stats.targetLatencyMs = static_cast<uint32_t>(options_.targetLatencyMs);
        return stats;
    }

private:
    bool ensureDevice(int sampleRate, int channels, int bytesPerSample) {
        if (sampleRate <= 0 || channels <= 0 || bytesPerSample != 2) {
            SPDLOG_WARN("Unsupported audio frame format: rate={}, channels={}, bytes_per_sample={}",
                        sampleRate, channels, bytesPerSample);
            return false;
        }

        if (device_ != 0 &&
            sampleRate_ == sampleRate &&
            channels_ == channels &&
            bytesPerSample_ == bytesPerSample) {
            return true;
        }

        if (device_ != 0) {
            SDL_ClearQueuedAudio(device_);
            SDL_CloseAudioDevice(device_);
            device_ = 0;
        }

        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            SPDLOG_WARN("SDL audio init failed: {}", SDL_GetError());
            return false;
        }

        SDL_AudioSpec desired{};
        desired.freq = sampleRate;
        desired.format = AUDIO_S16SYS;
        desired.channels = static_cast<uint8_t>(channels);
        desired.samples = 512;
        desired.callback = nullptr;

        SDL_AudioSpec obtained{};
        device_ = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
        if (device_ == 0) {
            SPDLOG_WARN("SDL_OpenAudioDevice failed: {}", SDL_GetError());
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            return false;
        }

        sampleRate_ = obtained.freq;
        channels_ = obtained.channels;
        bytesPerSample_ = bytesPerSample;
        queuedAudioEndSeconds_ = 0.0;
        hasClock_ = false;
        playbackStarted_ = options_.targetLatencyMs <= 0;
        SDL_PauseAudioDevice(device_, playbackStarted_ ? 0 : 1);
        SPDLOG_INFO("Audio playback opened: {} Hz, {} channels, target_latency_ms={}",
                    sampleRate_, channels_, options_.targetLatencyMs);
        return true;
    }

    uint32_t queuedMsLocked() const {
        if (device_ == 0) {
            return 0;
        }

        return bytesToMs(SDL_GetQueuedAudioSize(device_),
                         sampleRate_, channels_, bytesPerSample_);
    }

    void warnQueueLimited(const char* message, uint32_t queuedMs) {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastQueueWarningTime_ < std::chrono::seconds(1)) {
            return;
        }

        lastQueueWarningTime_ = now;
        SPDLOG_WARN("{}: queued={} ms, soft={} ms, hard={} ms",
                    message, queuedMs, options_.maxQueueMs, options_.hardResetQueueMs);
    }

    void maybeStartPlayback() {
        if (device_ == 0 || playbackStarted_) {
            return;
        }

        const uint32_t queuedMs = queuedMsLocked();
        if (queuedMs < static_cast<uint32_t>(options_.targetLatencyMs)) {
            return;
        }

        playbackStarted_ = true;
        SDL_PauseAudioDevice(device_, 0);
        SPDLOG_INFO("Audio playback started after prebuffer: queued={} ms", queuedMs);
    }

    AudioPlaybackOptions options_;
    mutable std::mutex mutex_;
    SDL_AudioDeviceID device_ = 0;
    int sampleRate_ = 0;
    int channels_ = 0;
    int bytesPerSample_ = 0;
    double queuedAudioEndSeconds_ = 0.0;
    bool hasClock_ = false;
    bool playbackStarted_ = false;
    AudioPlaybackStats stats_{};
    std::chrono::steady_clock::time_point lastQueueWarningTime_{};
};

AudioPlayer::AudioPlayer(AudioPlaybackOptions options)
    : pImpl_(std::make_unique<Impl>(options))
{}

AudioPlayer::~AudioPlayer() = default;

bool AudioPlayer::pushFrame(const std::shared_ptr<MediaFrame>& frame) {
    return pImpl_->pushFrame(frame);
}

void AudioPlayer::reset() {
    pImpl_->reset();
}

bool AudioPlayer::hasClock() const {
    return pImpl_->hasClock();
}

double AudioPlayer::clockSeconds() const {
    return pImpl_->clockSeconds();
}

uint32_t AudioPlayer::queuedMs() const {
    return pImpl_->queuedMs();
}

AudioPlaybackStats AudioPlayer::getStats() const {
    return pImpl_->getStats();
}

} // namespace rtsp
