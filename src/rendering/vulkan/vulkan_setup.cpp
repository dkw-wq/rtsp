#include "vulkan_video_renderer.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

#include <spdlog/spdlog.h>

namespace rtsp::rendering::vulkan {

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
    swapchainTransferSrcSupported_ =
        (support.capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    if (swapchainTransferSrcSupported_) {
        createInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    } else {
        SPDLOG_WARN("Vulkan swapchain does not support TRANSFER_SRC; screenshots are disabled");
    }

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

    for (size_t slot = 0; slot < kMaxVideoSlots; ++slot) {
        createVideoImagesForSlot(slot, width_, height_);
    }
}

void VulkanVideoRenderer::createVideoImagesForSlot(size_t slot, int width, int height) {
    if (slot >= kMaxVideoSlots || width <= 0 || height <= 0) {
        return;
    }

    destroyImage(multiYImages_[slot]);
    destroyImage(multiUvImages_[slot]);

    multiYImages_[slot] = createImage(static_cast<uint32_t>(width),
                                      static_cast<uint32_t>(height),
                                      VK_FORMAT_R8_UNORM,
                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    multiUvImages_[slot] = createImage(static_cast<uint32_t>(width / 2),
                                       static_cast<uint32_t>(height / 2),
                                       VK_FORMAT_R8G8_UNORM,
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

    multiYImages_[slot].view = createImageView(multiYImages_[slot].image, multiYImages_[slot].format);
    multiUvImages_[slot].view = createImageView(multiUvImages_[slot].image, multiUvImages_[slot].format);
    multiWidths_[slot] = width;
    multiHeights_[slot] = height;
    multiReady_[slot] = false;
    multiUploadPending_[slot] = false;
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
    poolSize.descriptorCount = 2 + static_cast<uint32_t>(kMaxVideoSlots) * 2U;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = 1 + static_cast<uint32_t>(kMaxVideoSlots);

    checkVk(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_),
            "vkCreateDescriptorPool");
}

void VulkanVideoRenderer::createDescriptorSet() {
    std::array<VkDescriptorSetLayout, 1 + kMaxVideoSlots> layouts{};
    layouts.fill(descriptorSetLayout_);

    std::array<VkDescriptorSet, 1 + kMaxVideoSlots> sets{};
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool_;
    allocInfo.descriptorSetCount = static_cast<uint32_t>(sets.size());
    allocInfo.pSetLayouts = layouts.data();

    checkVk(vkAllocateDescriptorSets(device_, &allocInfo, sets.data()),
            "vkAllocateDescriptorSets");
    descriptorSet_ = sets[0];
    for (size_t slot = 0; slot < kMaxVideoSlots; ++slot) {
        multiDescriptorSets_[slot] = sets[slot + 1U];
    }

    updateDescriptorSet();
    for (size_t slot = 0; slot < kMaxVideoSlots; ++slot) {
        updateMultiDescriptorSet(slot);
    }
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

void VulkanVideoRenderer::updateMultiDescriptorSet(size_t slot) {
    if (slot >= kMaxVideoSlots || multiDescriptorSets_[slot] == VK_NULL_HANDLE ||
        multiYImages_[slot].view == VK_NULL_HANDLE || multiUvImages_[slot].view == VK_NULL_HANDLE) {
        return;
    }

    VkDescriptorImageInfo yInfo{};
    yInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    yInfo.imageView = multiYImages_[slot].view;
    yInfo.sampler = textureSampler_;

    VkDescriptorImageInfo uvInfo{};
    uvInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    uvInfo.imageView = multiUvImages_[slot].view;
    uvInfo.sampler = textureSampler_;

    std::array<VkWriteDescriptorSet, 2> descriptorWrites{};
    descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[0].dstSet = multiDescriptorSets_[slot];
    descriptorWrites[0].dstBinding = 0;
    descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrites[0].descriptorCount = 1;
    descriptorWrites[0].pImageInfo = &yInfo;

    descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[1].dstSet = multiDescriptorSets_[slot];
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


} // namespace rtsp::rendering::vulkan

