#pragma once

#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/pixfmt.h>
}

namespace rtsp {

class HardwareDecoderContext {
public:
    HardwareDecoderContext() = default;
    ~HardwareDecoderContext();

    HardwareDecoderContext(const HardwareDecoderContext&) = delete;
    HardwareDecoderContext& operator=(const HardwareDecoderContext&) = delete;

    void setBackend(std::string backend);
    const std::string& backend() const;
    std::string status() const;
    AVPixelFormat pixelFormat() const;
    bool active() const;
    bool requested() const;

    const AVCodec* selectDecoder(AVCodecID codecId, const AVCodec* softwareCodec);
    bool prepare(AVCodecContext* codecContext, const AVCodec* codec);
    void resetForSoftwareFallback();
    void resetForDisconnect();
    void closeDevice();

private:
    static AVPixelFormat getHardwarePixelFormat(AVCodecContext* context,
                                                const AVPixelFormat* formats);

    std::string backend_;
    AVPixelFormat pixelFormat_ = AV_PIX_FMT_NONE;
    bool active_ = false;
    std::string status_ = "off";
    AVBufferRef* deviceContext_ = nullptr;
};

} // namespace rtsp
