#include "vulkan_video_renderer.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

extern "C" {
#include <libavutil/error.h>
}

namespace rtsp::rendering::vulkan {

const std::vector<const char*> kDeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
#ifdef RTSP_ENABLE_CUDA_INTEROP
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
#endif
};

void checkVk(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) +
                                 " failed with VkResult " +
                                 std::to_string(static_cast<int>(result)));
    }
}

std::string ffmpegError(int errorCode) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errorCode, buffer, sizeof(buffer));
    return buffer;
}

void copyPlane(uint8_t* dst, int dstStride, const uint8_t* src,
               int srcStride, int width, int height) {
    for (int row = 0; row < height; ++row) {
        std::memcpy(dst + row * dstStride, src + row * srcStride, width);
    }
}

const char* filterName(ShaderFilter filterMode) {
    switch (filterMode) {
        case ShaderFilter::Grayscale: return "grayscale";
        case ShaderFilter::Warm: return "warm";
        case ShaderFilter::Invert: return "invert";
        case ShaderFilter::Contrast: return "contrast";
        case ShaderFilter::Saturation: return "saturation";
        case ShaderFilter::None:
        default:
            return "none";
    }
}

#ifdef RTSP_ENABLE_CUDA_INTEROP
const char* cudaErrorName(cudaError_t error) {
    return cudaGetErrorString(error);
}

void checkCuda(cudaError_t error, const char* operation) {
    if (error != cudaSuccess) {
        throw std::runtime_error(std::string(operation) +
                                 " failed: " + cudaErrorName(error));
    }
}
#endif

namespace {

std::vector<uint32_t> readSpirvFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open shader: " + path.string());
    }

    const std::streamsize fileSize = file.tellg();
    if (fileSize <= 0 || fileSize % 4 != 0) {
        throw std::runtime_error("Invalid SPIR-V shader size: " + path.string());
    }

    std::vector<uint32_t> buffer(static_cast<size_t>(fileSize) / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), fileSize);
    if (!file) {
        throw std::runtime_error("Failed to read shader: " + path.string());
    }
    return buffer;
}

std::vector<std::filesystem::path> shaderSearchPaths(const char* fileName) {
    std::vector<std::filesystem::path> paths;
    paths.emplace_back(std::filesystem::current_path() / "shaders" / "vulkan" / fileName);
    paths.emplace_back(std::filesystem::current_path() / ".." / "shaders" / "vulkan" / fileName);

    char* basePath = SDL_GetBasePath();
    if (basePath) {
        paths.emplace_back(std::filesystem::path(basePath) / "shaders" / "vulkan" / fileName);
        SDL_free(basePath);
    }

    return paths;
}

} // namespace

std::vector<uint32_t> loadShader(const char* fileName) {
    std::string errors;
    for (const auto& path : shaderSearchPaths(fileName)) {
        try {
            return readSpirvFile(path);
        } catch (const std::exception& e) {
            if (!errors.empty()) {
                errors += "; ";
            }
            errors += e.what();
        }
    }

    throw std::runtime_error(errors.empty() ? "Shader not found" : errors);
}

} // namespace rtsp::rendering::vulkan

