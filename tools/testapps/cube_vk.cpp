// Capture test target: Vulkan, spinning cube with a random color per face, bytebeat audio.

#include "common.h"
#include "shaders_generated.h"

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "vulkan-1.lib")

namespace {

VkShaderModule CreateShaderModule(VkDevice device, const std::uint32_t* code, unsigned int byteSize)
{
    VkShaderModuleCreateInfo description = {};
    description.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    description.codeSize = byteSize;
    description.pCode = code;

    VkShaderModule module = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &description, nullptr, &module);
    return module;
}

std::uint32_t FindMemoryType(VkPhysicalDevice physical, std::uint32_t typeBits, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memory = {};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) != 0 && (memory.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0;
}

bool CreateBuffer(VkPhysicalDevice physical, VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage,
                  VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& memory)
{
    VkBufferCreateInfo description = {};
    description.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    description.size = size;
    description.usage = usage;
    description.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &description, nullptr, &buffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements requirements = {};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);

    VkMemoryAllocateInfo allocation = {};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = FindMemoryType(physical, requirements.memoryTypeBits, properties);
    if (vkAllocateMemory(device, &allocation, nullptr, &memory) != VK_SUCCESS) {
        return false;
    }
    vkBindBufferMemory(device, buffer, memory, 0);
    return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE /*previous*/, PWSTR /*commandLine*/, int /*show*/)
{
    TestWindow window;
    const bool fullscreen = TestFullscreenRequested();
    if (!CreateTestWindow(window, instance, L"wxr test - Vulkan", 800, 450, fullscreen)) {
        return 1;
    }
    const int width = window.width;
    const int height = window.height;
    StartTestAudio();

    VkApplicationInfo application = {};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "wxr test - Vulkan";
    application.apiVersion = VK_API_VERSION_1_0;

    const char* instanceExtensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};

    VkInstanceCreateInfo instanceDescription = {};
    instanceDescription.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceDescription.pApplicationInfo = &application;
    instanceDescription.enabledExtensionCount = 2;
    instanceDescription.ppEnabledExtensionNames = instanceExtensions;

    VkInstance vkInstance = VK_NULL_HANDLE;
    if (vkCreateInstance(&instanceDescription, nullptr, &vkInstance) != VK_SUCCESS) {
        MessageBoxW(window.handle, L"vkCreateInstance failed.", L"wxr test - Vulkan", MB_OK | MB_ICONERROR);
        return 1;
    }

    VkWin32SurfaceCreateInfoKHR surfaceDescription = {};
    surfaceDescription.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surfaceDescription.hinstance = instance;
    surfaceDescription.hwnd = window.handle;

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (vkCreateWin32SurfaceKHR(vkInstance, &surfaceDescription, nullptr, &surface) != VK_SUCCESS) {
        MessageBoxW(window.handle, L"vkCreateWin32SurfaceKHR failed.", L"wxr test - Vulkan", MB_OK | MB_ICONERROR);
        return 1;
    }

    std::uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(vkInstance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        MessageBoxW(window.handle, L"No Vulkan physical devices found.", L"wxr test - Vulkan", MB_OK | MB_ICONERROR);
        return 1;
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(vkInstance, &deviceCount, devices.data());

    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    std::uint32_t queueFamily = 0;
    for (VkPhysicalDevice candidate : devices) {
        std::uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
        for (std::uint32_t i = 0; i < familyCount; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &present);
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && present) {
                physicalDevice = candidate;
                queueFamily = i;
                break;
            }
        }
        if (physicalDevice != VK_NULL_HANDLE) {
            break;
        }
    }
    if (physicalDevice == VK_NULL_HANDLE) {
        MessageBoxW(window.handle, L"No Vulkan device supports graphics and presentation.", L"wxr test - Vulkan",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    const float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueDescription = {};
    queueDescription.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueDescription.queueFamilyIndex = queueFamily;
    queueDescription.queueCount = 1;
    queueDescription.pQueuePriorities = &queuePriority;

    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo deviceDescription = {};
    deviceDescription.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceDescription.queueCreateInfoCount = 1;
    deviceDescription.pQueueCreateInfos = &queueDescription;
    deviceDescription.enabledExtensionCount = 1;
    deviceDescription.ppEnabledExtensionNames = deviceExtensions;

    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(physicalDevice, &deviceDescription, nullptr, &device) != VK_SUCCESS) {
        MessageBoxW(window.handle, L"vkCreateDevice failed.", L"wxr test - Vulkan", MB_OK | MB_ICONERROR);
        return 1;
    }

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    VkSurfaceCapabilitiesKHR capabilities = {};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities);

    std::uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, formats.data());

    VkSurfaceFormatKHR surfaceFormat = formats[0];
    for (const VkSurfaceFormatKHR& candidate : formats) {
        if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surfaceFormat = candidate;
            break;
        }
    }

    VkExtent2D extent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
    if (capabilities.currentExtent.width != 0xFFFFFFFFu) {
        extent = capabilities.currentExtent;
    }

    std::uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR swapDescription = {};
    swapDescription.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapDescription.surface = surface;
    swapDescription.minImageCount = imageCount;
    swapDescription.imageFormat = surfaceFormat.format;
    swapDescription.imageColorSpace = surfaceFormat.colorSpace;
    swapDescription.imageExtent = extent;
    swapDescription.imageArrayLayers = 1;
    swapDescription.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapDescription.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapDescription.preTransform = capabilities.currentTransform;
    swapDescription.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapDescription.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapDescription.clipped = VK_TRUE;

    VkSwapchainKHR swapChain = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(device, &swapDescription, nullptr, &swapChain) != VK_SUCCESS) {
        MessageBoxW(window.handle, L"vkCreateSwapchainKHR failed.", L"wxr test - Vulkan", MB_OK | MB_ICONERROR);
        return 1;
    }

    std::uint32_t swapImageCount = 0;
    vkGetSwapchainImagesKHR(device, swapChain, &swapImageCount, nullptr);
    std::vector<VkImage> swapImages(swapImageCount);
    vkGetSwapchainImagesKHR(device, swapChain, &swapImageCount, swapImages.data());

    std::vector<VkImageView> swapViews(swapImageCount);
    for (std::uint32_t i = 0; i < swapImageCount; ++i) {
        VkImageViewCreateInfo viewDescription = {};
        viewDescription.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewDescription.image = swapImages[i];
        viewDescription.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewDescription.format = surfaceFormat.format;
        viewDescription.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewDescription.subresourceRange.levelCount = 1;
        viewDescription.subresourceRange.layerCount = 1;
        vkCreateImageView(device, &viewDescription, nullptr, &swapViews[i]);
    }

    VkImageCreateInfo depthDescription = {};
    depthDescription.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depthDescription.imageType = VK_IMAGE_TYPE_2D;
    depthDescription.extent = {extent.width, extent.height, 1};
    depthDescription.mipLevels = 1;
    depthDescription.arrayLayers = 1;
    depthDescription.format = VK_FORMAT_D32_SFLOAT;
    depthDescription.tiling = VK_IMAGE_TILING_OPTIMAL;
    depthDescription.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthDescription.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depthDescription.samples = VK_SAMPLE_COUNT_1_BIT;
    depthDescription.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkImage depthImage = VK_NULL_HANDLE;
    vkCreateImage(device, &depthDescription, nullptr, &depthImage);
    VkMemoryRequirements depthRequirements = {};
    vkGetImageMemoryRequirements(device, depthImage, &depthRequirements);
    VkMemoryAllocateInfo depthAllocation = {};
    depthAllocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    depthAllocation.allocationSize = depthRequirements.size;
    depthAllocation.memoryTypeIndex =
        FindMemoryType(physicalDevice, depthRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    vkAllocateMemory(device, &depthAllocation, nullptr, &depthMemory);
    vkBindImageMemory(device, depthImage, depthMemory, 0);

    VkImageViewCreateInfo depthViewDescription = {};
    depthViewDescription.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depthViewDescription.image = depthImage;
    depthViewDescription.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depthViewDescription.format = VK_FORMAT_D32_SFLOAT;
    depthViewDescription.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthViewDescription.subresourceRange.levelCount = 1;
    depthViewDescription.subresourceRange.layerCount = 1;
    VkImageView depthView = VK_NULL_HANDLE;
    vkCreateImageView(device, &depthViewDescription, nullptr, &depthView);

    VkAttachmentDescription attachments[2] = {};
    attachments[0].format = surfaceFormat.format;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    attachments[1].format = VK_FORMAT_D32_SFLOAT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorReference = {};
    colorReference.attachment = 0;
    colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference depthReference = {};
    depthReference.attachment = 1;
    depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorReference;
    subpass.pDepthStencilAttachment = &depthReference;

    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassDescription = {};
    renderPassDescription.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassDescription.attachmentCount = 2;
    renderPassDescription.pAttachments = attachments;
    renderPassDescription.subpassCount = 1;
    renderPassDescription.pSubpasses = &subpass;
    renderPassDescription.dependencyCount = 1;
    renderPassDescription.pDependencies = &dependency;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    vkCreateRenderPass(device, &renderPassDescription, nullptr, &renderPass);

    std::vector<VkFramebuffer> framebuffers(swapImageCount);
    for (std::uint32_t i = 0; i < swapImageCount; ++i) {
        VkImageView views[] = {swapViews[i], depthView};
        VkFramebufferCreateInfo framebufferDescription = {};
        framebufferDescription.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferDescription.renderPass = renderPass;
        framebufferDescription.attachmentCount = 2;
        framebufferDescription.pAttachments = views;
        framebufferDescription.width = extent.width;
        framebufferDescription.height = extent.height;
        framebufferDescription.layers = 1;
        vkCreateFramebuffer(device, &framebufferDescription, nullptr, &framebuffers[i]);
    }

    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo layoutDescription = {};
    layoutDescription.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutDescription.bindingCount = 1;
    layoutDescription.pBindings = &binding;
    VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
    vkCreateDescriptorSetLayout(device, &layoutDescription, nullptr, &descriptorLayout);

    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = 2;
    VkDescriptorPoolCreateInfo poolDescription = {};
    poolDescription.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolDescription.maxSets = 2;
    poolDescription.poolSizeCount = 1;
    poolDescription.pPoolSizes = &poolSize;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    vkCreateDescriptorPool(device, &poolDescription, nullptr, &descriptorPool);

    VkDescriptorSetAllocateInfo setAllocation = {};
    setAllocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAllocation.descriptorPool = descriptorPool;
    setAllocation.descriptorSetCount = 2;
    VkDescriptorSetLayout setLayouts[2] = {descriptorLayout, descriptorLayout};
    setAllocation.pSetLayouts = setLayouts;
    VkDescriptorSet descriptorSets[2] = {};
    vkAllocateDescriptorSets(device, &setAllocation, descriptorSets);

    VkBuffer uniformBuffer = VK_NULL_HANDLE;
    VkDeviceMemory uniformMemory = VK_NULL_HANDLE;
    CreateBuffer(physicalDevice, device, 64, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, uniformBuffer,
                 uniformMemory);
    void* uniformMapped = nullptr;
    vkMapMemory(device, uniformMemory, 0, 64, 0, &uniformMapped);

    VkBuffer textUniformBuffer = VK_NULL_HANDLE;
    VkDeviceMemory textUniformMemory = VK_NULL_HANDLE;
    CreateBuffer(physicalDevice, device, 64, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, textUniformBuffer,
                 textUniformMemory);
    void* textUniformMapped = nullptr;
    vkMapMemory(device, textUniformMemory, 0, 64, 0, &textUniformMapped);
    const Mat4 textIdentity = MatIdentity();
    std::memcpy(textUniformMapped, textIdentity.m, sizeof(textIdentity.m));

    VkDescriptorBufferInfo bufferInfos[2] = {};
    bufferInfos[0].buffer = uniformBuffer;
    bufferInfos[0].range = 64;
    bufferInfos[1].buffer = textUniformBuffer;
    bufferInfos[1].range = 64;

    VkWriteDescriptorSet writes[2] = {};
    for (int i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptorSets[i];
        writes[i].dstBinding = 0;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[i].pBufferInfo = &bufferInfos[i];
    }
    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

    VkShaderModule vertexModule = CreateShaderModule(device, kCubeVertSpv, kCubeVertSpvSize);
    VkShaderModule fragmentModule = CreateShaderModule(device, kCubeFragSpv, kCubeFragSpvSize);

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertexModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragmentModule;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vertexBinding = {};
    vertexBinding.binding = 0;
    vertexBinding.stride = sizeof(CubeVertexF);
    vertexBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription vertexAttributes[2] = {};
    vertexAttributes[0].location = 0;
    vertexAttributes[0].binding = 0;
    vertexAttributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    vertexAttributes[0].offset = 0;
    vertexAttributes[1].location = 1;
    vertexAttributes[1].binding = 0;
    vertexAttributes[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    vertexAttributes[1].offset = 12;

    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &vertexBinding;
    vertexInput.vertexAttributeDescriptionCount = 2;
    vertexInput.pVertexAttributeDescriptions = vertexAttributes;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport = {0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, extent};

    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterization = {};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample = {};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blendAttachment = {};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend = {};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    VkPipelineLayoutCreateInfo pipelineLayoutDescription = {};
    pipelineLayoutDescription.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutDescription.setLayoutCount = 1;
    pipelineLayoutDescription.pSetLayouts = &descriptorLayout;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(device, &pipelineLayoutDescription, nullptr, &pipelineLayout);

    VkGraphicsPipelineCreateInfo pipelineDescription = {};
    pipelineDescription.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineDescription.stageCount = 2;
    pipelineDescription.pStages = stages;
    pipelineDescription.pVertexInputState = &vertexInput;
    pipelineDescription.pInputAssemblyState = &inputAssembly;
    pipelineDescription.pViewportState = &viewportState;
    pipelineDescription.pRasterizationState = &rasterization;
    pipelineDescription.pMultisampleState = &multisample;
    pipelineDescription.pDepthStencilState = &depthStencil;
    pipelineDescription.pColorBlendState = &colorBlend;
    pipelineDescription.layout = pipelineLayout;
    pipelineDescription.renderPass = renderPass;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineDescription, nullptr, &pipeline) != VK_SUCCESS) {
        MessageBoxW(window.handle, L"vkCreateGraphicsPipelines failed.", L"wxr test - Vulkan", MB_OK | MB_ICONERROR);
        return 1;
    }

    CubeVertexF vertices[24] = {};
    std::uint16_t indices[36] = {};
    BuildCube(vertices, indices);

    std::vector<CubeVertexF> textVertices;
    std::vector<std::uint16_t> textIndices;
    const float textColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    BuildTextMesh("VULKAN", 2.0f * 8.0f / width, 2.0f * 8.0f / height, -0.95f, 0.90f, textColor, textVertices,
                  textIndices);

    // Vulkan's clip-space Y points down, so mirror the label to keep it upright at the top.
    for (CubeVertexF& vertex : textVertices) {
        vertex.position[1] = -vertex.position[1];
    }

    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    CreateBuffer(physicalDevice, device, sizeof(vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, vertexBuffer,
                 vertexMemory);
    void* vertexMapped = nullptr;
    vkMapMemory(device, vertexMemory, 0, sizeof(vertices), 0, &vertexMapped);
    std::memcpy(vertexMapped, vertices, sizeof(vertices));

    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexMemory = VK_NULL_HANDLE;
    CreateBuffer(physicalDevice, device, sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, indexBuffer, indexMemory);
    void* indexMapped = nullptr;
    vkMapMemory(device, indexMemory, 0, sizeof(indices), 0, &indexMapped);
    std::memcpy(indexMapped, indices, sizeof(indices));

    const VkDeviceSize textVertexBytes = textVertices.size() * sizeof(CubeVertexF);
    VkBuffer textVertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory textVertexMemory = VK_NULL_HANDLE;
    CreateBuffer(physicalDevice, device, textVertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, textVertexBuffer,
                 textVertexMemory);
    void* textVertexMapped = nullptr;
    vkMapMemory(device, textVertexMemory, 0, textVertexBytes, 0, &textVertexMapped);
    std::memcpy(textVertexMapped, textVertices.data(), static_cast<std::size_t>(textVertexBytes));

    const VkDeviceSize textIndexBytes = textIndices.size() * sizeof(std::uint16_t);
    VkBuffer textIndexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory textIndexMemory = VK_NULL_HANDLE;
    CreateBuffer(physicalDevice, device, textIndexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, textIndexBuffer,
                 textIndexMemory);
    void* textIndexMapped = nullptr;
    vkMapMemory(device, textIndexMemory, 0, textIndexBytes, 0, &textIndexMapped);
    std::memcpy(textIndexMapped, textIndices.data(), static_cast<std::size_t>(textIndexBytes));

    VkCommandPoolCreateInfo poolCreate = {};
    poolCreate.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCreate.queueFamilyIndex = queueFamily;
    poolCreate.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    vkCreateCommandPool(device, &poolCreate, nullptr, &commandPool);

    VkCommandBufferAllocateInfo commandAllocation = {};
    commandAllocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandAllocation.commandPool = commandPool;
    commandAllocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandAllocation.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &commandAllocation, &commandBuffer);

    VkSemaphoreCreateInfo semaphoreDescription = {};
    semaphoreDescription.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkSemaphore renderFinished = VK_NULL_HANDLE;
    vkCreateSemaphore(device, &semaphoreDescription, nullptr, &imageAvailable);
    vkCreateSemaphore(device, &semaphoreDescription, nullptr, &renderFinished);

    VkFenceCreateInfo fenceDescription = {};
    fenceDescription.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceDescription.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence inFlight = VK_NULL_HANDLE;
    vkCreateFence(device, &fenceDescription, nullptr, &inFlight);

    const float eye[3] = {0.0f, 0.9f, -4.2f};
    const float target[3] = {0.0f, 0.0f, 0.0f};
    const float up[3] = {0.0f, 1.0f, 0.0f};
    const Mat4 view = MatLookAt(eye, target, up);
    const Mat4 projection =
        MatPerspectiveD3D(60.0f * 3.14159265f / 180.0f, static_cast<float>(extent.width) / extent.height, 0.1f,
                          100.0f);

    VkClearValue clearValues[2] = {};
    clearValues[0].color = {{0.094f, 0.102f, 0.125f, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};

    while (PumpTestWindow(window)) {
        const double time = TestTimeSeconds();
        const Mat4 world = MatRotationYawPitch(static_cast<float>(time) * 0.9f, 0.55f);
        const Mat4 worldViewProjection = MatMultiply(MatMultiply(world, view), projection);
        std::memcpy(uniformMapped, worldViewProjection.m, sizeof(worldViewProjection.m));

        vkWaitForFences(device, 1, &inFlight, VK_TRUE, UINT64_MAX);

        std::uint32_t imageIndex = 0;
        const VkResult acquired =
            vkAcquireNextImageKHR(device, swapChain, UINT64_MAX, imageAvailable, VK_NULL_HANDLE, &imageIndex);
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
            break;
        }

        vkResetFences(device, 1, &inFlight);
        vkResetCommandBuffer(commandBuffer, 0);

        VkCommandBufferBeginInfo beginDescription = {};
        beginDescription.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(commandBuffer, &beginDescription);

        VkRenderPassBeginInfo renderBegin = {};
        renderBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderBegin.renderPass = renderPass;
        renderBegin.framebuffer = framebuffers[imageIndex];
        renderBegin.renderArea.extent = extent;
        renderBegin.clearValueCount = 2;
        renderBegin.pClearValues = clearValues;
        vkCmdBeginRenderPass(commandBuffer, &renderBegin, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1,
                                &descriptorSets[0], 0, nullptr);

        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &offset);
        vkCmdBindIndexBuffer(commandBuffer, indexBuffer, 0, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(commandBuffer, 36, 1, 0, 0, 0);

        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1,
                                &descriptorSets[1], 0, nullptr);
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &textVertexBuffer, &offset);
        vkCmdBindIndexBuffer(commandBuffer, textIndexBuffer, 0, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(commandBuffer, static_cast<std::uint32_t>(textIndices.size()), 1, 0, 0, 0);

        vkCmdEndRenderPass(commandBuffer);
        vkEndCommandBuffer(commandBuffer);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &imageAvailable;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &renderFinished;
        vkQueueSubmit(queue, 1, &submit, inFlight);

        VkPresentInfoKHR present = {};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &renderFinished;
        present.swapchainCount = 1;
        present.pSwapchains = &swapChain;
        present.pImageIndices = &imageIndex;
        vkQueuePresentKHR(queue, &present);

        LimitFrameRate();

        UpdateFpsTitle(window);
    }

    StopTestAudio();
    vkDeviceWaitIdle(device);

    vkDestroyFence(device, inFlight, nullptr);
    vkDestroySemaphore(device, renderFinished, nullptr);
    vkDestroySemaphore(device, imageAvailable, nullptr);
    vkDestroyCommandPool(device, commandPool, nullptr);
    vkUnmapMemory(device, textIndexMemory);
    vkDestroyBuffer(device, textIndexBuffer, nullptr);
    vkFreeMemory(device, textIndexMemory, nullptr);
    vkUnmapMemory(device, textVertexMemory);
    vkDestroyBuffer(device, textVertexBuffer, nullptr);
    vkFreeMemory(device, textVertexMemory, nullptr);
    vkUnmapMemory(device, indexMemory);
    vkDestroyBuffer(device, indexBuffer, nullptr);
    vkFreeMemory(device, indexMemory, nullptr);
    vkUnmapMemory(device, vertexMemory);
    vkDestroyBuffer(device, vertexBuffer, nullptr);
    vkFreeMemory(device, vertexMemory, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    vkDestroyShaderModule(device, fragmentModule, nullptr);
    vkDestroyShaderModule(device, vertexModule, nullptr);
    vkDestroyDescriptorPool(device, descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
    vkUnmapMemory(device, uniformMemory);
    vkDestroyBuffer(device, uniformBuffer, nullptr);
    vkFreeMemory(device, uniformMemory, nullptr);
    vkUnmapMemory(device, textUniformMemory);
    vkDestroyBuffer(device, textUniformBuffer, nullptr);
    vkFreeMemory(device, textUniformMemory, nullptr);
    for (VkFramebuffer framebuffer : framebuffers) {
        vkDestroyFramebuffer(device, framebuffer, nullptr);
    }
    vkDestroyRenderPass(device, renderPass, nullptr);
    vkDestroyImageView(device, depthView, nullptr);
    vkDestroyImage(device, depthImage, nullptr);
    vkFreeMemory(device, depthMemory, nullptr);
    for (VkImageView view : swapViews) {
        vkDestroyImageView(device, view, nullptr);
    }
    vkDestroySwapchainKHR(device, swapChain, nullptr);
    vkDestroySurfaceKHR(vkInstance, surface, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(vkInstance, nullptr);
    return 0;
}
