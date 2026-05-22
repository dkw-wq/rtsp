#include "opengl_video_renderer.hpp"
#include <algorithm>
#include <spdlog/spdlog.h>
namespace rtsp::rendering::opengl {
namespace {
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

} // namespace
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

void OpenGlVideoRenderer::applyVideoViewportFor(int videoWidth,
                                                int videoHeight,
                                                uint32_t slotIndex,
                                                uint32_t slotCount) const {
    if (!window_) {
        return;
    }

    int drawableWidth = 0;
    int drawableHeight = 0;
    SDL_GL_GetDrawableSize(window_, &drawableWidth, &drawableHeight);
    const ViewportRect viewport =
        calculateAspectFitViewport(drawableWidth,
                                   drawableHeight,
                                   videoWidth,
                                   videoHeight,
                                   slotIndex,
                                   slotCount);

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
    textureWidth_ = width_;
    textureHeight_ = height_;

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

} // namespace rtsp::rendering::opengl

