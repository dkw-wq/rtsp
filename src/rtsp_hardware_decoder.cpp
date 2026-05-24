#include "rtsp_hardware_decoder.hpp"

#include "rtsp_ffmpeg_utils.hpp"

#include <spdlog/spdlog.h>

#include <utility>
#include <vector>

extern "C" {
#include <libavutil/hwcontext.h>
}

namespace rtsp {

HardwareDecoderContext::~HardwareDecoderContext() {
    closeDevice();
}

void HardwareDecoderContext::setBackend(std::string backend) {
    backend_ = std::move(backend);
}

const std::string& HardwareDecoderContext::backend() const {
    return backend_;
}

std::string HardwareDecoderContext::status() const {
    return status_;
}

AVPixelFormat HardwareDecoderContext::pixelFormat() const {
    return pixelFormat_;
}

bool HardwareDecoderContext::active() const {
    return active_;
}

bool HardwareDecoderContext::requested() const {
    return !backend_.empty() &&
           backend_ != "none" &&
           backend_ != "off" &&
           backend_ != "software" &&
           backend_ != "cpu";
}

const AVCodec* HardwareDecoderContext::selectDecoder(AVCodecID codecId,
                                                     const AVCodec* softwareCodec) {
    if (backend_ != "cuda") {
        return softwareCodec;
    }

    const char* decoderName = ffmpeg::cudaDecoderName(codecId);
    if (decoderName == nullptr) {
        SPDLOG_WARN("No CUDA decoder mapping for codec '{}'. Falling back to software decoder '{}'",
                    avcodec_get_name(codecId), softwareCodec->name);
        status_ = "codec-unmapped";
        return softwareCodec;
    }

    const AVCodec* cudaCodec = avcodec_find_decoder_by_name(decoderName);
    if (cudaCodec == nullptr) {
        SPDLOG_WARN("CUDA decoder '{}' is not available in this FFmpeg build. Falling back to software decoder '{}'",
                    decoderName, softwareCodec->name);
        status_ = "cuvid-missing";
        return softwareCodec;
    }

    return cudaCodec;
}

bool HardwareDecoderContext::prepare(AVCodecContext* codecContext, const AVCodec* codec) {
    active_ = false;
    pixelFormat_ = AV_PIX_FMT_NONE;
    status_ = requested() ? "requested" : "off";

    if (!requested() || codecContext == nullptr) {
        return false;
    }

    std::vector<std::string> candidates = {backend_};
    if (backend_ == "cuda") {
        candidates.push_back("d3d11va");
        candidates.push_back("dxva2");
    }

    const std::string requestedBackend = backend_;
    for (const std::string& candidate : candidates) {
        pixelFormat_ = AV_PIX_FMT_NONE;

        const AVHWDeviceType deviceType = av_hwdevice_find_type_by_name(candidate.c_str());
        if (deviceType == AV_HWDEVICE_TYPE_NONE) {
            SPDLOG_WARN("Hardware decode backend '{}' is not supported by this FFmpeg build",
                        candidate);
            status_ = candidate + "-unsupported";
            continue;
        }

        for (int i = 0;; ++i) {
            const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
            if (config == nullptr) {
                break;
            }

            if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0 &&
                config->device_type == deviceType) {
                pixelFormat_ = config->pix_fmt;
                break;
            }
        }

        if (pixelFormat_ == AV_PIX_FMT_NONE) {
            SPDLOG_WARN("Codec '{}' does not expose {} hardware decode",
                        codec->name, candidate);
            status_ = candidate + "-codec";
            continue;
        }

        const int ret = av_hwdevice_ctx_create(&deviceContext_, deviceType, nullptr, nullptr, 0);
        if (ret < 0) {
            SPDLOG_WARN("Failed to create {} hardware device: {}",
                        candidate, ffmpeg::errorToString(ret));
            status_ = candidate + "-device";
            continue;
        }

        codecContext->opaque = this;
        codecContext->get_format = &HardwareDecoderContext::getHardwarePixelFormat;
        codecContext->hw_device_ctx = av_buffer_ref(deviceContext_);
        if (codecContext->hw_device_ctx == nullptr) {
            SPDLOG_WARN("Failed to reference {} hardware device", candidate);
            av_buffer_unref(&deviceContext_);
            status_ = candidate + "-ref";
            continue;
        }

        backend_ = candidate;
        active_ = true;
        status_ = candidate;
        if (candidate != requestedBackend) {
            SPDLOG_WARN("Requested hardware decode '{}' is unavailable; using '{}' instead",
                        requestedBackend, candidate);
        }
        return true;
    }

    backend_ = requestedBackend;
    SPDLOG_WARN("No requested hardware decode backend could be prepared. Falling back to software decode");
    return false;
}

void HardwareDecoderContext::resetForSoftwareFallback() {
    closeDevice();
    active_ = false;
    pixelFormat_ = AV_PIX_FMT_NONE;
    status_ = requested() ? "fallback-cpu" : "off";
}

void HardwareDecoderContext::resetForDisconnect() {
    closeDevice();
    pixelFormat_ = AV_PIX_FMT_NONE;
    active_ = false;
    status_ = requested() ? "not-connected" : "off";
}

void HardwareDecoderContext::closeDevice() {
    if (deviceContext_ != nullptr) {
        av_buffer_unref(&deviceContext_);
    }
}

AVPixelFormat HardwareDecoderContext::getHardwarePixelFormat(
    AVCodecContext* context,
    const AVPixelFormat* formats) {
    const auto* self = static_cast<const HardwareDecoderContext*>(context->opaque);
    for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == self->pixelFormat_) {
            return *format;
        }
    }

    SPDLOG_WARN("Requested hardware pixel format is unavailable, using software decode");
    return formats[0];
}

} // namespace rtsp
