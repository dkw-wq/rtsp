#pragma once

#include <mutex>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
}

namespace rtsp {

class RtspRecorder {
public:
    RtspRecorder() = default;
    ~RtspRecorder();

    RtspRecorder(const RtspRecorder&) = delete;
    RtspRecorder& operator=(const RtspRecorder&) = delete;

    bool start(AVFormatContext* inputContext, int videoStream, const std::string& path = "");
    void stop();
    void writePacket(const AVPacket* packet);
    bool isRecording() const;
    std::string path() const;

private:
    void closeLocked();

    mutable std::mutex mutex_;
    AVFormatContext* recordingContext_ = nullptr;
    std::string recordingPath_;
    int recordingInputStream_ = -1;
    int recordingOutputStream_ = -1;
    AVRational recordingInputTimeBase_{0, 1};
    AVRational recordingOutputTimeBase_{0, 1};
    int64_t recordingBasePts_ = AV_NOPTS_VALUE;
    int64_t recordingBaseDts_ = AV_NOPTS_VALUE;
    bool recordingHeaderWritten_ = false;
    bool recordingWaitingForKeyframe_ = false;
};

} // namespace rtsp
