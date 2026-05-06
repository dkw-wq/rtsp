#include "video_renderer.hpp"

#include <spdlog/spdlog.h>

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
}

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace rtsp {

namespace {

constexpr int kWindowStartX = SDL_WINDOWPOS_CENTERED;
constexpr int kWindowStartY = SDL_WINDOWPOS_CENTERED;

const std::vector<const char*> kDeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
#ifdef RTSP_ENABLE_CUDA_INTEROP
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
#endif
};

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

using Glyph = std::array<uint8_t, 7>;

Glyph glyphFor(char ch) {
    switch (ch) {
        case '0': return {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
        case '1': return {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
        case '2': return {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
        case '3': return {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
        case '4': return {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
        case '5': return {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};
        case '6': return {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E};
        case '7': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
        case '8': return {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
        case '9': return {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E};
        case 'A': return {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
        case 'B': return {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
        case 'C': return {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
        case 'D': return {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
        case 'E': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
        case 'F': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
        case 'G': return {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F};
        case 'H': return {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
        case 'I': return {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E};
        case 'J': return {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E};
        case 'K': return {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
        case 'L': return {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
        case 'M': return {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
        case 'N': return {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
        case 'O': return {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
        case 'P': return {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
        case 'Q': return {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
        case 'R': return {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
        case 'S': return {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
        case 'T': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
        case 'U': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
        case 'V': return {0x11, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04};
        case 'W': return {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A};
        case 'X': return {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
        case 'Y': return {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
        case 'Z': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};
        case ':': return {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00};
        case '-': return {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
        case '.': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
        default: return {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    }
}

void checkVk(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) +
                                 " failed with VkResult " +
                                 std::to_string(static_cast<int>(result)));
    }
}

#ifdef RTSP_ENABLE_CUDA_INTEROP
const char* cudaErrorName(cudaError_t error) {
    return cudaGetErrorString(error);
}

void checkCuda(cudaError_t error, const char* operation) {
    if (error != cudaSuccess) {
        throw std::runtime_error(std::string(operation) +
                                 " failed: " +
                                 cudaErrorName(error));
    }
}
#endif

std::vector<uint32_t> readSpirvFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open shader: " + path.string());
    }

    const std::streamsize byteSize = file.tellg();
    if (byteSize <= 0 || byteSize % 4 != 0) {
        throw std::runtime_error("Invalid SPIR-V shader size: " + path.string());
    }

    std::vector<uint32_t> words(static_cast<size_t>(byteSize) / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), byteSize);
    if (!file) {
        throw std::runtime_error("Failed to read shader: " + path.string());
    }

    return words;
}

std::vector<std::filesystem::path> shaderSearchPaths(const char* fileName) {
    std::vector<std::filesystem::path> paths;
    paths.emplace_back(std::filesystem::path("shaders") / "vulkan" / fileName);

    char* basePath = SDL_GetBasePath();
    if (basePath) {
        paths.emplace_back(std::filesystem::path(basePath) / "shaders" / "vulkan" / fileName);
        SDL_free(basePath);
    }

    return paths;
}

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

class VulkanVideoRenderer final : public VideoRenderer {
public:
    VulkanVideoRenderer();
    ~VulkanVideoRenderer() override;

    bool initialize(int width, int height, const std::string& title) override;
    bool render(const std::shared_ptr<MediaFrame>& frame) override;
    void setPlaybackStats(const PlaybackStats& stats) override;
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
    void createTextureSampler();
    void createDescriptorPool();
    void createDescriptorSet();
    void updateDescriptorSet();
    void createFramebuffers();
    void createCommandBuffers();
    void createSyncObjects();
    void createOrResizeStagingBuffer(VkDeviceSize size);
    void createOrResizeOverlayBuffer(VulkanBuffer& buffer, VkDeviceSize size);
#ifdef RTSP_ENABLE_CUDA_INTEROP
    void createOrResizeCudaUploadBuffers(int width, int height);
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
    bool submitUploadedFrame();
#ifdef RTSP_ENABLE_CUDA_INTEROP
    bool renderCudaNv12(const MediaFrame& frame);
    bool uploadCudaFrameToVulkanBuffers(const MediaFrame& frame);
#endif
    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex);
    void transitionImage(VkCommandBuffer commandBuffer,
                         VulkanImage& image,
                         VkImageLayout newLayout);
    void copyBufferToImage(VkCommandBuffer commandBuffer,
                           VkBuffer sourceBuffer,
                           const VulkanImage& image,
                           VkDeviceSize bufferOffset);

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
    VkViewport videoViewport() const;
    std::array<std::string, 12> makeStatusLines() const;
    void drawStatusLayout(VkCommandBuffer commandBuffer);
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

    int width_;
    int height_;
    PlaybackStats playbackStats_;
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
    VulkanBuffer stagingBuffer_;
#ifdef RTSP_ENABLE_CUDA_INTEROP
    VulkanCudaBuffer cudaYBuffer_;
    VulkanCudaBuffer cudaUvBuffer_;
    PFN_vkGetMemoryWin32HandleKHR vkGetMemoryWin32HandleKHR_;
    PFN_vkGetSemaphoreWin32HandleKHR vkGetSemaphoreWin32HandleKHR_;
    VkSemaphore cudaUploadSemaphore_;
    cudaExternalSemaphore_t cudaUploadExternalSemaphore_;
    cudaStream_t cudaUploadStream_;
    bool currentUploadUsesCudaSemaphore_;
    std::shared_ptr<void> pendingCudaUploadFrameRef_;
#endif
    VulkanBuffer overlayBackgroundBuffer_;
    VulkanBuffer overlayBuffer_;
    VkBuffer uploadYBuffer_;
    VkBuffer uploadUvBuffer_;
    VkDeviceSize uploadYBufferOffset_;
    VkDeviceSize uploadUvBufferOffset_;
    VkSampler textureSampler_;
    VkDescriptorPool descriptorPool_;
    VkDescriptorSet descriptorSet_;
    std::vector<float> overlayVertices_;

    VkSemaphore imageAvailableSemaphore_;
    VkSemaphore renderFinishedSemaphore_;
    VkFence inFlightFence_;
};

VulkanVideoRenderer::VulkanVideoRenderer()
    : window_(nullptr)
    , initialized_(false)
    , framebufferResized_(false)
    , fKeyDown_(false)
    , sKeyDown_(false)
    , rKeyDown_(false)
    , width_(0)
    , height_(0)
    , playbackStats_()
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
    , stagingBuffer_()
#ifdef RTSP_ENABLE_CUDA_INTEROP
    , cudaYBuffer_()
    , cudaUvBuffer_()
    , vkGetMemoryWin32HandleKHR_(nullptr)
    , vkGetSemaphoreWin32HandleKHR_(nullptr)
    , cudaUploadSemaphore_(VK_NULL_HANDLE)
    , cudaUploadExternalSemaphore_(nullptr)
    , cudaUploadStream_(nullptr)
    , currentUploadUsesCudaSemaphore_(false)
    , pendingCudaUploadFrameRef_()
#endif
    , overlayBackgroundBuffer_()
    , overlayBuffer_()
    , uploadYBuffer_(VK_NULL_HANDLE)
    , uploadUvBuffer_(VK_NULL_HANDLE)
    , uploadYBufferOffset_(0)
    , uploadUvBufferOffset_(0)
    , textureSampler_(VK_NULL_HANDLE)
    , descriptorPool_(VK_NULL_HANDLE)
    , descriptorSet_(VK_NULL_HANDLE)
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

void VulkanVideoRenderer::setPlaybackStats(const PlaybackStats& stats) {
    playbackStats_ = stats;
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
#ifdef RTSP_ENABLE_CUDA_INTEROP
    destroyCudaUploadSemaphore();
    destroyCudaBuffer(cudaYBuffer_);
    destroyCudaBuffer(cudaUvBuffer_);
#endif
    destroyBuffer(overlayBackgroundBuffer_);
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
    physicalDevice_ = VK_NULL_HANDLE;
    graphicsQueue_ = VK_NULL_HANDLE;
    presentQueue_ = VK_NULL_HANDLE;
    uploadYBuffer_ = VK_NULL_HANDLE;
    uploadUvBuffer_ = VK_NULL_HANDLE;
    uploadYBufferOffset_ = 0;
    uploadUvBufferOffset_ = 0;
#ifdef RTSP_ENABLE_CUDA_INTEROP
    vkGetMemoryWin32HandleKHR_ = nullptr;
    vkGetSemaphoreWin32HandleKHR_ = nullptr;
    currentUploadUsesCudaSemaphore_ = false;
    pendingCudaUploadFrameRef_.reset();
#endif
    overlayVertices_.clear();
}

int VulkanVideoRenderer::getWidth() const { return width_; }
int VulkanVideoRenderer::getHeight() const { return height_; }
bool VulkanVideoRenderer::isInitialized() const { return initialized_; }

void VulkanVideoRenderer::createInstance() {
#ifdef _WIN32
    const std::array<const char*, 2> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME
    };
#else
#error VulkanVideoRenderer currently supports Windows surfaces only.
#endif

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "RTSP Player";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "RTSP Player";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    checkVk(vkCreateInstance(&createInfo, nullptr, &instance_), "vkCreateInstance");
}

void VulkanVideoRenderer::createSurface() {
#ifdef _WIN32
    SDL_SysWMinfo wmInfo{};
    SDL_VERSION(&wmInfo.version);
    if (SDL_GetWindowWMInfo(window_, &wmInfo) != SDL_TRUE ||
        wmInfo.subsystem != SDL_SYSWM_WINDOWS ||
        wmInfo.info.win.window == nullptr) {
        throw std::runtime_error(std::string("SDL_GetWindowWMInfo failed: ") + SDL_GetError());
    }

    VkWin32SurfaceCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    createInfo.hinstance = GetModuleHandleW(nullptr);
    createInfo.hwnd = wmInfo.info.win.window;

    checkVk(vkCreateWin32SurfaceKHR(instance_, &createInfo, nullptr, &surface_),
            "vkCreateWin32SurfaceKHR");
#endif
}

void VulkanVideoRenderer::pickPhysicalDevice() {
    uint32_t deviceCount = 0;
    checkVk(vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr),
            "vkEnumeratePhysicalDevices");
    if (deviceCount == 0) {
        throw std::runtime_error("No Vulkan-capable GPU found");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    checkVk(vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data()),
            "vkEnumeratePhysicalDevices");

    auto selectDevice = [this](VkPhysicalDevice device) {
        if (isDeviceSuitable(device)) {
            physicalDevice_ = device;
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(device, &properties);
            SPDLOG_INFO("Selected Vulkan device: {}", properties.deviceName);
            return true;
        }
        return false;
    };

#ifdef RTSP_ENABLE_CUDA_INTEROP
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        if (properties.vendorID == 0x10DE && selectDevice(device)) {
            return;
        }
    }
#endif

    for (VkPhysicalDevice device : devices) {
        if (selectDevice(device)) {
            return;
        }
    }

    throw std::runtime_error("No suitable Vulkan GPU found");
}

void VulkanVideoRenderer::createLogicalDevice() {
    const QueueFamilyIndices indices = findQueueFamilies(physicalDevice_);
    const std::set<uint32_t> uniqueFamilies = {
        indices.graphicsFamily.value(),
        indices.presentFamily.value()
    };

    const float queuePriority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    for (uint32_t family : uniqueFamilies) {
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = family;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    VkPhysicalDeviceFeatures deviceFeatures{};
    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &deviceFeatures;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(kDeviceExtensions.size());
    createInfo.ppEnabledExtensionNames = kDeviceExtensions.data();

    checkVk(vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_), "vkCreateDevice");

#ifdef RTSP_ENABLE_CUDA_INTEROP
    vkGetMemoryWin32HandleKHR_ =
        reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(
            vkGetDeviceProcAddr(device_, "vkGetMemoryWin32HandleKHR"));
    if (!vkGetMemoryWin32HandleKHR_) {
        throw std::runtime_error("vkGetMemoryWin32HandleKHR is unavailable");
    }
    vkGetSemaphoreWin32HandleKHR_ =
        reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(
            vkGetDeviceProcAddr(device_, "vkGetSemaphoreWin32HandleKHR"));
    if (!vkGetSemaphoreWin32HandleKHR_) {
        throw std::runtime_error("vkGetSemaphoreWin32HandleKHR is unavailable");
    }
#endif

    vkGetDeviceQueue(device_, indices.graphicsFamily.value(), 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, indices.presentFamily.value(), 0, &presentQueue_);
}

void VulkanVideoRenderer::createSwapchain() {
    const SwapchainSupportDetails support = querySwapchainSupport(physicalDevice_);
    const VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(support.formats);
    const VkPresentModeKHR presentMode = chooseSwapPresentMode(support.presentModes);
    const VkExtent2D extent = chooseSwapExtent(support.capabilities);

    uint32_t imageCount = support.capabilities.minImageCount + 1U;
    if (support.capabilities.maxImageCount > 0 && imageCount > support.capabilities.maxImageCount) {
        imageCount = support.capabilities.maxImageCount;
    }

    const QueueFamilyIndices indices = findQueueFamilies(physicalDevice_);
    const std::array<uint32_t, 2> queueFamilyIndices = {
        indices.graphicsFamily.value(),
        indices.presentFamily.value()
    };

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface_;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    if (indices.graphicsFamily != indices.presentFamily) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = static_cast<uint32_t>(queueFamilyIndices.size());
        createInfo.pQueueFamilyIndices = queueFamilyIndices.data();
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = support.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = VK_NULL_HANDLE;

    checkVk(vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_),
            "vkCreateSwapchainKHR");

    checkVk(vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr),
            "vkGetSwapchainImagesKHR");
    swapchainImages_.resize(imageCount);
    checkVk(vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchainImages_.data()),
            "vkGetSwapchainImagesKHR");

    swapchainImageFormat_ = surfaceFormat.format;
    swapchainExtent_ = extent;
}

void VulkanVideoRenderer::createSwapchainImageViews() {
    swapchainImageViews_.resize(swapchainImages_.size());
    for (size_t i = 0; i < swapchainImages_.size(); ++i) {
        swapchainImageViews_[i] = createImageView(swapchainImages_[i], swapchainImageFormat_);
    }
}

void VulkanVideoRenderer::createRenderPass() {
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = swapchainImageFormat_;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    checkVk(vkCreateRenderPass(device_, &renderPassInfo, nullptr, &renderPass_),
            "vkCreateRenderPass");
}

void VulkanVideoRenderer::createDescriptorSetLayout() {
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    for (uint32_t i = 0; i < static_cast<uint32_t>(bindings.size()); ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorCount = 1;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    checkVk(vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descriptorSetLayout_),
            "vkCreateDescriptorSetLayout");
}

void VulkanVideoRenderer::createGraphicsPipeline() {
    const std::vector<uint32_t> vertShader = loadShader("video.vert.spv");
    const std::vector<uint32_t> fragShader = loadShader("nv12.frag.spv");
    const std::vector<uint32_t> overlayVertShader = loadShader("overlay.vert.spv");
    const std::vector<uint32_t> overlayFragShader = loadShader("overlay.frag.spv");
    const VkShaderModule vertModule = createShaderModule(vertShader);
    const VkShaderModule fragModule = createShaderModule(fragShader);
    const VkShaderModule overlayVertModule = createShaderModule(overlayVertShader);
    const VkShaderModule overlayFragModule = createShaderModule(overlayFragShader);

    VkPipelineShaderStageCreateInfo vertStage{};
    vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertModule;
    vertStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragStage{};
    fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = fragModule;
    fragStage.pName = "main";

    const std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages = {
        vertStage,
        fragStage
    };

    VkPipelineShaderStageCreateInfo overlayVertStage{};
    overlayVertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    overlayVertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    overlayVertStage.module = overlayVertModule;
    overlayVertStage.pName = "main";

    VkPipelineShaderStageCreateInfo overlayFragStage{};
    overlayFragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    overlayFragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    overlayFragStage.module = overlayFragModule;
    overlayFragStage.pName = "main";

    const std::array<VkPipelineShaderStageCreateInfo, 2> overlayShaderStages = {
        overlayVertStage,
        overlayFragStage
    };

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkVertexInputBindingDescription overlayBinding{};
    overlayBinding.binding = 0;
    overlayBinding.stride = sizeof(float) * 2U;
    overlayBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription overlayAttribute{};
    overlayAttribute.binding = 0;
    overlayAttribute.location = 0;
    overlayAttribute.format = VK_FORMAT_R32G32_SFLOAT;
    overlayAttribute.offset = 0;

    VkPipelineVertexInputStateCreateInfo overlayVertexInput{};
    overlayVertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    overlayVertexInput.vertexBindingDescriptionCount = 1;
    overlayVertexInput.pVertexBindingDescriptions = &overlayBinding;
    overlayVertexInput.vertexAttributeDescriptionCount = 1;
    overlayVertexInput.pVertexAttributeDescriptions = &overlayAttribute;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                          VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT |
                                          VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendAttachmentState overlayColorBlendAttachment = colorBlendAttachment;
    overlayColorBlendAttachment.blendEnable = VK_TRUE;
    overlayColorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    overlayColorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    overlayColorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    overlayColorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    overlayColorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    overlayColorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    VkPipelineColorBlendStateCreateInfo overlayColorBlending = colorBlending;
    overlayColorBlending.pAttachments = &overlayColorBlendAttachment;

    const std::array<VkDynamicState, 2> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPushConstantRange overlayPushConstantRange{};
    overlayPushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
                                          VK_SHADER_STAGE_FRAGMENT_BIT;
    overlayPushConstantRange.offset = 0;
    overlayPushConstantRange.size = sizeof(OverlayPushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout_;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &overlayPushConstantRange;

    checkVk(vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_),
            "vkCreatePipelineLayout");

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
    pipelineInfo.pStages = shaderStages.data();
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout_;
    pipelineInfo.renderPass = renderPass_;
    pipelineInfo.subpass = 0;

    checkVk(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo,
                                      nullptr, &graphicsPipeline_),
            "vkCreateGraphicsPipelines");

    VkGraphicsPipelineCreateInfo overlayPipelineInfo = pipelineInfo;
    overlayPipelineInfo.stageCount = static_cast<uint32_t>(overlayShaderStages.size());
    overlayPipelineInfo.pStages = overlayShaderStages.data();
    overlayPipelineInfo.pVertexInputState = &overlayVertexInput;
    overlayPipelineInfo.pColorBlendState = &overlayColorBlending;

    checkVk(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &overlayPipelineInfo,
                                      nullptr, &overlayPipeline_),
            "vkCreateGraphicsPipelines overlay");

    vkDestroyShaderModule(device_, overlayFragModule, nullptr);
    vkDestroyShaderModule(device_, overlayVertModule, nullptr);
    vkDestroyShaderModule(device_, fragModule, nullptr);
    vkDestroyShaderModule(device_, vertModule, nullptr);
}

void VulkanVideoRenderer::createCommandPool() {
    const QueueFamilyIndices indices = findQueueFamilies(physicalDevice_);
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = indices.graphicsFamily.value();

    checkVk(vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_),
            "vkCreateCommandPool");
}

void VulkanVideoRenderer::createVideoImages() {
    destroyVideoImages();

    yImage_ = createImage(static_cast<uint32_t>(width_),
                          static_cast<uint32_t>(height_),
                          VK_FORMAT_R8_UNORM,
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    uvImage_ = createImage(static_cast<uint32_t>(width_ / 2),
                           static_cast<uint32_t>(height_ / 2),
                           VK_FORMAT_R8G8_UNORM,
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

    yImage_.view = createImageView(yImage_.image, yImage_.format);
    uvImage_.view = createImageView(uvImage_.image, uvImage_.format);
}

void VulkanVideoRenderer::createTextureSampler() {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    checkVk(vkCreateSampler(device_, &samplerInfo, nullptr, &textureSampler_),
            "vkCreateSampler");
}

void VulkanVideoRenderer::createDescriptorPool() {
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 2;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = 1;

    checkVk(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_),
            "vkCreateDescriptorPool");
}

void VulkanVideoRenderer::createDescriptorSet() {
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &descriptorSetLayout_;

    checkVk(vkAllocateDescriptorSets(device_, &allocInfo, &descriptorSet_),
            "vkAllocateDescriptorSets");
    updateDescriptorSet();
}

void VulkanVideoRenderer::updateDescriptorSet() {
    VkDescriptorImageInfo yInfo{};
    yInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    yInfo.imageView = yImage_.view;
    yInfo.sampler = textureSampler_;

    VkDescriptorImageInfo uvInfo{};
    uvInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    uvInfo.imageView = uvImage_.view;
    uvInfo.sampler = textureSampler_;

    std::array<VkWriteDescriptorSet, 2> descriptorWrites{};
    descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[0].dstSet = descriptorSet_;
    descriptorWrites[0].dstBinding = 0;
    descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrites[0].descriptorCount = 1;
    descriptorWrites[0].pImageInfo = &yInfo;

    descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[1].dstSet = descriptorSet_;
    descriptorWrites[1].dstBinding = 1;
    descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrites[1].descriptorCount = 1;
    descriptorWrites[1].pImageInfo = &uvInfo;

    vkUpdateDescriptorSets(device_,
                           static_cast<uint32_t>(descriptorWrites.size()),
                           descriptorWrites.data(),
                           0,
                           nullptr);
}

void VulkanVideoRenderer::createFramebuffers() {
    swapchainFramebuffers_.resize(swapchainImageViews_.size());
    for (size_t i = 0; i < swapchainImageViews_.size(); ++i) {
        VkImageView attachment = swapchainImageViews_[i];
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass_;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &attachment;
        framebufferInfo.width = swapchainExtent_.width;
        framebufferInfo.height = swapchainExtent_.height;
        framebufferInfo.layers = 1;

        checkVk(vkCreateFramebuffer(device_, &framebufferInfo, nullptr,
                                    &swapchainFramebuffers_[i]),
                "vkCreateFramebuffer");
    }
}

void VulkanVideoRenderer::createCommandBuffers() {
    commandBuffers_.resize(swapchainFramebuffers_.size());

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers_.size());

    checkVk(vkAllocateCommandBuffers(device_, &allocInfo, commandBuffers_.data()),
            "vkAllocateCommandBuffers");
}

void VulkanVideoRenderer::createSyncObjects() {
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    checkVk(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailableSemaphore_),
            "vkCreateSemaphore");
    checkVk(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &renderFinishedSemaphore_),
            "vkCreateSemaphore");
    checkVk(vkCreateFence(device_, &fenceInfo, nullptr, &inFlightFence_),
            "vkCreateFence");
}

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
}

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
#ifdef RTSP_ENABLE_CUDA_INTEROP
        currentUploadUsesCudaSemaphore_ = false;
#endif

        return submitUploadedFrame();
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Vulkan render failed: {}", e.what());
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

    try {
        checkVk(vkWaitForFences(device_, 1, &inFlightFence_, VK_TRUE,
                                std::numeric_limits<uint64_t>::max()),
                "vkWaitForFences");
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
            return false;
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
            return false;
        }

        uploadYBuffer_ = cudaYBuffer_.vulkan.buffer;
        uploadUvBuffer_ = cudaUvBuffer_.vulkan.buffer;
        uploadYBufferOffset_ = 0;
        uploadUvBufferOffset_ = 0;
        uploadPath_ = "CUDA-VK-BUFFER";
        currentUploadUsesCudaSemaphore_ = true;

        return submitUploadedFrame();
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Vulkan CUDA render failed: {}", e.what());
        return false;
    }
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

    transitionImage(commandBuffer, yImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    transitionImage(commandBuffer, uvImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    copyBufferToImage(commandBuffer, uploadYBuffer_, yImage_, uploadYBufferOffset_);
    copyBufferToImage(commandBuffer, uploadUvBuffer_, uvImage_, uploadUvBufferOffset_);

    transitionImage(commandBuffer, yImage_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transitionImage(commandBuffer, uvImage_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

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

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline_);

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
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    drawStatusLayout(commandBuffer);
    vkCmdEndRenderPass(commandBuffer);

    checkVk(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer");
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

std::array<std::string, 12> VulkanVideoRenderer::makeStatusLines() const {
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

bool VulkanVideoRenderer::handleKeyboardShortcuts() {
    const uint8_t* keys = SDL_GetKeyboardState(nullptr);
    if (!keys) {
        return true;
    }

    if (keys[SDL_SCANCODE_ESCAPE] != 0 || keys[SDL_SCANCODE_Q] != 0) {
        return false;
    }

    const bool fDown = keys[SDL_SCANCODE_F] != 0;
    if (fDown && !fKeyDown_) {
        SPDLOG_INFO("Filter switching is not implemented in the first Vulkan renderer version");
    }
    fKeyDown_ = fDown;

    const bool sDown = keys[SDL_SCANCODE_S] != 0;
    if (sDown && !sKeyDown_) {
        SPDLOG_INFO("Screenshots are not implemented in the first Vulkan renderer version");
    }
    sKeyDown_ = sDown;

    const bool rDown = keys[SDL_SCANCODE_R] != 0;
    if (rDown && !rKeyDown_) {
        SPDLOG_INFO("Recording is not implemented in the first Vulkan renderer version");
    }
    rKeyDown_ = rDown;

    return true;
}

VkViewport VulkanVideoRenderer::videoViewport() const {
    const float surfaceWidth = static_cast<float>(swapchainExtent_.width);
    const float surfaceHeight = static_cast<float>(swapchainExtent_.height);
    const float videoAspect = static_cast<float>(width_) / static_cast<float>(height_);
    const float surfaceAspect = surfaceWidth / surfaceHeight;

    float viewportWidth = surfaceWidth;
    float viewportHeight = surfaceHeight;
    float viewportX = 0.0f;
    float viewportY = 0.0f;

    if (surfaceAspect > videoAspect) {
        viewportWidth = surfaceHeight * videoAspect;
        viewportX = (surfaceWidth - viewportWidth) * 0.5f;
    } else {
        viewportHeight = surfaceWidth / videoAspect;
        viewportY = (surfaceHeight - viewportHeight) * 0.5f;
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

} // namespace

std::unique_ptr<VideoRenderer> createVulkanVideoRenderer() {
    return std::make_unique<VulkanVideoRenderer>();
}

} // namespace rtsp
