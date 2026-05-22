#include "vulkan_video_renderer.hpp"

#include <algorithm>
#include <cstring>
#include <set>
#include <stdexcept>

#ifdef RTSP_ENABLE_CUDA_INTEROP
#include <cuda_runtime.h>
#endif

#include <spdlog/spdlog.h>

namespace rtsp::rendering::vulkan {

void VulkanVideoRenderer::createOrResizeStagingBuffer(VkDeviceSize size) {
    if (stagingBuffer_.buffer != VK_NULL_HANDLE && stagingBuffer_.size >= size) {
        return;
    }

    destroyBuffer(stagingBuffer_);
    stagingBuffer_ = createBuffer(size,
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}

void VulkanVideoRenderer::createOrResizeReadbackBuffer(VkDeviceSize size) {
    if (readbackBuffer_.buffer != VK_NULL_HANDLE && readbackBuffer_.size >= size) {
        return;
    }

    destroyBuffer(readbackBuffer_);
    readbackBuffer_ = createBuffer(size,
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}

#ifdef RTSP_ENABLE_CUDA_INTEROP
void VulkanVideoRenderer::createOrResizeCudaUploadBuffers(int width, int height) {
    const VkDeviceSize ySize = static_cast<VkDeviceSize>(width) *
                               static_cast<VkDeviceSize>(height);
    const VkDeviceSize uvSize = ySize / 2U;

    if (cudaYBuffer_.vulkan.buffer == VK_NULL_HANDLE ||
        cudaYBuffer_.vulkan.size < ySize) {
        destroyCudaBuffer(cudaYBuffer_);
        cudaYBuffer_ = createCudaInteropBuffer(ySize);
    }

    if (cudaUvBuffer_.vulkan.buffer == VK_NULL_HANDLE ||
        cudaUvBuffer_.vulkan.size < uvSize) {
        destroyCudaBuffer(cudaUvBuffer_);
        cudaUvBuffer_ = createCudaInteropBuffer(uvSize);
    }
}

void VulkanVideoRenderer::createOrResizeCudaUploadBuffersForSlot(size_t slot,
                                                                 int width,
                                                                 int height) {
    if (slot >= kMaxVideoSlots) {
        throw std::out_of_range("CUDA multi-stream slot is out of range");
    }

    const VkDeviceSize ySize = static_cast<VkDeviceSize>(width) *
                               static_cast<VkDeviceSize>(height);
    const VkDeviceSize uvSize = ySize / 2U;

    if (multiCudaYBuffers_[slot].vulkan.buffer == VK_NULL_HANDLE ||
        multiCudaYBuffers_[slot].vulkan.size < ySize) {
        destroyCudaBuffer(multiCudaYBuffers_[slot]);
        multiCudaYBuffers_[slot] = createCudaInteropBuffer(ySize);
    }

    if (multiCudaUvBuffers_[slot].vulkan.buffer == VK_NULL_HANDLE ||
        multiCudaUvBuffers_[slot].vulkan.size < uvSize) {
        destroyCudaBuffer(multiCudaUvBuffers_[slot]);
        multiCudaUvBuffers_[slot] = createCudaInteropBuffer(uvSize);
    }
}

bool VulkanVideoRenderer::ensureCudaUploadSemaphore() {
    if (cudaUploadSemaphore_ != VK_NULL_HANDLE &&
        cudaUploadExternalSemaphore_ != nullptr &&
        cudaUploadStream_ != nullptr) {
        return true;
    }

    destroyCudaUploadSemaphore();

    VkExportSemaphoreCreateInfo exportInfo{};
    exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
    exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphoreInfo.pNext = &exportInfo;
    checkVk(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &cudaUploadSemaphore_),
            "vkCreateSemaphore CUDA upload");

    HANDLE semaphoreHandle = nullptr;
    VkSemaphoreGetWin32HandleInfoKHR handleInfo{};
    handleInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR;
    handleInfo.semaphore = cudaUploadSemaphore_;
    handleInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    checkVk(vkGetSemaphoreWin32HandleKHR_(device_, &handleInfo, &semaphoreHandle),
            "vkGetSemaphoreWin32HandleKHR");

    cudaExternalSemaphoreHandleDesc semaphoreDesc{};
    semaphoreDesc.type = cudaExternalSemaphoreHandleTypeOpaqueWin32;
    semaphoreDesc.handle.win32.handle = semaphoreHandle;

    const cudaError_t importError =
        cudaImportExternalSemaphore(&cudaUploadExternalSemaphore_, &semaphoreDesc);
    if (semaphoreHandle != nullptr) {
        CloseHandle(semaphoreHandle);
    }
    checkCuda(importError, "cudaImportExternalSemaphore");

    checkCuda(cudaStreamCreateWithFlags(&cudaUploadStream_, cudaStreamNonBlocking),
              "cudaStreamCreateWithFlags");
    return true;
}

void VulkanVideoRenderer::destroyCudaUploadSemaphore() {
    pendingCudaUploadFrameRef_.reset();
    pendingMultiCudaUploadFrameRefs_.fill(nullptr);
    currentUploadUsesCudaSemaphore_ = false;

    if (cudaUploadStream_ != nullptr) {
        const cudaError_t syncError = cudaStreamSynchronize(cudaUploadStream_);
        if (syncError != cudaSuccess) {
            SPDLOG_WARN("Failed to synchronize CUDA upload stream during shutdown: {}",
                        cudaErrorName(syncError));
        }

        const cudaError_t destroyError = cudaStreamDestroy(cudaUploadStream_);
        if (destroyError != cudaSuccess) {
            SPDLOG_WARN("Failed to destroy CUDA upload stream: {}",
                        cudaErrorName(destroyError));
        }
        cudaUploadStream_ = nullptr;
    }

    if (cudaUploadExternalSemaphore_ != nullptr) {
        const cudaError_t error = cudaDestroyExternalSemaphore(cudaUploadExternalSemaphore_);
        if (error != cudaSuccess) {
            SPDLOG_WARN("Failed to destroy CUDA external semaphore: {}", cudaErrorName(error));
        }
        cudaUploadExternalSemaphore_ = nullptr;
    }

    if (device_ != VK_NULL_HANDLE && cudaUploadSemaphore_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, cudaUploadSemaphore_, nullptr);
    }
    cudaUploadSemaphore_ = VK_NULL_HANDLE;
}

void VulkanVideoRenderer::consumeCudaUploadSemaphore() {
    if (!currentUploadUsesCudaSemaphore_ || cudaUploadSemaphore_ == VK_NULL_HANDLE) {
        return;
    }

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &cudaUploadSemaphore_;
    submitInfo.pWaitDstStageMask = &waitStage;

    checkVk(vkResetFences(device_, 1, &inFlightFence_), "vkResetFences CUDA consume");
    checkVk(vkQueueSubmit(graphicsQueue_, 1, &submitInfo, inFlightFence_),
            "vkQueueSubmit CUDA consume");
    checkVk(vkWaitForFences(device_, 1, &inFlightFence_, VK_TRUE,
                            std::numeric_limits<uint64_t>::max()),
            "vkWaitForFences CUDA consume");

    currentUploadUsesCudaSemaphore_ = false;
    pendingCudaUploadFrameRef_.reset();
    pendingMultiCudaUploadFrameRefs_.fill(nullptr);
}
#endif

void VulkanVideoRenderer::createOrResizeOverlayBuffer(VulkanBuffer& buffer, VkDeviceSize size) {
    if (buffer.buffer != VK_NULL_HANDLE && buffer.size >= size) {
        return;
    }

    destroyBuffer(buffer);
    buffer = createBuffer(size,
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}

void VulkanVideoRenderer::cleanupSwapchain() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }

    if (!commandBuffers_.empty()) {
        vkFreeCommandBuffers(device_,
                             commandPool_,
                             static_cast<uint32_t>(commandBuffers_.size()),
                             commandBuffers_.data());
        commandBuffers_.clear();
    }

    for (VkFramebuffer framebuffer : swapchainFramebuffers_) {
        vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    swapchainFramebuffers_.clear();

    if (graphicsPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, graphicsPipeline_, nullptr);
        graphicsPipeline_ = VK_NULL_HANDLE;
    }
    if (overlayPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, overlayPipeline_, nullptr);
        overlayPipeline_ = VK_NULL_HANDLE;
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }
    if (renderPass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device_, renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
    }

    for (VkImageView imageView : swapchainImageViews_) {
        vkDestroyImageView(device_, imageView, nullptr);
    }
    swapchainImageViews_.clear();
    swapchainImages_.clear();

    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

void VulkanVideoRenderer::recreateSwapchain() {
    int drawableWidth = 0;
    int drawableHeight = 0;
    SDL_GetWindowSize(window_, &drawableWidth, &drawableHeight);
    if (drawableWidth <= 0 || drawableHeight <= 0) {
        return;
    }

    vkDeviceWaitIdle(device_);

    cleanupSwapchain();
    createSwapchain();
    createSwapchainImageViews();
    createRenderPass();
    createGraphicsPipeline();
    createFramebuffers();
    createCommandBuffers();

    framebufferResized_ = false;
}

void VulkanVideoRenderer::destroyBuffer(VulkanBuffer& buffer) {
    if (device_ == VK_NULL_HANDLE) {
        buffer = {};
        return;
    }

    if (buffer.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, buffer.buffer, nullptr);
    }
    if (buffer.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, buffer.memory, nullptr);
    }
    buffer = {};
}

#ifdef RTSP_ENABLE_CUDA_INTEROP
void VulkanVideoRenderer::destroyCudaBuffer(VulkanCudaBuffer& buffer) {
    if (buffer.cudaPtr != nullptr) {
        const cudaError_t error = cudaFree(buffer.cudaPtr);
        if (error != cudaSuccess) {
            SPDLOG_WARN("Failed to free CUDA mapped Vulkan buffer: {}", cudaErrorName(error));
        }
        buffer.cudaPtr = nullptr;
    }

    if (buffer.cudaMemory != nullptr) {
        const cudaError_t error = cudaDestroyExternalMemory(buffer.cudaMemory);
        if (error != cudaSuccess) {
            SPDLOG_WARN("Failed to destroy CUDA external memory: {}", cudaErrorName(error));
        }
        buffer.cudaMemory = nullptr;
    }

    destroyBuffer(buffer.vulkan);
    buffer = {};
}
#endif

void VulkanVideoRenderer::destroyImage(VulkanImage& image) {
    if (device_ == VK_NULL_HANDLE) {
        image = {};
        return;
    }

    if (image.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, image.view, nullptr);
    }
    if (image.image != VK_NULL_HANDLE) {
        vkDestroyImage(device_, image.image, nullptr);
    }
    if (image.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, image.memory, nullptr);
    }
    image = {};
}

void VulkanVideoRenderer::destroyVideoImages() {
    destroyImage(yImage_);
    destroyImage(uvImage_);
    for (size_t slot = 0; slot < kMaxVideoSlots; ++slot) {
        destroyImage(multiYImages_[slot]);
        destroyImage(multiUvImages_[slot]);
        multiWidths_[slot] = 0;
        multiHeights_[slot] = 0;
        multiReady_[slot] = false;
        multiUploadPending_[slot] = false;
    }
}


QueueFamilyIndices VulkanVideoRenderer::findQueueFamilies(VkPhysicalDevice device) const {
    QueueFamilyIndices indices;

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            indices.graphicsFamily = i;
        }

        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &presentSupport);
        if (presentSupport == VK_TRUE) {
            indices.presentFamily = i;
        }

        if (indices.complete()) {
            break;
        }
    }

    return indices;
}

bool VulkanVideoRenderer::isDeviceSuitable(VkPhysicalDevice device) const {
    const QueueFamilyIndices indices = findQueueFamilies(device);
    if (!indices.complete() || !checkDeviceExtensionSupport(device)) {
        return false;
    }

    const SwapchainSupportDetails support = querySwapchainSupport(device);
    return !support.formats.empty() && !support.presentModes.empty();
}

bool VulkanVideoRenderer::checkDeviceExtensionSupport(VkPhysicalDevice device) const {
    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(device,
                                         nullptr,
                                         &extensionCount,
                                         availableExtensions.data());

    std::set<std::string> requiredExtensions(kDeviceExtensions.begin(),
                                             kDeviceExtensions.end());
    for (const auto& extension : availableExtensions) {
        requiredExtensions.erase(extension.extensionName);
    }

    return requiredExtensions.empty();
}

SwapchainSupportDetails VulkanVideoRenderer::querySwapchainSupport(VkPhysicalDevice device) const {
    SwapchainSupportDetails details;

    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface_, &details.capabilities);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, nullptr);
    if (formatCount > 0) {
        details.formats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device,
                                            surface_,
                                            &formatCount,
                                            details.formats.data());
    }

    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount, nullptr);
    if (presentModeCount > 0) {
        details.presentModes.resize(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device,
                                                  surface_,
                                                  &presentModeCount,
                                                  details.presentModes.data());
    }

    return details;
}

VkSurfaceFormatKHR VulkanVideoRenderer::chooseSwapSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR>& formats) const {
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }

    return formats.front();
}

VkPresentModeKHR VulkanVideoRenderer::chooseSwapPresentMode(
    const std::vector<VkPresentModeKHR>& presentModes) const {
    for (VkPresentModeKHR mode : presentModes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
            return mode;
        }
    }
    for (VkPresentModeKHR mode : presentModes) {
        if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR) {
            return mode;
        }
    }

    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanVideoRenderer::chooseSwapExtent(
    const VkSurfaceCapabilitiesKHR& capabilities) const {
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return capabilities.currentExtent;
    }

    int drawableWidth = 0;
    int drawableHeight = 0;
    SDL_GetWindowSize(window_, &drawableWidth, &drawableHeight);

    VkExtent2D extent{};
    extent.width = static_cast<uint32_t>(std::max(drawableWidth, 1));
    extent.height = static_cast<uint32_t>(std::max(drawableHeight, 1));
    extent.width = std::clamp(extent.width,
                              capabilities.minImageExtent.width,
                              capabilities.maxImageExtent.width);
    extent.height = std::clamp(extent.height,
                               capabilities.minImageExtent.height,
                               capabilities.maxImageExtent.height);
    return extent;
}

uint32_t VulkanVideoRenderer::findMemoryType(uint32_t typeFilter,
                                             VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties);

    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if ((typeFilter & (1U << i)) != 0 &&
            (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    throw std::runtime_error("No suitable Vulkan memory type found");
}

VkShaderModule VulkanVideoRenderer::createShaderModule(const std::vector<uint32_t>& code) const {
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size() * sizeof(uint32_t);
    createInfo.pCode = code.data();

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    checkVk(vkCreateShaderModule(device_, &createInfo, nullptr, &shaderModule),
            "vkCreateShaderModule");
    return shaderModule;
}

VkImageView VulkanVideoRenderer::createImageView(VkImage image, VkFormat format) const {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView imageView = VK_NULL_HANDLE;
    checkVk(vkCreateImageView(device_, &viewInfo, nullptr, &imageView),
            "vkCreateImageView");
    return imageView;
}

VulkanBuffer VulkanVideoRenderer::createBuffer(VkDeviceSize size,
                                               VkBufferUsageFlags usage,
                                               VkMemoryPropertyFlags properties) const {
    VulkanBuffer buffer{};
    buffer.size = size;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    checkVk(vkCreateBuffer(device_, &bufferInfo, nullptr, &buffer.buffer),
            "vkCreateBuffer");

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, buffer.buffer, &requirements);
    buffer.memorySize = requirements.size;

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, properties);

    checkVk(vkAllocateMemory(device_, &allocInfo, nullptr, &buffer.memory),
            "vkAllocateMemory");
    checkVk(vkBindBufferMemory(device_, buffer.buffer, buffer.memory, 0),
            "vkBindBufferMemory");

    return buffer;
}

#ifdef RTSP_ENABLE_CUDA_INTEROP
VulkanCudaBuffer VulkanVideoRenderer::createCudaInteropBuffer(VkDeviceSize size) const {
    VulkanCudaBuffer buffer{};
    buffer.vulkan.size = size;

    VkExternalMemoryBufferCreateInfo externalBufferInfo{};
    externalBufferInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    externalBufferInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.pNext = &externalBufferInfo;
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    checkVk(vkCreateBuffer(device_, &bufferInfo, nullptr, &buffer.vulkan.buffer),
            "vkCreateBuffer CUDA interop");

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, buffer.vulkan.buffer, &requirements);
    buffer.vulkan.memorySize = requirements.size;

    VkExportMemoryAllocateInfo exportInfo{};
    exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &exportInfo;
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex =
        findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    checkVk(vkAllocateMemory(device_, &allocInfo, nullptr, &buffer.vulkan.memory),
            "vkAllocateMemory CUDA interop");
    checkVk(vkBindBufferMemory(device_, buffer.vulkan.buffer, buffer.vulkan.memory, 0),
            "vkBindBufferMemory CUDA interop");

    HANDLE memoryHandle = nullptr;
    VkMemoryGetWin32HandleInfoKHR handleInfo{};
    handleInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
    handleInfo.memory = buffer.vulkan.memory;
    handleInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    checkVk(vkGetMemoryWin32HandleKHR_(device_, &handleInfo, &memoryHandle),
            "vkGetMemoryWin32HandleKHR");

    cudaExternalMemoryHandleDesc memoryDesc{};
    memoryDesc.type = cudaExternalMemoryHandleTypeOpaqueWin32;
    memoryDesc.handle.win32.handle = memoryHandle;
    memoryDesc.size = static_cast<unsigned long long>(buffer.vulkan.memorySize);

    const cudaError_t importError = cudaImportExternalMemory(&buffer.cudaMemory, &memoryDesc);
    if (memoryHandle != nullptr) {
        CloseHandle(memoryHandle);
    }
    checkCuda(importError, "cudaImportExternalMemory");

    cudaExternalMemoryBufferDesc bufferDesc{};
    bufferDesc.offset = 0;
    bufferDesc.size = static_cast<unsigned long long>(buffer.vulkan.size);
    checkCuda(cudaExternalMemoryGetMappedBuffer(&buffer.cudaPtr,
                                                buffer.cudaMemory,
                                                &bufferDesc),
              "cudaExternalMemoryGetMappedBuffer");

    return buffer;
}
#endif

VulkanImage VulkanVideoRenderer::createImage(uint32_t width,
                                             uint32_t height,
                                             VkFormat format,
                                             VkImageUsageFlags usage) const {
    VulkanImage image{};
    image.extent = {width, height};
    image.format = format;
    image.layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    checkVk(vkCreateImage(device_, &imageInfo, nullptr, &image.image),
            "vkCreateImage");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, image.image, &requirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = requirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    checkVk(vkAllocateMemory(device_, &allocInfo, nullptr, &image.memory),
            "vkAllocateMemory");
    checkVk(vkBindImageMemory(device_, image.image, image.memory, 0),
            "vkBindImageMemory");

    return image;
}


} // namespace rtsp::rendering::vulkan

