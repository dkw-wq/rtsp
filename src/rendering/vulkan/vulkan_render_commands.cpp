#include "vulkan_video_renderer.hpp"

#include <algorithm>
#include <cstring>
#include <optional>

#include <spdlog/spdlog.h>

extern "C" {
#include <libavutil/hwcontext.h>
}

namespace rtsp::rendering::vulkan {

bool VulkanVideoRenderer::renderNv12(const MediaFrame& frame) {
    if (frame.width <= 0 || frame.height <= 0 ||
        frame.width % 2 != 0 || frame.height % 2 != 0) {
        SPDLOG_ERROR("Invalid NV12 frame dimensions: {}x{}", frame.width, frame.height);
        return false;
    }

    const VkDeviceSize ySize = static_cast<VkDeviceSize>(frame.width) *
                               static_cast<VkDeviceSize>(frame.height);
    const VkDeviceSize requiredSize = ySize * 3U / 2U;
    if (frame.data.size() < static_cast<size_t>(requiredSize)) {
        SPDLOG_ERROR("Frame data too small for NV12: {} < {}",
                     frame.data.size(), static_cast<uint64_t>(requiredSize));
        return false;
    }

    try {
        checkVk(vkWaitForFences(device_, 1, &inFlightFence_, VK_TRUE,
                                std::numeric_limits<uint64_t>::max()),
                "vkWaitForFences");
        if (capturePending_) {
            handleCaptureAfterRender();
            capturePending_ = false;
        }
#ifdef RTSP_ENABLE_CUDA_INTEROP
        pendingCudaUploadFrameRef_.reset();
#endif

        if (frame.width != width_ || frame.height != height_) {
            vkDeviceWaitIdle(device_);
            width_ = frame.width;
            height_ = frame.height;
            createVideoImages();
            updateDescriptorSet();
        }

        createOrResizeStagingBuffer(requiredSize);
        void* mappedMemory = nullptr;
        checkVk(vkMapMemory(device_, stagingBuffer_.memory, 0, requiredSize, 0, &mappedMemory),
                "vkMapMemory");
        std::memcpy(mappedMemory, frame.data.data(), static_cast<size_t>(requiredSize));
        vkUnmapMemory(device_, stagingBuffer_.memory);

        uploadYBuffer_ = stagingBuffer_.buffer;
        uploadUvBuffer_ = stagingBuffer_.buffer;
        uploadYBufferOffset_ = 0;
        uploadUvBufferOffset_ = ySize;
        uploadPath_ = "CPU-STAGING";
        activeVideoSlots_ = 1;
        multiUploadPending_.fill(false);
#ifdef RTSP_ENABLE_CUDA_INTEROP
        currentUploadUsesCudaSemaphore_ = false;
#endif

        return submitUploadedFrame();
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Vulkan render failed: {}", e.what());
        return false;
    }
}

bool VulkanVideoRenderer::renderMultiNv12(const std::vector<std::shared_ptr<MediaFrame>>& frames) {
    if (!initialized_) {
        return false;
    }

    const size_t slotCount = std::min(frames.size(), kMaxVideoSlots);
    if (slotCount == 0) {
        return false;
    }

    VkDeviceSize requiredSize = 0;
    std::array<VkDeviceSize, kMaxVideoSlots> yOffsets{};
    std::array<VkDeviceSize, kMaxVideoSlots> uvOffsets{};
    std::array<VkDeviceSize, kMaxVideoSlots> frameSizes{};
    std::array<const MediaFrame*, kMaxVideoSlots> uploadFrames{};

    for (size_t slot = 0; slot < slotCount; ++slot) {
        const auto& frame = frames[slot];
        multiUploadPending_[slot] = false;
        if (!frame) {
            continue;
        }
        if (frame->pixelFormat != MediaFrame::PixelFormat::NV12) {
            SPDLOG_ERROR("Vulkan multi-stream renderer expects CPU NV12 frames");
            return false;
        }
        if (frame->width <= 0 || frame->height <= 0 ||
            frame->width % 2 != 0 || frame->height % 2 != 0) {
            SPDLOG_ERROR("Invalid multi-stream NV12 frame dimensions: {}x{}",
                         frame->width, frame->height);
            return false;
        }

        const VkDeviceSize ySize = static_cast<VkDeviceSize>(frame->width) *
                                   static_cast<VkDeviceSize>(frame->height);
        const VkDeviceSize frameSize = ySize * 3U / 2U;
        if (frame->data.size() < static_cast<size_t>(frameSize)) {
            SPDLOG_ERROR("Multi-stream frame data too small for NV12: {} < {}",
                         frame->data.size(), static_cast<uint64_t>(frameSize));
            return false;
        }

        yOffsets[slot] = requiredSize;
        uvOffsets[slot] = requiredSize + ySize;
        frameSizes[slot] = frameSize;
        uploadFrames[slot] = frame.get();
        requiredSize += frameSize;
    }

    if (requiredSize == 0) {
        return false;
    }

    try {
        checkVk(vkWaitForFences(device_, 1, &inFlightFence_, VK_TRUE,
                                std::numeric_limits<uint64_t>::max()),
                "vkWaitForFences");
        if (capturePending_) {
            handleCaptureAfterRender();
            capturePending_ = false;
        }
#ifdef RTSP_ENABLE_CUDA_INTEROP
        pendingCudaUploadFrameRef_.reset();
        currentUploadUsesCudaSemaphore_ = false;
#endif

        for (size_t slot = 0; slot < slotCount; ++slot) {
            const MediaFrame* frame = uploadFrames[slot];
            if (frame == nullptr) {
                continue;
            }

            if (multiWidths_[slot] != frame->width ||
                multiHeights_[slot] != frame->height ||
                multiYImages_[slot].image == VK_NULL_HANDLE ||
                multiUvImages_[slot].image == VK_NULL_HANDLE) {
                createVideoImagesForSlot(slot, frame->width, frame->height);
                updateMultiDescriptorSet(slot);
            }
        }

        createOrResizeStagingBuffer(requiredSize);
        void* mappedMemory = nullptr;
        checkVk(vkMapMemory(device_, stagingBuffer_.memory, 0, requiredSize, 0, &mappedMemory),
                "vkMapMemory multi-stream");
        auto* mappedBytes = static_cast<uint8_t*>(mappedMemory);
        for (size_t slot = 0; slot < slotCount; ++slot) {
            const MediaFrame* frame = uploadFrames[slot];
            if (frame == nullptr) {
                continue;
            }
            std::memcpy(mappedBytes + yOffsets[slot],
                        frame->data.data(),
                        static_cast<size_t>(frameSizes[slot]));
            multiUploadYBuffers_[slot] = stagingBuffer_.buffer;
            multiUploadUvBuffers_[slot] = stagingBuffer_.buffer;
            multiUploadYBufferOffsets_[slot] = yOffsets[slot];
            multiUploadUvBufferOffsets_[slot] = uvOffsets[slot];
            multiUploadPending_[slot] = true;
            multiReady_[slot] = true;
        }
        vkUnmapMemory(device_, stagingBuffer_.memory);

        activeVideoSlots_ = static_cast<uint32_t>(slotCount);
        uploadPath_ = "CPU-STAGING-MULTI";
        return submitUploadedFrame();
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Vulkan multi-stream render failed: {}", e.what());
        return false;
    }
}

bool VulkanVideoRenderer::submitUploadedFrame() {
    try {
        uint32_t imageIndex = 0;
        VkResult acquireResult = vkAcquireNextImageKHR(device_,
                                                       swapchain_,
                                                       std::numeric_limits<uint64_t>::max(),
                                                       imageAvailableSemaphore_,
                                                       VK_NULL_HANDLE,
                                                       &imageIndex);
        if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
#ifdef RTSP_ENABLE_CUDA_INTEROP
            consumeCudaUploadSemaphore();
#endif
            recreateSwapchain();
            return true;
        }
        if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
#ifdef RTSP_ENABLE_CUDA_INTEROP
            consumeCudaUploadSemaphore();
#endif
            checkVk(acquireResult, "vkAcquireNextImageKHR");
        }

        checkVk(vkResetFences(device_, 1, &inFlightFence_), "vkResetFences");
        checkVk(vkResetCommandBuffer(commandBuffers_[imageIndex], 0), "vkResetCommandBuffer");
        captureThisFrame_ = captureNeeded();
        if (captureThisFrame_) {
            const VkDeviceSize readbackSize =
                static_cast<VkDeviceSize>(swapchainExtent_.width) *
                static_cast<VkDeviceSize>(swapchainExtent_.height) * 4U;
            createOrResizeReadbackBuffer(readbackSize);
        }
        recordCommandBuffer(commandBuffers_[imageIndex], imageIndex);

        std::array<VkSemaphore, 2> waitSemaphores = {imageAvailableSemaphore_, VK_NULL_HANDLE};
        std::array<VkPipelineStageFlags, 2> waitStages = {
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT
        };
        uint32_t waitSemaphoreCount = 1;
#ifdef RTSP_ENABLE_CUDA_INTEROP
        if (currentUploadUsesCudaSemaphore_) {
            waitSemaphores[waitSemaphoreCount] = cudaUploadSemaphore_;
            ++waitSemaphoreCount;
        }
#endif
        const VkSemaphore signalSemaphores[] = {renderFinishedSemaphore_};

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = waitSemaphoreCount;
        submitInfo.pWaitSemaphores = waitSemaphores.data();
        submitInfo.pWaitDstStageMask = waitStages.data();
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffers_[imageIndex];
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;

        checkVk(vkQueueSubmit(graphicsQueue_, 1, &submitInfo, inFlightFence_),
                "vkQueueSubmit");

        if (captureThisFrame_) {
            capturePending_ = true;
            captureThisFrame_ = false;
        }

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain_;
        presentInfo.pImageIndices = &imageIndex;

        const VkResult presentResult = vkQueuePresentKHR(presentQueue_, &presentInfo);
        if (presentResult == VK_ERROR_OUT_OF_DATE_KHR ||
            presentResult == VK_SUBOPTIMAL_KHR ||
            framebufferResized_) {
            recreateSwapchain();
        } else {
            checkVk(presentResult, "vkQueuePresentKHR");
        }

        return true;
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Vulkan render failed: {}", e.what());
        return false;
    }
}

#ifdef RTSP_ENABLE_CUDA_INTEROP
bool VulkanVideoRenderer::renderCudaNv12(const MediaFrame& frame) {
    if (frame.width <= 0 || frame.height <= 0 ||
        frame.width % 2 != 0 || frame.height % 2 != 0) {
        SPDLOG_ERROR("Invalid CUDA NV12 frame dimensions: {}x{}", frame.width, frame.height);
        return false;
    }

    if (frame.gpuData[0] == 0 || frame.gpuData[1] == 0 ||
        frame.gpuLinesize[0] <= 0 || frame.gpuLinesize[1] <= 0) {
        SPDLOG_ERROR("CUDA NV12 frame is missing GPU plane data");
        return false;
    }

    if (cudaInteropDisabled_) {
        return renderCudaNv12ViaCpuFallback(frame, "CUDA/Vulkan interop is disabled after a previous failure");
    }

    try {
        checkVk(vkWaitForFences(device_, 1, &inFlightFence_, VK_TRUE,
                                std::numeric_limits<uint64_t>::max()),
                "vkWaitForFences");
        if (capturePending_) {
            handleCaptureAfterRender();
            capturePending_ = false;
        }
        pendingCudaUploadFrameRef_.reset();

        if (frame.width != width_ || frame.height != height_) {
            vkDeviceWaitIdle(device_);
            width_ = frame.width;
            height_ = frame.height;
            createVideoImages();
            updateDescriptorSet();
        }

        createOrResizeCudaUploadBuffers(frame.width, frame.height);
        if (!ensureCudaUploadSemaphore()) {
            cudaInteropDisabled_ = true;
            destroyCudaUploadSemaphore();
            return renderCudaNv12ViaCpuFallback(frame, "failed to create CUDA/Vulkan upload semaphore");
        }
        pendingCudaUploadFrameRef_ = frame.hardwareFrameRef;
        if (!uploadCudaFrameToVulkanBuffers(frame)) {
            if (cudaUploadStream_ != nullptr) {
                const cudaError_t syncError = cudaStreamSynchronize(cudaUploadStream_);
                if (syncError != cudaSuccess) {
                    SPDLOG_WARN("Failed to synchronize CUDA upload stream after upload failure: {}",
                                cudaErrorName(syncError));
                }
            }
            pendingCudaUploadFrameRef_.reset();
            currentUploadUsesCudaSemaphore_ = false;
            return renderCudaNv12ViaCpuFallback(frame, "failed to copy CUDA frame into Vulkan upload buffer");
        }

        uploadYBuffer_ = cudaYBuffer_.vulkan.buffer;
        uploadUvBuffer_ = cudaUvBuffer_.vulkan.buffer;
        uploadYBufferOffset_ = 0;
        uploadUvBufferOffset_ = 0;
        uploadPath_ = "CUDA-VK-BUFFER";
        activeVideoSlots_ = 1;
        multiUploadPending_.fill(false);
        currentUploadUsesCudaSemaphore_ = true;

        return submitUploadedFrame();
    } catch (const std::exception& e) {
        SPDLOG_WARN("Vulkan CUDA interop failed: {}; falling back to CPU NV12 staging", e.what());
        cudaInteropDisabled_ = true;
        pendingCudaUploadFrameRef_.reset();
        currentUploadUsesCudaSemaphore_ = false;
        destroyCudaUploadSemaphore();
        return renderCudaNv12ViaCpuFallback(frame, e.what());
    }
}

bool VulkanVideoRenderer::renderCudaNv12ViaCpuFallback(const MediaFrame& frame, const char* reason) {
    if (!cudaFallbackLogged_) {
        SPDLOG_WARN("CUDA/Vulkan upload fallback active: {}", reason ? reason : "unknown reason");
        cudaFallbackLogged_ = true;
    }

    MediaFrame cpuFrame;
    if (!transferCudaFrameToCpuNv12(frame, cpuFrame)) {
        return false;
    }

    return renderNv12(cpuFrame);
}

bool VulkanVideoRenderer::transferCudaFrameToCpuNv12(const MediaFrame& frame,
                                                     MediaFrame& cpuFrame) const {
    AVFrame* hardwareFrame = static_cast<AVFrame*>(frame.hardwareFrameRef.get());
    if (hardwareFrame == nullptr) {
        SPDLOG_WARN("CUDA CPU fallback cannot run without a retained hardware AVFrame");
        return false;
    }

    AVFrame* softwareFrame = av_frame_alloc();
    if (softwareFrame == nullptr) {
        SPDLOG_WARN("Failed to allocate CPU fallback AVFrame");
        return false;
    }

    const int transferResult = av_hwframe_transfer_data(softwareFrame, hardwareFrame, 0);
    if (transferResult < 0) {
        SPDLOG_WARN("Failed to transfer CUDA frame to CPU fallback: {}",
                    ffmpegError(transferResult));
        av_frame_free(&softwareFrame);
        return false;
    }

    const auto cleanupFrame = std::unique_ptr<AVFrame, void (*)(AVFrame*)>(
        softwareFrame,
        [](AVFrame* framePtr) {
            AVFrame* frameToFree = framePtr;
            av_frame_free(&frameToFree);
        });

    if (softwareFrame->format != AV_PIX_FMT_NV12 ||
        softwareFrame->data[0] == nullptr ||
        softwareFrame->data[1] == nullptr ||
        softwareFrame->linesize[0] <= 0 ||
        softwareFrame->linesize[1] <= 0) {
        SPDLOG_WARN("CUDA CPU fallback produced unsupported pixel format {}", softwareFrame->format);
        return false;
    }

    cpuFrame.type = MediaFrame::Type::VIDEO;
    cpuFrame.pixelFormat = MediaFrame::PixelFormat::NV12;
    cpuFrame.width = softwareFrame->width;
    cpuFrame.height = softwareFrame->height;
    cpuFrame.pts = frame.pts;
    cpuFrame.dts = frame.dts;
    cpuFrame.ptsSeconds = frame.ptsSeconds;
    cpuFrame.durationSeconds = frame.durationSeconds;
    cpuFrame.keyFrame = frame.keyFrame;

    const int ySize = cpuFrame.width * cpuFrame.height;
    cpuFrame.data.resize(static_cast<size_t>(ySize) * 3U / 2U);

    copyPlane(cpuFrame.data.data(),
              cpuFrame.width,
              softwareFrame->data[0],
              softwareFrame->linesize[0],
              cpuFrame.width,
              cpuFrame.height);
    copyPlane(cpuFrame.data.data() + ySize,
              cpuFrame.width,
              softwareFrame->data[1],
              softwareFrame->linesize[1],
              cpuFrame.width,
              cpuFrame.height / 2);

    return true;
}

bool VulkanVideoRenderer::uploadCudaFrameToVulkanBuffers(const MediaFrame& frame) {
    if (cudaUploadExternalSemaphore_ == nullptr || cudaUploadStream_ == nullptr) {
        SPDLOG_WARN("CUDA/Vulkan upload semaphore is not initialized");
        return false;
    }

    const cudaError_t yError =
        cudaMemcpy2DAsync(cudaYBuffer_.cudaPtr,
                          static_cast<size_t>(frame.width),
                          reinterpret_cast<const void*>(frame.gpuData[0]),
                          static_cast<size_t>(frame.gpuLinesize[0]),
                          static_cast<size_t>(frame.width),
                          static_cast<size_t>(frame.height),
                          cudaMemcpyDeviceToDevice,
                          cudaUploadStream_);
    if (yError != cudaSuccess) {
        SPDLOG_WARN("Failed to copy CUDA Y plane to Vulkan upload buffer: {}",
                    cudaErrorName(yError));
        return false;
    }

    const cudaError_t uvError =
        cudaMemcpy2DAsync(cudaUvBuffer_.cudaPtr,
                          static_cast<size_t>(frame.width),
                          reinterpret_cast<const void*>(frame.gpuData[1]),
                          static_cast<size_t>(frame.gpuLinesize[1]),
                          static_cast<size_t>(frame.width),
                          static_cast<size_t>(frame.height / 2),
                          cudaMemcpyDeviceToDevice,
                          cudaUploadStream_);
    if (uvError != cudaSuccess) {
        SPDLOG_WARN("Failed to copy CUDA UV plane to Vulkan upload buffer: {}",
                    cudaErrorName(uvError));
        return false;
    }

    cudaExternalSemaphoreSignalParams signalParams{};
    const cudaError_t signalError =
        cudaSignalExternalSemaphoresAsync(&cudaUploadExternalSemaphore_,
                                          &signalParams,
                                          1,
                                          cudaUploadStream_);
    if (signalError != cudaSuccess) {
        SPDLOG_WARN("Failed to signal CUDA/Vulkan upload semaphore: {}",
                    cudaErrorName(signalError));
        return false;
    }

    return true;
}
#endif

void VulkanVideoRenderer::recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    checkVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "vkBeginCommandBuffer");

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass_;
    renderPassInfo.framebuffer = swapchainFramebuffers_[imageIndex];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = swapchainExtent_;

    VkClearValue clearColor{};
    clearColor.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearColor;

    if (activeVideoSlots_ > 1) {
        recordMultiVideo(commandBuffer);
    } else {
        recordSingleVideo(commandBuffer);
    }

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline_);
    if (activeVideoSlots_ > 1) {
        for (uint32_t slot = 0; slot < activeVideoSlots_ && slot < kMaxVideoSlots; ++slot) {
            if (!multiReady_[slot] || multiDescriptorSets_[slot] == VK_NULL_HANDLE) {
                continue;
            }
            const VkViewport viewport = videoViewportFor(multiWidths_[slot],
                                                         multiHeights_[slot],
                                                         slot,
                                                         activeVideoSlots_);
            VkRect2D scissor{};
            scissor.offset = {0, 0};
            scissor.extent = swapchainExtent_;
            vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
            vkCmdBindDescriptorSets(commandBuffer,
                                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipelineLayout_,
                                    0,
                                    1,
                                    &multiDescriptorSets_[slot],
                                    0,
                                    nullptr);
            VideoPushConstants videoPushConstants{};
            videoPushConstants.filterMode = static_cast<int32_t>(filterMode_);
            vkCmdPushConstants(commandBuffer,
                               pipelineLayout_,
                               VK_SHADER_STAGE_FRAGMENT_BIT,
                               0,
                               sizeof(videoPushConstants),
                               &videoPushConstants);
            vkCmdDraw(commandBuffer, 3, 1, 0, 0);
        }
    } else {
        const VkViewport viewport = videoViewport();
        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = swapchainExtent_;
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

        vkCmdBindDescriptorSets(commandBuffer,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_,
                                0,
                                1,
                                &descriptorSet_,
                                0,
                                nullptr);
        VideoPushConstants videoPushConstants{};
        videoPushConstants.filterMode = static_cast<int32_t>(filterMode_);
        vkCmdPushConstants(commandBuffer,
                           pipelineLayout_,
                           VK_SHADER_STAGE_FRAGMENT_BIT,
                           0,
                           sizeof(videoPushConstants),
                           &videoPushConstants);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    }
    drawStatusLayout(commandBuffer);
    vkCmdEndRenderPass(commandBuffer);

    if (captureThisFrame_) {
        transitionSwapchainImage(commandBuffer,
                                 swapchainImages_[imageIndex],
                                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        copySwapchainImageToBuffer(commandBuffer, swapchainImages_[imageIndex]);
        transitionSwapchainImage(commandBuffer,
                                 swapchainImages_[imageIndex],
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }

    checkVk(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer");
}

void VulkanVideoRenderer::recordSingleVideo(VkCommandBuffer commandBuffer) {
    transitionImage(commandBuffer, yImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    transitionImage(commandBuffer, uvImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    copyBufferToImage(commandBuffer, uploadYBuffer_, yImage_, uploadYBufferOffset_);
    copyBufferToImage(commandBuffer, uploadUvBuffer_, uvImage_, uploadUvBufferOffset_);

    transitionImage(commandBuffer, yImage_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transitionImage(commandBuffer, uvImage_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void VulkanVideoRenderer::recordMultiVideo(VkCommandBuffer commandBuffer) {
    for (uint32_t slot = 0; slot < activeVideoSlots_ && slot < kMaxVideoSlots; ++slot) {
        if (!multiUploadPending_[slot]) {
            continue;
        }

        transitionImage(commandBuffer, multiYImages_[slot], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        transitionImage(commandBuffer, multiUvImages_[slot], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        copyBufferToImage(commandBuffer,
                          multiUploadYBuffers_[slot],
                          multiYImages_[slot],
                          multiUploadYBufferOffsets_[slot]);
        copyBufferToImage(commandBuffer,
                          multiUploadUvBuffers_[slot],
                          multiUvImages_[slot],
                          multiUploadUvBufferOffsets_[slot]);

        transitionImage(commandBuffer, multiYImages_[slot], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transitionImage(commandBuffer, multiUvImages_[slot], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        multiUploadPending_[slot] = false;
    }
}

void VulkanVideoRenderer::transitionImage(VkCommandBuffer commandBuffer,
                                          VulkanImage& image,
                                          VkImageLayout newLayout) {
    if (image.layout == newLayout) {
        return;
    }

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = image.layout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

    if (image.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
        newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    } else if (image.layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        throw std::runtime_error("Unsupported Vulkan image layout transition");
    }

    vkCmdPipelineBarrier(commandBuffer,
                         srcStage,
                         dstStage,
                         0,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &barrier);
    image.layout = newLayout;
}

void VulkanVideoRenderer::copyBufferToImage(VkCommandBuffer commandBuffer,
                                            VkBuffer sourceBuffer,
                                            const VulkanImage& image,
                                            VkDeviceSize bufferOffset) {
    VkBufferImageCopy region{};
    region.bufferOffset = bufferOffset;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {image.extent.width, image.extent.height, 1};

    vkCmdCopyBufferToImage(commandBuffer,
                           sourceBuffer,
                           image.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1,
                           &region);
}

void VulkanVideoRenderer::transitionSwapchainImage(VkCommandBuffer commandBuffer,
                                                   VkImage image,
                                                   VkImageLayout oldLayout,
                                                   VkImageLayout newLayout) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags sourceStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkPipelineStageFlags destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

    if (oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR &&
        newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = 0;
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    } else {
        throw std::runtime_error("Unsupported swapchain image layout transition");
    }

    vkCmdPipelineBarrier(commandBuffer,
                         sourceStage,
                         destinationStage,
                         0,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &barrier);
}

void VulkanVideoRenderer::copySwapchainImageToBuffer(VkCommandBuffer commandBuffer, VkImage image) {
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {swapchainExtent_.width, swapchainExtent_.height, 1};

    vkCmdCopyImageToBuffer(commandBuffer,
                           image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readbackBuffer_.buffer,
                           1,
                           &region);
}

} // namespace rtsp::rendering::vulkan

