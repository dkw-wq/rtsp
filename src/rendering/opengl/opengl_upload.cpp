#include "opengl_video_renderer.hpp"
#include <vector>
#include <spdlog/spdlog.h>
namespace rtsp::rendering::opengl {
namespace {
#ifdef RTSP_ENABLE_CUDA_INTEROP
const char* cudaErrorName(cudaError_t error) {
    return cudaGetErrorString(error);
}
#endif
} // namespace
bool OpenGlVideoRenderer::renderNv12(const uint8_t* y, const uint8_t* uv,
                                     int width, int height) {
    if (!initialized_ || !y || !uv) {
        return false;
    }

    if (!uploadNv12Textures(y, uv, width, height)) {
        return false;
    }

    activeVideoSlots_ = 1;
    return finishFrameRender();
}

bool OpenGlVideoRenderer::uploadNv12Textures(const uint8_t* y,
                                             const uint8_t* uv,
                                             int width,
                                             int height) {
    if (!initialized_ || !y || !uv || width <= 0 || height <= 0) {
        return false;
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    gl_.activeTexture(GL_TEXTURE0);
    gl_.bindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, textures_[0]);

    gl_.activeTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, textures_[1]);

    if (textureWidth_ != width || textureHeight_ != height) {
        gl_.activeTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, textures_[0]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0,
                     GL_RED, GL_UNSIGNED_BYTE, nullptr);

        gl_.activeTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, textures_[1]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, width / 2, height / 2, 0,
                     GL_RG, GL_UNSIGNED_BYTE, nullptr);
        textureWidth_ = width;
        textureHeight_ = height;
    }

    gl_.activeTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textures_[0]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                    GL_RED, GL_UNSIGNED_BYTE, y);

    gl_.activeTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, textures_[1]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width / 2, height / 2,
                    GL_RG, GL_UNSIGNED_BYTE, uv);
    return true;
}

void OpenGlVideoRenderer::drawCurrentTextures() {
    gl_.activeTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textures_[0]);
    gl_.activeTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, textures_[1]);
    gl_.bindVertexArray(videoVao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gl_.bindVertexArray(0);
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

} // namespace rtsp::rendering::opengl

