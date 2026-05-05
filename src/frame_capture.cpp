#include "frame_capture.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace rtsp {

namespace {

std::string ffmpegError(int errorCode) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(errorCode, buffer.data(), static_cast<size_t>(buffer.size()));
    return buffer.data();
}

void writeLe16(std::ofstream& output, uint16_t value) {
    output.put(static_cast<char>(value & 0xFFU));
    output.put(static_cast<char>((value >> 8U) & 0xFFU));
}

void writeLe32(std::ofstream& output, uint32_t value) {
    output.put(static_cast<char>(value & 0xFFU));
    output.put(static_cast<char>((value >> 8U) & 0xFFU));
    output.put(static_cast<char>((value >> 16U) & 0xFFU));
    output.put(static_cast<char>((value >> 24U) & 0xFFU));
}

} // namespace

std::string makeCapturePath(const std::string& prefix, const std::string& extension) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &nowTime);
#else
    localtime_r(&nowTime, &localTime);
#endif

    std::ostringstream filename;
    filename << prefix << "_" << std::put_time(&localTime, "%Y%m%d_%H%M%S")
             << "_" << std::setw(3) << std::setfill('0') << millis
             << extension;

    const std::filesystem::path directory =
        std::filesystem::current_path() / "captures";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        SPDLOG_WARN("Failed to create capture directory '{}': {}",
                    directory.string(), error.message());
        return filename.str();
    }

    return (directory / filename.str()).string();
}

bool saveRgbFrameAsBmp(const RgbFrame& frame, const std::string& path) {
    if (frame.width <= 0 || frame.height <= 0) {
        SPDLOG_WARN("Cannot save empty RGB frame");
        return false;
    }

    const size_t expectedSize =
        static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height) * 3U;
    if (frame.pixels.size() < expectedSize) {
        SPDLOG_WARN("RGB frame is too small for BMP output: {} < {}",
                    frame.pixels.size(), expectedSize);
        return false;
    }

    const uint32_t rowStride =
        static_cast<uint32_t>(((frame.width * 3) + 3) & ~3);
    const uint32_t pixelDataSize = rowStride * static_cast<uint32_t>(frame.height);
    const uint32_t fileHeaderSize = 14;
    const uint32_t dibHeaderSize = 40;
    const uint32_t pixelOffset = fileHeaderSize + dibHeaderSize;
    const uint32_t fileSize = pixelOffset + pixelDataSize;

    std::ofstream output(path, std::ios::binary);
    if (!output) {
        SPDLOG_WARN("Failed to open screenshot output: {}", path);
        return false;
    }

    output.put('B');
    output.put('M');
    writeLe32(output, fileSize);
    writeLe16(output, 0);
    writeLe16(output, 0);
    writeLe32(output, pixelOffset);

    writeLe32(output, dibHeaderSize);
    writeLe32(output, static_cast<uint32_t>(frame.width));
    writeLe32(output, static_cast<uint32_t>(frame.height));
    writeLe16(output, 1);
    writeLe16(output, 24);
    writeLe32(output, 0);
    writeLe32(output, pixelDataSize);
    writeLe32(output, 2835);
    writeLe32(output, 2835);
    writeLe32(output, 0);
    writeLe32(output, 0);

    std::vector<uint8_t> row(rowStride, 0);
    for (int y = frame.height - 1; y >= 0; --y) {
        const uint8_t* source =
            frame.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(frame.width) * 3U;
        for (int x = 0; x < frame.width; ++x) {
            row[static_cast<size_t>(x) * 3U] = source[static_cast<size_t>(x) * 3U + 2U];
            row[static_cast<size_t>(x) * 3U + 1U] = source[static_cast<size_t>(x) * 3U + 1U];
            row[static_cast<size_t>(x) * 3U + 2U] = source[static_cast<size_t>(x) * 3U];
        }
        output.write(reinterpret_cast<const char*>(row.data()),
                     static_cast<std::streamsize>(row.size()));
    }

    return output.good();
}

class RgbVideoRecorder::Impl {
public:
    Impl() = default;
    ~Impl() {
        stop();
    }

    bool start(const std::string& path, int width, int height, int fps) {
        stop();

        if (width <= 0 || height <= 0 || fps <= 0) {
            SPDLOG_WARN("Invalid recorder dimensions or fps: {}x{} @ {}", width, height, fps);
            return false;
        }

        path_ = path;
        width_ = width;
        height_ = height;
        fps_ = fps;

        int ret = avformat_alloc_output_context2(&formatContext_, nullptr, nullptr, path.c_str());
        if (ret < 0 || formatContext_ == nullptr) {
            SPDLOG_WARN("Failed to create recorder output '{}': {}", path, ffmpegError(ret));
            closeResources();
            return false;
        }

        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        if (!codec) {
            SPDLOG_WARN("MJPEG encoder is not available in this FFmpeg build");
            closeResources();
            return false;
        }

        stream_ = avformat_new_stream(formatContext_, nullptr);
        if (!stream_) {
            SPDLOG_WARN("Failed to create recorder stream");
            closeResources();
            return false;
        }

        codecContext_ = avcodec_alloc_context3(codec);
        if (!codecContext_) {
            SPDLOG_WARN("Failed to allocate recorder codec context");
            closeResources();
            return false;
        }

        codecContext_->codec_id = codec->id;
        codecContext_->codec_type = AVMEDIA_TYPE_VIDEO;
        codecContext_->width = width_;
        codecContext_->height = height_;
        codecContext_->time_base = AVRational{1, fps_};
        codecContext_->framerate = AVRational{fps_, 1};
        codecContext_->pix_fmt = AV_PIX_FMT_YUV420P;
        codecContext_->color_range = AVCOL_RANGE_JPEG;
        codecContext_->bit_rate = static_cast<int64_t>(width_) * height_ * fps_ * 3;

        if ((formatContext_->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
            codecContext_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        ret = avcodec_open2(codecContext_, codec, nullptr);
        if (ret < 0) {
            SPDLOG_WARN("Failed to open MJPEG encoder: {}", ffmpegError(ret));
            closeResources();
            return false;
        }

        ret = avcodec_parameters_from_context(stream_->codecpar, codecContext_);
        if (ret < 0) {
            SPDLOG_WARN("Failed to copy recorder codec parameters: {}", ffmpegError(ret));
            closeResources();
            return false;
        }
        stream_->time_base = codecContext_->time_base;

        if ((formatContext_->oformat->flags & AVFMT_NOFILE) == 0) {
            ret = avio_open(&formatContext_->pb, path.c_str(), AVIO_FLAG_WRITE);
            if (ret < 0) {
                SPDLOG_WARN("Failed to open recorder file '{}': {}", path, ffmpegError(ret));
                closeResources();
                return false;
            }
        }

        ret = avformat_write_header(formatContext_, nullptr);
        if (ret < 0) {
            SPDLOG_WARN("Failed to write recorder header: {}", ffmpegError(ret));
            closeResources();
            return false;
        }
        headerWritten_ = true;

        frame_ = av_frame_alloc();
        packet_ = av_packet_alloc();
        if (!frame_ || !packet_) {
            SPDLOG_WARN("Failed to allocate recorder frame or packet");
            closeResources();
            return false;
        }

        frame_->format = codecContext_->pix_fmt;
        frame_->width = width_;
        frame_->height = height_;
        ret = av_frame_get_buffer(frame_, 32);
        if (ret < 0) {
            SPDLOG_WARN("Failed to allocate recorder frame buffer: {}", ffmpegError(ret));
            closeResources();
            return false;
        }

        swsContext_ = sws_getContext(width_, height_, AV_PIX_FMT_RGB24,
                                     width_, height_, codecContext_->pix_fmt,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!swsContext_) {
            SPDLOG_WARN("Failed to create recorder RGB converter");
            closeResources();
            return false;
        }

        frameIndex_ = 0;
        SPDLOG_INFO("Recording started: {}", path_);
        return true;
    }

    void stop() {
        if (formatContext_ && headerWritten_) {
            encodeFrame(nullptr);
            const int ret = av_write_trailer(formatContext_);
            if (ret < 0) {
                SPDLOG_WARN("Failed to finalize recording '{}': {}", path_, ffmpegError(ret));
            } else if (!path_.empty()) {
                SPDLOG_INFO("Recording saved: {}", path_);
            }
        }

        closeResources();
    }

    bool recordFrame(const RgbFrame& frame) {
        if (!isRecording()) {
            return false;
        }

        if (frame.width != width_ || frame.height != height_) {
            SPDLOG_WARN("Recorder frame size changed: {}x{} expected {}x{}",
                        frame.width, frame.height, width_, height_);
            return false;
        }

        const size_t expectedSize =
            static_cast<size_t>(width_) * static_cast<size_t>(height_) * 3U;
        if (frame.pixels.size() < expectedSize) {
            SPDLOG_WARN("Recorder RGB frame is too small");
            return false;
        }

        const int ret = av_frame_make_writable(frame_);
        if (ret < 0) {
            SPDLOG_WARN("Recorder frame is not writable: {}", ffmpegError(ret));
            return false;
        }

        const uint8_t* sourceData[4] = {frame.pixels.data(), nullptr, nullptr, nullptr};
        const int sourceLinesize[4] = {width_ * 3, 0, 0, 0};
        sws_scale(swsContext_, sourceData, sourceLinesize, 0, height_,
                  frame_->data, frame_->linesize);
        frame_->pts = frameIndex_++;

        return encodeFrame(frame_);
    }

    bool isRecording() const {
        return formatContext_ != nullptr && codecContext_ != nullptr && headerWritten_;
    }

    const std::string& outputPath() const {
        return path_;
    }

private:
    bool encodeFrame(AVFrame* frame) {
        int ret = avcodec_send_frame(codecContext_, frame);
        if (ret < 0) {
            SPDLOG_WARN("Failed to send recorder frame: {}", ffmpegError(ret));
            return false;
        }

        while (ret >= 0) {
            ret = avcodec_receive_packet(codecContext_, packet_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                return true;
            }
            if (ret < 0) {
                SPDLOG_WARN("Failed to receive recorder packet: {}", ffmpegError(ret));
                return false;
            }

            av_packet_rescale_ts(packet_, codecContext_->time_base, stream_->time_base);
            packet_->stream_index = stream_->index;
            ret = av_interleaved_write_frame(formatContext_, packet_);
            av_packet_unref(packet_);
            if (ret < 0) {
                SPDLOG_WARN("Failed to write recorder packet: {}", ffmpegError(ret));
                return false;
            }
        }

        return true;
    }

    void closeResources() {
        if (swsContext_) {
            sws_freeContext(swsContext_);
            swsContext_ = nullptr;
        }
        if (frame_) {
            av_frame_free(&frame_);
        }
        if (packet_) {
            av_packet_free(&packet_);
        }
        if (codecContext_) {
            avcodec_free_context(&codecContext_);
        }
        if (formatContext_) {
            if ((formatContext_->oformat->flags & AVFMT_NOFILE) == 0 && formatContext_->pb) {
                avio_closep(&formatContext_->pb);
            }
            avformat_free_context(formatContext_);
            formatContext_ = nullptr;
        }

        stream_ = nullptr;
        headerWritten_ = false;
        frameIndex_ = 0;
        width_ = 0;
        height_ = 0;
        fps_ = 0;
    }

    AVFormatContext* formatContext_ = nullptr;
    AVCodecContext* codecContext_ = nullptr;
    AVStream* stream_ = nullptr;
    SwsContext* swsContext_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    std::string path_;
    int width_ = 0;
    int height_ = 0;
    int fps_ = 0;
    int64_t frameIndex_ = 0;
    bool headerWritten_ = false;
};

RgbVideoRecorder::RgbVideoRecorder()
    : pImpl_(std::make_unique<Impl>())
{}

RgbVideoRecorder::~RgbVideoRecorder() = default;

bool RgbVideoRecorder::start(const std::string& path, int width, int height, int fps) {
    return pImpl_->start(path, width, height, fps);
}

void RgbVideoRecorder::stop() {
    pImpl_->stop();
}

bool RgbVideoRecorder::recordFrame(const RgbFrame& frame) {
    return pImpl_->recordFrame(frame);
}

bool RgbVideoRecorder::isRecording() const {
    return pImpl_->isRecording();
}

const std::string& RgbVideoRecorder::outputPath() const {
    return pImpl_->outputPath();
}

} // namespace rtsp
