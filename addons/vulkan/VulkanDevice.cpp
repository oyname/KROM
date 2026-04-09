#include "VulkanDevice.hpp"
#include "core/Debug.hpp"
#include <array>
#include <cstring>

#ifdef _WIN32
#   define WIN32_LEAN_AND_MEAN
#   define NOMINMAX
#   include <windows.h>
#endif

namespace engine::renderer::vulkan {
namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void*)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        Debug::LogError("Vulkan: %s", data && data->pMessage ? data->pMessage : "unknown validation error");
    else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        Debug::LogWarning("Vulkan: %s", data && data->pMessage ? data->pMessage : "unknown validation warning");
    else
        Debug::Log("Vulkan: %s", data && data->pMessage ? data->pMessage : "validation");
    return VK_FALSE;
}

bool HasFlag(ShaderStageMask mask, ShaderStageMask bit)
{
    return (static_cast<uint8_t>(mask) & static_cast<uint8_t>(bit)) != 0u;
}

VkShaderStageFlags ToVkStageFlags(ShaderStageMask mask)
{
    VkShaderStageFlags flags = 0u;
    if (HasFlag(mask, ShaderStageMask::Vertex))   flags |= VK_SHADER_STAGE_VERTEX_BIT;
    if (HasFlag(mask, ShaderStageMask::Fragment)) flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
    if (HasFlag(mask, ShaderStageMask::Compute))  flags |= VK_SHADER_STAGE_COMPUTE_BIT;
    if (HasFlag(mask, ShaderStageMask::Geometry)) flags |= VK_SHADER_STAGE_GEOMETRY_BIT;
    if (HasFlag(mask, ShaderStageMask::Hull))     flags |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    if (HasFlag(mask, ShaderStageMask::Domain))   flags |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    return flags;
}

void DestroyDebugMessenger(VkInstance instance, VkDebugUtilsMessengerEXT messenger)
{
    if (!instance || !messenger)
        return;
    auto fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (fn)
        fn(instance, messenger, nullptr);
}

VkFormat FindSupportedDepthFormat(VkPhysicalDevice physicalDevice)
{
    const std::array<VkFormat, 2> candidates{
        VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D32_SFLOAT
    };
    for (VkFormat fmt : candidates)
    {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice, fmt, &props);
        if ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0u)
            return fmt;
    }
    return VK_FORMAT_D32_SFLOAT;
}

} // namespace

VulkanDevice::VulkanDevice() = default;
VulkanDevice::~VulkanDevice() { Shutdown(); }

bool VulkanDevice::CreateInstance(const DeviceDesc& desc)
{
    std::vector<const char*> layers;
    std::vector<const char*> extensions{ VK_KHR_SURFACE_EXTENSION_NAME };

#ifdef _WIN32
    extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#endif

    if (desc.enableDebugLayer)
    {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        m_debugEnabled = true;
    }

    VkApplicationInfo appInfo{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    appInfo.pApplicationName = desc.appName.empty() ? "KROM Vulkan" : desc.appName.c_str();
    appInfo.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    appInfo.pEngineName = "KROM";
    appInfo.engineVersion = VK_MAKE_API_VERSION(0, 0, 7, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ci{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &appInfo;
    ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
    ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();

    if (vkCreateInstance(&ci, nullptr, &m_instance) != VK_SUCCESS)
    {
        Debug::LogError("VulkanDevice: vkCreateInstance failed");
        return false;
    }

    if (m_debugEnabled)
    {
        VkDebugUtilsMessengerCreateInfoEXT dbg{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        dbg.pfnUserCallback = &DebugCallback;

        auto fn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
        if (fn)
            fn(m_instance, &dbg, nullptr, &m_debugMessenger);
    }

    return true;
}

bool VulkanDevice::PickPhysicalDevice(uint32_t adapterIndex)
{
    uint32_t count = 0u;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0u)
    {
        Debug::LogError("VulkanDevice: no Vulkan physical devices found");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

    if (adapterIndex >= count)
        adapterIndex = 0u;

    m_physicalDevice = devices[adapterIndex];
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &m_memoryProperties);

    uint32_t familyCount = 0u;
    vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &familyCount, families.data());

    for (uint32_t i = 0u; i < familyCount; ++i)
    {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u)
        {
            m_graphicsQueueFamily = i;
            return true;
        }
    }

    Debug::LogError("VulkanDevice: no graphics queue family found");
    return false;
}

bool VulkanDevice::CreateLogicalDevice(const DeviceDesc&)
{
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = m_graphicsQueueFamily;
    qci.queueCount = 1u;
    qci.pQueuePriorities = &priority;

    std::vector<const char*> extensions{
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME
    };

    VkPhysicalDeviceDynamicRenderingFeatures dynamicRendering{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES };
    dynamicRendering.dynamicRendering = VK_TRUE;

    VkDeviceCreateInfo ci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    ci.pNext = &dynamicRendering;
    ci.queueCreateInfoCount = 1u;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();

    if (vkCreateDevice(m_physicalDevice, &ci, nullptr, &m_device) != VK_SUCCESS)
    {
        Debug::LogError("VulkanDevice: vkCreateDevice failed");
        return false;
    }

    vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0u, &m_graphicsQueue);
    return true;
}

bool VulkanDevice::CreateGlobalDescriptors()
{
    std::array<VkDescriptorSetLayoutBinding, CBSlots::COUNT + TexSlots::COUNT + SamplerSlots::COUNT> bindings{};
    uint32_t idx = 0u;

    for (uint32_t i = 0u; i < CBSlots::COUNT; ++i)
    {
        bindings[idx].binding = i;
        bindings[idx].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        bindings[idx].descriptorCount = 1u;
        bindings[idx].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
        ++idx;
    }

    for (uint32_t i = 0u; i < TexSlots::COUNT; ++i)
    {
        bindings[idx].binding = 16u + i;
        bindings[idx].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        bindings[idx].descriptorCount = 1u;
        bindings[idx].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        ++idx;
    }

    for (uint32_t i = 0u; i < SamplerSlots::COUNT; ++i)
    {
        bindings[idx].binding = 32u + i;
        bindings[idx].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        bindings[idx].descriptorCount = 1u;
        bindings[idx].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        ++idx;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    layoutInfo.bindingCount = idx;
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_globalSetLayout) != VK_SUCCESS)
    {
        Debug::LogError("VulkanDevice: vkCreateDescriptorSetLayout failed");
        return false;
    }

    VkPipelineLayoutCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pipelineInfo.setLayoutCount = 1u;
    pipelineInfo.pSetLayouts = &m_globalSetLayout;
    if (vkCreatePipelineLayout(m_device, &pipelineInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
    {
        Debug::LogError("VulkanDevice: vkCreatePipelineLayout failed");
        return false;
    }

    return true;
}

void VulkanDevice::DestroyGlobalDescriptors()
{
    if (m_device && m_pipelineLayout)
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    if (m_device && m_globalSetLayout)
        vkDestroyDescriptorSetLayout(m_device, m_globalSetLayout, nullptr);
    m_pipelineLayout = VK_NULL_HANDLE;
    m_globalSetLayout = VK_NULL_HANDLE;
}

bool VulkanDevice::Initialize(const DeviceDesc& desc)
{
    if (m_initialized)
        return true;

    if (!CreateInstance(desc))
        return false;
    if (!PickPhysicalDevice(desc.adapterIndex))
        return false;
    if (!CreateLogicalDevice(desc))
        return false;
    if (!CreateGlobalDescriptors())
        return false;

    m_frameContexts.assign(std::max(1u, m_framesInFlight), {});
    for (FrameContext& frame : m_frameContexts)
    {
        VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(m_device, &fenceInfo, nullptr, &frame.submitFence) != VK_SUCCESS)
        {
            Debug::LogError("VulkanDevice: vkCreateFence failed");
            for (FrameContext& cleanupFrame : m_frameContexts)
            {
                if (cleanupFrame.submitFence)
                    vkDestroyFence(m_device, cleanupFrame.submitFence, nullptr);
            }
            m_frameContexts.clear();
            DestroyGlobalDescriptors();
            vkDestroyDevice(m_device, nullptr);
            DestroyDebugMessenger(m_instance, m_debugMessenger);
            vkDestroyInstance(m_instance, nullptr);
            m_device = VK_NULL_HANDLE;
            m_instance = VK_NULL_HANDLE;
            m_physicalDevice = VK_NULL_HANDLE;
            m_graphicsQueue = VK_NULL_HANDLE;
            m_debugMessenger = VK_NULL_HANDLE;
            return false;
        }
    }
    m_currentFrameSlot = 0u;
    m_completedFenceValue = 0u;

    m_initialized = true;
    Debug::Log("VulkanDevice: initialized");
    return true;
}

void VulkanDevice::Shutdown()
{
    WaitIdle();

    if (!m_initialized)
        return;

    WaitIdle();

    m_resources.pipelines.ForEach([&](VulkanPipelineEntry& p) {
        if (p.pipeline) vkDestroyPipeline(m_device, p.pipeline, nullptr);
    });
    m_resources.shaders.ForEach([&](VulkanShaderEntry& s) {
        if (s.module) vkDestroyShaderModule(m_device, s.module, nullptr);
    });
    m_resources.renderTargets.Clear();
    m_resources.textures.ForEach([&](VulkanTextureEntry& t) {
        if (t.view) vkDestroyImageView(m_device, t.view, nullptr);
        if (t.ownsImage && t.image) vkDestroyImage(m_device, t.image, nullptr);
        if (t.memory) vkFreeMemory(m_device, t.memory, nullptr);
    });
    m_resources.buffers.ForEach([&](VulkanBufferEntry& b) {
        if (b.mapped) vkUnmapMemory(m_device, b.memory);
        if (b.buffer) vkDestroyBuffer(m_device, b.buffer, nullptr);
        if (b.memory) vkFreeMemory(m_device, b.memory, nullptr);
    });
    for (auto& s : m_resources.samplers)
        if (s.sampler) vkDestroySampler(m_device, s.sampler, nullptr);
    m_resources.samplers.clear();

    DestroyGlobalDescriptors();

    for (FrameContext& frame : m_frameContexts)
    {
        if (frame.submitFence)
            vkDestroyFence(m_device, frame.submitFence, nullptr);
    }
    m_frameContexts.clear();

    if (m_device)
        vkDestroyDevice(m_device, nullptr);
    DestroyDebugMessenger(m_instance, m_debugMessenger);
    if (m_instance)
        vkDestroyInstance(m_instance, nullptr);

    m_device = VK_NULL_HANDLE;
    m_instance = VK_NULL_HANDLE;
    m_physicalDevice = VK_NULL_HANDLE;
    m_graphicsQueue = VK_NULL_HANDLE;
    m_debugMessenger = VK_NULL_HANDLE;
    m_activeSwapchain = nullptr;
    m_initialized = false;
}

void VulkanDevice::WaitIdle()
{
    if (m_device)
        vkDeviceWaitIdle(m_device);
}

std::unique_ptr<ISwapchain> VulkanDevice::CreateSwapchain(const SwapchainDesc& desc)
{
    auto sc = std::make_unique<VulkanSwapchain>(*this, desc);
    if (!sc->Initialize())
        return nullptr;
    m_activeSwapchain = sc.get();
    if (m_activeSwapchain)
    {
        const uint32_t desiredFrames = std::max(2u, m_activeSwapchain->GetBufferCount() + 1u);
        if (desiredFrames != m_framesInFlight)
        {
            WaitIdle();
            for (FrameContext& frame : m_frameContexts)
            {
                if (frame.submitFence)
                    vkDestroyFence(m_device, frame.submitFence, nullptr);
            }
            m_frameContexts.clear();
            m_framesInFlight = desiredFrames;
            m_frameContexts.assign(std::max(1u, m_framesInFlight), {});
            for (FrameContext& frame : m_frameContexts)
            {
                VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
                fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
                if (vkCreateFence(m_device, &fenceInfo, nullptr, &frame.submitFence) != VK_SUCCESS)
                    Debug::LogError("VulkanDevice: vkCreateFence failed while resizing frame contexts");
            }
            m_currentFrameSlot = 0u;
            m_completedFenceValue = 0u;
        }
    }
    return sc;
}

uint32_t VulkanDevice::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const
{
    for (uint32_t i = 0u; i < m_memoryProperties.memoryTypeCount; ++i)
    {
        const bool supported = (typeBits & (1u << i)) != 0u;
        const bool match = (m_memoryProperties.memoryTypes[i].propertyFlags & properties) == properties;
        if (supported && match)
            return i;
    }
    return 0u;
}

BufferHandle VulkanDevice::CreateBuffer(const BufferDesc& desc)
{
    VulkanBufferEntry entry{};
    entry.byteSize = desc.byteSize;
    entry.stride = desc.stride;
    entry.usage = ToVkBufferUsage(desc) | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    entry.memoryFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = desc.byteSize;
    bci.usage = entry.usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &bci, nullptr, &entry.buffer) != VK_SUCCESS)
    {
        Debug::LogError("VulkanDevice: vkCreateBuffer failed");
        return BufferHandle::Invalid();
    }

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(m_device, entry.buffer, &req);

    VkMemoryAllocateInfo alloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, entry.memoryFlags);

    if (vkAllocateMemory(m_device, &alloc, nullptr, &entry.memory) != VK_SUCCESS)
    {
        vkDestroyBuffer(m_device, entry.buffer, nullptr);
        Debug::LogError("VulkanDevice: vkAllocateMemory(buffer) failed");
        return BufferHandle::Invalid();
    }

    vkBindBufferMemory(m_device, entry.buffer, entry.memory, 0u);
    return m_resources.buffers.Add(entry);
}

void VulkanDevice::DestroyBuffer(BufferHandle handle)
{
    auto* buffer = m_resources.buffers.Get(handle);
    if (!buffer) return;
    if (buffer->mapped)
        vkUnmapMemory(m_device, buffer->memory);
    if (buffer->buffer)
        vkDestroyBuffer(m_device, buffer->buffer, nullptr);
    if (buffer->memory)
        vkFreeMemory(m_device, buffer->memory, nullptr);
    m_resources.buffers.Remove(handle);
}

void* VulkanDevice::MapBuffer(BufferHandle handle)
{
    auto* buffer = m_resources.buffers.Get(handle);
    if (!buffer) return nullptr;
    if (!buffer->mapped)
        vkMapMemory(m_device, buffer->memory, 0u, buffer->byteSize, 0u, &buffer->mapped);
    return buffer->mapped;
}

void VulkanDevice::UnmapBuffer(BufferHandle handle)
{
    auto* buffer = m_resources.buffers.Get(handle);
    if (!buffer || !buffer->mapped) return;
    vkUnmapMemory(m_device, buffer->memory);
    buffer->mapped = nullptr;
}

bool VulkanDevice::CreateImage(uint32_t width, uint32_t height, uint32_t mipLevels, VkFormat format,
                               VkImageUsageFlags usage, VkImageAspectFlags aspect,
                               VulkanTextureEntry& outEntry)
{
    outEntry.width = width;
    outEntry.height = height;
    outEntry.mipLevels = mipLevels;
    outEntry.format = format;
    outEntry.aspect = aspect;

    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.extent.width = width;
    ici.extent.height = height;
    ici.extent.depth = 1u;
    ici.mipLevels = mipLevels;
    ici.arrayLayers = 1u;
    ici.format = format;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ici.usage = usage;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(m_device, &ici, nullptr, &outEntry.image) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(m_device, outEntry.image, &req);

    VkMemoryAllocateInfo alloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_device, &alloc, nullptr, &outEntry.memory) != VK_SUCCESS)
        return false;

    vkBindImageMemory(m_device, outEntry.image, outEntry.memory, 0u);

    VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = outEntry.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange.aspectMask = aspect;
    vci.subresourceRange.levelCount = mipLevels;
    vci.subresourceRange.layerCount = 1u;

    if (vkCreateImageView(m_device, &vci, nullptr, &outEntry.view) != VK_SUCCESS)
        return false;

    outEntry.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

TextureHandle VulkanDevice::CreateTexture(const TextureDesc& desc)
{
    VulkanTextureEntry entry{};
    const VkFormat format = (desc.format == Format::D24_UNORM_S8_UINT && m_physicalDevice)
        ? FindSupportedDepthFormat(m_physicalDevice)
        : ToVkFormat(desc.format);
    const VkImageAspectFlags aspect = ToVkAspectFlags(desc.format);

    if (format == VK_FORMAT_UNDEFINED)
    {
        Debug::LogError("VulkanDevice: unsupported texture format");
        return TextureHandle::Invalid();
    }

    if (!CreateImage(desc.width, desc.height, desc.mipLevels, format, ToVkImageUsage(desc), aspect, entry))
    {
        Debug::LogError("VulkanDevice: CreateImage failed");
        return TextureHandle::Invalid();
    }

    return m_resources.textures.Add(entry);
}

void VulkanDevice::DestroyTexture(TextureHandle handle)
{
    auto* tex = m_resources.textures.Get(handle);
    if (!tex) return;
    if (tex->view) vkDestroyImageView(m_device, tex->view, nullptr);
    if (tex->ownsImage && tex->image) vkDestroyImage(m_device, tex->image, nullptr);
    if (tex->memory) vkFreeMemory(m_device, tex->memory, nullptr);
    m_resources.textures.Remove(handle);
}

RenderTargetHandle VulkanDevice::CreateRenderTarget(const RenderTargetDesc& desc)
{
    VulkanRenderTargetEntry rt{};
    rt.width = desc.width;
    rt.height = desc.height;
    rt.hasColor = desc.hasColor;
    rt.hasDepth = desc.hasDepth;

    if (desc.hasColor)
    {
        TextureDesc td{};
        td.width = desc.width;
        td.height = desc.height;
        td.mipLevels = 1u;
        td.format = desc.colorFormat;
        td.usage = ResourceUsage::RenderTarget | ResourceUsage::ShaderResource | ResourceUsage::CopySource | ResourceUsage::CopyDest;
        rt.colorHandle = CreateTexture(td);
        rt.hasColor = rt.colorHandle.IsValid();
    }

    if (desc.hasDepth)
    {
        TextureDesc td{};
        td.width = desc.width;
        td.height = desc.height;
        td.mipLevels = 1u;
        td.format = desc.depthFormat;
        td.usage = ResourceUsage::DepthStencil | ResourceUsage::ShaderResource;
        rt.depthHandle = CreateTexture(td);
        rt.hasDepth = rt.depthHandle.IsValid();
    }

    return m_resources.renderTargets.Add(rt);
}

void VulkanDevice::DestroyRenderTarget(RenderTargetHandle handle)
{
    auto* rt = m_resources.renderTargets.Get(handle);
    if (!rt) return;
    if (rt->colorHandle.IsValid()) DestroyTexture(rt->colorHandle);
    if (rt->depthHandle.IsValid()) DestroyTexture(rt->depthHandle);
    m_resources.renderTargets.Remove(handle);
}

TextureHandle VulkanDevice::GetRenderTargetColorTexture(RenderTargetHandle handle) const
{
    auto* rt = m_resources.renderTargets.Get(handle);
    return rt ? rt->colorHandle : TextureHandle::Invalid();
}

TextureHandle VulkanDevice::GetRenderTargetDepthTexture(RenderTargetHandle handle) const
{
    auto* rt = m_resources.renderTargets.Get(handle);
    return rt ? rt->depthHandle : TextureHandle::Invalid();
}

ShaderHandle VulkanDevice::CreateShaderFromSource(const std::string&, ShaderStageMask, const std::string&, const std::string&)
{
    Debug::LogError("VulkanDevice: CreateShaderFromSource requires precompiled SPIR-V");
    return ShaderHandle::Invalid();
}

ShaderHandle VulkanDevice::CreateShaderFromBytecode(const void* data, size_t byteSize, ShaderStageMask stage, const std::string& debugName)
{
    if (!data || byteSize == 0u || (byteSize % sizeof(uint32_t)) != 0u)
        return ShaderHandle::Invalid();

    VulkanShaderEntry entry{};
    entry.stage = stage;
    entry.debugName = debugName;

    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = byteSize;
    ci.pCode = static_cast<const uint32_t*>(data);

    if (vkCreateShaderModule(m_device, &ci, nullptr, &entry.module) != VK_SUCCESS)
    {
        Debug::LogError("VulkanDevice: vkCreateShaderModule failed");
        return ShaderHandle::Invalid();
    }

    return m_resources.shaders.Add(entry);
}

void VulkanDevice::DestroyShader(ShaderHandle handle)
{
    auto* shader = m_resources.shaders.Get(handle);
    if (!shader) return;
    if (shader->module) vkDestroyShaderModule(m_device, shader->module, nullptr);
    m_resources.shaders.Remove(handle);
}

PipelineHandle VulkanDevice::CreatePipeline(const PipelineDesc& desc)
{
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    for (const auto& stageDesc : desc.shaderStages)
    {
        const auto* shader = m_resources.shaders.Get(stageDesc.handle);
        if (!shader || !shader->module)
            continue;
        VkPipelineShaderStageCreateInfo sci{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        sci.stage = ToVkShaderStage(stageDesc.stage);
        sci.module = shader->module;
        sci.pName = stageDesc.entry.empty() ? "main" : stageDesc.entry.c_str();
        stages.push_back(sci);
    }

    if (stages.empty())
    {
        Debug::LogError("VulkanDevice: pipeline creation failed - no shader stages");
        return PipelineHandle::Invalid();
    }

    std::vector<VkVertexInputBindingDescription> bindings;
    bindings.reserve(desc.vertexLayout.bindings.size());
    for (const auto& b : desc.vertexLayout.bindings)
    {
        VkVertexInputBindingDescription vkb{};
        vkb.binding = b.binding;
        vkb.stride = b.stride;
        vkb.inputRate = (b.inputRate == VertexInputRate::PerInstance) ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
        bindings.push_back(vkb);
    }

    std::vector<VkVertexInputAttributeDescription> attrs;
    attrs.reserve(desc.vertexLayout.attributes.size());
    for (uint32_t i = 0u; i < desc.vertexLayout.attributes.size(); ++i)
    {
        const auto& a = desc.vertexLayout.attributes[i];
        VkVertexInputAttributeDescription vka{};
        vka.location = i;
        vka.binding = a.binding;
        vka.format = ToVkFormat(a.format);
        vka.offset = a.offset;
        attrs.push_back(vka);
    }

    VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
    vertexInput.pVertexBindingDescriptions = bindings.empty() ? nullptr : bindings.data();
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vertexInput.pVertexAttributeDescriptions = attrs.empty() ? nullptr : attrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = ToVkTopology(desc.topology);

    VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewportState.viewportCount = 1u;
    viewportState.scissorCount = 1u;

    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = (desc.rasterizer.fillMode == FillMode::Wireframe) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = ToVkCullMode(desc.rasterizer.cullMode);
    rs.frontFace = ToVkFrontFace(desc.rasterizer.frontFace);
    rs.lineWidth = 1.0f;
    rs.depthClampEnable = desc.rasterizer.depthClip ? VK_FALSE : VK_TRUE;

    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = desc.depthStencil.depthEnable ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = desc.depthStencil.depthWrite ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = ToVkCompareOp(desc.depthStencil.depthFunc);
    ds.stencilTestEnable = desc.depthStencil.stencilEnable ? VK_TRUE : VK_FALSE;

    VkPipelineColorBlendAttachmentState attachment{};
    const auto& blend = desc.blendStates[0];
    attachment.blendEnable = blend.blendEnable ? VK_TRUE : VK_FALSE;
    attachment.srcColorBlendFactor = ToVkBlendFactor(blend.srcBlend);
    attachment.dstColorBlendFactor = ToVkBlendFactor(blend.dstBlend);
    attachment.colorBlendOp = ToVkBlendOp(blend.blendOp);
    attachment.srcAlphaBlendFactor = ToVkBlendFactor(blend.srcBlendAlpha);
    attachment.dstAlphaBlendFactor = ToVkBlendFactor(blend.dstBlendAlpha);
    attachment.alphaBlendOp = ToVkBlendOp(blend.blendOpAlpha);
    attachment.colorWriteMask = 0u;
    if ((blend.writeMask & 0x1u) != 0u) attachment.colorWriteMask |= VK_COLOR_COMPONENT_R_BIT;
    if ((blend.writeMask & 0x2u) != 0u) attachment.colorWriteMask |= VK_COLOR_COMPONENT_G_BIT;
    if ((blend.writeMask & 0x4u) != 0u) attachment.colorWriteMask |= VK_COLOR_COMPONENT_B_BIT;
    if ((blend.writeMask & 0x8u) != 0u) attachment.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blendState{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    if (desc.colorFormat != Format::Unknown)
    {
        blendState.attachmentCount = 1u;
        blendState.pAttachments = &attachment;
    }

    const std::array<VkDynamicState, 2> dynamicStates{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkFormat colorFormat = ToVkFormat(desc.colorFormat);
    VkFormat depthFormat = ToVkFormat(desc.depthFormat);
    if (desc.depthFormat == Format::D24_UNORM_S8_UINT && m_physicalDevice)
        depthFormat = FindSupportedDepthFormat(m_physicalDevice);

    VkPipelineRenderingCreateInfo rendering{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    if (desc.colorFormat != Format::Unknown)
    {
        rendering.colorAttachmentCount = 1u;
        rendering.pColorAttachmentFormats = &colorFormat;
    }
    rendering.depthAttachmentFormat = desc.depthFormat != Format::Unknown ? depthFormat : VK_FORMAT_UNDEFINED;
    rendering.stencilAttachmentFormat = (desc.depthFormat == Format::D24_UNORM_S8_UINT) ? depthFormat : VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo pci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pci.pNext = &rendering;
    pci.stageCount = static_cast<uint32_t>(stages.size());
    pci.pStages = stages.data();
    pci.pVertexInputState = &vertexInput;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &viewportState;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &blendState;
    pci.pDynamicState = &dynamicState;
    pci.layout = m_pipelineLayout;

    VulkanPipelineEntry entry{};
    entry.topology = desc.topology;
    entry.hasColor = desc.colorFormat != Format::Unknown;
    entry.hasDepth = desc.depthFormat != Format::Unknown;

    if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1u, &pci, nullptr, &entry.pipeline) != VK_SUCCESS)
    {
        Debug::LogError("VulkanDevice: vkCreateGraphicsPipelines failed");
        return PipelineHandle::Invalid();
    }

    return m_resources.pipelines.Add(entry);
}

void VulkanDevice::DestroyPipeline(PipelineHandle handle)
{
    auto* pipeline = m_resources.pipelines.Get(handle);
    if (!pipeline) return;
    if (pipeline->pipeline) vkDestroyPipeline(m_device, pipeline->pipeline, nullptr);
    m_resources.pipelines.Remove(handle);
}

uint32_t VulkanDevice::CreateSampler(const SamplerDesc& desc)
{
    VulkanSamplerEntry entry{};
    VkSamplerCreateInfo ci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    ci.magFilter = ToVkFilter(desc.magFilter);
    ci.minFilter = ToVkFilter(desc.minFilter);
    ci.mipmapMode = ToVkMipmapMode(desc.mipFilter);
    ci.addressModeU = ToVkAddressMode(desc.addressU);
    ci.addressModeV = ToVkAddressMode(desc.addressV);
    ci.addressModeW = ToVkAddressMode(desc.addressW);
    ci.mipLodBias = desc.mipLodBias;
    ci.minLod = desc.minLod;
    ci.maxLod = desc.maxLod;
    ci.maxAnisotropy = static_cast<float>(desc.maxAniso);
    ci.anisotropyEnable = desc.maxAniso > 1u ? VK_TRUE : VK_FALSE;
    ci.compareEnable = desc.compareFunc != CompareFunc::Never ? VK_TRUE : VK_FALSE;
    ci.compareOp = ToVkCompareOp(desc.compareFunc);
    ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;

    if (vkCreateSampler(m_device, &ci, nullptr, &entry.sampler) != VK_SUCCESS)
        return 0u;

    m_resources.samplers.push_back(entry);
    return static_cast<uint32_t>(m_resources.samplers.size() - 1u);
}

std::unique_ptr<ICommandList> VulkanDevice::CreateCommandList(QueueType)
{
    return std::make_unique<VulkanCommandList>(*this, m_resources);
}

std::unique_ptr<IFence> VulkanDevice::CreateFence(uint64_t initialValue)
{
    return std::make_unique<VulkanFence>(*this, initialValue);
}

void VulkanDevice::ImmediateSubmit(const std::function<void(VkCommandBuffer)>& fn)
{
    VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.queueFamilyIndex = m_graphicsQueueFamily;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(m_device, &poolInfo, nullptr, &pool);

    VkCommandBufferAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocInfo.commandPool = pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1u;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_device, &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);
    fn(cmd);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(m_graphicsQueue, 1u, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);

    vkFreeCommandBuffers(m_device, pool, 1u, &cmd);
    vkDestroyCommandPool(m_device, pool, nullptr);
}

void VulkanDevice::UploadBufferData(BufferHandle handle, const void* data, size_t byteSize, size_t dstOffset)
{
    auto* buffer = m_resources.buffers.Get(handle);
    if (!buffer || !data || byteSize == 0u) return;
    void* mapped = MapBuffer(handle);
    if (!mapped) return;
    std::memcpy(static_cast<uint8_t*>(mapped) + dstOffset, data, byteSize);
}

void VulkanDevice::UploadTextureData(TextureHandle handle, const void* data, size_t byteSize, uint32_t, uint32_t)
{
    auto* tex = m_resources.textures.Get(handle);
    if (!tex || !data || byteSize == 0u)
        return;

    BufferDesc stagingDesc{};
    stagingDesc.byteSize = byteSize;
    stagingDesc.type = BufferType::Raw;
    stagingDesc.usage = ResourceUsage::CopySource;
    stagingDesc.access = MemoryAccess::CpuWrite;
    stagingDesc.debugName = "VulkanTextureUploadStaging";

    const BufferHandle staging = CreateBuffer(stagingDesc);
    UploadBufferData(staging, data, byteSize, 0u);
    auto* stagingEntry = m_resources.buffers.Get(staging);

    ImmediateSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier toCopy{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        toCopy.oldLayout = tex->layout;
        toCopy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toCopy.srcAccessMask = ToVkAccessFlags(ResourceState::Common);
        toCopy.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toCopy.image = tex->image;
        toCopy.subresourceRange.aspectMask = tex->aspect;
        toCopy.subresourceRange.levelCount = tex->mipLevels;
        toCopy.subresourceRange.layerCount = 1u;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &toCopy);

        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = tex->aspect;
        copy.imageSubresource.layerCount = 1u;
        copy.imageExtent.width = tex->width;
        copy.imageExtent.height = tex->height;
        copy.imageExtent.depth = 1u;
        vkCmdCopyBufferToImage(cmd, stagingEntry->buffer, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &copy);

        VkImageMemoryBarrier toRead{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toRead.image = tex->image;
        toRead.subresourceRange.aspectMask = tex->aspect;
        toRead.subresourceRange.levelCount = tex->mipLevels;
        toRead.subresourceRange.layerCount = 1u;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &toRead);
    });

    tex->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    DestroyBuffer(staging);
}

VkFence VulkanDevice::GetCurrentFrameFence() const noexcept
{
    if (m_frameContexts.empty())
        return VK_NULL_HANDLE;
    return m_frameContexts[m_currentFrameSlot % static_cast<uint32_t>(m_frameContexts.size())].submitFence;
}

void VulkanDevice::MarkCurrentFrameSubmitted(uint64_t fenceValue) noexcept
{
    if (m_frameContexts.empty())
        return;
    FrameContext& frame = m_frameContexts[m_currentFrameSlot % static_cast<uint32_t>(m_frameContexts.size())];
    frame.submittedFenceValue = fenceValue;
    frame.inFlight = fenceValue != 0u;
}

void VulkanDevice::RefreshCompletedFrameFences() noexcept
{
    if (!m_device)
        return;

    for (FrameContext& frame : m_frameContexts)
    {
        if (!frame.inFlight || frame.submitFence == VK_NULL_HANDLE)
            continue;
        if (vkGetFenceStatus(m_device, frame.submitFence) != VK_SUCCESS)
            continue;

        frame.inFlight = false;
        frame.completedFenceValue = frame.submittedFenceValue;
        if (frame.completedFenceValue > m_completedFenceValue)
            m_completedFenceValue = frame.completedFenceValue;
    }
}

void VulkanDevice::WaitForFenceValue(uint64_t value, uint64_t timeoutNs)
{
    if (!m_device || value == 0u)
        return;

    RefreshCompletedFrameFences();
    if (m_completedFenceValue >= value)
        return;

    for (FrameContext& frame : m_frameContexts)
    {
        if (!frame.inFlight || frame.submitFence == VK_NULL_HANDLE)
            continue;
        if (frame.submittedFenceValue < value)
            continue;

        vkWaitForFences(m_device, 1u, &frame.submitFence, VK_TRUE, timeoutNs);
        RefreshCompletedFrameFences();
        return;
    }

    RefreshCompletedFrameFences();
}

void VulkanDevice::BeginFrame()
{
    ++m_frameIndex;
    m_currentFrameSlot = (m_currentFrameSlot + 1u) % std::max(1u, m_framesInFlight);

    if (!m_frameContexts.empty())
    {
        FrameContext& frame = m_frameContexts[m_currentFrameSlot % static_cast<uint32_t>(m_frameContexts.size())];
        if (frame.submitFence != VK_NULL_HANDLE)
        {
            vkWaitForFences(m_device, 1u, &frame.submitFence, VK_TRUE, UINT64_MAX);
            if (frame.inFlight)
            {
                frame.inFlight = false;
                frame.completedFenceValue = frame.submittedFenceValue;
                if (frame.completedFenceValue > m_completedFenceValue)
                    m_completedFenceValue = frame.completedFenceValue;
            }
            vkResetFences(m_device, 1u, &frame.submitFence);
        }
    }

    if (m_activeSwapchain)
        m_activeSwapchain->AcquireNextImage();
}

void VulkanDevice::EndFrame()
{
    RefreshCompletedFrameFences();
}

bool VulkanDevice::SupportsFeature(const char* feature) const
{
    if (!feature) return false;
    const std::string f(feature);
    return f == "dynamic_rendering" || f == "spirv";
}

} // namespace engine::renderer::vulkan
