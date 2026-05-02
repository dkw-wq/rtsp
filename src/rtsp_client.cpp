#include "rtsp_client.hpp"
#include "decoder.hpp"
#include "video_renderer.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>
#include <cstring>
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
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
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
        , swsContext_(nullptr)
        , hwDeviceContext_(nullptr)
        , videoStream_(-1)
        , hwPixelFormat_(AV_PIX_FMT_NONE)
        , hwDecodeActive_(false)
        , hwDecodeStatus_("off")
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

        openDeadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        formatContext_->interrupt_callback.callback = &Impl::interruptCallback;
        formatContext_->interrupt_callback.opaque = this;

        AVDictionary* options = nullptr;
        av_dict_set(&options, "rtsp_transport", "tcp", 0);  // 使用TCP
        av_dict_set(&options, "buffer_size", "1024000", 0);
        av_dict_set(&options, "stimeout", "5000000", 0);   // 兼容旧版本FFmpeg
        av_dict_set(&options, "timeout", "5000000", 0);    // 5秒超时
        av_dict_set(&options, "rw_timeout", "5000000", 0);

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

        // 查找视频流
        videoStream_ = -1;
        for (unsigned int i = 0; i < formatContext_->nb_streams; i++) {
            if (formatContext_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                videoStream_ = i;
                break;
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

        SPDLOG_INFO("Video: {}x{}, codec: {}", width_, height_, 
                    avcodec_get_name(codecParams->codec_id));

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

        SPDLOG_INFO("RTSP client connected successfully");
        return true;
    }

    void disconnect() {
        close();
    }

    void close() {
        stop();
        
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
        receiveThread_ = std::thread(&Impl::receiveLoop, this);
    }

    void stop() {
        running_ = false;
        if (receiveThread_.joinable()) {
            receiveThread_.join();
        }
    }

    void setFrameCallback(FrameCallback callback) {
        frameCallback_ = std::move(callback);
    }

    void setErrorCallback(ErrorCallback callback) {
        errorCallback_ = std::move(callback);
    }

    void setHardwareDecode(const std::string& backend) {
        hwDecodeBackend_ = toLower(backend);
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
            sourceFormat,
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

        sws_scale(swsContext_, sourceFrame->data, sourceFrame->linesize, 0,
                  sourceFrame->height, dstData, dstLinesize);
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
                        if (hwDecodeActive_ && frame->format == hwPixelFormat_) {
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
                        mediaFrame->pts = outputFrame->pts;
                        mediaFrame->dts = outputFrame->pkt_dts;
                        mediaFrame->keyFrame = (outputFrame->flags & AV_FRAME_FLAG_KEY) != 0;

                        if (!copyFrameAsNv12(outputFrame, *mediaFrame)) {
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
            }

            av_packet_unref(packet);
        }

        av_frame_free(&frame);
        av_frame_free(&softwareFrame);
        av_packet_free(&packet);

        running_ = false;
        SPDLOG_INFO("Receive loop ended");
    }

    std::atomic<bool> running_;
    std::thread receiveThread_;
    FrameCallback frameCallback_;
    ErrorCallback errorCallback_;

    int width_;
    int height_;

    AVFormatContext* formatContext_;
    AVCodecContext* codecContext_;
    SwsContext* swsContext_;
    AVBufferRef* hwDeviceContext_;
    int videoStream_;
    std::string hwDecodeBackend_;
    AVPixelFormat hwPixelFormat_;
    bool hwDecodeActive_;
    std::string hwDecodeStatus_;
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

void RtspClient::setHardwareDecode(const std::string& backend) {
    pImpl_->setHardwareDecode(backend);
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
