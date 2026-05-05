#include "rtsp_client.hpp"
#include "jitter_buffer.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <condition_variable>
#include <cctype>
#include <cstring>
#include <queue>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

namespace rtsp {

namespace {

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string ffmpegError(int errorCode) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errorCode, errbuf, sizeof(errbuf));
    return errbuf;
}

AVPixelFormat normalizeDeprecatedPixelFormat(AVPixelFormat format) {
    switch (format) {
        case AV_PIX_FMT_YUVJ420P:
            return AV_PIX_FMT_YUV420P;
        case AV_PIX_FMT_YUVJ422P:
            return AV_PIX_FMT_YUV422P;
        case AV_PIX_FMT_YUVJ444P:
            return AV_PIX_FMT_YUV444P;
        case AV_PIX_FMT_YUVJ440P:
            return AV_PIX_FMT_YUV440P;
        case AV_PIX_FMT_YUVJ411P:
            return AV_PIX_FMT_YUV411P;
        default:
            return format;
    }
}

bool isDeprecatedFullRangePixelFormat(AVPixelFormat format) {
    switch (format) {
        case AV_PIX_FMT_YUVJ420P:
        case AV_PIX_FMT_YUVJ422P:
        case AV_PIX_FMT_YUVJ444P:
        case AV_PIX_FMT_YUVJ440P:
        case AV_PIX_FMT_YUVJ411P:
            return true;
        default:
            return false;
    }
}

const char* cudaDecoderName(AVCodecID codecId) {
    switch (codecId) {
        case AV_CODEC_ID_H264:
            return "h264_cuvid";
        case AV_CODEC_ID_HEVC:
            return "hevc_cuvid";
        case AV_CODEC_ID_MPEG2VIDEO:
            return "mpeg2_cuvid";
        case AV_CODEC_ID_MPEG4:
            return "mpeg4_cuvid";
        case AV_CODEC_ID_VC1:
            return "vc1_cuvid";
        case AV_CODEC_ID_VP8:
            return "vp8_cuvid";
        case AV_CODEC_ID_VP9:
            return "vp9_cuvid";
        case AV_CODEC_ID_AV1:
            return "av1_cuvid";
        case AV_CODEC_ID_MJPEG:
            return "mjpeg_cuvid";
        default:
            return nullptr;
    }
}

} // namespace

// Implementation struct
class RtspClient::Impl {
public:
    Impl() 
        : running_(false)
        , width_(0)
        , height_(0)
        , formatContext_(nullptr)
        , codecContext_(nullptr)
        , audioCodecContext_(nullptr)
        , swsContext_(nullptr)
        , swrContext_(nullptr)
        , hwDeviceContext_(nullptr)
        , videoStream_(-1)
        , audioStream_(-1)
        , videoFrameDurationSeconds_(1.0 / 30.0)
        , nextSyntheticVideoPtsSeconds_(0.0)
        , lastVideoPtsSeconds_(0.0)
        , hasLastVideoPts_(false)
        , hwPixelFormat_(AV_PIX_FMT_NONE)
        , hwDecodeActive_(false)
        , hwDecodeStatus_("off")
        , hardwareFrameOutputEnabled_(false)
        , audioEnabled_(true)
        , connectionOptions_()
        , cudaFrameLayoutLogged_(false)
        , waitingForVideoKeyframe_(true)
        , videoStarted_(false)
        , openDeadline_(std::chrono::steady_clock::time_point::max())
    {}

    ~Impl() {
        close();
    }

    bool connect(const std::string& url) {
        close();

        SPDLOG_INFO("Connecting to RTSP stream: {}", url);

        // 打开输入流
        formatContext_ = avformat_alloc_context();
        if (!formatContext_) {
            SPDLOG_ERROR("Failed to allocate format context");
            return false;
        }

        openDeadline_ = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(connectionOptions_.timeoutMs);
        formatContext_->interrupt_callback.callback = &Impl::interruptCallback;
        formatContext_->interrupt_callback.opaque = this;

        AVDictionary* options = nullptr;
        applyConnectionOptions(&options);

        int ret = avformat_open_input(&formatContext_, url.c_str(), nullptr, &options);
        av_dict_free(&options);
        openDeadline_ = std::chrono::steady_clock::time_point::max();

        if (ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            SPDLOG_ERROR("Failed to open input: {}", errbuf);
            close();
            return false;
        }

        // 获取流信息
        ret = avformat_find_stream_info(formatContext_, nullptr);
        if (ret < 0) {
            SPDLOG_ERROR("Failed to find stream info");
            close();
            return false;
        }

        // 查找音视频流
        videoStream_ = -1;
        audioStream_ = -1;
        for (unsigned int i = 0; i < formatContext_->nb_streams; i++) {
            const AVMediaType streamType = formatContext_->streams[i]->codecpar->codec_type;
            if (videoStream_ < 0 && streamType == AVMEDIA_TYPE_VIDEO) {
                videoStream_ = i;
            } else if (audioStream_ < 0 && streamType == AVMEDIA_TYPE_AUDIO) {
                audioStream_ = i;
            }
        }

        if (videoStream_ < 0) {
            SPDLOG_ERROR("No video stream found");
            close();
            return false;
        }

        // 获取编解码器参数
        AVCodecParameters* codecParams = formatContext_->streams[videoStream_]->codecpar;
        width_ = codecParams->width;
        height_ = codecParams->height;
        videoFrameDurationSeconds_ = streamFrameDurationSeconds();
        nextSyntheticVideoPtsSeconds_ = 0.0;
        lastVideoPtsSeconds_ = 0.0;
        hasLastVideoPts_ = false;
        waitingForVideoKeyframe_ = true;
        videoStarted_ = false;

        SPDLOG_INFO("Video: {}x{}, codec: {}, frame_duration={:.3f} ms",
                    width_, height_,
                    avcodec_get_name(codecParams->codec_id),
                    videoFrameDurationSeconds_ * 1000.0);

        // 查找解码器
        const AVCodec* softwareCodec = avcodec_find_decoder(codecParams->codec_id);
        if (!softwareCodec) {
            SPDLOG_ERROR("Codec not found");
            close();
            return false;
        }

        const AVCodec* codec = selectDecoder(codecParams->codec_id, softwareCodec);
        if (codec != softwareCodec) {
            SPDLOG_INFO("Selected hardware decoder: {}", codec->name);
        }

        // 创建解码器上下文
        codecContext_ = avcodec_alloc_context3(codec);
        if (!codecContext_) {
            SPDLOG_ERROR("Failed to allocate codec context");
            close();
            return false;
        }

        ret = avcodec_parameters_to_context(codecContext_, codecParams);
        if (ret < 0) {
            SPDLOG_ERROR("Failed to copy codec parameters");
            close();
            return false;
        }
        applyVideoStreamTiming(codecContext_);

        bool hardwareDecodePrepared = prepareHardwareDecoder(codec);
        if (codec != softwareCodec && !hardwareDecodePrepared) {
            SPDLOG_WARN("Hardware decoder '{}' could not be prepared. Falling back to software decoder '{}'",
                        codec->name, softwareCodec->name);
            codec = softwareCodec;
            if (codecContext_) {
                avcodec_free_context(&codecContext_);
            }
            codecContext_ = avcodec_alloc_context3(codec);
            if (!codecContext_) {
                SPDLOG_ERROR("Failed to allocate fallback software codec context");
                close();
                return false;
            }
            ret = avcodec_parameters_to_context(codecContext_, codecParams);
            if (ret < 0) {
                SPDLOG_ERROR("Failed to copy codec parameters for software fallback: {}", ffmpegError(ret));
                close();
                return false;
            }
            applyVideoStreamTiming(codecContext_);
        }

        // 打开解码器
        ret = avcodec_open2(codecContext_, codec, nullptr);
        if (ret < 0) {
            if (hardwareDecodePrepared) {
                SPDLOG_WARN("Failed to open codec with {} hardware decode: {}. Falling back to software decode",
                            hwDecodeBackend_, ffmpegError(ret));
                if (!recreateSoftwareCodecContext(softwareCodec, codecParams)) {
                    close();
                    return false;
                }
            } else {
                SPDLOG_ERROR("Failed to open codec: {}", ffmpegError(ret));
                close();
                return false;
            }
        }

        if (hwDecodeActive_) {
            SPDLOG_INFO("Hardware decode active: {}", hwDecodeBackend_);
        } else {
            SPDLOG_INFO("Hardware decode inactive, using software decode");
        }

        if (audioEnabled_ && audioStream_ >= 0) {
            if (!openAudioDecoder()) {
                SPDLOG_WARN("Audio stream found but could not be opened; continuing video-only");
                closeAudioDecoder();
            }
        } else if (audioEnabled_) {
            SPDLOG_INFO("No audio stream found");
        } else {
            SPDLOG_INFO("Audio decode disabled");
        }

        SPDLOG_INFO("RTSP client connected successfully");
        return true;
    }

    void disconnect() {
        close();
    }

    void close() {
        stop();
        closeAudioDecoder();
        
        if (codecContext_) {
            avcodec_free_context(&codecContext_);
            codecContext_ = nullptr;
        }

        if (swsContext_) {
            sws_freeContext(swsContext_);
            swsContext_ = nullptr;
        }

        if (hwDeviceContext_) {
            av_buffer_unref(&hwDeviceContext_);
        }

        if (formatContext_) {
            avformat_close_input(&formatContext_);
            formatContext_ = nullptr;
        }

        videoStream_ = -1;
        audioStream_ = -1;
        nextSyntheticVideoPtsSeconds_ = 0.0;
        lastVideoPtsSeconds_ = 0.0;
        hasLastVideoPts_ = false;
        waitingForVideoKeyframe_ = true;
        videoStarted_ = false;
        hwPixelFormat_ = AV_PIX_FMT_NONE;
        hwDecodeActive_ = false;
        hwDecodeStatus_ = hardwareDecodeRequested() ? "not-connected" : "off";
        width_ = 0;
        height_ = 0;
    }

    void start() {
        if (running_) {
            return;
        }
        running_ = true;
        if (audioCodecContext_ != nullptr) {
            audioThread_ = std::thread(&Impl::audioDecodeLoop, this);
        }
        receiveThread_ = std::thread(&Impl::receiveLoop, this);
    }

    void stop() {
        running_ = false;
        audioCv_.notify_all();
        if (receiveThread_.joinable()) {
            receiveThread_.join();
        }
        if (audioThread_.joinable()) {
            audioThread_.join();
        }
        clearAudioPacketQueue();
    }

    void setFrameCallback(FrameCallback callback) {
        frameCallback_ = std::move(callback);
    }

    void setErrorCallback(ErrorCallback callback) {
        errorCallback_ = std::move(callback);
    }

    void setConnectionOptions(const RtspConnectionOptions& options) {
        connectionOptions_ = normalizeConnectionOptions(options);
    }

    void setAudioEnabled(bool enabled) {
        audioEnabled_ = enabled;
    }

    void setHardwareDecode(const std::string& backend) {
        hwDecodeBackend_ = toLower(backend);
    }

    void setHardwareFrameOutput(bool enabled) {
        hardwareFrameOutputEnabled_ = enabled;
    }

    std::string getDecodeBackend() const {
        return hwDecodeActive_ ? hwDecodeBackend_ : "cpu";
    }

    std::string getHardwareDecodeStatus() const {
        return hwDecodeStatus_;
    }

    bool isRunning() const {
        return running_.load();
    }

    int getWidth() const { return width_; }
    int getHeight() const { return height_; }

private:
    static constexpr int kOutputAudioSampleRate = 48000;
    static constexpr int kOutputAudioChannels = 2;
    static constexpr int kOutputAudioBytesPerSample = 2;
    static constexpr size_t kMaxAudioPacketQueue = 128;

    static RtspConnectionOptions normalizeConnectionOptions(RtspConnectionOptions options) {
        options.transport = toLower(options.transport);
        if (options.transport != "tcp" && options.transport != "udp") {
            SPDLOG_WARN("Unknown RTSP transport '{}', using tcp", options.transport);
            options.transport = "tcp";
        }

        options.timeoutMs = std::max(options.timeoutMs, 1);
        options.bufferSize = std::max(options.bufferSize, 0);
        options.maxDelayMs = std::max(options.maxDelayMs, 0);
        options.analyzeDurationMs = std::max(options.analyzeDurationMs, 0);
        options.probeSizeBytes = std::max(options.probeSizeBytes, 0);
        options.reorderQueueSize = std::max(options.reorderQueueSize, 0);
        return options;
    }

    static void setDictionaryInt(AVDictionary** options, const char* key, int value) {
        const std::string text = std::to_string(value);
        av_dict_set(options, key, text.c_str(), 0);
    }

    void applyConnectionOptions(AVDictionary** options) const {
        const int timeoutUs = connectionOptions_.timeoutMs * 1000;
        av_dict_set(options, "rtsp_transport", connectionOptions_.transport.c_str(), 0);
        setDictionaryInt(options, "buffer_size", connectionOptions_.bufferSize);
        setDictionaryInt(options, "stimeout", timeoutUs);
        setDictionaryInt(options, "timeout", timeoutUs);
        setDictionaryInt(options, "rw_timeout", timeoutUs);

        if (!connectionOptions_.lowLatency) {
            return;
        }

        av_dict_set(options, "fflags", "nobuffer", 0);
        av_dict_set(options, "flags", "low_delay", 0);
        setDictionaryInt(options, "max_delay", connectionOptions_.maxDelayMs * 1000);
        setDictionaryInt(options, "analyzeduration", connectionOptions_.analyzeDurationMs * 1000);
        setDictionaryInt(options, "probesize", connectionOptions_.probeSizeBytes);
        setDictionaryInt(options, "reorder_queue_size", connectionOptions_.reorderQueueSize);
    }

    bool openAudioDecoder() {
        AVCodecParameters* codecParams = formatContext_->streams[audioStream_]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
        if (!codec) {
            SPDLOG_WARN("Audio codec '{}' not found", avcodec_get_name(codecParams->codec_id));
            return false;
        }

        audioCodecContext_ = avcodec_alloc_context3(codec);
        if (!audioCodecContext_) {
            SPDLOG_WARN("Failed to allocate audio codec context");
            return false;
        }

        int ret = avcodec_parameters_to_context(audioCodecContext_, codecParams);
        if (ret < 0) {
            SPDLOG_WARN("Failed to copy audio codec parameters: {}", ffmpegError(ret));
            return false;
        }

        if (audioCodecContext_->ch_layout.nb_channels <= 0) {
            av_channel_layout_default(&audioCodecContext_->ch_layout, 2);
        }

        ret = avcodec_open2(audioCodecContext_, codec, nullptr);
        if (ret < 0) {
            SPDLOG_WARN("Failed to open audio codec '{}': {}", codec->name, ffmpegError(ret));
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
            SPDLOG_WARN("Failed to allocate audio resampler: {}", ffmpegError(ret));
            return false;
        }

        ret = swr_init(swrContext_);
        if (ret < 0) {
            SPDLOG_WARN("Failed to initialize audio resampler: {}", ffmpegError(ret));
            return false;
        }

        SPDLOG_INFO("Audio: codec={}, input={} Hz/{} ch, output={} Hz/{} ch S16",
                    codec->name,
                    audioCodecContext_->sample_rate,
                    audioCodecContext_->ch_layout.nb_channels,
                    kOutputAudioSampleRate,
                    kOutputAudioChannels);
        return true;
    }

    void closeAudioDecoder() {
        if (swrContext_) {
            swr_free(&swrContext_);
        }
        if (audioCodecContext_) {
            avcodec_free_context(&audioCodecContext_);
        }
    }

    bool frameTimestampSeconds(const AVFrame* frame, int streamIndex, double& seconds) const {
        int64_t timestamp = frame->best_effort_timestamp;
        if (timestamp == AV_NOPTS_VALUE) {
            timestamp = frame->pts;
        }
        if (timestamp == AV_NOPTS_VALUE || streamIndex < 0) {
            return false;
        }

        seconds = static_cast<double>(timestamp) *
                  av_q2d(formatContext_->streams[streamIndex]->time_base);
        return true;
    }

    double timestampSeconds(const AVFrame* frame, int streamIndex) const {
        double seconds = 0.0;
        return frameTimestampSeconds(frame, streamIndex, seconds) ? seconds : 0.0;
    }

    double streamFrameDurationSeconds() const {
        if (!formatContext_ || videoStream_ < 0) {
            return 1.0 / 30.0;
        }

        const AVStream* stream = formatContext_->streams[videoStream_];
        AVRational frameRate = stream->avg_frame_rate;
        if (frameRate.num <= 0 || frameRate.den <= 0) {
            frameRate = stream->r_frame_rate;
        }

        if (frameRate.num > 0 && frameRate.den > 0) {
            return av_q2d(av_inv_q(frameRate));
        }

        return 1.0 / 30.0;
    }

    void applyVideoStreamTiming(AVCodecContext* context) const {
        if (!context || !formatContext_ || videoStream_ < 0) {
            return;
        }

        const AVStream* stream = formatContext_->streams[videoStream_];
        if (stream->time_base.num > 0 && stream->time_base.den > 0) {
            context->pkt_timebase = stream->time_base;
        }

        AVRational frameRate = stream->avg_frame_rate;
        if (frameRate.num <= 0 || frameRate.den <= 0) {
            frameRate = stream->r_frame_rate;
        }
        if (frameRate.num > 0 && frameRate.den > 0) {
            context->framerate = frameRate;
        }
    }

    double stableVideoTimestampSeconds(const AVFrame* frame) {
        double candidate = 0.0;
        const bool hasCandidate = frameTimestampSeconds(frame, videoStream_, candidate);
        bool useCandidate = hasCandidate;

        if (hasLastVideoPts_ && hasCandidate) {
            const double delta = candidate - lastVideoPtsSeconds_;
            const double maxReasonableDelta = std::max(0.20, videoFrameDurationSeconds_ * 4.0);
            if (delta <= 0.0 || delta > maxReasonableDelta) {
                useCandidate = false;
            }
        }

        const double ptsSeconds =
            useCandidate ? candidate : nextSyntheticVideoPtsSeconds_;
        hasLastVideoPts_ = true;
        lastVideoPtsSeconds_ = ptsSeconds;
        nextSyntheticVideoPtsSeconds_ = ptsSeconds + videoFrameDurationSeconds_;
        return ptsSeconds;
    }

    uint64_t normalizedTimestamp(const AVFrame* frame) const {
        int64_t timestamp = frame->best_effort_timestamp;
        if (timestamp == AV_NOPTS_VALUE) {
            timestamp = frame->pts;
        }
        if (timestamp == AV_NOPTS_VALUE || timestamp < 0) {
            return 0;
        }

        return static_cast<uint64_t>(timestamp);
    }

    bool fillAudioFrame(const AVFrame* sourceFrame, MediaFrame& mediaFrame) {
        if (!swrContext_ || !audioCodecContext_) {
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
        mediaFrame.pts = normalizedTimestamp(sourceFrame);
        mediaFrame.ptsSeconds = timestampSeconds(sourceFrame, audioStream_);
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
            SPDLOG_WARN("Audio resample failed: {}", ffmpegError(convertedSamples));
            return false;
        }

        mediaFrame.data.resize(static_cast<size_t>(convertedSamples) *
                               static_cast<size_t>(kOutputAudioChannels) *
                               static_cast<size_t>(kOutputAudioBytesPerSample));
        mediaFrame.durationSeconds =
            static_cast<double>(convertedSamples) / static_cast<double>(kOutputAudioSampleRate);
        return !mediaFrame.data.empty();
    }

    void enqueueAudioPacket(const AVPacket* packet) {
        AVPacket* queuedPacket = av_packet_alloc();
        if (!queuedPacket) {
            return;
        }

        const int ret = av_packet_ref(queuedPacket, packet);
        if (ret < 0) {
            SPDLOG_WARN("Failed to reference audio packet: {}", ffmpegError(ret));
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

    void clearAudioPacketQueue() {
        std::lock_guard<std::mutex> lock(audioMutex_);
        while (!audioPacketQueue_.empty()) {
            AVPacket* packet = audioPacketQueue_.front();
            audioPacketQueue_.pop();
            av_packet_free(&packet);
        }
    }

    void audioDecodeLoop() {
        SPDLOG_INFO("Audio decode loop started");

        AVFrame* audioFrame = av_frame_alloc();
        if (!audioFrame) {
            SPDLOG_WARN("Failed to allocate audio decode frame");
            return;
        }

        while (running_) {
            AVPacket* packet = nullptr;
            {
                std::unique_lock<std::mutex> lock(audioMutex_);
                audioCv_.wait(lock, [this] {
                    return !running_ || !audioPacketQueue_.empty();
                });

                if (!running_) {
                    break;
                }

                packet = audioPacketQueue_.front();
                audioPacketQueue_.pop();
            }

            if (!packet || audioCodecContext_ == nullptr) {
                av_packet_free(&packet);
                continue;
            }

            int ret = avcodec_send_packet(audioCodecContext_, packet);
            av_packet_free(&packet);
            if (ret < 0) {
                SPDLOG_WARN("Send audio packet error: {}", ffmpegError(ret));
                continue;
            }

            while (running_ && ret >= 0) {
                ret = avcodec_receive_frame(audioCodecContext_, audioFrame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                }
                if (ret < 0) {
                    SPDLOG_WARN("Receive audio frame error: {}", ffmpegError(ret));
                    break;
                }

                auto mediaFrame = std::make_shared<MediaFrame>();
                if (fillAudioFrame(audioFrame, *mediaFrame) && frameCallback_) {
                    frameCallback_(mediaFrame);
                }

                av_frame_unref(audioFrame);
            }
        }

        av_frame_free(&audioFrame);
        SPDLOG_INFO("Audio decode loop ended");
    }

    static int interruptCallback(void* opaque) {
        auto* self = static_cast<Impl*>(opaque);
        return std::chrono::steady_clock::now() > self->openDeadline_;
    }

    static AVPixelFormat getHardwarePixelFormat(AVCodecContext* context,
                                                const AVPixelFormat* formats) {
        const auto* self = static_cast<const Impl*>(context->opaque);
        for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
            if (*format == self->hwPixelFormat_) {
                return *format;
            }
        }

        SPDLOG_WARN("Requested hardware pixel format is unavailable, using software decode");
        return formats[0];
    }

    static void copyPlane(uint8_t* dst, int dstStride, const uint8_t* src,
                          int srcStride, int width, int height) {
        for (int row = 0; row < height; ++row) {
            std::memcpy(dst + row * dstStride, src + row * srcStride, width);
        }
    }

    bool copyFrameAsNv12(const AVFrame* sourceFrame, MediaFrame& mediaFrame) {
        const int ySize = sourceFrame->width * sourceFrame->height;
        mediaFrame.pixelFormat = MediaFrame::PixelFormat::NV12;
        mediaFrame.data.resize(ySize * 3 / 2);

        uint8_t* dstData[4] = {
            mediaFrame.data.data(),
            mediaFrame.data.data() + ySize,
            nullptr,
            nullptr
        };
        int dstLinesize[4] = {
            sourceFrame->width,
            sourceFrame->width,
            0,
            0
        };

        const auto sourceFormat = static_cast<AVPixelFormat>(sourceFrame->format);
        const AVPixelFormat swsSourceFormat = normalizeDeprecatedPixelFormat(sourceFormat);
        if (sourceFormat == AV_PIX_FMT_NV12) {
            copyPlane(dstData[0], dstLinesize[0], sourceFrame->data[0],
                      sourceFrame->linesize[0], sourceFrame->width, sourceFrame->height);
            copyPlane(dstData[1], dstLinesize[1], sourceFrame->data[1],
                      sourceFrame->linesize[1], sourceFrame->width, sourceFrame->height / 2);
            return true;
        }

        swsContext_ = sws_getCachedContext(
            swsContext_,
            sourceFrame->width,
            sourceFrame->height,
            swsSourceFormat,
            sourceFrame->width,
            sourceFrame->height,
            AV_PIX_FMT_NV12,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr);

        if (!swsContext_) {
            SPDLOG_ERROR("Failed to create NV12 pixel format converter");
            return false;
        }

        const int colorspace =
            sourceFrame->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_DEFAULT;
        const int sourceRange =
            sourceFrame->color_range == AVCOL_RANGE_JPEG ||
            isDeprecatedFullRangePixelFormat(sourceFormat)
                ? 1
                : 0;
        const int destinationRange = 0;
        sws_setColorspaceDetails(
            swsContext_,
            sws_getCoefficients(colorspace),
            sourceRange,
            sws_getCoefficients(colorspace),
            destinationRange,
            0,
            1 << 16,
            1 << 16);

        sws_scale(swsContext_, sourceFrame->data, sourceFrame->linesize, 0,
                  sourceFrame->height, dstData, dstLinesize);
        return true;
    }

    bool fillCudaNv12Frame(const AVFrame* sourceFrame, MediaFrame& mediaFrame) const {
        if (sourceFrame->format != AV_PIX_FMT_CUDA ||
            sourceFrame->data[0] == nullptr ||
            sourceFrame->data[1] == nullptr ||
            sourceFrame->linesize[0] <= 0 ||
            sourceFrame->linesize[1] <= 0) {
            return false;
        }

        if (sourceFrame->hw_frames_ctx != nullptr) {
            const auto* framesContext =
                reinterpret_cast<const AVHWFramesContext*>(sourceFrame->hw_frames_ctx->data);
            if (framesContext != nullptr && framesContext->sw_format != AV_PIX_FMT_NV12) {
                SPDLOG_WARN("CUDA frame software format is {}, expected nv12; using CPU fallback",
                            av_get_pix_fmt_name(framesContext->sw_format));
                return false;
            }
        }

        if (!cudaFrameLayoutLogged_) {
            cudaFrameLayoutLogged_ = true;
            SPDLOG_INFO("CUDA NV12 frame layout: size={}x{}, y_pitch={}, uv_pitch={}, y_ptr={}, uv_ptr={}",
                        sourceFrame->width,
                        sourceFrame->height,
                        sourceFrame->linesize[0],
                        sourceFrame->linesize[1],
                        static_cast<const void*>(sourceFrame->data[0]),
                        static_cast<const void*>(sourceFrame->data[1]));
        }

        AVFrame* retainedFrame = av_frame_alloc();
        if (!retainedFrame) {
            SPDLOG_WARN("Failed to allocate CUDA frame reference");
            return false;
        }

        const int ret = av_frame_ref(retainedFrame, sourceFrame);
        if (ret < 0) {
            SPDLOG_WARN("Failed to retain CUDA frame: {}", ffmpegError(ret));
            av_frame_free(&retainedFrame);
            return false;
        }

        mediaFrame.pixelFormat = MediaFrame::PixelFormat::CUDA_NV12;
        mediaFrame.gpuData[0] = reinterpret_cast<uintptr_t>(sourceFrame->data[0]);
        mediaFrame.gpuData[1] = reinterpret_cast<uintptr_t>(sourceFrame->data[1]);
        mediaFrame.gpuLinesize[0] = sourceFrame->linesize[0];
        mediaFrame.gpuLinesize[1] = sourceFrame->linesize[1];
        mediaFrame.hardwareFrameRef = std::shared_ptr<void>(
            retainedFrame,
            [](void* framePtr) {
                AVFrame* frame = static_cast<AVFrame*>(framePtr);
                av_frame_free(&frame);
            });
        return true;
    }

    bool hardwareDecodeRequested() const {
        return !hwDecodeBackend_.empty() &&
               hwDecodeBackend_ != "none" &&
               hwDecodeBackend_ != "off" &&
               hwDecodeBackend_ != "software" &&
               hwDecodeBackend_ != "cpu";
    }

    const AVCodec* selectDecoder(AVCodecID codecId, const AVCodec* softwareCodec) {
        if (hwDecodeBackend_ != "cuda") {
            return softwareCodec;
        }

        const char* decoderName = cudaDecoderName(codecId);
        if (!decoderName) {
            SPDLOG_WARN("No CUDA decoder mapping for codec '{}'. Falling back to software decoder '{}'",
                        avcodec_get_name(codecId), softwareCodec->name);
            hwDecodeStatus_ = "codec-unmapped";
            return softwareCodec;
        }

        const AVCodec* cudaCodec = avcodec_find_decoder_by_name(decoderName);
        if (!cudaCodec) {
            SPDLOG_WARN("CUDA decoder '{}' is not available in this FFmpeg build. Falling back to software decoder '{}'",
                        decoderName, softwareCodec->name);
            hwDecodeStatus_ = "cuvid-missing";
            return softwareCodec;
        }

        return cudaCodec;
    }

    bool prepareHardwareDecoder(const AVCodec* codec) {
        hwDecodeActive_ = false;
        hwPixelFormat_ = AV_PIX_FMT_NONE;
        hwDecodeStatus_ = hardwareDecodeRequested() ? "requested" : "off";

        if (!hardwareDecodeRequested()) {
            return false;
        }

        std::vector<std::string> candidates = {hwDecodeBackend_};
        if (hwDecodeBackend_ == "cuda") {
            candidates.push_back("d3d11va");
            candidates.push_back("dxva2");
        }

        const std::string requestedBackend = hwDecodeBackend_;
        for (const std::string& candidate : candidates) {
            hwPixelFormat_ = AV_PIX_FMT_NONE;

            const AVHWDeviceType deviceType = av_hwdevice_find_type_by_name(candidate.c_str());
            if (deviceType == AV_HWDEVICE_TYPE_NONE) {
                SPDLOG_WARN("Hardware decode backend '{}' is not supported by this FFmpeg build",
                            candidate);
                hwDecodeStatus_ = candidate + "-unsupported";
                continue;
            }

            for (int i = 0;; ++i) {
                const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
                if (!config) {
                    break;
                }

                if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0 &&
                    config->device_type == deviceType) {
                    hwPixelFormat_ = config->pix_fmt;
                    break;
                }
            }

            if (hwPixelFormat_ == AV_PIX_FMT_NONE) {
                SPDLOG_WARN("Codec '{}' does not expose {} hardware decode",
                            codec->name, candidate);
                hwDecodeStatus_ = candidate + "-codec";
                continue;
            }

            const int ret = av_hwdevice_ctx_create(&hwDeviceContext_, deviceType, nullptr, nullptr, 0);
            if (ret < 0) {
                SPDLOG_WARN("Failed to create {} hardware device: {}",
                            candidate, ffmpegError(ret));
                hwDecodeStatus_ = candidate + "-device";
                continue;
            }

            codecContext_->opaque = this;
            codecContext_->get_format = &Impl::getHardwarePixelFormat;
            codecContext_->hw_device_ctx = av_buffer_ref(hwDeviceContext_);
            if (!codecContext_->hw_device_ctx) {
                SPDLOG_WARN("Failed to reference {} hardware device", candidate);
                av_buffer_unref(&hwDeviceContext_);
                hwDecodeStatus_ = candidate + "-ref";
                continue;
            }

            hwDecodeBackend_ = candidate;
            hwDecodeActive_ = true;
            hwDecodeStatus_ = candidate;
            if (candidate != requestedBackend) {
                SPDLOG_WARN("Requested hardware decode '{}' is unavailable; using '{}' instead",
                            requestedBackend, candidate);
            }
            return true;
        }

        hwDecodeBackend_ = requestedBackend;
        SPDLOG_WARN("No requested hardware decode backend could be prepared. Falling back to software decode");
        return false;
    }

    bool recreateSoftwareCodecContext(const AVCodec* codec, const AVCodecParameters* codecParams) {
        if (codecContext_) {
            avcodec_free_context(&codecContext_);
        }
        if (hwDeviceContext_) {
            av_buffer_unref(&hwDeviceContext_);
        }

        hwDecodeActive_ = false;
        hwPixelFormat_ = AV_PIX_FMT_NONE;
        hwDecodeStatus_ = hardwareDecodeRequested() ? "fallback-cpu" : "off";
        waitingForVideoKeyframe_ = true;
        videoStarted_ = false;

        codecContext_ = avcodec_alloc_context3(codec);
        if (!codecContext_) {
            SPDLOG_ERROR("Failed to allocate fallback software codec context");
            return false;
        }

        int ret = avcodec_parameters_to_context(codecContext_, codecParams);
        if (ret < 0) {
            SPDLOG_ERROR("Failed to copy codec parameters for software fallback: {}", ffmpegError(ret));
            return false;
        }
        applyVideoStreamTiming(codecContext_);

        ret = avcodec_open2(codecContext_, codec, nullptr);
        if (ret < 0) {
            SPDLOG_ERROR("Failed to open fallback software codec: {}", ffmpegError(ret));
            return false;
        }

        return true;
    }

    void receiveLoop() {
        SPDLOG_INFO("Starting receive loop");

        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        AVFrame* softwareFrame = av_frame_alloc();
        if (!packet || !frame || !softwareFrame) {
            SPDLOG_ERROR("Failed to allocate decode packet or frame");
            av_packet_free(&packet);
            av_frame_free(&frame);
            av_frame_free(&softwareFrame);
            running_ = false;
            audioCv_.notify_all();
            return;
        }
        auto lastReadProgress = std::chrono::steady_clock::now();

        while (running_) {
            int ret = av_read_frame(formatContext_, packet);
            if (ret < 0) {
                if (ret == AVERROR(EAGAIN)) {
                    const auto stalledFor = std::chrono::steady_clock::now() - lastReadProgress;
                    if (stalledFor >= std::chrono::seconds(5)) {
                        SPDLOG_ERROR("RTSP read stalled for 5 seconds");
                        if (errorCallback_) {
                            errorCallback_("RTSP read stalled");
                        }
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }

                char errbuf[128];
                av_strerror(ret, errbuf, sizeof(errbuf));
                SPDLOG_ERROR("Read frame error: {}", errbuf);
                if (errorCallback_) {
                    errorCallback_(std::string("Read frame error: ") + errbuf);
                }
                break;
            }

            lastReadProgress = std::chrono::steady_clock::now();

            if (packet->stream_index == videoStream_) {
                if (waitingForVideoKeyframe_) {
                    if ((packet->flags & AV_PKT_FLAG_KEY) == 0) {
                        av_packet_unref(packet);
                        continue;
                    }

                    waitingForVideoKeyframe_ = false;
                    videoStarted_ = true;
                    clearAudioPacketQueue();
                    avcodec_flush_buffers(codecContext_);
                    SPDLOG_INFO("Video keyframe acquired; starting audio/video decode");
                }

                // 发送数据包到解码器
                ret = avcodec_send_packet(codecContext_, packet);
                if (ret < 0) {
                    SPDLOG_WARN("Send packet error: {}", ret);
                } else {
                    // 获取解码后的帧
                    while (ret >= 0) {
                        ret = avcodec_receive_frame(codecContext_, frame);
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                            break;
                        } else if (ret < 0) {
                            SPDLOG_ERROR("Receive frame error: {}", ret);
                            break;
                        }

                        AVFrame* outputFrame = frame;
                        if (hwDecodeActive_ && frame->format == hwPixelFormat_ &&
                            !(hardwareFrameOutputEnabled_ && frame->format == AV_PIX_FMT_CUDA)) {
                            av_frame_unref(softwareFrame);
                            ret = av_hwframe_transfer_data(softwareFrame, frame, 0);
                            if (ret < 0) {
                                SPDLOG_WARN("Failed to transfer hardware frame to CPU: {}", ffmpegError(ret));
                                av_frame_unref(frame);
                                continue;
                            }
                            softwareFrame->pts = frame->pts;
                            softwareFrame->pkt_dts = frame->pkt_dts;
                            softwareFrame->flags = frame->flags;
                            outputFrame = softwareFrame;
                        }

                        // 创建MediaFrame
                        auto mediaFrame = std::make_shared<MediaFrame>();
                        mediaFrame->type = MediaFrame::Type::VIDEO;
                        mediaFrame->width = outputFrame->width;
                        mediaFrame->height = outputFrame->height;
                        mediaFrame->pts = normalizedTimestamp(outputFrame);
                        mediaFrame->dts = outputFrame->pkt_dts;
                        mediaFrame->ptsSeconds = stableVideoTimestampSeconds(outputFrame);
                        mediaFrame->durationSeconds = videoFrameDurationSeconds_;
                        mediaFrame->keyFrame = (outputFrame->flags & AV_FRAME_FLAG_KEY) != 0;

                        bool mediaFrameReady = false;
                        if (hardwareFrameOutputEnabled_ &&
                            hwDecodeActive_ &&
                            frame->format == AV_PIX_FMT_CUDA &&
                            fillCudaNv12Frame(frame, *mediaFrame)) {
                            // Keep the frame in GPU memory; OpenGL will consume it through CUDA interop.
                            mediaFrameReady = true;
                        }

                        if (!mediaFrameReady &&
                            hwDecodeActive_ &&
                            outputFrame == frame &&
                            frame->format == hwPixelFormat_) {
                            av_frame_unref(softwareFrame);
                            ret = av_hwframe_transfer_data(softwareFrame, frame, 0);
                            if (ret < 0) {
                                SPDLOG_WARN("Failed to transfer hardware frame to CPU fallback: {}",
                                            ffmpegError(ret));
                                av_frame_unref(frame);
                                continue;
                            }
                            softwareFrame->pts = frame->pts;
                            softwareFrame->pkt_dts = frame->pkt_dts;
                            softwareFrame->flags = frame->flags;
                            outputFrame = softwareFrame;
                        }

                        if (!mediaFrameReady && !copyFrameAsNv12(outputFrame, *mediaFrame)) {
                            av_frame_unref(frame);
                            av_frame_unref(softwareFrame);
                            continue;
                        }

                        if (frameCallback_) {
                            frameCallback_(mediaFrame);
                        }

                        av_frame_unref(frame);
                        av_frame_unref(softwareFrame);
                    }
                }
            } else if (packet->stream_index == audioStream_ && audioCodecContext_ != nullptr) {
                if (!videoStarted_) {
                    av_packet_unref(packet);
                    continue;
                }
                enqueueAudioPacket(packet);
            }

            av_packet_unref(packet);
        }

        av_frame_free(&frame);
        av_frame_free(&softwareFrame);
        av_packet_free(&packet);

        running_ = false;
        audioCv_.notify_all();
        SPDLOG_INFO("Receive loop ended");
    }

    std::atomic<bool> running_;
    std::thread receiveThread_;
    std::thread audioThread_;
    std::mutex audioMutex_;
    std::condition_variable audioCv_;
    std::queue<AVPacket*> audioPacketQueue_;
    FrameCallback frameCallback_;
    ErrorCallback errorCallback_;

    int width_;
    int height_;

    AVFormatContext* formatContext_;
    AVCodecContext* codecContext_;
    AVCodecContext* audioCodecContext_;
    SwsContext* swsContext_;
    SwrContext* swrContext_;
    AVBufferRef* hwDeviceContext_;
    int videoStream_;
    int audioStream_;
    double videoFrameDurationSeconds_;
    double nextSyntheticVideoPtsSeconds_;
    double lastVideoPtsSeconds_;
    bool hasLastVideoPts_;
    std::string hwDecodeBackend_;
    AVPixelFormat hwPixelFormat_;
    bool hwDecodeActive_;
    std::string hwDecodeStatus_;
    bool hardwareFrameOutputEnabled_;
    bool audioEnabled_;
    RtspConnectionOptions connectionOptions_;
    mutable bool cudaFrameLayoutLogged_;
    bool waitingForVideoKeyframe_;
    bool videoStarted_;
    std::chrono::steady_clock::time_point openDeadline_;
};

// RtspClient implementation
RtspClient::RtspClient() : pImpl_(std::make_unique<Impl>()) {}

RtspClient::~RtspClient() = default;

bool RtspClient::connect(const std::string& url) {
    return pImpl_->connect(url);
}

void RtspClient::disconnect() {
    pImpl_->disconnect();
}

void RtspClient::start() {
    pImpl_->start();
}

void RtspClient::stop() {
    pImpl_->stop();
}

void RtspClient::setFrameCallback(FrameCallback callback) {
    pImpl_->setFrameCallback(std::move(callback));
}

void RtspClient::setErrorCallback(ErrorCallback callback) {
    pImpl_->setErrorCallback(std::move(callback));
}

void RtspClient::setConnectionOptions(const RtspConnectionOptions& options) {
    pImpl_->setConnectionOptions(options);
}

void RtspClient::setAudioEnabled(bool enabled) {
    pImpl_->setAudioEnabled(enabled);
}

void RtspClient::setHardwareDecode(const std::string& backend) {
    pImpl_->setHardwareDecode(backend);
}

void RtspClient::setHardwareFrameOutput(bool enabled) {
    pImpl_->setHardwareFrameOutput(enabled);
}

std::string RtspClient::getDecodeBackend() const {
    return pImpl_->getDecodeBackend();
}

std::string RtspClient::getHardwareDecodeStatus() const {
    return pImpl_->getHardwareDecodeStatus();
}

bool RtspClient::isRunning() const {
    return pImpl_->isRunning();
}

int RtspClient::getWidth() const { return pImpl_->getWidth(); }
int RtspClient::getHeight() const { return pImpl_->getHeight(); }

} // namespace rtsp
