#include "decoder.hpp"
#include <spdlog/spdlog.h>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

namespace rtsp {

namespace {

void copyPlane(uint8_t* dst, int dstStride, const uint8_t* src,
               int srcStride, int width, int height) {
    for (int row = 0; row < height; ++row) {
        std::memcpy(dst + row * dstStride, src + row * srcStride, width);
    }
}

} // namespace

Decoder::Decoder()
    : codecContext_(nullptr)
    , frame_(nullptr)
    , packet_(nullptr)
    , swsContext_(nullptr)
    , width_(0)
    , height_(0)
    , initialized_(false)
{}

Decoder::~Decoder() {
    close();
}

bool Decoder::initialize(AVCodecID codecId, int width, int height) {
    close();

    width_ = width;
    height_ = height;

    // 查找解码器
    const AVCodec* codec = avcodec_find_decoder(codecId);
    if (!codec) {
        SPDLOG_ERROR("Codec not found: {}", avcodec_get_name(codecId));
        return false;
    }

    // 创建解码器上下文
    codecContext_ = avcodec_alloc_context3(codec);
    if (!codecContext_) {
        SPDLOG_ERROR("Failed to allocate codec context");
        return false;
    }

    // 设置解码器参数
    codecContext_->thread_count = 0;  // 自动选择线程数
    codecContext_->flags2 |= AV_CODEC_FLAG2_FAST;  // 启用快速解码

    // 打开解码器
    int ret = avcodec_open2(codecContext_, codec, nullptr);
    if (ret < 0) {
        SPDLOG_ERROR("Failed to open codec");
        avcodec_free_context(&codecContext_);
        return false;
    }

    // 分配帧和包
    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();

    if (!frame_ || !packet_) {
        SPDLOG_ERROR("Failed to allocate frame or packet");
        close();
        return false;
    }

    initialized_ = true;
    SPDLOG_INFO("Decoder initialized: {} {}x{}", avcodec_get_name(codecId), width, height);
    return true;
}

std::shared_ptr<MediaFrame> Decoder::decode(const uint8_t* data, size_t size, 
                                              int64_t pts, int64_t dts) {
    return decode(data, size, pts, dts, false);
}

std::shared_ptr<MediaFrame> Decoder::decode(const uint8_t* data, size_t size,
                                              int64_t pts, int64_t dts, bool keyFrame) {
    if (!initialized_ || !data || size == 0) {
        return nullptr;
    }

    // 设置包数据
    av_packet_unref(packet_);
    packet_->data = const_cast<uint8_t*>(data);
    packet_->size = static_cast<int>(size);
    packet_->pts = pts;
    packet_->dts = dts;
    packet_->flags = keyFrame ? AV_PKT_FLAG_KEY : 0;

    // 发送包到解码器
    int ret = avcodec_send_packet(codecContext_, packet_);
    if (ret < 0) {
        SPDLOG_WARN("Send packet error: {}", ret);
        return nullptr;
    }

    // 接收解码后的帧
    ret = avcodec_receive_frame(codecContext_, frame_);
    if (ret < 0) {
        if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
            SPDLOG_WARN("Receive frame error: {}", ret);
        }
        return nullptr;
    }

    // 创建MediaFrame
    auto mediaFrame = std::make_shared<MediaFrame>();
    mediaFrame->type = MediaFrame::Type::VIDEO;
    mediaFrame->pixelFormat = MediaFrame::PixelFormat::YUV420P;
    mediaFrame->width = frame_->width;
    mediaFrame->height = frame_->height;
    mediaFrame->pts = frame_->pts;
    mediaFrame->dts = frame_->pkt_dts;
    mediaFrame->keyFrame = (frame_->flags & AV_FRAME_FLAG_KEY) != 0;

    // 转成渲染器需要的紧密排列YUV420P数据
    int ySize = frame_->width * frame_->height;
    int uvSize = ySize / 4;
    mediaFrame->data.resize(ySize * 3 / 2);

    uint8_t* dstData[4] = {
        mediaFrame->data.data(),
        mediaFrame->data.data() + ySize,
        mediaFrame->data.data() + ySize + uvSize,
        nullptr
    };
    int dstLinesize[4] = {
        frame_->width,
        frame_->width / 2,
        frame_->width / 2,
        0
    };

    const auto sourceFormat = static_cast<AVPixelFormat>(frame_->format);
    if (sourceFormat == AV_PIX_FMT_YUV420P) {
        copyPlane(dstData[0], dstLinesize[0], frame_->data[0],
                  frame_->linesize[0], frame_->width, frame_->height);
        copyPlane(dstData[1], dstLinesize[1], frame_->data[1],
                  frame_->linesize[1], frame_->width / 2, frame_->height / 2);
        copyPlane(dstData[2], dstLinesize[2], frame_->data[2],
                  frame_->linesize[2], frame_->width / 2, frame_->height / 2);
    } else {
        swsContext_ = sws_getCachedContext(
            swsContext_,
            frame_->width,
            frame_->height,
            sourceFormat,
            frame_->width,
            frame_->height,
            AV_PIX_FMT_YUV420P,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr);

        if (!swsContext_) {
            SPDLOG_ERROR("Failed to create pixel format converter");
            av_frame_unref(frame_);
            return nullptr;
        }

        sws_scale(swsContext_, frame_->data, frame_->linesize, 0,
                  frame_->height, dstData, dstLinesize);
    }

    // 重置frame以释放引用
    av_frame_unref(frame_);

    return mediaFrame;
}

void Decoder::flush() {
    if (!initialized_) {
        return;
    }

    // 刷新解码器
    avcodec_send_packet(codecContext_, nullptr);
    while (avcodec_receive_frame(codecContext_, frame_) >= 0) {
        av_frame_unref(frame_);
    }
}

void Decoder::close() {
    if (frame_) {
        av_frame_free(&frame_);
        frame_ = nullptr;
    }

    if (packet_) {
        av_packet_free(&packet_);
        packet_ = nullptr;
    }

    if (swsContext_) {
        sws_freeContext(swsContext_);
        swsContext_ = nullptr;
    }

    if (codecContext_) {
        avcodec_free_context(&codecContext_);
        codecContext_ = nullptr;
    }

    initialized_ = false;
    width_ = 0;
    height_ = 0;
}

int Decoder::getWidth() const { return width_; }
int Decoder::getHeight() const { return height_; }
bool Decoder::isInitialized() const { return initialized_; }

} // namespace rtsp
