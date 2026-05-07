#include "frame_capture.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
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

struct EncoderCandidate {
    const char* name;
    AVCodecID codecId;
};

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

        stream_ = avformat_new_stream(formatContext_, nullptr);
        if (!stream_) {
            SPDLOG_WARN("Failed to create recorder stream");
            closeResources();
            return false;
        }

        if (!openBestEncoder()) {
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

        {
            std::lock_guard<std::mutex> lock(mutex_);
            queuedFrames_.clear();
            recording_ = true;
            stopping_ = false;
            firstFrameQueued_ = false;
            firstFrameTime_ = Clock::time_point{};
            lastQueuedPts_ = -1;
            droppedFrames_ = 0;
            maxQueuedFrames_ = static_cast<size_t>(std::max(fps_ * 2, 30));
        }

        worker_ = std::thread(&Impl::workerLoop, this);
        SPDLOG_INFO("Recording started: {}", path_);
        return true;
    }

    void stop() {
        const bool hadResources = formatContext_ != nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            recording_ = false;
            stopping_ = true;
        }
        queueCondition_.notify_one();

        if (worker_.joinable()) {
            worker_.join();
        }

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

        if (hadResources && droppedFrames_ > 0) {
            SPDLOG_INFO("Recording dropped {} frame(s) while the encoder was behind",
                        droppedFrames_);
        }
    }

    bool recordFrame(RgbFrame frame) {
        const auto now = Clock::now();

        std::lock_guard<std::mutex> lock(mutex_);
        if (!recording_) {
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

        int64_t pts = 0;
        if (!firstFrameQueued_) {
            firstFrameQueued_ = true;
            firstFrameTime_ = now;
        } else {
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                now - firstFrameTime_);
            pts = (elapsed.count() * static_cast<int64_t>(fps_)) / 1'000'000LL;
            if (pts <= lastQueuedPts_) {
                return true;
            }
        }

        while (queuedFrames_.size() >= maxQueuedFrames_) {
            queuedFrames_.pop_front();
            ++droppedFrames_;
            if (droppedFrames_ == 1 || (fps_ > 0 && droppedFrames_ % (fps_ * 10) == 0)) {
                SPDLOG_WARN("Recorder encoder is behind; dropping queued frames");
            }
        }

        lastQueuedPts_ = pts;
        queuedFrames_.push_back(QueuedFrame{std::move(frame), pts});
        queueCondition_.notify_one();
        return true;
    }

    bool isRecording() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return recording_;
    }

    bool wantsFrame() const {
        const auto now = Clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!recording_) {
            return false;
        }
        if (!firstFrameQueued_) {
            return true;
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            now - firstFrameTime_);
        const int64_t pts =
            (elapsed.count() * static_cast<int64_t>(fps_)) / 1'000'000LL;
        return pts > lastQueuedPts_;
    }

    const std::string& outputPath() const {
        return path_;
    }

private:
    using Clock = std::chrono::steady_clock;

    struct QueuedFrame {
        RgbFrame frame;
        int64_t pts = 0;
    };

    void workerLoop() {
        for (;;) {
            QueuedFrame queuedFrame;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                queueCondition_.wait(lock, [this]() {
                    return stopping_ || !queuedFrames_.empty();
                });

                if (queuedFrames_.empty()) {
                    if (stopping_) {
                        break;
                    }
                    continue;
                }

                queuedFrame = std::move(queuedFrames_.front());
                queuedFrames_.pop_front();
            }

            if (!encodeRgbFrame(queuedFrame.frame, queuedFrame.pts)) {
                std::lock_guard<std::mutex> lock(mutex_);
                recording_ = false;
                stopping_ = true;
                queuedFrames_.clear();
                break;
            }
        }
    }

    bool encodeRgbFrame(const RgbFrame& frame, int64_t pts) {
        const int ret = av_frame_make_writable(frame_);
        if (ret < 0) {
            SPDLOG_WARN("Recorder frame is not writable: {}", ffmpegError(ret));
            return false;
        }

        const uint8_t* sourceData[4] = {frame.pixels.data(), nullptr, nullptr, nullptr};
        const int sourceLinesize[4] = {width_ * 3, 0, 0, 0};
        sws_scale(swsContext_, sourceData, sourceLinesize, 0, height_,
                  frame_->data, frame_->linesize);
        frame_->pts = pts;

        return encodeFrame(frame_);
    }

    bool openBestEncoder() {
        const std::string muxerName =
            formatContext_->oformat && formatContext_->oformat->name
                ? formatContext_->oformat->name
                : "";
        const bool mp4Output = muxerName.find("mp4") != std::string::npos;
        const std::array<EncoderCandidate, 5> candidates = {{
            {"h264_nvenc", AV_CODEC_ID_H264},
            {"h264_mf", AV_CODEC_ID_H264},
            {"libx264", AV_CODEC_ID_H264},
            {"mpeg4", AV_CODEC_ID_MPEG4},
            {mp4Output ? nullptr : "mjpeg",
             mp4Output ? AV_CODEC_ID_NONE : AV_CODEC_ID_MJPEG}
        }};

        for (const EncoderCandidate& candidate : candidates) {
            if (candidate.codecId == AV_CODEC_ID_NONE) {
                continue;
            }

            const AVCodec* codec = nullptr;
            if (candidate.name) {
                codec = avcodec_find_encoder_by_name(candidate.name);
            } else {
                codec = avcodec_find_encoder(candidate.codecId);
            }
            if (!codec) {
                continue;
            }

            AVCodecContext* nextContext = avcodec_alloc_context3(codec);
            if (!nextContext) {
                SPDLOG_WARN("Failed to allocate recorder codec context");
                return false;
            }

            nextContext->codec_id = codec->id;
            nextContext->codec_type = AVMEDIA_TYPE_VIDEO;
            nextContext->width = width_;
            nextContext->height = height_;
            nextContext->time_base = AVRational{1, fps_};
            nextContext->framerate = AVRational{fps_, 1};
            nextContext->pix_fmt = AV_PIX_FMT_YUV420P;
            nextContext->gop_size = std::max(fps_, 1);
            nextContext->max_b_frames = 0;
            nextContext->bit_rate = targetBitRate(codec->id);
            nextContext->color_range =
                codec->id == AV_CODEC_ID_MJPEG ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;

            if ((formatContext_->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
                nextContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            }

            AVDictionary* options = nullptr;
            applyEncoderOptions(codec->name, codec->id, &options);
            const int ret = avcodec_open2(nextContext, codec, &options);
            av_dict_free(&options);
            if (ret < 0) {
                SPDLOG_WARN("Failed to open recorder encoder '{}': {}",
                            codec->name, ffmpegError(ret));
                avcodec_free_context(&nextContext);
                continue;
            }

            codecContext_ = nextContext;
            SPDLOG_INFO("Recording encoder: {}", codec->name);
            return true;
        }

        SPDLOG_WARN("No suitable video recorder encoder is available");
        return false;
    }

    int64_t targetBitRate(AVCodecID codecId) const {
        if (codecId == AV_CODEC_ID_MJPEG) {
            return static_cast<int64_t>(width_) * height_ * fps_ * 3;
        }

        const int64_t pixels = static_cast<int64_t>(width_) * height_;
        return std::clamp(pixels * 4LL, 2'000'000LL, 16'000'000LL);
    }

    void applyEncoderOptions(const char* encoderName,
                             AVCodecID codecId,
                             AVDictionary** options) const {
        const std::string name = encoderName ? encoderName : "";
        if (name == "h264_nvenc") {
            av_dict_set(options, "preset", "p1", 0);
            av_dict_set(options, "tune", "ull", 0);
            av_dict_set(options, "delay", "0", 0);
        } else if (name == "h264_mf") {
            av_dict_set(options, "rate_control", "cbr", 0);
        } else if (name == "libx264") {
            av_dict_set(options, "preset", "ultrafast", 0);
            av_dict_set(options, "tune", "zerolatency", 0);
            av_dict_set(options, "crf", "24", 0);
        } else if (codecId == AV_CODEC_ID_MPEG4) {
            av_dict_set(options, "qscale", "5", 0);
        }
    }

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
    bool headerWritten_ = false;
    mutable std::mutex mutex_;
    std::condition_variable queueCondition_;
    std::deque<QueuedFrame> queuedFrames_;
    std::thread worker_;
    Clock::time_point firstFrameTime_{};
    size_t maxQueuedFrames_ = 0;
    int64_t lastQueuedPts_ = -1;
    int64_t droppedFrames_ = 0;
    bool recording_ = false;
    bool stopping_ = false;
    bool firstFrameQueued_ = false;
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
    return pImpl_->recordFrame(RgbFrame(frame));
}

bool RgbVideoRecorder::recordFrame(RgbFrame&& frame) {
    return pImpl_->recordFrame(std::move(frame));
}

bool RgbVideoRecorder::isRecording() const {
    return pImpl_->isRecording();
}

bool RgbVideoRecorder::wantsFrame() const {
    return pImpl_->wantsFrame();
}

const std::string& RgbVideoRecorder::outputPath() const {
    return pImpl_->outputPath();
}

} // namespace rtsp
