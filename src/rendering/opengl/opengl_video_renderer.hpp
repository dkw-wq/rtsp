#pragma once

#include "frame_capture.hpp"
#include "video_renderer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
}

#ifdef RTSP_ENABLE_CUDA_INTEROP
#include <cuda_gl_interop.h>
#include <cuda_runtime.h>
#endif

#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_INFO_LOG_LENGTH
#define GL_INFO_LOG_LENGTH 0x8B84
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_TEXTURE1
#define GL_TEXTURE1 0x84C1
#endif
#ifndef GL_TEXTURE2
#define GL_TEXTURE2 0x84C2
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW 0x88E0
#endif
#ifndef GL_RED
#define GL_RED 0x1903
#endif
#ifndef GL_RG
#define GL_RG 0x8227
#endif
#ifndef GL_R8
#define GL_R8 0x8229
#endif
#ifndef GL_RG8
#define GL_RG8 0x822B
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW 0x88E4
#endif
#ifndef GL_DYNAMIC_DRAW
#define GL_DYNAMIC_DRAW 0x88E8
#endif
#ifndef GL_RGB
#define GL_RGB 0x1907
#endif

namespace rtsp::rendering::opengl {

using GlChar = char;
using GlCreateShader = GLuint(APIENTRY*)(GLenum);
using GlShaderSource = void(APIENTRY*)(GLuint, GLsizei, const GlChar* const*, const GLint*);
using GlCompileShader = void(APIENTRY*)(GLuint);
using GlGetShaderiv = void(APIENTRY*)(GLuint, GLenum, GLint*);
using GlGetShaderInfoLog = void(APIENTRY*)(GLuint, GLsizei, GLsizei*, GlChar*);
using GlDeleteShader = void(APIENTRY*)(GLuint);
using GlCreateProgram = GLuint(APIENTRY*)();
using GlAttachShader = void(APIENTRY*)(GLuint, GLuint);
using GlLinkProgram = void(APIENTRY*)(GLuint);
using GlGetProgramiv = void(APIENTRY*)(GLuint, GLenum, GLint*);
using GlGetProgramInfoLog = void(APIENTRY*)(GLuint, GLsizei, GLsizei*, GlChar*);
using GlDeleteProgram = void(APIENTRY*)(GLuint);
using GlUseProgram = void(APIENTRY*)(GLuint);
using GlGetUniformLocation = GLint(APIENTRY*)(GLuint, const GlChar*);
using GlUniform1i = void(APIENTRY*)(GLint, GLint);
using GlUniform1iv = void(APIENTRY*)(GLint, GLsizei, const GLint*);
using GlActiveTexture = void(APIENTRY*)(GLenum);
using GlGenBuffers = void(APIENTRY*)(GLsizei, GLuint*);
using GlBindBuffer = void(APIENTRY*)(GLenum, GLuint);
using GlBufferData = void(APIENTRY*)(GLenum, std::ptrdiff_t, const void*, GLenum);
using GlDeleteBuffers = void(APIENTRY*)(GLsizei, const GLuint*);
using GlGenVertexArrays = void(APIENTRY*)(GLsizei, GLuint*);
using GlBindVertexArray = void(APIENTRY*)(GLuint);
using GlDeleteVertexArrays = void(APIENTRY*)(GLsizei, const GLuint*);
using GlEnableVertexAttribArray = void(APIENTRY*)(GLuint);
using GlVertexAttribPointer = void(APIENTRY*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
using GlUniform2f = void(APIENTRY*)(GLint, GLfloat, GLfloat);
using GlUniform4f = void(APIENTRY*)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);

struct GlApi {
    GlCreateShader createShader = nullptr;
    GlShaderSource shaderSource = nullptr;
    GlCompileShader compileShader = nullptr;
    GlGetShaderiv getShaderiv = nullptr;
    GlGetShaderInfoLog getShaderInfoLog = nullptr;
    GlDeleteShader deleteShader = nullptr;
    GlCreateProgram createProgram = nullptr;
    GlAttachShader attachShader = nullptr;
    GlLinkProgram linkProgram = nullptr;
    GlGetProgramiv getProgramiv = nullptr;
    GlGetProgramInfoLog getProgramInfoLog = nullptr;
    GlDeleteProgram deleteProgram = nullptr;
    GlUseProgram useProgram = nullptr;
    GlGetUniformLocation getUniformLocation = nullptr;
    GlUniform1i uniform1i = nullptr;
    GlUniform1iv uniform1iv = nullptr;
    GlActiveTexture activeTexture = nullptr;
    GlGenBuffers genBuffers = nullptr;
    GlBindBuffer bindBuffer = nullptr;
    GlBufferData bufferData = nullptr;
    GlDeleteBuffers deleteBuffers = nullptr;
    GlGenVertexArrays genVertexArrays = nullptr;
    GlBindVertexArray bindVertexArray = nullptr;
    GlDeleteVertexArrays deleteVertexArrays = nullptr;
    GlEnableVertexAttribArray enableVertexAttribArray = nullptr;
    GlVertexAttribPointer vertexAttribPointer = nullptr;
    GlUniform2f uniform2f = nullptr;
    GlUniform4f uniform4f = nullptr;

    bool load();
};

constexpr size_t kMaxFilterStages = 4;

enum class ShaderFilter : GLint {
    None = 0,
    Grayscale = 1,
    Warm = 2,
    Invert = 3,
    Contrast = 4,
    Saturation = 5
};

ShaderFilter parseFilterMode(std::string filterName);
const char* filterName(ShaderFilter filterMode);

struct ShaderFilterPipeline {
    std::array<ShaderFilter, kMaxFilterStages> filters{};
    size_t count = 0;

    static ShaderFilterPipeline fromNames(const std::vector<std::string>& filterNames);
    static ShaderFilterPipeline single(ShaderFilter filter);
    void setPreviewFilter(ShaderFilter filter);
    std::array<GLint, kMaxFilterStages> toUniformModes() const;
    std::string describe() const;
};

struct ViewportRect {
    GLint x = 0;
    GLint y = 0;
    GLsizei width = 0;
    GLsizei height = 0;
};

ViewportRect calculateAspectFitViewport(int drawableWidth,
                                        int drawableHeight,
                                        int videoWidth,
                                        int videoHeight);
ViewportRect calculateAspectFitViewport(int drawableWidth,
                                        int drawableHeight,
                                        int videoWidth,
                                        int videoHeight,
                                        uint32_t slotIndex,
                                        uint32_t slotCount);

class OpenGlVideoRenderer final : public VideoRenderer {
public:
    explicit OpenGlVideoRenderer(const std::string& filterName = "none");
    explicit OpenGlVideoRenderer(const std::vector<std::string>& filterNames);
    ~OpenGlVideoRenderer() override;

    bool initialize(int width, int height, const std::string& title) override;
    bool render(const std::shared_ptr<MediaFrame>& frame) override;
    bool render(const std::vector<std::shared_ptr<MediaFrame>>& frames) override;
    void setPlaybackStats(const PlaybackStats& stats) override;
    void setFaceOverlays(const std::vector<FaceDetectionResult>& overlays) override;
    bool handleEvents() override;
    void close() override;

    int getWidth() const override;
    int getHeight() const override;
    bool isInitialized() const override;

private:
    bool createPrograms();
    bool createGeometry();
    GLuint compileShader(GLenum type, const char* source);
    GLuint linkProgram(GLuint vertexShader, GLuint fragmentShader);
    bool createTextures();
    void setupTexture(GLuint texture) const;
    void uploadFilterPipeline();
    void applyVideoViewport() const;
    void applyVideoViewportFor(int videoWidth,
                               int videoHeight,
                               uint32_t slotIndex,
                               uint32_t slotCount) const;
    bool uploadNv12Textures(const uint8_t* y, const uint8_t* uv, int width, int height);
    void drawCurrentTextures();
    bool renderNv12(const uint8_t* y, const uint8_t* uv, int width, int height);
    bool renderCudaNv12(const MediaFrame& frame);
    bool finishFrameRender();
    bool handleKeyboardShortcuts();
    bool readBackBufferRgb(RgbFrame& frame) const;
    void handleCaptureAfterRender();
    void saveScreenshot(const RgbFrame& frame);
    void toggleRecording();
#ifdef RTSP_ENABLE_CUDA_INTEROP
    bool registerCudaInterop();
    void unregisterCudaInterop();
    bool uploadCudaFrameToTextures(const MediaFrame& frame);
    bool downloadCudaFrameToNv12(const MediaFrame& frame, std::vector<uint8_t>& data) const;
#endif
    void drawOverlay();
    void drawFaceOverlays(int drawableWidth, int drawableHeight);
    void drawRectOutline(float x, float y, float width, float height, float thickness);
    void drawText(float x, float y, const std::string& text, float scale);
    void drawRect(float x, float y, float width, float height);
    void flushOverlay(float red, float green, float blue, float alpha);
    std::array<std::string, 12> makeOverlayLines() const;

    SDL_Window* window_;
    SDL_GLContext glContext_;
    GlApi gl_;

    GLuint program_;
    GLuint overlayProgram_;
    GLuint videoVao_;
    GLuint videoVbo_;
    GLuint overlayVao_;
    GLuint overlayVbo_;
    std::array<GLuint, 2> textures_;
#ifdef RTSP_ENABLE_CUDA_INTEROP
    std::array<GLuint, 2> pixelUnpackBuffers_;
    std::array<cudaGraphicsResource_t, 2> cudaResources_;
    bool cudaInteropRegistered_;
#endif
    GLint yLocation_;
    GLint uvLocation_;
    GLint filterCountLocation_;
    GLint filterModesLocation_;
    GLint overlayScreenSizeLocation_;
    GLint overlayColorLocation_;
    ShaderFilterPipeline filterPipeline_;
    std::vector<float> overlayVertices_;
    int previewFilterMode_;
    PlaybackStats playbackStats_;
    std::vector<FaceDetectionResult> faceOverlays_;
    RgbVideoRecorder recorder_;
    bool screenshotRequested_;
    int recordingFps_;
    bool fKeyDown_;
    bool sKeyDown_;
    bool rKeyDown_;
    int textureWidth_;
    int textureHeight_;
    uint32_t activeVideoSlots_;

    int width_;
    int height_;
    bool initialized_;
    bool apiLoaded_;
};

} // namespace rtsp::rendering::opengl
