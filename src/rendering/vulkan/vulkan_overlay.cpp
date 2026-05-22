#include "vulkan_video_renderer.hpp"
#include "../common/overlay_font.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace rtsp::rendering::vulkan {

std::array<std::string, 13> VulkanVideoRenderer::makeStatusLines() const {
    std::ostringstream fps;
    fps << "FPS: " << std::fixed << std::setprecision(1) << playbackStats_.fps;

    return {
        fps.str(),
        "DECODER: " + playbackStats_.decoderBackend,
        "HW: " + playbackStats_.hardwareDecodeStatus,
        "UPLOAD: " + uploadPath_,
        "DECODED: " + std::to_string(playbackStats_.decodedFrames),
        "DROPPED: " + std::to_string(playbackStats_.droppedFrames),
        "SYNC DROP: " + std::to_string(playbackStats_.syncDroppedFrames),
        "BUFFER: " + std::to_string(playbackStats_.jitterBufferSize),
        "LATENCY: " + std::to_string(playbackStats_.latencyMs) + "MS",
        "AUDIO: " + std::string(playbackStats_.audioActive ? "ON " : "OFF ") +
            std::to_string(playbackStats_.audioQueueMs) + "MS",
        "AV DIFF: " + std::to_string(playbackStats_.avSyncDiffMs) + "MS",
        "FILTER: " + std::string(filterName(filterMode_)),
        "RENDERER: VULKAN"
    };
}

void VulkanVideoRenderer::drawStatusLayout(VkCommandBuffer commandBuffer) {
    if (swapchainExtent_.width == 0 || swapchainExtent_.height == 0 ||
        overlayPipeline_ == VK_NULL_HANDLE) {
        return;
    }

    const auto lines = makeStatusLines();
    const float scale = 2.0F;
    const float lineHeight = 8.0F * scale;
    const float left = 10.0F;
    const float top = 10.0F;

    size_t maxLineLength = 0;
    for (const std::string& line : lines) {
        maxLineLength = std::max(maxLineLength, line.size());
    }

    const float layoutWidth = static_cast<float>(maxLineLength) * 6.0F * scale + 12.0F;
    const float layoutHeight = 12.0F + lineHeight * static_cast<float>(lines.size());

    VkViewport viewport{};
    viewport.x = 0.0F;
    viewport.y = 0.0F;
    viewport.width = static_cast<float>(swapchainExtent_.width);
    viewport.height = static_cast<float>(swapchainExtent_.height);
    viewport.minDepth = 0.0F;
    viewport.maxDepth = 1.0F;

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = swapchainExtent_;

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, overlayPipeline_);
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    overlayVertices_.clear();
    drawFaceOverlays(commandBuffer);

    overlayVertices_.clear();
    drawRect(left - 6.0F, top - 6.0F, layoutWidth, layoutHeight);
    flushOverlay(commandBuffer, overlayBackgroundBuffer_, 0.0F, 0.0F, 0.0F, 0.62F);

    overlayVertices_.clear();
    float y = top;
    for (const std::string& line : lines) {
        drawText(left, y, line, scale);
        y += lineHeight;
    }
    flushOverlay(commandBuffer, overlayBuffer_, 0.72F, 1.0F, 0.86F, 1.0F);
}

void VulkanVideoRenderer::drawFaceOverlays(VkCommandBuffer commandBuffer) {
    if (faceOverlays_.empty()) {
        return;
    }

    const float thickness =
        std::max(2.0F, static_cast<float>(swapchainExtent_.width) / 640.0F);

    for (const FaceDetectionResult& result : faceOverlays_) {
        if (result.streamIndex >= kMaxVideoSlots ||
            result.streamIndex >= activeVideoSlots_ ||
            result.frameWidth <= 0 ||
            result.frameHeight <= 0) {
            continue;
        }

        const VkViewport viewport =
            activeVideoSlots_ > 1
                ? videoViewportFor(result.frameWidth,
                                   result.frameHeight,
                                   static_cast<uint32_t>(result.streamIndex),
                                   activeVideoSlots_)
                : videoViewport();
        const float scaleX = viewport.width / static_cast<float>(result.frameWidth);
        const float scaleY = viewport.height / static_cast<float>(result.frameHeight);

        for (const FaceBox& face : result.faces) {
            drawRectOutline(viewport.x + face.x * scaleX,
                            viewport.y + face.y * scaleY,
                            face.width * scaleX,
                            face.height * scaleY,
                            thickness);
        }
    }

    flushOverlay(commandBuffer, faceOverlayBuffer_, 0.12F, 0.94F, 0.55F, 1.0F);
}

void VulkanVideoRenderer::drawRectOutline(float x,
                                          float y,
                                          float width,
                                          float height,
                                          float thickness) {
    if (width <= 0.0F || height <= 0.0F) {
        return;
    }

    drawRect(x, y, width, thickness);
    drawRect(x, y + height - thickness, width, thickness);
    drawRect(x, y, thickness, height);
    drawRect(x + width - thickness, y, thickness, height);
}

void VulkanVideoRenderer::drawText(float x, float y, const std::string& text, float scale) {
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

void VulkanVideoRenderer::drawRect(float x, float y, float width, float height) {
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

void VulkanVideoRenderer::flushOverlay(VkCommandBuffer commandBuffer,
                                       VulkanBuffer& buffer,
                                       float red,
                                       float green,
                                       float blue,
                                       float alpha) {
    if (overlayVertices_.empty()) {
        return;
    }

    const VkDeviceSize vertexBytes =
        static_cast<VkDeviceSize>(overlayVertices_.size() * sizeof(float));
    createOrResizeOverlayBuffer(buffer, vertexBytes);

    void* mappedMemory = nullptr;
    checkVk(vkMapMemory(device_, buffer.memory, 0, vertexBytes, 0, &mappedMemory),
            "vkMapMemory overlay");
    std::memcpy(mappedMemory, overlayVertices_.data(), static_cast<size_t>(vertexBytes));
    vkUnmapMemory(device_, buffer.memory);

    OverlayPushConstants pushConstants{};
    pushConstants.screenSize[0] = static_cast<float>(swapchainExtent_.width);
    pushConstants.screenSize[1] = static_cast<float>(swapchainExtent_.height);
    pushConstants.color[0] = red;
    pushConstants.color[1] = green;
    pushConstants.color[2] = blue;
    pushConstants.color[3] = alpha;

    vkCmdPushConstants(commandBuffer,
                       pipelineLayout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0,
                       sizeof(pushConstants),
                       &pushConstants);

    const VkBuffer vertexBuffers[] = {buffer.buffer};
    const VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
    vkCmdDraw(commandBuffer,
              static_cast<uint32_t>(overlayVertices_.size() / 2U),
              1,
              0,
              0);
}


} // namespace rtsp::rendering::vulkan

