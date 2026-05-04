#include "frame_capture.hpp"
#include "video_renderer.hpp"
#include <spdlog/spdlog.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

extern "C" {
#include <SDL2/SDL.h>
}

namespace rtsp {

namespace {

bool isKeyDown(SDL_Scancode scancode, int windowsVirtualKey) {
    const uint8_t* keys = SDL_GetKeyboardState(nullptr);
    bool down = keys != nullptr && keys[scancode] != 0;
#ifdef _WIN32
    down = down || ((GetAsyncKeyState(windowsVirtualKey) & 0x8000) != 0);
#else
    (void)windowsVirtualKey;
#endif
    return down;
}

bool keyJustPressed(bool currentDown, bool& previousDown) {
    const bool pressed = currentDown && !previousDown;
    previousDown = currentDown;
    return pressed;
}

class SdlVideoRenderer final : public VideoRenderer {
public:
    SdlVideoRenderer();
    ~SdlVideoRenderer() override;

    bool initialize(int width, int height, const std::string& title) override;
    bool render(const std::shared_ptr<MediaFrame>& frame) override;
    void setPlaybackStats(const PlaybackStats& stats) override;
    bool handleEvents() override;
    void close() override;

    int getWidth() const override;
    int getHeight() const override;
    bool isInitialized() const override;

private:
    bool renderNv12(const uint8_t* y, const uint8_t* uv, int width, int height);
    bool handleKeyboardShortcuts();
    bool readRendererRgb(RgbFrame& frame) const;
    void handleCaptureAfterRender();
    void saveScreenshot(const RgbFrame& frame);
    void toggleRecording();

    SDL_Window* window_;
    SDL_Renderer* renderer_;
    SDL_Texture* texture_;

    int width_;
    int height_;
    bool initialized_;
    bool screenshotRequested_;
    int recordingFps_;
    RgbVideoRecorder recorder_;
    bool fKeyDown_;
    bool sKeyDown_;
    bool rKeyDown_;
};

SdlVideoRenderer::SdlVideoRenderer()
    : window_(nullptr)
    , renderer_(nullptr)
    , texture_(nullptr)
    , width_(0)
    , height_(0)
    , initialized_(false)
    , screenshotRequested_(false)
    , recordingFps_(30)
    , recorder_()
    , fKeyDown_(false)
    , sKeyDown_(false)
    , rKeyDown_(false)
{}

SdlVideoRenderer::~SdlVideoRenderer() {
    close();
}

bool SdlVideoRenderer::initialize(int width, int height, const std::string& title) {
    close();

    width_ = width;
    height_ = height;

    // 初始化SDL
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        SPDLOG_ERROR("SDL_Init failed: {}", SDL_GetError());
        return false;
    }

    // 创建窗口
    window_ = SDL_CreateWindow(
        title.c_str(),
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        width,
        height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );

    if (!window_) {
        SPDLOG_ERROR("Failed to create window: {}", SDL_GetError());
        SDL_Quit();
        return false;
    }

    // 创建渲染器
    renderer_ = SDL_CreateRenderer(
        window_,
        -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );

    if (!renderer_) {
        SPDLOG_ERROR("Failed to create renderer: {}", SDL_GetError());
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        SDL_Quit();
        return false;
    }

    // 创建NV12纹理
    texture_ = SDL_CreateTexture(
        renderer_,
        SDL_PIXELFORMAT_NV12,
        SDL_TEXTUREACCESS_STREAMING,
        width,
        height
    );

    if (!texture_) {
        SPDLOG_ERROR("Failed to create texture: {}", SDL_GetError());
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        SDL_Quit();
        return false;
    }

    // 设置渲染器颜色
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);

    initialized_ = true;
    SPDLOG_INFO("Video renderer initialized: {}x{}", width, height);
    return true;
}

bool SdlVideoRenderer::render(const std::shared_ptr<MediaFrame>& frame) {
    if (!initialized_ || !frame) {
        return false;
    }

    if (frame->pixelFormat != MediaFrame::PixelFormat::NV12) {
        SPDLOG_ERROR("SDL renderer expected NV12 frame");
        return false;
    }

    const size_t ySize = static_cast<size_t>(frame->width) * static_cast<size_t>(frame->height);
    return renderNv12(frame->data.data(), frame->data.data() + ySize,
                      frame->width, frame->height);
}

void SdlVideoRenderer::setPlaybackStats(const PlaybackStats&) {}

bool SdlVideoRenderer::renderNv12(const uint8_t* y, const uint8_t* uv,
                                  int width, int height) {
    if (!initialized_ || !y || !uv) {
        return false;
    }

    if (width != width_ || height != height_) {
        SPDLOG_ERROR("Frame size {}x{} does not match texture size {}x{}",
                     width, height, width_, height_);
        return false;
    }

    // 更新纹理
    const int pitch = width;
    int ret = SDL_UpdateNVTexture(
        texture_,
        nullptr,
        y,
        pitch,
        uv,
        pitch);

    if (ret < 0) {
        SPDLOG_ERROR("SDL_UpdateNVTexture failed: {}", SDL_GetError());
        return false;
    }

    // 清除渲染器
    SDL_RenderClear(renderer_);

    // 复制纹理到渲染器
    SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);

    handleCaptureAfterRender();

    // 渲染
    SDL_RenderPresent(renderer_);

    return true;
}

bool SdlVideoRenderer::handleEvents() {
    SDL_PumpEvents();
    SDL_Event event;
    
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                return false;
            case SDL_KEYDOWN:
                if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE ||
                    event.key.keysym.scancode == SDL_SCANCODE_Q ||
                    event.key.keysym.sym == SDLK_ESCAPE ||
                    event.key.keysym.sym == SDLK_q) {
                    return false;
                }
                break;
            default:
                break;
        }
    }

    return handleKeyboardShortcuts();
}

bool SdlVideoRenderer::handleKeyboardShortcuts() {
    SDL_PumpEvents();

    if (isKeyDown(SDL_SCANCODE_ESCAPE, VK_ESCAPE) ||
        isKeyDown(SDL_SCANCODE_Q, 'Q')) {
        return false;
    }

    if (keyJustPressed(isKeyDown(SDL_SCANCODE_S, 'S'), sKeyDown_)) {
        screenshotRequested_ = true;
        SPDLOG_INFO("Screenshot requested");
    }

    if (keyJustPressed(isKeyDown(SDL_SCANCODE_R, 'R'), rKeyDown_)) {
        toggleRecording();
    }

    if (keyJustPressed(isKeyDown(SDL_SCANCODE_F, 'F'), fKeyDown_)) {
        SPDLOG_INFO("Filter switching is only available with the OpenGL renderer");
    }

    return true;
}

void SdlVideoRenderer::close() {
    recorder_.stop();
    screenshotRequested_ = false;

    if (texture_) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }

    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }

    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }

    SDL_Quit();

    initialized_ = false;
    width_ = 0;
    height_ = 0;
}

bool SdlVideoRenderer::readRendererRgb(RgbFrame& frame) const {
    if (!initialized_ || !renderer_) {
        return false;
    }

    int outputWidth = 0;
    int outputHeight = 0;
    if (SDL_GetRendererOutputSize(renderer_, &outputWidth, &outputHeight) < 0 ||
        outputWidth <= 0 || outputHeight <= 0) {
        return false;
    }

    frame.width = outputWidth;
    frame.height = outputHeight;
    frame.pixels.resize(static_cast<size_t>(outputWidth) *
                        static_cast<size_t>(outputHeight) * 3U);

    if (SDL_RenderReadPixels(renderer_, nullptr, SDL_PIXELFORMAT_RGB24,
                             frame.pixels.data(), outputWidth * 3) < 0) {
        SPDLOG_WARN("SDL_RenderReadPixels failed: {}", SDL_GetError());
        return false;
    }

    return true;
}

void SdlVideoRenderer::handleCaptureAfterRender() {
    if (!screenshotRequested_ && !recorder_.isRecording()) {
        return;
    }

    RgbFrame frame;
    if (!readRendererRgb(frame)) {
        SPDLOG_WARN("Failed to capture SDL frame");
        screenshotRequested_ = false;
        return;
    }

    if (screenshotRequested_) {
        saveScreenshot(frame);
        screenshotRequested_ = false;
    }

    if (recorder_.isRecording() && !recorder_.recordFrame(frame)) {
        SPDLOG_WARN("Stopping recording after frame write failure");
        recorder_.stop();
    }
}

void SdlVideoRenderer::saveScreenshot(const RgbFrame& frame) {
    const std::string path = makeCapturePath("screenshot", ".bmp");
    if (saveRgbFrameAsBmp(frame, path)) {
        SPDLOG_INFO("Screenshot saved: {}", path);
    } else {
        SPDLOG_WARN("Failed to save screenshot: {}", path);
    }
}

void SdlVideoRenderer::toggleRecording() {
    if (recorder_.isRecording()) {
        recorder_.stop();
        return;
    }

    RgbFrame frame;
    if (!readRendererRgb(frame)) {
        SPDLOG_WARN("Cannot start recording before the first frame is rendered");
        return;
    }

    const std::string path = makeCapturePath("recording", ".avi");
    if (!recorder_.start(path, frame.width, frame.height, recordingFps_)) {
        SPDLOG_WARN("Failed to start recording: {}", path);
    }
}

int SdlVideoRenderer::getWidth() const { return width_; }
int SdlVideoRenderer::getHeight() const { return height_; }
bool SdlVideoRenderer::isInitialized() const { return initialized_; }

} // namespace

std::unique_ptr<VideoRenderer> createSdlVideoRenderer() {
    return std::make_unique<SdlVideoRenderer>();
}

} // namespace rtsp
