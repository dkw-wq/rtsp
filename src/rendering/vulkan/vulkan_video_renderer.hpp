#pragma once

#include "frame_capture.hpp"
#include "video_renderer.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#endif

#include <vulkan/vulkan.h>

#ifdef RTSP_ENABLE_CUDA_INTEROP
#include <cuda_runtime.h>
#endif

extern "C" {
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <libavutil/frame.h>
}

namespace rtsp::rendering::vulkan {

constexpr int kWindowStartX = SDL_WINDOWPOS_CENTERED;
constexpr int kWindowStartY = SDL_WINDOWPOS_CENTERED;
constexpr size_t kMaxVideoSlots = 40;

extern const std::vector<const char*> kDeviceExtensions;

struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;

    bool complete() const {
        return graphicsFamily.has_value() && presentFamily.has_value();
    }
};

struct SwapchainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

struct VulkanBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkDeviceSize memorySize = 0;
};

#ifdef RTSP_ENABLE_CUDA_INTEROP
struct VulkanCudaBuffer {
    VulkanBuffer vulkan;
    cudaExternalMemory_t cudaMemory = nullptr;
    void* cudaPtr = nullptr;
};
#endif

struct VulkanImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
};

struct OverlayPushConstants {
    float screenSize[2] = {};
    float padding[2] = {};
    float color[4] = {};
};

enum class ShaderFilter : int32_t {
    None = 0,
    Grayscale = 1,
    Warm = 2,
    Invert = 3,
    Contrast = 4,
    Saturation = 5
};

struct VideoPushConstants {
    int32_t filterMode = 0;
};

void checkVk(VkResult result, const char* operation);
std::string ffmpegError(int errorCode);
void copyPlane(uint8_t* dst, int dstStride, const uint8_t* src,
               int srcStride, int width, int height);
const char* filterName(ShaderFilter filterMode);
std::vector<uint32_t> loadShader(const char* fileName);
#ifdef RTSP_ENABLE_CUDA_INTEROP
const char* cudaErrorName(cudaError_t error);
void checkCuda(cudaError_t error, const char* operation);
#endif

class VulkanVideoRenderer final : public VideoRenderer {
public:
    VulkanVideoRenderer();
    ~VulkanVideoRenderer() override;

    bool initialize(int width, int height, const std::string& title) override;
    bool render(const std::shared_ptr<MediaFrame>& frame) override;
    bool render(const std::vector<std::shared_ptr<MediaFrame>>& frames) override;
    void setPlaybackStats(const PlaybackStats& stats) override;
    void setFaceOverlays(const std::vector<FaceDetectionResult>& overlays) override;
    void setCommandCallback(std::function<void(RendererCommand)> callback) override;
    bool handleEvents() override;
    void close() override;

    int getWidth() const override;
    int getHeight() const override;
    bool isInitialized() const override;

private:
    void createInstance();
    void createSurface();
    void pickPhysicalDevice();
    void createLogicalDevice();
    void createSwapchain();
    void createSwapchainImageViews();
    void createRenderPass();
    void createDescriptorSetLayout();
    void createGraphicsPipeline();
    void createCommandPool();
    void createVideoImages();
    void createVideoImagesForSlot(size_t slot, int width, int height);
    void createTextureSampler();
    void createDescriptorPool();
    void createDescriptorSet();
    void updateDescriptorSet();
    void updateMultiDescriptorSet(size_t slot);
    void createFramebuffers();
    void createCommandBuffers();
    void createSyncObjects();
    void createOrResizeStagingBuffer(VkDeviceSize size);
    void createOrResizeReadbackBuffer(VkDeviceSize size);
    void createOrResizeOverlayBuffer(VulkanBuffer& buffer, VkDeviceSize size);
#ifdef RTSP_ENABLE_CUDA_INTEROP
    void createOrResizeCudaUploadBuffers(int width, int height);
    void createOrResizeCudaUploadBuffersForSlot(size_t slot, int width, int height);
    bool ensureCudaUploadSemaphore();
    void destroyCudaUploadSemaphore();
    void consumeCudaUploadSemaphore();
#endif

    void cleanupSwapchain();
    void recreateSwapchain();
    void destroyBuffer(VulkanBuffer& buffer);
#ifdef RTSP_ENABLE_CUDA_INTEROP
    void destroyCudaBuffer(VulkanCudaBuffer& buffer);
#endif
    void destroyImage(VulkanImage& image);
    void destroyVideoImages();

    bool renderNv12(const MediaFrame& frame);
    bool renderMultiNv12(const std::vector<std::shared_ptr<MediaFrame>>& frames);
    bool submitUploadedFrame();
#ifdef RTSP_ENABLE_CUDA_INTEROP
    bool renderCudaNv12(const MediaFrame& frame);
    bool renderCudaNv12ViaCpuFallback(const MediaFrame& frame, const char* reason);
    bool transferCudaFrameToCpuNv12(const MediaFrame& frame, MediaFrame& cpuFrame) const;
    bool uploadCudaFrameToVulkanBuffers(const MediaFrame& frame);
    bool uploadCudaFrameToVulkanBuffersForSlot(const MediaFrame& frame, size_t slot);
    bool signalCudaUploadSemaphore();
#endif
    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex);
    void transitionImage(VkCommandBuffer commandBuffer,
                         VulkanImage& image,
                         VkImageLayout newLayout);
    void copyBufferToImage(VkCommandBuffer commandBuffer,
                           VkBuffer sourceBuffer,
                           const VulkanImage& image,
                           VkDeviceSize bufferOffset);
    void transitionSwapchainImage(VkCommandBuffer commandBuffer,
                                  VkImage image,
                                  VkImageLayout oldLayout,
                                  VkImageLayout newLayout);
    void copySwapchainImageToBuffer(VkCommandBuffer commandBuffer, VkImage image);
    void recordSingleVideo(VkCommandBuffer commandBuffer);
    void recordMultiVideo(VkCommandBuffer commandBuffer);

    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) const;
    bool isDeviceSuitable(VkPhysicalDevice device) const;
    bool checkDeviceExtensionSupport(VkPhysicalDevice device) const;
    SwapchainSupportDetails querySwapchainSupport(VkPhysicalDevice device) const;
    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const;
    VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& presentModes) const;
    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) const;
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
    VkShaderModule createShaderModule(const std::vector<uint32_t>& code) const;
    VkImageView createImageView(VkImage image, VkFormat format) const;
    VulkanBuffer createBuffer(VkDeviceSize size,
                              VkBufferUsageFlags usage,
                              VkMemoryPropertyFlags properties) const;
#ifdef RTSP_ENABLE_CUDA_INTEROP
    VulkanCudaBuffer createCudaInteropBuffer(VkDeviceSize size) const;
#endif
    VulkanImage createImage(uint32_t width,
                            uint32_t height,
                            VkFormat format,
                            VkImageUsageFlags usage) const;

    bool handleKeyboardShortcuts();
    bool captureNeeded() const;
    bool readCapturedFrame(RgbFrame& frame) const;
    void handleCaptureAfterRender();
    void saveScreenshot(const RgbFrame& frame);
    VkViewport videoViewport() const;
    VkViewport videoViewportFor(int videoWidth,
                                int videoHeight,
                                uint32_t slotIndex,
                                uint32_t slotCount) const;
    std::array<std::string, 13> makeStatusLines() const;
    void drawStatusLayout(VkCommandBuffer commandBuffer);
    void drawFaceOverlays(VkCommandBuffer commandBuffer);
    void drawRectOutline(float x, float y, float width, float height, float thickness);
    void drawText(float x, float y, const std::string& text, float scale);
    void drawRect(float x, float y, float width, float height);
    void flushOverlay(VkCommandBuffer commandBuffer,
                      VulkanBuffer& buffer,
                      float red,
                      float green,
                      float blue,
                      float alpha);

    SDL_Window* window_;
    bool initialized_;
    bool framebufferResized_;
    bool fKeyDown_;
    bool sKeyDown_;
    bool rKeyDown_;
    bool screenshotRequested_;
    bool captureThisFrame_;
    bool capturePending_;
    bool swapchainTransferSrcSupported_;
    ShaderFilter filterMode_;

    int width_;
    int height_;
    PlaybackStats playbackStats_;
    std::vector<FaceDetectionResult> faceOverlays_;
    std::string uploadPath_;

    VkInstance instance_;
    VkSurfaceKHR surface_;
    VkPhysicalDevice physicalDevice_;
    VkDevice device_;
    VkQueue graphicsQueue_;
    VkQueue presentQueue_;
    VkSwapchainKHR swapchain_;
    VkFormat swapchainImageFormat_;
    VkExtent2D swapchainExtent_;
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainImageViews_;
    std::vector<VkFramebuffer> swapchainFramebuffers_;

    VkRenderPass renderPass_;
    VkDescriptorSetLayout descriptorSetLayout_;
    VkPipelineLayout pipelineLayout_;
    VkPipeline graphicsPipeline_;
    VkPipeline overlayPipeline_;
    VkCommandPool commandPool_;
    std::vector<VkCommandBuffer> commandBuffers_;

    VulkanImage yImage_;
    VulkanImage uvImage_;
    std::array<VulkanImage, kMaxVideoSlots> multiYImages_;
    std::array<VulkanImage, kMaxVideoSlots> multiUvImages_;
    std::array<int, kMaxVideoSlots> multiWidths_;
    std::array<int, kMaxVideoSlots> multiHeights_;
    std::array<bool, kMaxVideoSlots> multiReady_;
    std::array<bool, kMaxVideoSlots> multiUploadPending_;
    uint32_t activeVideoSlots_;
    VulkanBuffer stagingBuffer_;
    VulkanBuffer readbackBuffer_;
#ifdef RTSP_ENABLE_CUDA_INTEROP
    VulkanCudaBuffer cudaYBuffer_;
    VulkanCudaBuffer cudaUvBuffer_;
    std::array<VulkanCudaBuffer, kMaxVideoSlots> multiCudaYBuffers_;
    std::array<VulkanCudaBuffer, kMaxVideoSlots> multiCudaUvBuffers_;
    PFN_vkGetMemoryWin32HandleKHR vkGetMemoryWin32HandleKHR_;
    PFN_vkGetSemaphoreWin32HandleKHR vkGetSemaphoreWin32HandleKHR_;
    VkSemaphore cudaUploadSemaphore_;
    cudaExternalSemaphore_t cudaUploadExternalSemaphore_;
    cudaStream_t cudaUploadStream_;
    bool currentUploadUsesCudaSemaphore_;
    bool cudaInteropDisabled_;
    bool cudaFallbackLogged_;
    std::shared_ptr<void> pendingCudaUploadFrameRef_;
    std::array<std::shared_ptr<void>, kMaxVideoSlots> pendingMultiCudaUploadFrameRefs_;
#endif
    VulkanBuffer overlayBackgroundBuffer_;
    VulkanBuffer faceOverlayBuffer_;
    VulkanBuffer overlayBuffer_;
    VkBuffer uploadYBuffer_;
    VkBuffer uploadUvBuffer_;
    VkDeviceSize uploadYBufferOffset_;
    VkDeviceSize uploadUvBufferOffset_;
    std::array<VkBuffer, kMaxVideoSlots> multiUploadYBuffers_;
    std::array<VkBuffer, kMaxVideoSlots> multiUploadUvBuffers_;
    std::array<VkDeviceSize, kMaxVideoSlots> multiUploadYBufferOffsets_;
    std::array<VkDeviceSize, kMaxVideoSlots> multiUploadUvBufferOffsets_;
    VkSampler textureSampler_;
    VkDescriptorPool descriptorPool_;
    VkDescriptorSet descriptorSet_;
    std::array<VkDescriptorSet, kMaxVideoSlots> multiDescriptorSets_;
    std::function<void(RendererCommand)> commandCallback_;
    std::vector<float> overlayVertices_;

    VkSemaphore imageAvailableSemaphore_;
    VkSemaphore renderFinishedSemaphore_;
    VkFence inFlightFence_;
};

} // namespace rtsp::rendering::vulkan

