#pragma once

#include "jitter_buffer.hpp"

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace rtsp {

class Nv12FrameConverter {
public:
    Nv12FrameConverter() = default;
    ~Nv12FrameConverter();

    Nv12FrameConverter(const Nv12FrameConverter&) = delete;
    Nv12FrameConverter& operator=(const Nv12FrameConverter&) = delete;

    bool copyFrameAsNv12(const AVFrame* sourceFrame, MediaFrame& mediaFrame);
    bool fillCudaNv12Frame(const AVFrame* sourceFrame, MediaFrame& mediaFrame);

private:
    SwsContext* swsContext_ = nullptr;
    bool cudaFrameLayoutLogged_ = false;
};

} // namespace rtsp
