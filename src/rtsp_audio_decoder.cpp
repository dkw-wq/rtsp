#include "rtsp_audio_decoder.hpp"

#include "rtsp_ffmpeg_utils.hpp"

#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace rtsp {

RtspAudioDecoder::~RtspAudioDecoder() {
    close();
}

bool RtspAudioDecoder::open(AVFormatContext* formatContext, int audioStream) {
    close();

    if (formatContext == nullptr || audioStream < 0) {
        return false;
    }

    AVCodecParameters* codecParams = formatContext->streams[audioStream]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
    if (codec == nullptr) {
        SPDLOG_WARN("Audio codec '{}' not found", avcodec_get_name(codecParams->codec_id));
        return false;
    }

    audioCodecContext_ = avcodec_alloc_context3(codec);
    if (audioCodecContext_ == nullptr) {
        SPDLOG_WARN("Failed to allocate audio codec context");
        return false;
    }

    int ret = avcodec_parameters_to_context(audioCodecContext_, codecParams);
    if (ret < 0) {
        SPDLOG_WARN("Failed to copy audio codec parameters: {}", ffmpeg::errorToString(ret));
        return false;
    }

    if (audioCodecContext_->ch_layout.nb_channels <= 0) {
        av_channel_layout_default(&audioCodecContext_->ch_layout, 2);
    }

    ret = avcodec_open2(audioCodecContext_, codec, nullptr);
    if (ret < 0) {
        SPDLOG_WARN("Failed to open audio codec '{}': {}", codec->name, ffmpeg::errorToString(ret));
        return false;
    }

    AVChannelLayout outputLayout{};
    av_channel_layout_default(&outputLayout, kOutputAudioChannels);
    ret = swr_alloc_set_opts2(
        &swrContext_,
        &outputLayout,
        AV_SAMPLE_FMT_S16,
        kOutputAudioSampleRate,
        &audioCodecContext_->ch_layout,
        audioCodecContext_->sample_fmt,
        audioCodecContext_->sample_rate,
        0,
        nullptr);
    av_channel_layout_uninit(&outputLayout);
    if (ret < 0 || swrContext_ == nullptr) {
        SPDLOG_WARN("Failed to allocate audio resampler: {}", ffmpeg::errorToString(ret));
        return false;
    }

    ret = swr_init(swrContext_);
    if (ret < 0) {
        SPDLOG_WARN("Failed to initialize audio resampler: {}", ffmpeg::errorToString(ret));
        return false;
    }

    formatContext_ = formatContext;
    audioStream_ = audioStream;
    SPDLOG_INFO("Audio: codec={}, input={} Hz/{} ch, output={} Hz/{} ch S16",
                codec->name,
                audioCodecContext_->sample_rate,
                audioCodecContext_->ch_layout.nb_channels,
                kOutputAudioSampleRate,
                kOutputAudioChannels);
    return true;
}

void RtspAudioDecoder::close() {
    notifyStop();
    join();
    clearPacketQueue();

    if (swrContext_ != nullptr) {
        swr_free(&swrContext_);
    }
    if (audioCodecContext_ != nullptr) {
        avcodec_free_context(&audioCodecContext_);
    }

    formatContext_ = nullptr;
    audioStream_ = -1;
    running_ = nullptr;
    frameCallback_ = nullptr;
}

bool RtspAudioDecoder::isOpen() const {
    return audioCodecContext_ != nullptr;
}

void RtspAudioDecoder::start(std::atomic<bool>& running, FrameCallback callback) {
    if (!isOpen() || audioThread_.joinable()) {
        return;
    }

    running_ = &running;
    frameCallback_ = std::move(callback);
    audioThread_ = std::thread(&RtspAudioDecoder::decodeLoop, this);
}

void RtspAudioDecoder::notifyStop() {
    audioCv_.notify_all();
}

void RtspAudioDecoder::join() {
    if (audioThread_.joinable()) {
        audioThread_.join();
    }
}

void RtspAudioDecoder::enqueuePacket(const AVPacket* packet) {
    AVPacket* queuedPacket = av_packet_alloc();
    if (queuedPacket == nullptr) {
        return;
    }

    const int ret = av_packet_ref(queuedPacket, packet);
    if (ret < 0) {
        SPDLOG_WARN("Failed to reference audio packet: {}", ffmpeg::errorToString(ret));
        av_packet_free(&queuedPacket);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(audioMutex_);
        while (audioPacketQueue_.size() >= kMaxAudioPacketQueue) {
            AVPacket* oldPacket = audioPacketQueue_.front();
            audioPacketQueue_.pop();
            av_packet_free(&oldPacket);
        }
        audioPacketQueue_.push(queuedPacket);
    }
    audioCv_.notify_one();
}

void RtspAudioDecoder::clearPacketQueue() {
    std::lock_guard<std::mutex> lock(audioMutex_);
    while (!audioPacketQueue_.empty()) {
        AVPacket* packet = audioPacketQueue_.front();
        audioPacketQueue_.pop();
        av_packet_free(&packet);
    }
}

bool RtspAudioDecoder::fillAudioFrame(const AVFrame* sourceFrame, MediaFrame& mediaFrame) {
    if (swrContext_ == nullptr || audioCodecContext_ == nullptr) {
        return false;
    }

    const int sourceRate =
        sourceFrame->sample_rate > 0 ? sourceFrame->sample_rate : audioCodecContext_->sample_rate;
    const int64_t delay = swr_get_delay(swrContext_, sourceRate);
    const int outputSamples = static_cast<int>(av_rescale_rnd(
        delay + sourceFrame->nb_samples,
        kOutputAudioSampleRate,
        sourceRate,
        AV_ROUND_UP));
    if (outputSamples <= 0) {
        return false;
    }

    mediaFrame.type = MediaFrame::Type::AUDIO;
    mediaFrame.sampleRate = kOutputAudioSampleRate;
    mediaFrame.channels = kOutputAudioChannels;
    mediaFrame.bytesPerSample = kOutputAudioBytesPerSample;
    mediaFrame.pts = ffmpeg::normalizedTimestamp(sourceFrame);
    mediaFrame.ptsSeconds =
        ffmpeg::timestampSeconds(formatContext_, sourceFrame, audioStream_);
    mediaFrame.data.resize(static_cast<size_t>(outputSamples) *
                           static_cast<size_t>(kOutputAudioChannels) *
                           static_cast<size_t>(kOutputAudioBytesPerSample));

    uint8_t* outputData[1] = {mediaFrame.data.data()};
    const int convertedSamples = swr_convert(
        swrContext_,
        outputData,
        outputSamples,
        const_cast<const uint8_t**>(sourceFrame->extended_data),
        sourceFrame->nb_samples);
    if (convertedSamples < 0) {
        SPDLOG_WARN("Audio resample failed: {}", ffmpeg::errorToString(convertedSamples));
        return false;
    }

    mediaFrame.data.resize(static_cast<size_t>(convertedSamples) *
                           static_cast<size_t>(kOutputAudioChannels) *
                           static_cast<size_t>(kOutputAudioBytesPerSample));
    mediaFrame.durationSeconds =
        static_cast<double>(convertedSamples) / static_cast<double>(kOutputAudioSampleRate);
    return !mediaFrame.data.empty();
}

void RtspAudioDecoder::decodeLoop() {
    SPDLOG_INFO("Audio decode loop started");

    AVFrame* audioFrame = av_frame_alloc();
    if (audioFrame == nullptr) {
        SPDLOG_WARN("Failed to allocate audio decode frame");
        return;
    }

    while (running_ != nullptr && running_->load()) {
        AVPacket* packet = nullptr;
        {
            std::unique_lock<std::mutex> lock(audioMutex_);
            audioCv_.wait(lock, [this] {
                return running_ == nullptr ||
                       !running_->load() ||
                       !audioPacketQueue_.empty();
            });

            if (running_ == nullptr || !running_->load()) {
                break;
            }

            packet = audioPacketQueue_.front();
            audioPacketQueue_.pop();
        }

        if (packet == nullptr || audioCodecContext_ == nullptr) {
            av_packet_free(&packet);
            continue;
        }

        int ret = avcodec_send_packet(audioCodecContext_, packet);
        av_packet_free(&packet);
        if (ret < 0) {
            SPDLOG_WARN("Send audio packet error: {}", ffmpeg::errorToString(ret));
            continue;
        }

        while (running_ != nullptr && running_->load() && ret >= 0) {
            ret = avcodec_receive_frame(audioCodecContext_, audioFrame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                SPDLOG_WARN("Receive audio frame error: {}", ffmpeg::errorToString(ret));
                break;
            }

            auto mediaFrame = std::make_shared<MediaFrame>();
            if (fillAudioFrame(audioFrame, *mediaFrame) && frameCallback_) {
                mediaFrame->recvTime = ffmpeg::steadyNowMicros();
                frameCallback_(mediaFrame);
            }

            av_frame_unref(audioFrame);
        }
    }

    av_frame_free(&audioFrame);
    SPDLOG_INFO("Audio decode loop ended");
}

} // namespace rtsp
