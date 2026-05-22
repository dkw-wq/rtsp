#include "opengl_video_renderer.hpp"
#include <cstring>
#include <spdlog/spdlog.h>
namespace rtsp::rendering::opengl {
namespace {
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
} // namespace
bool GlApi::load() {
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
} // namespace rtsp::rendering::opengl

