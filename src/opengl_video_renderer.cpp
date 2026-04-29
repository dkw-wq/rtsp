#include "video_renderer.hpp"

#include <array>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>

#include <spdlog/spdlog.h>

extern "C" {
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
}

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
using GlActiveTexture = void(APIENTRY*)(GLenum);

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
    GlActiveTexture activeTexture = nullptr;

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
               loadGlFunction(activeTexture, "glActiveTexture");
    }
};

constexpr const char* kVertexShader = R"(
#version 120
varying vec2 v_texCoord;

void main() {
    gl_Position = gl_Vertex;
    v_texCoord = gl_MultiTexCoord0.st;
}
)";

constexpr const char* kFragmentShader = R"(
#version 120
uniform sampler2D tex_y;
uniform sampler2D tex_u;
uniform sampler2D tex_v;
uniform int filter_mode;
varying vec2 v_texCoord;

void main() {
    float y = texture2D(tex_y, v_texCoord).r;
    float u = texture2D(tex_u, v_texCoord).r - 0.5;
    float v = texture2D(tex_v, v_texCoord).r - 0.5;

    vec3 rgb;
    rgb.r = y + 1.402 * v;
    rgb.g = y - 0.344136 * u - 0.714136 * v;
    rgb.b = y + 1.772 * u;

    if (filter_mode == 1) {
        float gray = dot(rgb, vec3(0.299, 0.587, 0.114));
        rgb = vec3(gray);
    } else if (filter_mode == 2) {
        rgb = vec3(rgb.r * 1.08 + 0.03, rgb.g * 1.02, rgb.b * 0.90);
    } else if (filter_mode == 3) {
        rgb = vec3(1.0) - rgb;
    }

    gl_FragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)";

int parseFilterMode(std::string filterName) {
    std::transform(filterName.begin(), filterName.end(), filterName.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    if (filterName == "none" || filterName == "off" || filterName == "normal") {
        return 0;
    }
    if (filterName == "grayscale" || filterName == "gray" || filterName == "mono") {
        return 1;
    }
    if (filterName == "warm") {
        return 2;
    }
    if (filterName == "invert" || filterName == "negative") {
        return 3;
    }

    SPDLOG_WARN("Unknown OpenGL filter '{}', using none", filterName);
    return 0;
}

const char* filterName(int filterMode) {
    switch (filterMode) {
        case 1:
            return "grayscale";
        case 2:
            return "warm";
        case 3:
            return "invert";
        default:
            return "none";
    }
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
        case 'I': return {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E};
        case 'L': return {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
        case 'M': return {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
        case 'N': return {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
        case 'O': return {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
        case 'P': return {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
        case 'R': return {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
        case 'S': return {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
        case 'T': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
        case 'U': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
        case 'Y': return {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
        case ':': return {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00};
        case '.': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
        default: return {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    }
}

class OpenGlVideoRenderer final : public VideoRenderer {
public:
    explicit OpenGlVideoRenderer(const std::string& filterName = "none");
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
    bool createProgram();
    GLuint compileShader(GLenum type, const char* source);
    bool createTextures();
    void setupTexture(GLuint texture) const;
    bool renderYuv(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                   int width, int height);
    void drawOverlay();
    void drawText(float x, float y, const std::string& text, float scale) const;
    void drawRect(float x, float y, float width, float height) const;
    std::array<std::string, 5> makeOverlayLines() const;

    SDL_Window* window_;
    SDL_GLContext glContext_;
    GlApi gl_;

    GLuint program_;
    std::array<GLuint, 3> textures_;
    GLint yLocation_;
    GLint uLocation_;
    GLint vLocation_;
    GLint filterModeLocation_;
    int filterMode_;
    PlaybackStats playbackStats_;

    int width_;
    int height_;
    bool initialized_;
    bool apiLoaded_;
};

OpenGlVideoRenderer::OpenGlVideoRenderer(const std::string& filterName)
    : window_(nullptr)
    , glContext_(nullptr)
    , program_(0)
    , textures_{0, 0, 0}
    , yLocation_(-1)
    , uLocation_(-1)
    , vLocation_(-1)
    , filterModeLocation_(-1)
    , filterMode_(parseFilterMode(filterName))
    , playbackStats_()
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

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
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
    if (!apiLoaded_ || !createProgram() || !createTextures()) {
        close();
        return false;
    }

    glViewport(0, 0, width, height);
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);

    initialized_ = true;
    SPDLOG_INFO("OpenGL renderer initialized: {}x{}", width, height);
    return true;
}

bool OpenGlVideoRenderer::render(const std::shared_ptr<MediaFrame>& frame) {
    if (!initialized_ || !frame) {
        return false;
    }

    const size_t ySize = static_cast<size_t>(frame->width) * static_cast<size_t>(frame->height);
    const size_t requiredSize = ySize * 3 / 2;
    if (frame->data.size() < requiredSize) {
        SPDLOG_ERROR("Frame data too small for YUV420P: {} < {}", frame->data.size(), requiredSize);
        return false;
    }

    return renderYuv(
        frame->data.data(),
        frame->data.data() + ySize,
        frame->data.data() + ySize + (ySize / 4),
        frame->width,
        frame->height);
}

void OpenGlVideoRenderer::setPlaybackStats(const PlaybackStats& stats) {
    playbackStats_ = stats;
}

bool OpenGlVideoRenderer::handleEvents() {
    SDL_Event event;

    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                return false;
            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_ESCAPE ||
                    event.key.keysym.sym == SDLK_q) {
                    return false;
                }
                if (event.key.keysym.sym == SDLK_f) {
                    filterMode_ = (filterMode_ + 1) % 4;
                    if (apiLoaded_ && filterModeLocation_ >= 0) {
                        gl_.useProgram(program_);
                        gl_.uniform1i(filterModeLocation_, filterMode_);
                    }
                    SPDLOG_INFO("OpenGL filter: {}", filterName(filterMode_));
                }
                break;
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    glViewport(0, 0, event.window.data1, event.window.data2);
                }
                break;
            default:
                break;
        }
    }

    return true;
}

void OpenGlVideoRenderer::close() {
    if (glContext_) {
        SDL_GL_MakeCurrent(window_, glContext_);

        if (textures_[0] != 0) {
            glDeleteTextures(static_cast<GLsizei>(textures_.size()), textures_.data());
            textures_ = {0, 0, 0};
        }

        if (apiLoaded_ && program_ != 0) {
            gl_.deleteProgram(program_);
            program_ = 0;
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
    uLocation_ = -1;
    vLocation_ = -1;
    filterModeLocation_ = -1;
    width_ = 0;
    height_ = 0;
    initialized_ = false;
    apiLoaded_ = false;
}

int OpenGlVideoRenderer::getWidth() const { return width_; }
int OpenGlVideoRenderer::getHeight() const { return height_; }
bool OpenGlVideoRenderer::isInitialized() const { return initialized_; }

bool OpenGlVideoRenderer::createProgram() {
    const GLuint vertexShader = compileShader(GL_VERTEX_SHADER, kVertexShader);
    if (vertexShader == 0) {
        return false;
    }

    const GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (fragmentShader == 0) {
        gl_.deleteShader(vertexShader);
        return false;
    }

    program_ = gl_.createProgram();
    gl_.attachShader(program_, vertexShader);
    gl_.attachShader(program_, fragmentShader);
    gl_.linkProgram(program_);

    GLint linked = GL_FALSE;
    gl_.getProgramiv(program_, GL_LINK_STATUS, &linked);

    gl_.deleteShader(vertexShader);
    gl_.deleteShader(fragmentShader);

    if (linked != GL_TRUE) {
        GLint logLength = 0;
        gl_.getProgramiv(program_, GL_INFO_LOG_LENGTH, &logLength);
        std::string log(static_cast<size_t>(logLength), '\0');
        gl_.getProgramInfoLog(program_, logLength, nullptr, log.data());
        SPDLOG_ERROR("OpenGL shader program link failed: {}", log);
        gl_.deleteProgram(program_);
        program_ = 0;
        return false;
    }

    gl_.useProgram(program_);
    yLocation_ = gl_.getUniformLocation(program_, "tex_y");
    uLocation_ = gl_.getUniformLocation(program_, "tex_u");
    vLocation_ = gl_.getUniformLocation(program_, "tex_v");
    filterModeLocation_ = gl_.getUniformLocation(program_, "filter_mode");
    gl_.uniform1i(yLocation_, 0);
    gl_.uniform1i(uLocation_, 1);
    gl_.uniform1i(vLocation_, 2);
    gl_.uniform1i(filterModeLocation_, filterMode_);
    SPDLOG_INFO("OpenGL filter: {}", filterName(filterMode_));

    return true;
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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width_, height_, 0,
                 GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);

    glBindTexture(GL_TEXTURE_2D, textures_[1]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width_ / 2, height_ / 2, 0,
                 GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);

    glBindTexture(GL_TEXTURE_2D, textures_[2]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width_ / 2, height_ / 2, 0,
                 GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);

    return true;
}

void OpenGlVideoRenderer::setupTexture(GLuint texture) const {
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

bool OpenGlVideoRenderer::renderYuv(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                                    int width, int height) {
    if (!initialized_ || !y || !u || !v) {
        return false;
    }

    if (width != width_ || height != height_) {
        SPDLOG_ERROR("Frame size {}x{} does not match texture size {}x{}",
                     width, height, width_, height_);
        return false;
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    gl_.activeTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textures_[0]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                    GL_LUMINANCE, GL_UNSIGNED_BYTE, y);

    gl_.activeTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, textures_[1]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width / 2, height / 2,
                    GL_LUMINANCE, GL_UNSIGNED_BYTE, u);

    gl_.activeTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, textures_[2]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width / 2, height / 2,
                    GL_LUMINANCE, GL_UNSIGNED_BYTE, v);

    gl_.activeTexture(GL_TEXTURE0);
    gl_.useProgram(program_);
    glClear(GL_COLOR_BUFFER_BIT);

    glBegin(GL_TRIANGLE_STRIP);
    glTexCoord2f(0.0F, 1.0F);
    glVertex2f(-1.0F, -1.0F);
    glTexCoord2f(1.0F, 1.0F);
    glVertex2f(1.0F, -1.0F);
    glTexCoord2f(0.0F, 0.0F);
    glVertex2f(-1.0F, 1.0F);
    glTexCoord2f(1.0F, 0.0F);
    glVertex2f(1.0F, 1.0F);
    glEnd();

    drawOverlay();

    SDL_GL_SwapWindow(window_);
    return true;
}

std::array<std::string, 5> OpenGlVideoRenderer::makeOverlayLines() const {
    std::ostringstream fps;
    fps << "FPS: " << std::fixed << std::setprecision(1) << playbackStats_.fps;

    return {
        fps.str(),
        "DECODED: " + std::to_string(playbackStats_.decodedFrames),
        "DROPPED: " + std::to_string(playbackStats_.droppedFrames),
        "BUFFER: " + std::to_string(playbackStats_.jitterBufferSize),
        "LATENCY: " + std::to_string(playbackStats_.latencyMs) + "MS"
    };
}

void OpenGlVideoRenderer::drawOverlay() {
    if (!window_) {
        return;
    }

    int windowWidth = 0;
    int windowHeight = 0;
    SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
    if (windowWidth <= 0 || windowHeight <= 0) {
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

    gl_.useProgram(0);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, static_cast<double>(windowWidth), static_cast<double>(windowHeight), 0.0, -1.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_TEXTURE_2D);

    glColor4f(0.0F, 0.0F, 0.0F, 0.62F);
    drawRect(left - 6.0F, top - 6.0F, width, height);

    glColor4f(0.72F, 1.0F, 0.86F, 1.0F);
    float y = top;
    for (const std::string& line : lines) {
        drawText(left, y, line, scale);
        y += lineHeight;
    }

    glEnable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
}

void OpenGlVideoRenderer::drawText(float x, float y, const std::string& text, float scale) const {
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

void OpenGlVideoRenderer::drawRect(float x, float y, float width, float height) const {
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + width, y);
    glVertex2f(x + width, y + height);
    glVertex2f(x, y + height);
    glEnd();
}

} // namespace

std::unique_ptr<VideoRenderer> createOpenGlVideoRenderer() {
    return std::make_unique<OpenGlVideoRenderer>();
}

std::unique_ptr<VideoRenderer> createOpenGlVideoRenderer(const std::string& filterName) {
    return std::make_unique<OpenGlVideoRenderer>(filterName);
}

} // namespace rtsp
