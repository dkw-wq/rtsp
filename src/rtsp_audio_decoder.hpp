#pragma once

#include "jitter_buffer.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

namespace rtsp {

class RtspAudioDecoder {
public:
    using FrameCallback = std::function<void(const std::shared_ptr<MediaFrame>&)>;

    RtspAudioDecoder() = default;
    ~RtspAudioDecoder();

    RtspAudioDecoder(const RtspAudioDecoder&) = delete;
    RtspAudioDecoder& operator=(const RtspAudioDecoder&) = delete;

    bool open(AVFormatContext* formatContext, int audioStream);
    void close();
    bool isOpen() const;

    void start(std::atomic<bool>& running, FrameCallback callback);
    void notifyStop();
    void join();

    void enqueuePacket(const AVPacket* packet);
    void clearPacketQueue();

private:
    bool fillAudioFrame(const AVFrame* sourceFrame, MediaFrame& mediaFrame);
    void decodeLoop();

    static constexpr int kOutputAudioSampleRate = 48000;
    static constexpr int kOutputAudioChannels = 2;
    static constexpr int kOutputAudioBytesPerSample = 2;
    static constexpr size_t kMaxAudioPacketQueue = 128;

    AVFormatContext* formatContext_ = nullptr;
    AVCodecContext* audioCodecContext_ = nullptr;
    SwrContext* swrContext_ = nullptr;
    int audioStream_ = -1;
    std::atomic<bool>* running_ = nullptr;
    FrameCallback frameCallback_;
    std::thread audioThread_;
    std::mutex audioMutex_;
    std::condition_variable audioCv_;
    std::queue<AVPacket*> audioPacketQueue_;
};

} // namespace rtsp
