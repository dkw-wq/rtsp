#include "rtsp_frame_converter.hpp"

#include "rtsp_ffmpeg_utils.hpp"

#include <spdlog/spdlog.h>

#include <cstring>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

namespace rtsp {

namespace {

void copyPlane(uint8_t* dst,
               int dstStride,
               const uint8_t* src,
               int srcStride,
               int width,
               int height) {
    for (int row = 0; row < height; ++row) {
        std::memcpy(dst + row * dstStride, src + row * srcStride, width);
    }
}

} // namespace

Nv12FrameConverter::~Nv12FrameConverter() {
    if (swsContext_ != nullptr) {
        sws_freeContext(swsContext_);
    }
}

bool Nv12FrameConverter::copyFrameAsNv12(const AVFrame* sourceFrame, MediaFrame& mediaFrame) {
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
    const AVPixelFormat swsSourceFormat = ffmpeg::normalizeDeprecatedPixelFormat(sourceFormat);
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

    if (swsContext_ == nullptr) {
        SPDLOG_ERROR("Failed to create NV12 pixel format converter");
        return false;
    }

    const int colorspace =
        sourceFrame->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_DEFAULT;
    const int sourceRange =
        sourceFrame->color_range == AVCOL_RANGE_JPEG ||
        ffmpeg::isDeprecatedFullRangePixelFormat(sourceFormat)
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

bool Nv12FrameConverter::fillCudaNv12Frame(const AVFrame* sourceFrame, MediaFrame& mediaFrame) {
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
    if (retainedFrame == nullptr) {
        SPDLOG_WARN("Failed to allocate CUDA frame reference");
        return false;
    }

    const int ret = av_frame_ref(retainedFrame, sourceFrame);
    if (ret < 0) {
        SPDLOG_WARN("Failed to retain CUDA frame: {}", ffmpeg::errorToString(ret));
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

} // namespace rtsp
