#include "frame_capture.hpp"
#include "video_renderer.hpp"

#include <array>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

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

namespace rtsp {

namespace {

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

#ifdef RTSP_ENABLE_CUDA_INTEROP
const char* cudaErrorName(cudaError_t error) {
    return cudaGetErrorString(error);
}
#endif

template <typename T>
bool loadGlFunction(T& target, const char* name) {
    void* proc = SDL_GL_GetProcAddress(name);
    if (!proc || sizeof(target) != sizeof(proc)) {
        SPDLOG_ERROR("Failed to load OpenGL function: {}", name);
        return false;
    }

    std::memcpy(&target, &proc, sizeof(target));
    return true;
}

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

    bool load() {
        return loadGlFunction(createShader, "glCreateShader") &&
               loadGlFunction(shaderSource, "glShaderSource") &&
               loadGlFunction(compileShader, "glCompileShader") &&
               loadGlFunction(getShaderiv, "glGetShaderiv") &&
               loadGlFunction(getShaderInfoLog, "glGetShaderInfoLog") &&
               loadGlFunction(deleteShader, "glDeleteShader") &&
               loadGlFunction(createProgram, "glCreateProgram") &&
               loadGlFunction(attachShader, "glAttachShader") &&
               loadGlFunction(linkProgram, "glLinkProgram") &&
               loadGlFunction(getProgramiv, "glGetProgramiv") &&
               loadGlFunction(getProgramInfoLog, "glGetProgramInfoLog") &&
               loadGlFunction(deleteProgram, "glDeleteProgram") &&
               loadGlFunction(useProgram, "glUseProgram") &&
               loadGlFunction(getUniformLocation, "glGetUniformLocation") &&
               loadGlFunction(uniform1i, "glUniform1i") &&
               loadGlFunction(uniform1iv, "glUniform1iv") &&
               loadGlFunction(activeTexture, "glActiveTexture") &&
               loadGlFunction(genBuffers, "glGenBuffers") &&
               loadGlFunction(bindBuffer, "glBindBuffer") &&
               loadGlFunction(bufferData, "glBufferData") &&
               loadGlFunction(deleteBuffers, "glDeleteBuffers") &&
               loadGlFunction(genVertexArrays, "glGenVertexArrays") &&
               loadGlFunction(bindVertexArray, "glBindVertexArray") &&
               loadGlFunction(deleteVertexArrays, "glDeleteVertexArrays") &&
               loadGlFunction(enableVertexAttribArray, "glEnableVertexAttribArray") &&
               loadGlFunction(vertexAttribPointer, "glVertexAttribPointer") &&
               loadGlFunction(uniform2f, "glUniform2f") &&
               loadGlFunction(uniform4f, "glUniform4f");
    }
};

constexpr const char* kVertexShader = R"(
#version 330 core
layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_texCoord;
out vec2 v_texCoord;

void main() {
    gl_Position = vec4(in_position, 0.0, 1.0);
    v_texCoord = in_texCoord;
}
)";

constexpr const char* kFragmentShader = R"(
#version 330 core
uniform sampler2D tex_y;
uniform sampler2D tex_uv;
uniform int filter_count;
uniform int filter_modes[4];
in vec2 v_texCoord;
out vec4 fragColor;

vec3 applyFilter(int mode, vec3 rgb) {
    if (mode == 1) {
        float gray = dot(rgb, vec3(0.299, 0.587, 0.114));
        return vec3(gray);
    }
    if (mode == 2) {
        return vec3(rgb.r * 1.08 + 0.03, rgb.g * 1.02, rgb.b * 0.90);
    }
    if (mode == 3) {
        return vec3(1.0) - rgb;
    }
    if (mode == 4) {
        return (rgb - vec3(0.5)) * 1.18 + vec3(0.5);
    }
    if (mode == 5) {
        float gray = dot(rgb, vec3(0.299, 0.587, 0.114));
        return mix(vec3(gray), rgb, 1.22);
    }

    return rgb;
}

void main() {
    float y = texture(tex_y, v_texCoord).r;
    vec2 uv = texture(tex_uv, v_texCoord).rg - vec2(0.5, 0.5);
    float u = uv.x;
    float v = uv.y;

    vec3 rgb;
    rgb.r = y + 1.402 * v;
    rgb.g = y - 0.344136 * u - 0.714136 * v;
    rgb.b = y + 1.772 * u;

    if (filter_count > 0) {
        rgb = applyFilter(filter_modes[0], rgb);
    }
    if (filter_count > 1) {
        rgb = applyFilter(filter_modes[1], rgb);
    }
    if (filter_count > 2) {
        rgb = applyFilter(filter_modes[2], rgb);
    }
    if (filter_count > 3) {
        rgb = applyFilter(filter_modes[3], rgb);
    }

    fragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)";

constexpr const char* kSolidVertexShader = R"(
#version 330 core
layout(location = 0) in vec2 in_position;
uniform vec2 screen_size;

void main() {
    vec2 zeroToOne = in_position / screen_size;
    vec2 clip = zeroToOne * 2.0 - 1.0;
    gl_Position = vec4(clip.x, -clip.y, 0.0, 1.0);
}
)";

constexpr const char* kSolidFragmentShader = R"(
#version 330 core
uniform vec4 solid_color;
out vec4 fragColor;

void main() {
    fragColor = solid_color;
}
)";

constexpr size_t kMaxFilterStages = 4;

enum class ShaderFilter : GLint {
    None = 0,
    Grayscale = 1,
    Warm = 2,
    Invert = 3,
    Contrast = 4,
    Saturation = 5
};

ShaderFilter parseFilterMode(std::string filterName) {
    std::transform(filterName.begin(), filterName.end(), filterName.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    if (filterName == "none" || filterName == "off" || filterName == "normal") {
        return ShaderFilter::None;
    }
    if (filterName == "grayscale" || filterName == "gray" || filterName == "mono") {
        return ShaderFilter::Grayscale;
    }
    if (filterName == "warm") {
        return ShaderFilter::Warm;
    }
    if (filterName == "invert" || filterName == "negative") {
        return ShaderFilter::Invert;
    }
    if (filterName == "contrast") {
        return ShaderFilter::Contrast;
    }
    if (filterName == "saturation" || filterName == "saturate") {
        return ShaderFilter::Saturation;
    }

    SPDLOG_WARN("Unknown OpenGL filter '{}', using none", filterName);
    return ShaderFilter::None;
}

const char* filterName(ShaderFilter filterMode) {
    switch (filterMode) {
        case ShaderFilter::Grayscale:
            return "grayscale";
        case ShaderFilter::Warm:
            return "warm";
        case ShaderFilter::Invert:
            return "invert";
        case ShaderFilter::Contrast:
            return "contrast";
        case ShaderFilter::Saturation:
            return "saturation";
        default:
            return "none";
    }
}

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

struct ShaderFilterPipeline {
    std::array<ShaderFilter, kMaxFilterStages> filters{};
    size_t count = 0;

    static ShaderFilterPipeline fromNames(const std::vector<std::string>& filterNames) {
        ShaderFilterPipeline pipeline;
        for (const std::string& name : filterNames) {
            if (pipeline.count >= pipeline.filters.size()) {
                SPDLOG_WARN("Ignoring OpenGL filter '{}' because the shader pipeline is full", name);
                continue;
            }

            const ShaderFilter filter = parseFilterMode(name);
            if (filter == ShaderFilter::None) {
                continue;
            }

            pipeline.filters[pipeline.count] = filter;
            ++pipeline.count;
        }

        return pipeline;
    }

    static ShaderFilterPipeline single(ShaderFilter filter) {
        ShaderFilterPipeline pipeline;
        if (filter != ShaderFilter::None) {
            pipeline.filters[0] = filter;
            pipeline.count = 1;
        }
        return pipeline;
    }

    void setPreviewFilter(ShaderFilter filter) {
        *this = single(filter);
    }

    std::array<GLint, kMaxFilterStages> toUniformModes() const {
        std::array<GLint, kMaxFilterStages> values{};
        for (size_t i = 0; i < count; ++i) {
            values[i] = static_cast<GLint>(filters[i]);
        }
        return values;
    }

    std::string describe() const {
        if (count == 0) {
            return "none";
        }

        std::string result;
        for (size_t i = 0; i < count; ++i) {
            if (!result.empty()) {
                result += " -> ";
            }
            result += filterName(filters[i]);
        }
        return result;
    }
};

struct ViewportRect {
    GLint x = 0;
    GLint y = 0;
    GLsizei width = 0;
    GLsizei height = 0;
};

ViewportRect calculateAspectFitViewport(int drawableWidth, int drawableHeight,
                                        int videoWidth, int videoHeight) {
    if (drawableWidth <= 0 || drawableHeight <= 0 ||
        videoWidth <= 0 || videoHeight <= 0) {
        return {};
    }

    const double drawableAspect =
        static_cast<double>(drawableWidth) / static_cast<double>(drawableHeight);
    const double videoAspect =
        static_cast<double>(videoWidth) / static_cast<double>(videoHeight);

    int fittedWidth = drawableWidth;
    int fittedHeight = drawableHeight;

    if (drawableAspect > videoAspect) {
        fittedWidth = static_cast<int>(static_cast<double>(drawableHeight) * videoAspect);
    } else {
        fittedHeight = static_cast<int>(static_cast<double>(drawableWidth) / videoAspect);
    }

    fittedWidth = std::max(1, fittedWidth);
    fittedHeight = std::max(1, fittedHeight);

    return {
        static_cast<GLint>((drawableWidth - fittedWidth) / 2),
        static_cast<GLint>((drawableHeight - fittedHeight) / 2),
        static_cast<GLsizei>(fittedWidth),
        static_cast<GLsizei>(fittedHeight)
    };
}

using Glyph = std::array<uint8_t, 7>;

Glyph glyphFor(char ch) {
    switch (ch) {
        case '0': return {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
        case '1': return {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
        case '2': return {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
        case '3': return {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
        case '4': return {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
        case '5': return {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};
        case '6': return {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E};
        case '7': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
        case '8': return {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
        case '9': return {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E};
        case 'A': return {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
        case 'B': return {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
        case 'C': return {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
        case 'D': return {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
        case 'E': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
        case 'F': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
        case 'G': return {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F};
        case 'H': return {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
        case 'I': return {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E};
        case 'K': return {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
        case 'L': return {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
        case 'M': return {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
        case 'N': return {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
        case 'O': return {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
        case 'P': return {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
        case 'R': return {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
        case 'S': return {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
        case 'T': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
        case 'U': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
        case 'V': return {0x11, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04};
        case 'W': return {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A};
        case 'X': return {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
        case 'Y': return {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
        case ':': return {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00};
        case '-': return {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
        case '.': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
        default: return {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    }
}

class OpenGlVideoRenderer final : public VideoRenderer {
public:
    explicit OpenGlVideoRenderer(const std::string& filterName = "none");
    explicit OpenGlVideoRenderer(const std::vector<std::string>& filterNames);
    ~OpenGlVideoRenderer() override;

    bool initialize(int width, int height, const std::string& title) override;
    bool render(const std::shared_ptr<MediaFrame>& frame) override;
    void setPlaybackStats(const PlaybackStats& stats) override;
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
    void drawText(float x, float y, const std::string& text, float scale);
    void drawRect(float x, float y, float width, float height);
    void flushOverlay(float red, float green, float blue, float alpha);
    std::array<std::string, 9> makeOverlayLines() const;

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
    RgbVideoRecorder recorder_;
    bool screenshotRequested_;
    int recordingFps_;
    bool fKeyDown_;
    bool sKeyDown_;
    bool rKeyDown_;

    int width_;
    int height_;
    bool initialized_;
    bool apiLoaded_;
};

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
    , recorder_()
    , screenshotRequested_(false)
    , recordingFps_(30)
    , fKeyDown_(false)
    , sKeyDown_(false)
    , rKeyDown_(false)
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
        SDL_Quit();
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

void OpenGlVideoRenderer::setPlaybackStats(const PlaybackStats& stats) {
    playbackStats_ = stats;
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

    SDL_Quit();

    yLocation_ = -1;
    uvLocation_ = -1;
    filterCountLocation_ = -1;
    filterModesLocation_ = -1;
    overlayScreenSizeLocation_ = -1;
    overlayColorLocation_ = -1;
    overlayVertices_.clear();
    width_ = 0;
    height_ = 0;
    initialized_ = false;
    apiLoaded_ = false;
}

int OpenGlVideoRenderer::getWidth() const { return width_; }
int OpenGlVideoRenderer::getHeight() const { return height_; }
bool OpenGlVideoRenderer::isInitialized() const { return initialized_; }

bool OpenGlVideoRenderer::createPrograms() {
    const GLuint vertexShader = compileShader(GL_VERTEX_SHADER, kVertexShader);
    if (vertexShader == 0) {
        return false;
    }

    const GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (fragmentShader == 0) {
        gl_.deleteShader(vertexShader);
        return false;
    }

    program_ = linkProgram(vertexShader, fragmentShader);
    if (program_ == 0) {
        return false;
    }

    gl_.useProgram(program_);
    yLocation_ = gl_.getUniformLocation(program_, "tex_y");
    uvLocation_ = gl_.getUniformLocation(program_, "tex_uv");
    filterCountLocation_ = gl_.getUniformLocation(program_, "filter_count");
    filterModesLocation_ = gl_.getUniformLocation(program_, "filter_modes[0]");
    gl_.uniform1i(yLocation_, 0);
    gl_.uniform1i(uvLocation_, 1);
    uploadFilterPipeline();
    SPDLOG_INFO("OpenGL shader pipeline: {}", filterPipeline_.describe());

    const GLuint solidVertexShader = compileShader(GL_VERTEX_SHADER, kSolidVertexShader);
    if (solidVertexShader == 0) {
        return false;
    }

    const GLuint solidFragmentShader = compileShader(GL_FRAGMENT_SHADER, kSolidFragmentShader);
    if (solidFragmentShader == 0) {
        gl_.deleteShader(solidVertexShader);
        return false;
    }

    overlayProgram_ = linkProgram(solidVertexShader, solidFragmentShader);
    if (overlayProgram_ == 0) {
        return false;
    }

    gl_.useProgram(overlayProgram_);
    overlayScreenSizeLocation_ = gl_.getUniformLocation(overlayProgram_, "screen_size");
    overlayColorLocation_ = gl_.getUniformLocation(overlayProgram_, "solid_color");

    return true;
}

GLuint OpenGlVideoRenderer::linkProgram(GLuint vertexShader, GLuint fragmentShader) {
    const GLuint linkedProgram = gl_.createProgram();
    gl_.attachShader(linkedProgram, vertexShader);
    gl_.attachShader(linkedProgram, fragmentShader);
    gl_.linkProgram(linkedProgram);

    GLint linked = GL_FALSE;
    gl_.getProgramiv(linkedProgram, GL_LINK_STATUS, &linked);

    gl_.deleteShader(vertexShader);
    gl_.deleteShader(fragmentShader);

    if (linked == GL_TRUE) {
        return linkedProgram;
    }

    GLint logLength = 0;
    gl_.getProgramiv(linkedProgram, GL_INFO_LOG_LENGTH, &logLength);
    std::string log(static_cast<size_t>(logLength), '\0');
    gl_.getProgramInfoLog(linkedProgram, logLength, nullptr, log.data());
    SPDLOG_ERROR("OpenGL shader program link failed: {}", log);
    gl_.deleteProgram(linkedProgram);
    return 0;
}

void OpenGlVideoRenderer::uploadFilterPipeline() {
    if (!apiLoaded_ || program_ == 0) {
        return;
    }

    gl_.useProgram(program_);
    if (filterCountLocation_ >= 0) {
        gl_.uniform1i(filterCountLocation_, static_cast<GLint>(filterPipeline_.count));
    }
    if (filterModesLocation_ >= 0) {
        const auto uniformModes = filterPipeline_.toUniformModes();
        gl_.uniform1iv(filterModesLocation_,
                       static_cast<GLsizei>(uniformModes.size()),
                       uniformModes.data());
    }
}

void OpenGlVideoRenderer::applyVideoViewport() const {
    if (!window_) {
        return;
    }

    int drawableWidth = 0;
    int drawableHeight = 0;
    SDL_GL_GetDrawableSize(window_, &drawableWidth, &drawableHeight);
    const ViewportRect viewport =
        calculateAspectFitViewport(drawableWidth, drawableHeight, width_, height_);

    glViewport(viewport.x, viewport.y, viewport.width, viewport.height);
}

GLuint OpenGlVideoRenderer::compileShader(GLenum type, const char* source) {
    const GLuint shader = gl_.createShader(type);
    const GlChar* sourcePtr = source;
    gl_.shaderSource(shader, 1, &sourcePtr, nullptr);
    gl_.compileShader(shader);

    GLint compiled = GL_FALSE;
    gl_.getShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) {
        return shader;
    }

    GLint logLength = 0;
    gl_.getShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
    std::string log(static_cast<size_t>(logLength), '\0');
    gl_.getShaderInfoLog(shader, logLength, nullptr, log.data());
    SPDLOG_ERROR("OpenGL shader compile failed: {}", log);
    gl_.deleteShader(shader);
    return 0;
}

bool OpenGlVideoRenderer::createGeometry() {
    constexpr std::array<float, 16> videoVertices = {
        -1.0F, -1.0F, 0.0F, 1.0F,
         1.0F, -1.0F, 1.0F, 1.0F,
        -1.0F,  1.0F, 0.0F, 0.0F,
         1.0F,  1.0F, 1.0F, 0.0F
    };

    gl_.genVertexArrays(1, &videoVao_);
    gl_.genBuffers(1, &videoVbo_);
    if (videoVao_ == 0 || videoVbo_ == 0) {
        SPDLOG_ERROR("Failed to create OpenGL video geometry");
        return false;
    }

    gl_.bindVertexArray(videoVao_);
    gl_.bindBuffer(GL_ARRAY_BUFFER, videoVbo_);
    gl_.bufferData(GL_ARRAY_BUFFER,
                   static_cast<std::ptrdiff_t>(videoVertices.size() * sizeof(float)),
                   videoVertices.data(),
                   GL_STATIC_DRAW);
    gl_.enableVertexAttribArray(0);
    gl_.vertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                            static_cast<GLsizei>(4 * sizeof(float)),
                            nullptr);
    gl_.enableVertexAttribArray(1);
    gl_.vertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                            static_cast<GLsizei>(4 * sizeof(float)),
                            reinterpret_cast<const void*>(2 * sizeof(float)));

    gl_.genVertexArrays(1, &overlayVao_);
    gl_.genBuffers(1, &overlayVbo_);
    if (overlayVao_ == 0 || overlayVbo_ == 0) {
        SPDLOG_ERROR("Failed to create OpenGL overlay geometry");
        return false;
    }

    gl_.bindVertexArray(overlayVao_);
    gl_.bindBuffer(GL_ARRAY_BUFFER, overlayVbo_);
    gl_.bufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);
    gl_.enableVertexAttribArray(0);
    gl_.vertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                            static_cast<GLsizei>(2 * sizeof(float)),
                            nullptr);

    gl_.bindBuffer(GL_ARRAY_BUFFER, 0);
    gl_.bindVertexArray(0);
    return true;
}

bool OpenGlVideoRenderer::createTextures() {
    glGenTextures(static_cast<GLsizei>(textures_.size()), textures_.data());
    for (GLuint texture : textures_) {
        if (texture == 0) {
            SPDLOG_ERROR("Failed to create OpenGL texture");
            return false;
        }
        setupTexture(texture);
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glBindTexture(GL_TEXTURE_2D, textures_[0]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width_, height_, 0,
                 GL_RED, GL_UNSIGNED_BYTE, nullptr);

    glBindTexture(GL_TEXTURE_2D, textures_[1]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, width_ / 2, height_ / 2, 0,
                 GL_RG, GL_UNSIGNED_BYTE, nullptr);

#ifdef RTSP_ENABLE_CUDA_INTEROP
    gl_.genBuffers(static_cast<GLsizei>(pixelUnpackBuffers_.size()), pixelUnpackBuffers_.data());
    for (GLuint buffer : pixelUnpackBuffers_) {
        if (buffer == 0) {
            SPDLOG_ERROR("Failed to create OpenGL pixel unpack buffer");
            return false;
        }
    }

    gl_.bindBuffer(GL_PIXEL_UNPACK_BUFFER, pixelUnpackBuffers_[0]);
    gl_.bufferData(GL_PIXEL_UNPACK_BUFFER,
                   static_cast<std::ptrdiff_t>(width_ * height_),
                   nullptr,
                   GL_STREAM_DRAW);

    gl_.bindBuffer(GL_PIXEL_UNPACK_BUFFER, pixelUnpackBuffers_[1]);
    gl_.bufferData(GL_PIXEL_UNPACK_BUFFER,
                   static_cast<std::ptrdiff_t>(width_ * height_ / 2),
                   nullptr,
                   GL_STREAM_DRAW);

    gl_.bindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
#endif

    return true;
}

void OpenGlVideoRenderer::setupTexture(GLuint texture) const {
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

bool OpenGlVideoRenderer::renderNv12(const uint8_t* y, const uint8_t* uv,
                                     int width, int height) {
    if (!initialized_ || !y || !uv) {
        return false;
    }

    if (width != width_ || height != height_) {
        SPDLOG_ERROR("Frame size {}x{} does not match texture size {}x{}",
                     width, height, width_, height_);
        return false;
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    gl_.activeTexture(GL_TEXTURE0);
    gl_.bindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, textures_[0]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                    GL_RED, GL_UNSIGNED_BYTE, y);

    gl_.activeTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, textures_[1]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width / 2, height / 2,
                    GL_RG, GL_UNSIGNED_BYTE, uv);

    return finishFrameRender();
}

bool OpenGlVideoRenderer::renderCudaNv12(const MediaFrame& frame) {
    if (!initialized_) {
        return false;
    }

    if (frame.width != width_ || frame.height != height_) {
        SPDLOG_ERROR("CUDA frame size {}x{} does not match texture size {}x{}",
                     frame.width, frame.height, width_, height_);
        return false;
    }

    if (frame.gpuData[0] == 0 || frame.gpuData[1] == 0 ||
        frame.gpuLinesize[0] <= 0 || frame.gpuLinesize[1] <= 0) {
        SPDLOG_ERROR("CUDA NV12 frame is missing GPU plane data");
        return false;
    }

#ifdef RTSP_ENABLE_CUDA_INTEROP
    if (uploadCudaFrameToTextures(frame)) {
        return finishFrameRender();
    }

    SPDLOG_WARN("CUDA/OpenGL interop upload failed; falling back to CUDA-to-CPU NV12 copy");
    std::vector<uint8_t> cpuData;
    if (!downloadCudaFrameToNv12(frame, cpuData)) {
        return false;
    }

    const size_t ySize = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height);
    return renderNv12(cpuData.data(), cpuData.data() + ySize, frame.width, frame.height);
#else
    SPDLOG_ERROR("CUDA/OpenGL interop is not compiled in");
    return false;
#endif
}

bool OpenGlVideoRenderer::finishFrameRender() {
    gl_.activeTexture(GL_TEXTURE0);
    gl_.useProgram(program_);
    glClear(GL_COLOR_BUFFER_BIT);
    applyVideoViewport();

    gl_.activeTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textures_[0]);
    gl_.activeTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, textures_[1]);
    gl_.bindVertexArray(videoVao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gl_.bindVertexArray(0);

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
    if (!screenshotRequested_ && !recorder_.isRecording()) {
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

    if (recorder_.isRecording() && !recorder_.recordFrame(frame)) {
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

    const std::string path = makeCapturePath("recording", ".avi");
    if (!recorder_.start(path, drawableWidth, drawableHeight, recordingFps_)) {
        SPDLOG_WARN("Failed to start recording: {}", path);
    }
}

#ifdef RTSP_ENABLE_CUDA_INTEROP
bool OpenGlVideoRenderer::registerCudaInterop() {
    if (cudaInteropRegistered_) {
        return true;
    }

    if (pixelUnpackBuffers_[0] == 0 || pixelUnpackBuffers_[1] == 0) {
        return false;
    }

    cudaError_t error =
        cudaGraphicsGLRegisterBuffer(&cudaResources_[0],
                                     pixelUnpackBuffers_[0],
                                     cudaGraphicsRegisterFlagsWriteDiscard);
    if (error != cudaSuccess) {
        SPDLOG_WARN("Failed to register OpenGL Y pixel unpack buffer with CUDA: {}",
                    cudaErrorName(error));
        cudaResources_[0] = nullptr;
        return false;
    }

    error = cudaGraphicsGLRegisterBuffer(&cudaResources_[1],
                                         pixelUnpackBuffers_[1],
                                         cudaGraphicsRegisterFlagsWriteDiscard);
    if (error != cudaSuccess) {
        SPDLOG_WARN("Failed to register OpenGL UV pixel unpack buffer with CUDA: {}",
                    cudaErrorName(error));
        cudaGraphicsUnregisterResource(cudaResources_[0]);
        cudaResources_[0] = nullptr;
        cudaResources_[1] = nullptr;
        return false;
    }

    cudaInteropRegistered_ = true;
    SPDLOG_INFO("CUDA/OpenGL interop enabled through NV12 pixel unpack buffers");
    return true;
}

void OpenGlVideoRenderer::unregisterCudaInterop() {
    for (auto& resource : cudaResources_) {
        if (resource != nullptr) {
            const cudaError_t error = cudaGraphicsUnregisterResource(resource);
            if (error != cudaSuccess) {
                SPDLOG_WARN("Failed to unregister CUDA/OpenGL resource: {}", cudaErrorName(error));
            }
            resource = nullptr;
        }
    }

    cudaInteropRegistered_ = false;
}

bool OpenGlVideoRenderer::uploadCudaFrameToTextures(const MediaFrame& frame) {
    if (!registerCudaInterop()) {
        return false;
    }

    cudaGraphicsResource_t resources[] = {cudaResources_[0], cudaResources_[1]};
    cudaError_t error = cudaGraphicsMapResources(2, resources, 0);
    if (error != cudaSuccess) {
        SPDLOG_WARN("Failed to map CUDA/OpenGL resources: {}", cudaErrorName(error));
        return false;
    }

    bool ok = true;
    void* yBuffer = nullptr;
    void* uvBuffer = nullptr;
    size_t yBufferSize = 0;
    size_t uvBufferSize = 0;

    error = cudaGraphicsResourceGetMappedPointer(&yBuffer, &yBufferSize, cudaResources_[0]);
    if (error != cudaSuccess) {
        SPDLOG_WARN("Failed to access mapped CUDA Y pixel unpack buffer: {}", cudaErrorName(error));
        ok = false;
    }

    if (ok) {
        error = cudaGraphicsResourceGetMappedPointer(&uvBuffer, &uvBufferSize, cudaResources_[1]);
        if (error != cudaSuccess) {
            SPDLOG_WARN("Failed to access mapped CUDA UV pixel unpack buffer: {}", cudaErrorName(error));
            ok = false;
        }
    }

    if (ok) {
        const size_t requiredYSize = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height);
        const size_t requiredUvSize = requiredYSize / 2;
        if (yBufferSize < requiredYSize || uvBufferSize < requiredUvSize) {
            SPDLOG_WARN("CUDA/OpenGL PBO is too small: y={} uv={}, required y={} uv={}",
                        yBufferSize, uvBufferSize, requiredYSize, requiredUvSize);
            ok = false;
        }
    }

    if (ok) {
        error = cudaMemcpy2D(yBuffer,
                             static_cast<size_t>(frame.width),
                             reinterpret_cast<const void*>(frame.gpuData[0]),
                             static_cast<size_t>(frame.gpuLinesize[0]),
                             static_cast<size_t>(frame.width),
                             static_cast<size_t>(frame.height),
                             cudaMemcpyDeviceToDevice);
        if (error != cudaSuccess) {
            SPDLOG_WARN("Failed to copy CUDA Y plane to OpenGL pixel unpack buffer: {}",
                        cudaErrorName(error));
            ok = false;
        }
    }

    if (ok) {
        error = cudaMemcpy2D(uvBuffer,
                             static_cast<size_t>(frame.width),
                             reinterpret_cast<const void*>(frame.gpuData[1]),
                             static_cast<size_t>(frame.gpuLinesize[1]),
                             static_cast<size_t>(frame.width),
                             static_cast<size_t>(frame.height / 2),
                             cudaMemcpyDeviceToDevice);
        if (error != cudaSuccess) {
            SPDLOG_WARN("Failed to copy CUDA UV plane to OpenGL pixel unpack buffer: {}",
                        cudaErrorName(error));
            ok = false;
        }
    }

    error = cudaGraphicsUnmapResources(2, resources, 0);
    if (error != cudaSuccess) {
        SPDLOG_WARN("Failed to unmap CUDA/OpenGL resources: {}", cudaErrorName(error));
        ok = false;
    }

    if (!ok) {
        return false;
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    gl_.activeTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textures_[0]);
    gl_.bindBuffer(GL_PIXEL_UNPACK_BUFFER, pixelUnpackBuffers_[0]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height,
                    GL_RED, GL_UNSIGNED_BYTE, nullptr);

    gl_.activeTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, textures_[1]);
    gl_.bindBuffer(GL_PIXEL_UNPACK_BUFFER, pixelUnpackBuffers_[1]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width / 2, frame.height / 2,
                    GL_RG, GL_UNSIGNED_BYTE, nullptr);

    gl_.bindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    return ok;
}

bool OpenGlVideoRenderer::downloadCudaFrameToNv12(const MediaFrame& frame,
                                                 std::vector<uint8_t>& data) const {
    const size_t ySize = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height);
    data.resize(ySize * 3 / 2);

    cudaError_t error = cudaMemcpy2D(data.data(),
                                     static_cast<size_t>(frame.width),
                                     reinterpret_cast<const void*>(frame.gpuData[0]),
                                     static_cast<size_t>(frame.gpuLinesize[0]),
                                     static_cast<size_t>(frame.width),
                                     static_cast<size_t>(frame.height),
                                     cudaMemcpyDeviceToHost);
    if (error != cudaSuccess) {
        SPDLOG_WARN("Failed to copy CUDA Y plane to CPU fallback buffer: {}", cudaErrorName(error));
        return false;
    }

    error = cudaMemcpy2D(data.data() + ySize,
                         static_cast<size_t>(frame.width),
                         reinterpret_cast<const void*>(frame.gpuData[1]),
                         static_cast<size_t>(frame.gpuLinesize[1]),
                         static_cast<size_t>(frame.width),
                         static_cast<size_t>(frame.height / 2),
                         cudaMemcpyDeviceToHost);
    if (error != cudaSuccess) {
        SPDLOG_WARN("Failed to copy CUDA UV plane to CPU fallback buffer: {}", cudaErrorName(error));
        return false;
    }

    return true;
}
#endif

std::array<std::string, 9> OpenGlVideoRenderer::makeOverlayLines() const {
    std::ostringstream fps;
    fps << "FPS: " << std::fixed << std::setprecision(1) << playbackStats_.fps;

    return {
        fps.str(),
        "DECODER: " + playbackStats_.decoderBackend,
        "HW: " + playbackStats_.hardwareDecodeStatus,
        "DECODED: " + std::to_string(playbackStats_.decodedFrames),
        "DROPPED: " + std::to_string(playbackStats_.droppedFrames),
        "BUFFER: " + std::to_string(playbackStats_.jitterBufferSize),
        "LATENCY: " + std::to_string(playbackStats_.latencyMs) + "MS",
        "FILTER: " + filterPipeline_.describe(),
        recorder_.isRecording() ? "REC: ON" : "REC: OFF"
    };
}

void OpenGlVideoRenderer::drawOverlay() {
    if (!window_) {
        return;
    }

    int drawableWidth = 0;
    int drawableHeight = 0;
    SDL_GL_GetDrawableSize(window_, &drawableWidth, &drawableHeight);
    if (drawableWidth <= 0 || drawableHeight <= 0) {
        return;
    }

    const auto lines = makeOverlayLines();
    const float scale = 2.0F;
    const float lineHeight = 8.0F * scale;
    const float left = 10.0F;
    const float top = 10.0F;
    size_t maxLineLength = 0;
    for (const std::string& line : lines) {
        maxLineLength = std::max(maxLineLength, line.size());
    }
    const float width = static_cast<float>(maxLineLength) * 6.0F * scale + 12.0F;
    const float height = 12.0F + lineHeight * static_cast<float>(lines.size());

    glViewport(0, 0, drawableWidth, drawableHeight);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    gl_.useProgram(overlayProgram_);
    if (overlayScreenSizeLocation_ >= 0) {
        gl_.uniform2f(overlayScreenSizeLocation_,
                      static_cast<GLfloat>(drawableWidth),
                      static_cast<GLfloat>(drawableHeight));
    }

    overlayVertices_.clear();
    drawRect(left - 6.0F, top - 6.0F, width, height);
    flushOverlay(0.0F, 0.0F, 0.0F, 0.62F);

    overlayVertices_.clear();
    float y = top;
    for (const std::string& line : lines) {
        drawText(left, y, line, scale);
        y += lineHeight;
    }
    flushOverlay(0.72F, 1.0F, 0.86F, 1.0F);

    glDisable(GL_BLEND);
}

void OpenGlVideoRenderer::drawText(float x, float y, const std::string& text, float scale) {
    float cursorX = x;
    for (char rawCh : text) {
        const char ch = static_cast<char>(std::toupper(static_cast<unsigned char>(rawCh)));
        if (ch == ' ') {
            cursorX += 4.0F * scale;
            continue;
        }

        const Glyph glyph = glyphFor(ch);
        for (size_t row = 0; row < glyph.size(); ++row) {
            for (int col = 0; col < 5; ++col) {
                const uint8_t mask = static_cast<uint8_t>(1U << (4 - col));
                if ((glyph[row] & mask) == 0) {
                    continue;
                }

                drawRect(cursorX + static_cast<float>(col) * scale,
                         y + static_cast<float>(row) * scale,
                         scale,
                         scale);
            }
        }

        cursorX += 6.0F * scale;
    }
}

void OpenGlVideoRenderer::drawRect(float x, float y, float width, float height) {
    const float right = x + width;
    const float bottom = y + height;
    const std::array<float, 12> vertices = {
        x, y,
        right, y,
        right, bottom,
        x, y,
        right, bottom,
        x, bottom
    };
    overlayVertices_.insert(overlayVertices_.end(), vertices.begin(), vertices.end());
}

void OpenGlVideoRenderer::flushOverlay(float red, float green, float blue, float alpha) {
    if (overlayVertices_.empty()) {
        return;
    }

    if (overlayColorLocation_ >= 0) {
        gl_.uniform4f(overlayColorLocation_, red, green, blue, alpha);
    }

    gl_.bindVertexArray(overlayVao_);
    gl_.bindBuffer(GL_ARRAY_BUFFER, overlayVbo_);
    gl_.bufferData(GL_ARRAY_BUFFER,
                   static_cast<std::ptrdiff_t>(overlayVertices_.size() * sizeof(float)),
                   overlayVertices_.data(),
                   GL_DYNAMIC_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(overlayVertices_.size() / 2));
    gl_.bindBuffer(GL_ARRAY_BUFFER, 0);
    gl_.bindVertexArray(0);
}

} // namespace

std::unique_ptr<VideoRenderer> createOpenGlVideoRenderer() {
    return std::make_unique<OpenGlVideoRenderer>();
}

std::unique_ptr<VideoRenderer> createOpenGlVideoRenderer(const std::string& filterName) {
    return std::make_unique<OpenGlVideoRenderer>(filterName);
}

std::unique_ptr<VideoRenderer> createOpenGlVideoRenderer(const std::vector<std::string>& filterNames) {
    return std::make_unique<OpenGlVideoRenderer>(filterNames);
}

} // namespace rtsp
