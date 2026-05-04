#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rtsp {

struct RgbFrame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
};

std::string makeCapturePath(const std::string& prefix, const std::string& extension);
bool saveRgbFrameAsBmp(const RgbFrame& frame, const std::string& path);

class RgbVideoRecorder {
public:
    RgbVideoRecorder();
    ~RgbVideoRecorder();

    RgbVideoRecorder(const RgbVideoRecorder&) = delete;
    RgbVideoRecorder& operator=(const RgbVideoRecorder&) = delete;

    bool start(const std::string& path, int width, int height, int fps);
    void stop();
    bool recordFrame(const RgbFrame& frame);

    bool isRecording() const;
    const std::string& outputPath() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl_;
};

} // namespace rtsp
