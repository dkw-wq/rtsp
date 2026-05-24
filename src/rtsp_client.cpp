#include "rtsp_client.hpp"
#include "jitter_buffer.hpp"
#include "rtsp_audio_decoder.hpp"
#include "rtsp_ffmpeg_utils.hpp"
#include "rtsp_frame_converter.hpp"
#include "rtsp_hardware_decoder.hpp"
#include "rtsp_recorder.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
}

namespace rtsp {

// Implementation struct
class RtspClient::Impl {
public:
    Impl() 
        : running_(false)
        , width_(0)
        , height_(0)
        , formatContext_(nullptr)
        , codecContext_(nullptr)
        , videoStream_(-1)
        , audioStream_(-1)
        , videoFrameDurationSeconds_(1.0 / 30.0)
        , nextSyntheticVideoPtsSeconds_(0.0)
        , lastVideoPtsSeconds_(0.0)
        , hasLastVideoPts_(false)
        , hardwareFrameOutputEnabled_(false)
        , audioEnabled_(true)
        , videoEnabled_(true)
        , connectionOptions_()
        , waitingForVideoKeyframe_(true)
        , videoStarted_(false)
        , interruptRequested_(false)
        , openDeadline_(std::chrono::steady_clock::time_point::max())
    {}

    ~Impl() {
        close();
    }

    bool connect(const std::string& url) {
        close();
        interruptRequested_ = false;

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
        ffmpeg::applyConnectionOptions(&options, connectionOptions_);

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
        openDeadline_ = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(connectionOptions_.timeoutMs);
        ret = avformat_find_stream_info(formatContext_, nullptr);
        openDeadline_ = std::chrono::steady_clock::time_point::max();
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

        if (videoEnabled_ && videoStream_ < 0) {
            SPDLOG_ERROR("No video stream found");
            close();
            return false;
        }

        if (videoEnabled_) {
            // 获取编解码器参数
            AVCodecParameters* codecParams = formatContext_->streams[videoStream_]->codecpar;
            width_ = codecParams->width;
            height_ = codecParams->height;
            videoFrameDurationSeconds_ =
                ffmpeg::streamFrameDurationSeconds(formatContext_, videoStream_);
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

            const AVCodec* codec =
                hardwareDecoder_.selectDecoder(codecParams->codec_id, softwareCodec);
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
            ffmpeg::applyVideoStreamTiming(formatContext_, videoStream_, codecContext_);

            bool hardwareDecodePrepared = hardwareDecoder_.prepare(codecContext_, codec);
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
                    SPDLOG_ERROR("Failed to copy codec parameters for software fallback: {}", ffmpeg::errorToString(ret));
                    close();
                    return false;
                }
                ffmpeg::applyVideoStreamTiming(formatContext_, videoStream_, codecContext_);
            }

            // 打开解码器
            ret = avcodec_open2(codecContext_, codec, nullptr);
            if (ret < 0) {
                if (hardwareDecodePrepared) {
                    SPDLOG_WARN("Failed to open codec with {} hardware decode: {}. Falling back to software decode",
                                hardwareDecoder_.backend(), ffmpeg::errorToString(ret));
                    if (!recreateSoftwareCodecContext(softwareCodec, codecParams)) {
                        close();
                        return false;
                    }
                } else {
                    SPDLOG_ERROR("Failed to open codec: {}", ffmpeg::errorToString(ret));
                    close();
                    return false;
                }
            }

            if (hardwareDecoder_.active()) {
                SPDLOG_INFO("Hardware decode active: {}", hardwareDecoder_.backend());
            } else {
                SPDLOG_INFO("Hardware decode inactive, using software decode");
            }
        } else {
            SPDLOG_INFO("Video decode disabled");
            videoStarted_ = true;
        }

        if (audioEnabled_ && audioStream_ >= 0) {
            if (!audioDecoder_.open(formatContext_, audioStream_)) {
                SPDLOG_WARN("Audio stream found but could not be opened; continuing video-only");
                audioDecoder_.close();
            }
        } else if (audioEnabled_) {
            SPDLOG_INFO("No audio stream found");
            if (!videoEnabled_) {
                close();
                return false;
            }
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
        recorder_.stop();
        audioDecoder_.close();
        
        if (codecContext_) {
            avcodec_free_context(&codecContext_);
            codecContext_ = nullptr;
        }

        hardwareDecoder_.resetForDisconnect();

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
        width_ = 0;
        height_ = 0;
    }

    void start() {
        if (running_) {
            return;
        }
        interruptRequested_ = false;
        running_ = true;
        if (audioDecoder_.isOpen()) {
            audioDecoder_.start(running_, frameCallback_);
        }
        receiveThread_ = std::thread(&Impl::receiveLoop, this);
    }

    void stop() {
        interruptRequested_ = true;
        running_ = false;
        audioDecoder_.notifyStop();
        if (receiveThread_.joinable()) {
            receiveThread_.join();
        }
        audioDecoder_.join();
        audioDecoder_.clearPacketQueue();
    }

    void setFrameCallback(FrameCallback callback) {
        frameCallback_ = std::move(callback);
    }

    void setErrorCallback(ErrorCallback callback) {
        errorCallback_ = std::move(callback);
    }

    void setConnectionOptions(const RtspConnectionOptions& options) {
        connectionOptions_ = ffmpeg::normalizeConnectionOptions(options);
    }

    void setAudioEnabled(bool enabled) {
        audioEnabled_ = enabled;
    }

    void setVideoEnabled(bool enabled) {
        videoEnabled_ = enabled;
    }

    void setHardwareDecode(const std::string& backend) {
        hardwareDecoder_.setBackend(ffmpeg::toLower(backend));
    }

    void setHardwareFrameOutput(bool enabled) {
        hardwareFrameOutputEnabled_ = enabled;
    }

    bool startRecording(const std::string& path) {
        return recorder_.start(formatContext_, videoStream_, path);
    }

    void stopRecording() {
        recorder_.stop();
    }

    bool isRecording() const {
        return recorder_.isRecording();
    }

    std::string recordingPath() const {
        return recorder_.path();
    }

    std::string getDecodeBackend() const {
        return hardwareDecoder_.active() ? hardwareDecoder_.backend() : "cpu";
    }

    std::string getHardwareDecodeStatus() const {
        return hardwareDecoder_.status();
    }

    bool isRunning() const {
        return running_.load();
    }

    int getWidth() const { return width_; }
    int getHeight() const { return height_; }

private:
    double stableVideoTimestampSeconds(const AVFrame* frame) {
        double candidate = 0.0;
        const bool hasCandidate =
            ffmpeg::frameTimestampSeconds(formatContext_, frame, videoStream_, candidate);
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

    static int interruptCallback(void* opaque) {
        auto* self = static_cast<Impl*>(opaque);
        if (self->interruptRequested_.load()) {
            return 1;
        }
        return std::chrono::steady_clock::now() > self->openDeadline_;
    }

    bool recreateSoftwareCodecContext(const AVCodec* codec, const AVCodecParameters* codecParams) {
        if (codecContext_) {
            avcodec_free_context(&codecContext_);
        }

        hardwareDecoder_.resetForSoftwareFallback();
        waitingForVideoKeyframe_ = true;
        videoStarted_ = false;

        codecContext_ = avcodec_alloc_context3(codec);
        if (!codecContext_) {
            SPDLOG_ERROR("Failed to allocate fallback software codec context");
            return false;
        }

        int ret = avcodec_parameters_to_context(codecContext_, codecParams);
        if (ret < 0) {
            SPDLOG_ERROR("Failed to copy codec parameters for software fallback: {}", ffmpeg::errorToString(ret));
            return false;
        }
        ffmpeg::applyVideoStreamTiming(formatContext_, videoStream_, codecContext_);

        ret = avcodec_open2(codecContext_, codec, nullptr);
        if (ret < 0) {
            SPDLOG_ERROR("Failed to open fallback software codec: {}", ffmpeg::errorToString(ret));
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
            audioDecoder_.notifyStop();
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

            if (videoEnabled_ && packet->stream_index == videoStream_ && codecContext_ != nullptr) {
                if (waitingForVideoKeyframe_) {
                    if ((packet->flags & AV_PKT_FLAG_KEY) == 0) {
                        av_packet_unref(packet);
                        continue;
                    }

                    waitingForVideoKeyframe_ = false;
                    videoStarted_ = true;
                    audioDecoder_.clearPacketQueue();
                    avcodec_flush_buffers(codecContext_);
                    SPDLOG_INFO("Video keyframe acquired; starting audio/video decode");
                }

                recorder_.writePacket(packet);

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
                        if (hardwareDecoder_.active() &&
                            frame->format == hardwareDecoder_.pixelFormat() &&
                            !(hardwareFrameOutputEnabled_ && frame->format == AV_PIX_FMT_CUDA)) {
                            av_frame_unref(softwareFrame);
                            ret = av_hwframe_transfer_data(softwareFrame, frame, 0);
                            if (ret < 0) {
                                SPDLOG_WARN("Failed to transfer hardware frame to CPU: {}", ffmpeg::errorToString(ret));
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
                        mediaFrame->pts = ffmpeg::normalizedTimestamp(outputFrame);
                        mediaFrame->dts = outputFrame->pkt_dts;
                        mediaFrame->ptsSeconds = stableVideoTimestampSeconds(outputFrame);
                        mediaFrame->durationSeconds = videoFrameDurationSeconds_;
                        mediaFrame->keyFrame = (outputFrame->flags & AV_FRAME_FLAG_KEY) != 0;
                        mediaFrame->recvTime = ffmpeg::steadyNowMicros();

                        bool mediaFrameReady = false;
                        if (hardwareFrameOutputEnabled_ &&
                            hardwareDecoder_.active() &&
                            frame->format == AV_PIX_FMT_CUDA &&
                            frameConverter_.fillCudaNv12Frame(frame, *mediaFrame)) {
                            // Keep the frame in GPU memory; OpenGL will consume it through CUDA interop.
                            mediaFrameReady = true;
                        }

                        if (!mediaFrameReady &&
                            hardwareDecoder_.active() &&
                            outputFrame == frame &&
                            frame->format == hardwareDecoder_.pixelFormat()) {
                            av_frame_unref(softwareFrame);
                            ret = av_hwframe_transfer_data(softwareFrame, frame, 0);
                            if (ret < 0) {
                                SPDLOG_WARN("Failed to transfer hardware frame to CPU fallback: {}",
                                            ffmpeg::errorToString(ret));
                                av_frame_unref(frame);
                                continue;
                            }
                            softwareFrame->pts = frame->pts;
                            softwareFrame->pkt_dts = frame->pkt_dts;
                            softwareFrame->flags = frame->flags;
                            outputFrame = softwareFrame;
                        }

                        if (!mediaFrameReady &&
                            !frameConverter_.copyFrameAsNv12(outputFrame, *mediaFrame)) {
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
            } else if (packet->stream_index == audioStream_ && audioDecoder_.isOpen()) {
                if (videoEnabled_ && !videoStarted_) {
                    av_packet_unref(packet);
                    continue;
                }
                audioDecoder_.enqueuePacket(packet);
            }

            av_packet_unref(packet);
        }

        av_frame_free(&frame);
        av_frame_free(&softwareFrame);
        av_packet_free(&packet);

        running_ = false;
        audioDecoder_.notifyStop();
        SPDLOG_INFO("Receive loop ended");
    }

    std::atomic<bool> running_;
    std::thread receiveThread_;
    FrameCallback frameCallback_;
    ErrorCallback errorCallback_;
    RtspAudioDecoder audioDecoder_;
    Nv12FrameConverter frameConverter_;
    HardwareDecoderContext hardwareDecoder_;
    RtspRecorder recorder_;

    int width_;
    int height_;

    AVFormatContext* formatContext_;
    AVCodecContext* codecContext_;
    int videoStream_;
    int audioStream_;
    double videoFrameDurationSeconds_;
    double nextSyntheticVideoPtsSeconds_;
    double lastVideoPtsSeconds_;
    bool hasLastVideoPts_;
    bool hardwareFrameOutputEnabled_;
    bool audioEnabled_;
    bool videoEnabled_;
    RtspConnectionOptions connectionOptions_;
    bool waitingForVideoKeyframe_;
    bool videoStarted_;
    std::atomic<bool> interruptRequested_;
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

void RtspClient::setVideoEnabled(bool enabled) {
    pImpl_->setVideoEnabled(enabled);
}

void RtspClient::setHardwareDecode(const std::string& backend) {
    pImpl_->setHardwareDecode(backend);
}

void RtspClient::setHardwareFrameOutput(bool enabled) {
    pImpl_->setHardwareFrameOutput(enabled);
}

bool RtspClient::startRecording(const std::string& path) {
    return pImpl_->startRecording(path);
}

void RtspClient::stopRecording() {
    pImpl_->stopRecording();
}

bool RtspClient::isRecording() const {
    return pImpl_->isRecording();
}

std::string RtspClient::recordingPath() const {
    return pImpl_->recordingPath();
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
