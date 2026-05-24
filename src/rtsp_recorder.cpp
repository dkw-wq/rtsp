#include "rtsp_recorder.hpp"

#include "frame_capture.hpp"
#include "rtsp_ffmpeg_utils.hpp"

#include <spdlog/spdlog.h>

namespace rtsp {

RtspRecorder::~RtspRecorder() {
    stop();
}

bool RtspRecorder::start(AVFormatContext* inputContext,
                         int videoStream,
                         const std::string& path) {
    if (inputContext == nullptr || videoStream < 0) {
        SPDLOG_WARN("Cannot start recording before RTSP is connected");
        return false;
    }

    const std::string outputPath =
        path.empty() ? makeCapturePath("recording", ".mp4") : path;

    std::lock_guard<std::mutex> lock(mutex_);
    closeLocked();

    int ret = avformat_alloc_output_context2(&recordingContext_,
                                             nullptr,
                                             nullptr,
                                             outputPath.c_str());
    if (ret < 0 || recordingContext_ == nullptr) {
        SPDLOG_WARN("Failed to create recording output '{}': {}",
                    outputPath, ffmpeg::errorToString(ret));
        closeLocked();
        return false;
    }

    const AVStream* inputStream = inputContext->streams[videoStream];
    AVStream* outputStream = avformat_new_stream(recordingContext_, nullptr);
    if (outputStream == nullptr) {
        SPDLOG_WARN("Failed to create recording video stream");
        closeLocked();
        return false;
    }

    ret = avcodec_parameters_copy(outputStream->codecpar, inputStream->codecpar);
    if (ret < 0) {
        SPDLOG_WARN("Failed to copy recording codec parameters: {}", ffmpeg::errorToString(ret));
        closeLocked();
        return false;
    }
    outputStream->codecpar->codec_tag = 0;
    outputStream->time_base = inputStream->time_base;

    if ((recordingContext_->oformat->flags & AVFMT_NOFILE) == 0) {
        ret = avio_open(&recordingContext_->pb, outputPath.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            SPDLOG_WARN("Failed to open recording file '{}': {}",
                        outputPath, ffmpeg::errorToString(ret));
            closeLocked();
            return false;
        }
    }

    ret = avformat_write_header(recordingContext_, nullptr);
    if (ret < 0) {
        SPDLOG_WARN("Failed to write recording header: {}", ffmpeg::errorToString(ret));
        closeLocked();
        return false;
    }

    recordingPath_ = outputPath;
    recordingInputStream_ = videoStream;
    recordingOutputStream_ = outputStream->index;
    recordingInputTimeBase_ = inputStream->time_base;
    recordingOutputTimeBase_ = outputStream->time_base;
    recordingBasePts_ = AV_NOPTS_VALUE;
    recordingBaseDts_ = AV_NOPTS_VALUE;
    recordingHeaderWritten_ = true;
    recordingWaitingForKeyframe_ = true;

    SPDLOG_INFO("RTSP remux recording started: {}", recordingPath_);
    return true;
}

void RtspRecorder::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    closeLocked();
}

bool RtspRecorder::isRecording() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return recordingContext_ != nullptr && recordingHeaderWritten_;
}

std::string RtspRecorder::path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return recordingPath_;
}

void RtspRecorder::writePacket(const AVPacket* packet) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (recordingContext_ == nullptr ||
        !recordingHeaderWritten_ ||
        packet->stream_index != recordingInputStream_) {
        return;
    }

    if (recordingWaitingForKeyframe_) {
        if ((packet->flags & AV_PKT_FLAG_KEY) == 0) {
            return;
        }
        recordingWaitingForKeyframe_ = false;
        recordingBasePts_ = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
        recordingBaseDts_ = packet->dts != AV_NOPTS_VALUE ? packet->dts : recordingBasePts_;
        if (recordingBasePts_ == AV_NOPTS_VALUE) {
            recordingBasePts_ = 0;
        }
        if (recordingBaseDts_ == AV_NOPTS_VALUE) {
            recordingBaseDts_ = recordingBasePts_;
        }
        SPDLOG_INFO("Recording first keyframe received");
    }

    AVPacket* outputPacket = av_packet_alloc();
    if (outputPacket == nullptr) {
        SPDLOG_WARN("Failed to allocate recording packet");
        closeLocked();
        return;
    }

    int ret = av_packet_ref(outputPacket, packet);
    if (ret < 0) {
        SPDLOG_WARN("Failed to reference recording packet: {}", ffmpeg::errorToString(ret));
        av_packet_free(&outputPacket);
        closeLocked();
        return;
    }

    if (outputPacket->pts != AV_NOPTS_VALUE) {
        outputPacket->pts -= recordingBasePts_;
    }
    if (outputPacket->dts != AV_NOPTS_VALUE) {
        outputPacket->dts -= recordingBaseDts_;
    }
    if (outputPacket->pts != AV_NOPTS_VALUE && outputPacket->pts < 0) {
        outputPacket->pts = 0;
    }
    if (outputPacket->dts != AV_NOPTS_VALUE && outputPacket->dts < 0) {
        outputPacket->dts = 0;
    }

    av_packet_rescale_ts(outputPacket,
                         recordingInputTimeBase_,
                         recordingOutputTimeBase_);
    outputPacket->stream_index = recordingOutputStream_;
    outputPacket->pos = -1;

    ret = av_interleaved_write_frame(recordingContext_, outputPacket);
    av_packet_free(&outputPacket);
    if (ret < 0) {
        SPDLOG_WARN("Failed to write recording packet: {}", ffmpeg::errorToString(ret));
        closeLocked();
    }
}

void RtspRecorder::closeLocked() {
    if (recordingContext_ != nullptr && recordingHeaderWritten_) {
        const int ret = av_write_trailer(recordingContext_);
        if (ret < 0) {
            SPDLOG_WARN("Failed to finalize recording '{}': {}",
                        recordingPath_, ffmpeg::errorToString(ret));
        } else if (!recordingPath_.empty()) {
            SPDLOG_INFO("RTSP remux recording saved: {}", recordingPath_);
        }
    }

    if (recordingContext_ != nullptr) {
        if ((recordingContext_->oformat->flags & AVFMT_NOFILE) == 0 &&
            recordingContext_->pb != nullptr) {
            avio_closep(&recordingContext_->pb);
        }
        avformat_free_context(recordingContext_);
        recordingContext_ = nullptr;
    }

    recordingInputStream_ = -1;
    recordingOutputStream_ = -1;
    recordingInputTimeBase_ = AVRational{0, 1};
    recordingOutputTimeBase_ = AVRational{0, 1};
    recordingBasePts_ = AV_NOPTS_VALUE;
    recordingBaseDts_ = AV_NOPTS_VALUE;
    recordingHeaderWritten_ = false;
    recordingWaitingForKeyframe_ = false;
}

} // namespace rtsp
