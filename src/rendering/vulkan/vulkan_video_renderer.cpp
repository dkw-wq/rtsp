#include "vulkan_video_renderer.hpp"
#include "../common/renderer_input.hpp"

#include <algorithm>
#include <memory>

#include <spdlog/spdlog.h>

namespace rtsp::rendering::vulkan {

VulkanVideoRenderer::VulkanVideoRenderer()
    : window_(nullptr)
    , initialized_(false)
    , framebufferResized_(false)
    , fKeyDown_(false)
    , sKeyDown_(false)
    , rKeyDown_(false)
    , screenshotRequested_(false)
    , captureThisFrame_(false)
    , capturePending_(false)
    , swapchainTransferSrcSupported_(false)
    , filterMode_(ShaderFilter::None)
    , width_(0)
    , height_(0)
    , playbackStats_()
    , faceOverlays_()
    , uploadPath_("CPU-STAGING")
    , instance_(VK_NULL_HANDLE)
    , surface_(VK_NULL_HANDLE)
    , physicalDevice_(VK_NULL_HANDLE)
    , device_(VK_NULL_HANDLE)
    , graphicsQueue_(VK_NULL_HANDLE)
    , presentQueue_(VK_NULL_HANDLE)
    , swapchain_(VK_NULL_HANDLE)
    , swapchainImageFormat_(VK_FORMAT_UNDEFINED)
    , swapchainExtent_{}
    , swapchainImages_()
    , swapchainImageViews_()
    , swapchainFramebuffers_()
    , renderPass_(VK_NULL_HANDLE)
    , descriptorSetLayout_(VK_NULL_HANDLE)
    , pipelineLayout_(VK_NULL_HANDLE)
    , graphicsPipeline_(VK_NULL_HANDLE)
    , overlayPipeline_(VK_NULL_HANDLE)
    , commandPool_(VK_NULL_HANDLE)
    , commandBuffers_()
    , yImage_()
    , uvImage_()
    , multiYImages_()
    , multiUvImages_()
    , multiWidths_{}
    , multiHeights_{}
    , multiReady_{}
    , multiUploadPending_{}
    , activeVideoSlots_(1)
    , stagingBuffer_()
    , readbackBuffer_()
#ifdef RTSP_ENABLE_CUDA_INTEROP
    , cudaYBuffer_()
    , cudaUvBuffer_()
    , vkGetMemoryWin32HandleKHR_(nullptr)
    , vkGetSemaphoreWin32HandleKHR_(nullptr)
    , cudaUploadSemaphore_(VK_NULL_HANDLE)
    , cudaUploadExternalSemaphore_(nullptr)
    , cudaUploadStream_(nullptr)
    , currentUploadUsesCudaSemaphore_(false)
    , cudaInteropDisabled_(false)
    , cudaFallbackLogged_(false)
    , pendingCudaUploadFrameRef_()
#endif
    , overlayBackgroundBuffer_()
    , faceOverlayBuffer_()
    , overlayBuffer_()
    , uploadYBuffer_(VK_NULL_HANDLE)
    , uploadUvBuffer_(VK_NULL_HANDLE)
    , uploadYBufferOffset_(0)
    , uploadUvBufferOffset_(0)
    , multiUploadYBuffers_{}
    , multiUploadUvBuffers_{}
    , multiUploadYBufferOffsets_{}
    , multiUploadUvBufferOffsets_{}
    , textureSampler_(VK_NULL_HANDLE)
    , descriptorPool_(VK_NULL_HANDLE)
    , descriptorSet_(VK_NULL_HANDLE)
    , multiDescriptorSets_{}
    , commandCallback_()
    , overlayVertices_()
    , imageAvailableSemaphore_(VK_NULL_HANDLE)
    , renderFinishedSemaphore_(VK_NULL_HANDLE)
    , inFlightFence_(VK_NULL_HANDLE)
{}

VulkanVideoRenderer::~VulkanVideoRenderer() {
    close();
}

bool VulkanVideoRenderer::initialize(int width, int height, const std::string& title) {
    close();

    width_ = width;
    height_ = height;

    try {
        if (SDL_Init(SDL_INIT_VIDEO) < 0) {
            throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
        }

        window_ = SDL_CreateWindow(title.c_str(),
                                   kWindowStartX,
                                   kWindowStartY,
                                   width,
                                   height,
                                   SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if (!window_) {
            throw std::runtime_error(std::string("Failed to create Vulkan window: ") + SDL_GetError());
        }

        createInstance();
        createSurface();
        pickPhysicalDevice();
        createLogicalDevice();
        createSwapchain();
        createSwapchainImageViews();
        createRenderPass();
        createDescriptorSetLayout();
        createGraphicsPipeline();
        createCommandPool();
        createVideoImages();
        createTextureSampler();
        createDescriptorPool();
        createDescriptorSet();
        createFramebuffers();
        createCommandBuffers();
        createSyncObjects();
        createOrResizeStagingBuffer(static_cast<VkDeviceSize>(width_) *
                                    static_cast<VkDeviceSize>(height_) * 3U / 2U);

        initialized_ = true;
        SPDLOG_INFO("Vulkan renderer initialized: {}x{}", width_, height_);
        return true;
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Vulkan renderer initialization failed: {}", e.what());
        close();
        return false;
    }
}

bool VulkanVideoRenderer::render(const std::shared_ptr<MediaFrame>& frame) {
    if (!initialized_ || !frame) {
        return false;
    }

    if (frame->pixelFormat == MediaFrame::PixelFormat::NV12) {
        return renderNv12(*frame);
    }

#ifdef RTSP_ENABLE_CUDA_INTEROP
    if (frame->pixelFormat == MediaFrame::PixelFormat::CUDA_NV12) {
        return renderCudaNv12(*frame);
    }
#endif

    SPDLOG_ERROR("Vulkan renderer expected NV12 or CUDA_NV12 frame");
    return false;
}

bool VulkanVideoRenderer::render(const std::vector<std::shared_ptr<MediaFrame>>& frames) {
    if (frames.size() <= 1) {
        return frames.empty() ? false : render(frames.front());
    }

    return renderMultiNv12(frames);
}

void VulkanVideoRenderer::setPlaybackStats(const PlaybackStats& stats) {
    playbackStats_ = stats;
}

void VulkanVideoRenderer::setFaceOverlays(const std::vector<FaceDetectionResult>& overlays) {
    faceOverlays_ = overlays;
}

void VulkanVideoRenderer::setCommandCallback(std::function<void(RendererCommand)> callback) {
    commandCallback_ = std::move(callback);
}

bool VulkanVideoRenderer::handleEvents() {
    SDL_PumpEvents();

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                return false;
            case SDL_WINDOWEVENT:
                if (event.window.windowID == SDL_GetWindowID(window_) &&
                    event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    framebufferResized_ = true;
                }
                break;
            case SDL_KEYDOWN:
                if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE ||
                    event.key.keysym.scancode == SDL_SCANCODE_Q ||
                    event.key.keysym.sym == SDLK_ESCAPE ||
                    event.key.keysym.sym == SDLK_q) {
                    return false;
                }
                break;
            default:
                break;
        }
    }

    return handleKeyboardShortcuts();
}

void VulkanVideoRenderer::close() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        if (capturePending_) {
            handleCaptureAfterRender();
            capturePending_ = false;
        }
    }

    screenshotRequested_ = false;
    captureThisFrame_ = false;
    capturePending_ = false;

    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }

    cleanupSwapchain();

    if (imageAvailableSemaphore_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, imageAvailableSemaphore_, nullptr);
        imageAvailableSemaphore_ = VK_NULL_HANDLE;
    }
    if (renderFinishedSemaphore_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, renderFinishedSemaphore_, nullptr);
        renderFinishedSemaphore_ = VK_NULL_HANDLE;
    }
    if (inFlightFence_ != VK_NULL_HANDLE) {
        vkDestroyFence(device_, inFlightFence_, nullptr);
        inFlightFence_ = VK_NULL_HANDLE;
    }

    destroyBuffer(stagingBuffer_);
    destroyBuffer(readbackBuffer_);
#ifdef RTSP_ENABLE_CUDA_INTEROP
    destroyCudaUploadSemaphore();
    destroyCudaBuffer(cudaYBuffer_);
    destroyCudaBuffer(cudaUvBuffer_);
#endif
    destroyBuffer(overlayBackgroundBuffer_);
    destroyBuffer(faceOverlayBuffer_);
    destroyBuffer(overlayBuffer_);
    destroyVideoImages();

    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
        descriptorSet_ = VK_NULL_HANDLE;
    }
    if (textureSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, textureSampler_, nullptr);
        textureSampler_ = VK_NULL_HANDLE;
    }
    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
        descriptorSetLayout_ = VK_NULL_HANDLE;
    }
    if (commandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
        commandBuffers_.clear();
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);

    initialized_ = false;
    framebufferResized_ = false;
    width_ = 0;
    height_ = 0;
    uploadPath_ = "CPU-STAGING";
    swapchainTransferSrcSupported_ = false;
    filterMode_ = ShaderFilter::None;
    physicalDevice_ = VK_NULL_HANDLE;
    graphicsQueue_ = VK_NULL_HANDLE;
    presentQueue_ = VK_NULL_HANDLE;
    uploadYBuffer_ = VK_NULL_HANDLE;
    uploadUvBuffer_ = VK_NULL_HANDLE;
    uploadYBufferOffset_ = 0;
    uploadUvBufferOffset_ = 0;
    multiUploadYBuffers_.fill(VK_NULL_HANDLE);
    multiUploadUvBuffers_.fill(VK_NULL_HANDLE);
    multiUploadYBufferOffsets_.fill(0);
    multiUploadUvBufferOffsets_.fill(0);
    multiReady_.fill(false);
    multiUploadPending_.fill(false);
    activeVideoSlots_ = 1;
    multiDescriptorSets_.fill(VK_NULL_HANDLE);
#ifdef RTSP_ENABLE_CUDA_INTEROP
    vkGetMemoryWin32HandleKHR_ = nullptr;
    vkGetSemaphoreWin32HandleKHR_ = nullptr;
    currentUploadUsesCudaSemaphore_ = false;
    cudaInteropDisabled_ = false;
    cudaFallbackLogged_ = false;
    pendingCudaUploadFrameRef_.reset();
#endif
    overlayVertices_.clear();
}

int VulkanVideoRenderer::getWidth() const { return width_; }
int VulkanVideoRenderer::getHeight() const { return height_; }
bool VulkanVideoRenderer::isInitialized() const { return initialized_; }


} // namespace rtsp::rendering::vulkan

namespace rtsp {

std::unique_ptr<VideoRenderer> createVulkanVideoRenderer() {
    return std::make_unique<rendering::vulkan::VulkanVideoRenderer>();
}

} // namespace rtsp

