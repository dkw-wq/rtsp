#include "opengl_video_renderer.hpp"
#include "../common/renderer_input.hpp"
#include <memory>
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
namespace rtsp::rendering::opengl {
OpenGlVideoRenderer::OpenGlVideoRenderer(const std::string& filterName)
    : OpenGlVideoRenderer(std::vector<std::string>{filterName})
{}

OpenGlVideoRenderer::OpenGlVideoRenderer(const std::vector<std::string>& filterNames)
    : window_(nullptr)
    , glContext_(nullptr)
    , program_(0)
    , overlayProgram_(0)
    , videoVao_(0)
    , videoVbo_(0)
    , overlayVao_(0)
    , overlayVbo_(0)
    , textures_{0, 0}
#ifdef RTSP_ENABLE_CUDA_INTEROP
    , pixelUnpackBuffers_{0, 0}
    , cudaResources_{nullptr, nullptr}
    , cudaInteropRegistered_(false)
#endif
    , yLocation_(-1)
    , uvLocation_(-1)
    , filterCountLocation_(-1)
    , filterModesLocation_(-1)
    , overlayScreenSizeLocation_(-1)
    , overlayColorLocation_(-1)
    , filterPipeline_(ShaderFilterPipeline::fromNames(filterNames))
    , overlayVertices_()
    , previewFilterMode_(0)
    , playbackStats_()
    , faceOverlays_()
    , recorder_()
    , screenshotRequested_(false)
    , recordingFps_(15)
    , fKeyDown_(false)
    , sKeyDown_(false)
    , rKeyDown_(false)
    , textureWidth_(0)
    , textureHeight_(0)
    , activeVideoSlots_(1)
    , width_(0)
    , height_(0)
    , initialized_(false)
    , apiLoaded_(false)
{}

OpenGlVideoRenderer::~OpenGlVideoRenderer() {
    close();
}

bool OpenGlVideoRenderer::initialize(int width, int height, const std::string& title) {
    close();

    width_ = width;
    height_ = height;

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        SPDLOG_ERROR("SDL_Init failed: {}", SDL_GetError());
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    window_ = SDL_CreateWindow(
        title.c_str(),
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        width,
        height,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);

    if (!window_) {
        SPDLOG_ERROR("Failed to create OpenGL window: {}", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    }

    glContext_ = SDL_GL_CreateContext(window_);
    if (!glContext_) {
        SPDLOG_ERROR("Failed to create OpenGL context: {}", SDL_GetError());
        close();
        return false;
    }

    if (SDL_GL_MakeCurrent(window_, glContext_) < 0) {
        SPDLOG_ERROR("Failed to activate OpenGL context: {}", SDL_GetError());
        close();
        return false;
    }

    SDL_GL_SetSwapInterval(1);

    apiLoaded_ = gl_.load();
    if (!apiLoaded_ || !createPrograms() || !createGeometry() || !createTextures()) {
        close();
        return false;
    }

    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);

    initialized_ = true;
    SPDLOG_INFO("OpenGL 3.3 renderer initialized: {}x{}", width, height);
    return true;
}

bool OpenGlVideoRenderer::render(const std::shared_ptr<MediaFrame>& frame) {
    if (!initialized_ || !frame) {
        return false;
    }

    if (frame->pixelFormat == MediaFrame::PixelFormat::CUDA_NV12) {
        activeVideoSlots_ = 1;
        return renderCudaNv12(*frame);
    }

    if (frame->pixelFormat != MediaFrame::PixelFormat::NV12) {
        SPDLOG_ERROR("OpenGL renderer expected NV12 or CUDA_NV12 frame");
        return false;
    }

    const size_t ySize = static_cast<size_t>(frame->width) * static_cast<size_t>(frame->height);
    const size_t requiredSize = ySize * 3 / 2;
    if (frame->data.size() < requiredSize) {
        SPDLOG_ERROR("Frame data too small for NV12: {} < {}", frame->data.size(), requiredSize);
        return false;
    }

    return renderNv12(
        frame->data.data(),
        frame->data.data() + ySize,
        frame->width,
        frame->height);
}

bool OpenGlVideoRenderer::render(const std::vector<std::shared_ptr<MediaFrame>>& frames) {
    if (!initialized_ || frames.empty()) {
        return false;
    }

    const size_t slotCount = std::min<size_t>(frames.size(), 2);
    bool renderedAny = false;

    glClear(GL_COLOR_BUFFER_BIT);
    gl_.activeTexture(GL_TEXTURE0);
    gl_.useProgram(program_);

    for (size_t slot = 0; slot < slotCount; ++slot) {
        const auto& frame = frames[slot];
        if (!frame) {
            continue;
        }
        if (frame->pixelFormat != MediaFrame::PixelFormat::NV12) {
            SPDLOG_WARN("OpenGL multi-stream renderer expected CPU NV12 frame for stream {}",
                        slot + 1);
            continue;
        }

        const size_t ySize =
            static_cast<size_t>(frame->width) * static_cast<size_t>(frame->height);
        const size_t requiredSize = ySize * 3 / 2;
        if (frame->data.size() < requiredSize) {
            SPDLOG_WARN("Frame data too small for OpenGL stream {}: {} < {}",
                        slot + 1, frame->data.size(), requiredSize);
            continue;
        }

        if (!uploadNv12Textures(frame->data.data(),
                                frame->data.data() + ySize,
                                frame->width,
                                frame->height)) {
            continue;
        }

        applyVideoViewportFor(frame->width,
                              frame->height,
                              static_cast<uint32_t>(slot),
                              static_cast<uint32_t>(slotCount));
        drawCurrentTextures();
        renderedAny = true;
    }

    if (!renderedAny) {
        return false;
    }

    activeVideoSlots_ = static_cast<uint32_t>(slotCount);
    handleCaptureAfterRender();
    drawOverlay();
    SDL_GL_SwapWindow(window_);
    return true;
}

void OpenGlVideoRenderer::setPlaybackStats(const PlaybackStats& stats) {
    playbackStats_ = stats;
}

void OpenGlVideoRenderer::setFaceOverlays(const std::vector<FaceDetectionResult>& overlays) {
    faceOverlays_ = overlays;
}

bool OpenGlVideoRenderer::handleEvents() {
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
            case SDL_WINDOWEVENT:
                break;
            default:
                break;
        }
    }

    return handleKeyboardShortcuts();
}

bool OpenGlVideoRenderer::handleKeyboardShortcuts() {
    SDL_PumpEvents();

    if (isKeyDown(SDL_SCANCODE_ESCAPE, VK_ESCAPE) ||
        isKeyDown(SDL_SCANCODE_Q, 'Q')) {
        return false;
    }

    if (keyJustPressed(isKeyDown(SDL_SCANCODE_F, 'F'), fKeyDown_)) {
        previewFilterMode_ = (previewFilterMode_ + 1) % 6;
        filterPipeline_.setPreviewFilter(static_cast<ShaderFilter>(previewFilterMode_));
        uploadFilterPipeline();
        SPDLOG_INFO("OpenGL shader pipeline: {}", filterPipeline_.describe());
    }

    if (keyJustPressed(isKeyDown(SDL_SCANCODE_S, 'S'), sKeyDown_)) {
        screenshotRequested_ = true;
        SPDLOG_INFO("Screenshot requested");
    }

    if (keyJustPressed(isKeyDown(SDL_SCANCODE_R, 'R'), rKeyDown_)) {
        toggleRecording();
    }

    return true;
}

void OpenGlVideoRenderer::close() {
    recorder_.stop();
    screenshotRequested_ = false;

    if (glContext_) {
        SDL_GL_MakeCurrent(window_, glContext_);

#ifdef RTSP_ENABLE_CUDA_INTEROP
        unregisterCudaInterop();

        if (pixelUnpackBuffers_[0] != 0) {
            gl_.deleteBuffers(static_cast<GLsizei>(pixelUnpackBuffers_.size()),
                              pixelUnpackBuffers_.data());
            pixelUnpackBuffers_ = {0, 0};
        }
#endif

        if (textures_[0] != 0) {
            glDeleteTextures(static_cast<GLsizei>(textures_.size()), textures_.data());
            textures_ = {0, 0};
        }

        if (videoVbo_ != 0) {
            gl_.deleteBuffers(1, &videoVbo_);
            videoVbo_ = 0;
        }
        if (overlayVbo_ != 0) {
            gl_.deleteBuffers(1, &overlayVbo_);
            overlayVbo_ = 0;
        }
        if (videoVao_ != 0) {
            gl_.deleteVertexArrays(1, &videoVao_);
            videoVao_ = 0;
        }
        if (overlayVao_ != 0) {
            gl_.deleteVertexArrays(1, &overlayVao_);
            overlayVao_ = 0;
        }

        if (apiLoaded_ && program_ != 0) {
            gl_.deleteProgram(program_);
            program_ = 0;
        }
        if (apiLoaded_ && overlayProgram_ != 0) {
            gl_.deleteProgram(overlayProgram_);
            overlayProgram_ = 0;
        }
    }

    if (glContext_) {
        SDL_GL_DeleteContext(glContext_);
        glContext_ = nullptr;
    }

    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }

    SDL_QuitSubSystem(SDL_INIT_VIDEO);

    yLocation_ = -1;
    uvLocation_ = -1;
    filterCountLocation_ = -1;
    filterModesLocation_ = -1;
    overlayScreenSizeLocation_ = -1;
    overlayColorLocation_ = -1;
    overlayVertices_.clear();
    textureWidth_ = 0;
    textureHeight_ = 0;
    activeVideoSlots_ = 1;
    width_ = 0;
    height_ = 0;
    initialized_ = false;
    apiLoaded_ = false;
}

int OpenGlVideoRenderer::getWidth() const { return width_; }
int OpenGlVideoRenderer::getHeight() const { return height_; }
bool OpenGlVideoRenderer::isInitialized() const { return initialized_; }
} // namespace rtsp::rendering::opengl
namespace rtsp {
std::unique_ptr<VideoRenderer> createOpenGlVideoRenderer() {
    return std::make_unique<rendering::opengl::OpenGlVideoRenderer>();
}
std::unique_ptr<VideoRenderer> createOpenGlVideoRenderer(const std::string& filterName) {
    return std::make_unique<rendering::opengl::OpenGlVideoRenderer>(filterName);
}
std::unique_ptr<VideoRenderer> createOpenGlVideoRenderer(const std::vector<std::string>& filterNames) {
    return std::make_unique<rendering::opengl::OpenGlVideoRenderer>(filterNames);
}
} // namespace rtsp

