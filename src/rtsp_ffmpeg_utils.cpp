#include "rtsp_ffmpeg_utils.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/opt.h>
}

namespace rtsp::ffmpeg {

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string errorToString(int errorCode) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errorCode, errbuf, sizeof(errbuf));
    return errbuf;
}

std::chrono::microseconds steadyNowMicros() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
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

RtspConnectionOptions normalizeConnectionOptions(RtspConnectionOptions options) {
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

namespace {

void setDictionaryInt(AVDictionary** options, const char* key, int value) {
    const std::string text = std::to_string(value);
    av_dict_set(options, key, text.c_str(), 0);
}

} // namespace

void applyConnectionOptions(AVDictionary** options, const RtspConnectionOptions& connectionOptions) {
    const int timeoutUs = connectionOptions.timeoutMs * 1000;
    av_dict_set(options, "rtsp_transport", connectionOptions.transport.c_str(), 0);
    setDictionaryInt(options, "buffer_size", connectionOptions.bufferSize);
    setDictionaryInt(options, "stimeout", timeoutUs);
    setDictionaryInt(options, "timeout", timeoutUs);
    setDictionaryInt(options, "rw_timeout", timeoutUs);

    if (!connectionOptions.lowLatency) {
        return;
    }

    av_dict_set(options, "fflags", "nobuffer", 0);
    av_dict_set(options, "flags", "low_delay", 0);
    setDictionaryInt(options, "max_delay", connectionOptions.maxDelayMs * 1000);
    setDictionaryInt(options, "analyzeduration", connectionOptions.analyzeDurationMs * 1000);
    setDictionaryInt(options, "probesize", connectionOptions.probeSizeBytes);
    setDictionaryInt(options, "reorder_queue_size", connectionOptions.reorderQueueSize);
}

bool frameTimestampSeconds(const AVFormatContext* formatContext,
                           const AVFrame* frame,
                           int streamIndex,
                           double& seconds) {
    int64_t timestamp = frame->best_effort_timestamp;
    if (timestamp == AV_NOPTS_VALUE) {
        timestamp = frame->pts;
    }
    if (formatContext == nullptr || timestamp == AV_NOPTS_VALUE || streamIndex < 0) {
        return false;
    }

    seconds = static_cast<double>(timestamp) *
              av_q2d(formatContext->streams[streamIndex]->time_base);
    return true;
}

double timestampSeconds(const AVFormatContext* formatContext,
                        const AVFrame* frame,
                        int streamIndex) {
    double seconds = 0.0;
    return frameTimestampSeconds(formatContext, frame, streamIndex, seconds) ? seconds : 0.0;
}

double streamFrameDurationSeconds(const AVFormatContext* formatContext, int videoStream) {
    if (formatContext == nullptr || videoStream < 0) {
        return 1.0 / 30.0;
    }

    const AVStream* stream = formatContext->streams[videoStream];
    AVRational frameRate = stream->avg_frame_rate;
    if (frameRate.num <= 0 || frameRate.den <= 0) {
        frameRate = stream->r_frame_rate;
    }

    if (frameRate.num > 0 && frameRate.den > 0) {
        return av_q2d(av_inv_q(frameRate));
    }

    return 1.0 / 30.0;
}

void applyVideoStreamTiming(const AVFormatContext* formatContext,
                            int videoStream,
                            AVCodecContext* context) {
    if (context == nullptr || formatContext == nullptr || videoStream < 0) {
        return;
    }

    const AVStream* stream = formatContext->streams[videoStream];
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

uint64_t normalizedTimestamp(const AVFrame* frame) {
    int64_t timestamp = frame->best_effort_timestamp;
    if (timestamp == AV_NOPTS_VALUE) {
        timestamp = frame->pts;
    }
    if (timestamp == AV_NOPTS_VALUE || timestamp < 0) {
        return 0;
    }

    return static_cast<uint64_t>(timestamp);
}

} // namespace rtsp::ffmpeg
