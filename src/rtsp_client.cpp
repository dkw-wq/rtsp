#include "rtsp_client.hpp"
#include "decoder.hpp"
#include "video_renderer.hpp"
#include <spdlog/spdlog.h>
#include <cstring>
#include <stdexcept>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
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
        , swsContext_(nullptr)
        , videoStream_(-1)
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
        const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
        if (!codec) {
            SPDLOG_ERROR("Codec not found");
            close();
            return false;
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

        // 打开解码器
        ret = avcodec_open2(codecContext_, codec, nullptr);
        if (ret < 0) {
            SPDLOG_ERROR("Failed to open codec");
            close();
            return false;
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

        if (formatContext_) {
            avformat_close_input(&formatContext_);
            formatContext_ = nullptr;
        }

        videoStream_ = -1;
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

    static void copyPlane(uint8_t* dst, int dstStride, const uint8_t* src,
                          int srcStride, int width, int height) {
        for (int row = 0; row < height; ++row) {
            std::memcpy(dst + row * dstStride, src + row * srcStride, width);
        }
    }

    void receiveLoop() {
        SPDLOG_INFO("Starting receive loop");

        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
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

                        // 创建MediaFrame
                        auto mediaFrame = std::make_shared<MediaFrame>();
                        mediaFrame->type = MediaFrame::Type::VIDEO;
                        mediaFrame->width = frame->width;
                        mediaFrame->height = frame->height;
                        mediaFrame->pts = frame->pts;
                        mediaFrame->dts = frame->pkt_dts;
                        mediaFrame->keyFrame = (frame->flags & AV_FRAME_FLAG_KEY) != 0;

                        // 转成渲染器需要的紧密排列YUV420P数据
                        int ySize = frame->width * frame->height;
                        int uvSize = ySize / 4;
                        mediaFrame->data.resize(ySize * 3 / 2);

                        uint8_t* dstData[4] = {
                            mediaFrame->data.data(),
                            mediaFrame->data.data() + ySize,
                            mediaFrame->data.data() + ySize + uvSize,
                            nullptr
                        };
                        int dstLinesize[4] = {
                            frame->width,
                            frame->width / 2,
                            frame->width / 2,
                            0
                        };

                        const auto sourceFormat = static_cast<AVPixelFormat>(frame->format);
                        if (sourceFormat == AV_PIX_FMT_YUV420P) {
                            copyPlane(dstData[0], dstLinesize[0], frame->data[0],
                                      frame->linesize[0], frame->width, frame->height);
                            copyPlane(dstData[1], dstLinesize[1], frame->data[1],
                                      frame->linesize[1], frame->width / 2, frame->height / 2);
                            copyPlane(dstData[2], dstLinesize[2], frame->data[2],
                                      frame->linesize[2], frame->width / 2, frame->height / 2);
                        } else {
                            swsContext_ = sws_getCachedContext(
                                swsContext_,
                                frame->width,
                                frame->height,
                                sourceFormat,
                                frame->width,
                                frame->height,
                                AV_PIX_FMT_YUV420P,
                                SWS_BILINEAR,
                                nullptr,
                                nullptr,
                                nullptr);

                            if (!swsContext_) {
                                SPDLOG_ERROR("Failed to create pixel format converter");
                                av_frame_unref(frame);
                                continue;
                            }

                            sws_scale(swsContext_, frame->data, frame->linesize, 0,
                                      frame->height, dstData, dstLinesize);
                        }

                        if (frameCallback_) {
                            frameCallback_(mediaFrame);
                        }

                        av_frame_unref(frame);
                    }
                }
            }

            av_packet_unref(packet);
        }

        av_frame_free(&frame);
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
    int videoStream_;
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

bool RtspClient::isRunning() const {
    return pImpl_->isRunning();
}

int RtspClient::getWidth() const { return pImpl_->getWidth(); }
int RtspClient::getHeight() const { return pImpl_->getHeight(); }

} // namespace rtsp
