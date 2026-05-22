#include "opengl_video_renderer.hpp"
#include <utility>
#include <spdlog/spdlog.h>
namespace rtsp::rendering::opengl {
bool OpenGlVideoRenderer::finishFrameRender() {
    gl_.activeTexture(GL_TEXTURE0);
    gl_.useProgram(program_);
    glClear(GL_COLOR_BUFFER_BIT);
    applyVideoViewport();

    drawCurrentTextures();

    handleCaptureAfterRender();
    drawOverlay();

    SDL_GL_SwapWindow(window_);
    return true;
}

bool OpenGlVideoRenderer::readBackBufferRgb(RgbFrame& frame) const {
    if (!window_) {
        return false;
    }

    int drawableWidth = 0;
    int drawableHeight = 0;
    SDL_GL_GetDrawableSize(window_, &drawableWidth, &drawableHeight);
    if (drawableWidth <= 0 || drawableHeight <= 0) {
        return false;
    }

    const size_t rowSize = static_cast<size_t>(drawableWidth) * 3U;
    std::vector<uint8_t> bottomUp(rowSize * static_cast<size_t>(drawableHeight));

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, drawableWidth, drawableHeight, GL_RGB, GL_UNSIGNED_BYTE,
                 bottomUp.data());

    frame.width = drawableWidth;
    frame.height = drawableHeight;
    frame.pixels.resize(bottomUp.size());

    for (int y = 0; y < drawableHeight; ++y) {
        const uint8_t* source =
            bottomUp.data() + static_cast<size_t>(drawableHeight - 1 - y) * rowSize;
        uint8_t* destination = frame.pixels.data() + static_cast<size_t>(y) * rowSize;
        std::memcpy(destination, source, rowSize);
    }

    return true;
}

void OpenGlVideoRenderer::handleCaptureAfterRender() {
    if (!screenshotRequested_ && !recorder_.wantsFrame()) {
        return;
    }

    RgbFrame frame;
    if (!readBackBufferRgb(frame)) {
        SPDLOG_WARN("Failed to capture OpenGL frame");
        screenshotRequested_ = false;
        return;
    }

    if (screenshotRequested_) {
        saveScreenshot(frame);
        screenshotRequested_ = false;
    }

    if (recorder_.isRecording() && !recorder_.recordFrame(std::move(frame))) {
        SPDLOG_WARN("Stopping recording after frame write failure");
        recorder_.stop();
    }
}

void OpenGlVideoRenderer::saveScreenshot(const RgbFrame& frame) {
    const std::string path = makeCapturePath("screenshot", ".bmp");
    if (saveRgbFrameAsBmp(frame, path)) {
        SPDLOG_INFO("Screenshot saved: {}", path);
    } else {
        SPDLOG_WARN("Failed to save screenshot: {}", path);
    }
}

void OpenGlVideoRenderer::toggleRecording() {
    if (recorder_.isRecording()) {
        recorder_.stop();
        return;
    }

    if (!window_) {
        SPDLOG_WARN("Cannot start recording before renderer is initialized");
        return;
    }

    int drawableWidth = 0;
    int drawableHeight = 0;
    SDL_GL_GetDrawableSize(window_, &drawableWidth, &drawableHeight);
    if (drawableWidth <= 0 || drawableHeight <= 0) {
        SPDLOG_WARN("Cannot start recording with empty drawable size");
        return;
    }

    const std::string path = makeCapturePath("recording", ".mp4");
    if (!recorder_.start(path, drawableWidth, drawableHeight, recordingFps_)) {
        SPDLOG_WARN("Failed to start recording: {}", path);
    }
}

} // namespace rtsp::rendering::opengl

