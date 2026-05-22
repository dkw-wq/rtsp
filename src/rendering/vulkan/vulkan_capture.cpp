#include "vulkan_video_renderer.hpp"
#include "../common/renderer_input.hpp"

#include <algorithm>
#include <cstring>

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

namespace rtsp::rendering::vulkan {

bool VulkanVideoRenderer::captureNeeded() const {
    return swapchainTransferSrcSupported_ &&
           screenshotRequested_ &&
           swapchainExtent_.width > 0 &&
           swapchainExtent_.height > 0;
}

bool VulkanVideoRenderer::readCapturedFrame(RgbFrame& frame) const {
    if (!capturePending_ || readbackBuffer_.memory == VK_NULL_HANDLE ||
        swapchainExtent_.width == 0 || swapchainExtent_.height == 0) {
        return false;
    }

    const uint32_t width = swapchainExtent_.width;
    const uint32_t height = swapchainExtent_.height;
    const VkDeviceSize byteSize =
        static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4U;

    void* mappedMemory = nullptr;
    checkVk(vkMapMemory(device_, readbackBuffer_.memory, 0, byteSize, 0, &mappedMemory),
            "vkMapMemory readback");

    frame.width = static_cast<int>(width);
    frame.height = static_cast<int>(height);
    frame.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 3U);

    const auto* source = static_cast<const uint8_t*>(mappedMemory);
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* sourceRow = source + static_cast<size_t>(row) * static_cast<size_t>(width) * 4U;
        uint8_t* destRow = frame.pixels.data() +
                           static_cast<size_t>(row) * static_cast<size_t>(width) * 3U;
        for (uint32_t col = 0; col < width; ++col) {
            const uint8_t* pixel = sourceRow + static_cast<size_t>(col) * 4U;
            uint8_t* dest = destRow + static_cast<size_t>(col) * 3U;
            if (swapchainImageFormat_ == VK_FORMAT_R8G8B8A8_UNORM ||
                swapchainImageFormat_ == VK_FORMAT_R8G8B8A8_SRGB) {
                dest[0] = pixel[0];
                dest[1] = pixel[1];
                dest[2] = pixel[2];
            } else {
                dest[0] = pixel[2];
                dest[1] = pixel[1];
                dest[2] = pixel[0];
            }
        }
    }

    vkUnmapMemory(device_, readbackBuffer_.memory);
    return true;
}

void VulkanVideoRenderer::handleCaptureAfterRender() {
    RgbFrame frame;
    if (!readCapturedFrame(frame)) {
        SPDLOG_WARN("Failed to capture Vulkan frame");
        screenshotRequested_ = false;
        return;
    }

    if (screenshotRequested_) {
        saveScreenshot(frame);
        screenshotRequested_ = false;
    }
}

void VulkanVideoRenderer::saveScreenshot(const RgbFrame& frame) {
    const std::string path = makeCapturePath("screenshot", ".bmp");
    if (saveRgbFrameAsBmp(frame, path)) {
        SPDLOG_INFO("Screenshot saved: {}", path);
    } else {
        SPDLOG_WARN("Failed to save screenshot: {}", path);
    }
}

bool VulkanVideoRenderer::handleKeyboardShortcuts() {
    SDL_PumpEvents();

    if (isKeyDown(SDL_SCANCODE_ESCAPE, VK_ESCAPE) ||
        isKeyDown(SDL_SCANCODE_Q, 'Q')) {
        return false;
    }

    if (keyJustPressed(isKeyDown(SDL_SCANCODE_F, 'F'), fKeyDown_)) {
        const int nextMode = (static_cast<int>(filterMode_) + 1) % 6;
        filterMode_ = static_cast<ShaderFilter>(nextMode);
        SPDLOG_INFO("Vulkan filter: {}", filterName(filterMode_));
    }

    if (keyJustPressed(isKeyDown(SDL_SCANCODE_S, 'S'), sKeyDown_)) {
        if (!swapchainTransferSrcSupported_) {
            SPDLOG_WARN("Cannot take screenshot because this Vulkan swapchain does not support image readback");
        } else {
            screenshotRequested_ = true;
            SPDLOG_INFO("Screenshot requested");
        }
    }

    if (keyJustPressed(isKeyDown(SDL_SCANCODE_R, 'R'), rKeyDown_) && commandCallback_) {
        commandCallback_(RendererCommand::ToggleRecording);
    }

    return true;
}

VkViewport VulkanVideoRenderer::videoViewport() const {
    return videoViewportFor(width_, height_, 0, 1);
}

VkViewport VulkanVideoRenderer::videoViewportFor(int videoWidth,
                                                 int videoHeight,
                                                 uint32_t slotIndex,
                                                 uint32_t slotCount) const {
    const float surfaceWidth = static_cast<float>(swapchainExtent_.width);
    const float surfaceHeight = static_cast<float>(swapchainExtent_.height);
    const uint32_t columns = std::max<uint32_t>(slotCount, 1U);
    const float panelWidth = surfaceWidth / static_cast<float>(columns);
    const float panelHeight = surfaceHeight;
    const float panelX = panelWidth * static_cast<float>(std::min(slotIndex, columns - 1U));

    const float videoAspect = static_cast<float>(videoWidth) / static_cast<float>(videoHeight);
    const float panelAspect = panelWidth / panelHeight;

    float viewportWidth = panelWidth;
    float viewportHeight = panelHeight;
    float viewportX = panelX;
    float viewportY = 0.0f;

    if (panelAspect > videoAspect) {
        viewportWidth = panelHeight * videoAspect;
        viewportX = panelX + (panelWidth - viewportWidth) * 0.5f;
    } else {
        viewportHeight = panelWidth / videoAspect;
        viewportY = (panelHeight - viewportHeight) * 0.5f;
    }

    VkViewport viewport{};
    viewport.x = viewportX;
    viewport.y = viewportY;
    viewport.width = viewportWidth;
    viewport.height = viewportHeight;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    return viewport;
}

} // namespace rtsp::rendering::vulkan

