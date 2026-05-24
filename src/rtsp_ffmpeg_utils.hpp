#pragma once

#include "rtsp_client.hpp"

#include <chrono>
#include <cstdint>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

namespace rtsp::ffmpeg {

std::string toLower(std::string value);
std::string errorToString(int errorCode);
std::chrono::microseconds steadyNowMicros();

AVPixelFormat normalizeDeprecatedPixelFormat(AVPixelFormat format);
bool isDeprecatedFullRangePixelFormat(AVPixelFormat format);
const char* cudaDecoderName(AVCodecID codecId);

RtspConnectionOptions normalizeConnectionOptions(RtspConnectionOptions options);
void applyConnectionOptions(AVDictionary** options, const RtspConnectionOptions& connectionOptions);

bool frameTimestampSeconds(const AVFormatContext* formatContext,
                           const AVFrame* frame,
                           int streamIndex,
                           double& seconds);
double timestampSeconds(const AVFormatContext* formatContext,
                        const AVFrame* frame,
                        int streamIndex);
double streamFrameDurationSeconds(const AVFormatContext* formatContext, int videoStream);
void applyVideoStreamTiming(const AVFormatContext* formatContext,
                            int videoStream,
                            AVCodecContext* context);
uint64_t normalizedTimestamp(const AVFrame* frame);

} // namespace rtsp::ffmpeg
