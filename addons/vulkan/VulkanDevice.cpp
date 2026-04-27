#include "VulkanDevice.hpp"
#include "core/Debug.hpp"
#include "renderer/RenderWorld.hpp"
#include "renderer/TextureFormatUtils.hpp"
#include <algorithm>
#include <array>
#include <chrono>
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

        VkDescriptorType ToVkDescriptorType(DescriptorType type, bool usesDynamicOffset = false)
        {
            switch (type)
            {
            case DescriptorType::ConstantBuffer: return usesDynamicOffset ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                                                         : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            case DescriptorType::ShaderResource: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            case DescriptorType::UnorderedAccess: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            case DescriptorType::Sampler:         return VK_DESCRIPTOR_TYPE_SAMPLER;
            default: return VK_DESCRIPTOR_TYPE_MAX_ENUM;
            }
        }

        constexpr size_t QueueIndex(QueueType queue) noexcept
        {
            switch (queue)
            {
            case QueueType::Graphics: return 0u;
            case QueueType::Compute: return 1u;
            case QueueType::Transfer: return 2u;
            default: return 0u;
            }
        }

        const char* QueueName(QueueType queue) noexcept
        {
            switch (queue)
            {
            case QueueType::Graphics: return "Graphics";
            case QueueType::Compute: return "Compute";
            case QueueType::Transfer: return "Transfer";
            default: return "Unknown";
            }
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

        VkImageAspectFlags AspectMaskForVkFormat(VkFormat format)
        {
            switch (format)
            {
            case VK_FORMAT_D16_UNORM:
            case VK_FORMAT_X8_D24_UNORM_PACK32:
            case VK_FORMAT_D32_SFLOAT:
                return VK_IMAGE_ASPECT_DEPTH_BIT;
            case VK_FORMAT_S8_UINT:
                return VK_IMAGE_ASPECT_STENCIL_BIT;
            case VK_FORMAT_D16_UNORM_S8_UINT:
            case VK_FORMAT_D24_UNORM_S8_UINT:
            case VK_FORMAT_D32_SFLOAT_S8_UINT:
                return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            default:
                return VK_IMAGE_ASPECT_COLOR_BIT;
            }
        }

        struct DepthFormatSupportInfo
        {
            VkFormatFeatureFlags optimalFeatures = 0u;
            bool depthAttachment = false;
            bool sampled = false;
            bool linearFilter = false;
            bool combinedUsage = false;
        };

        DepthFormatSupportInfo QueryDepthFormatSupport(VkPhysicalDevice physicalDevice, VkFormat format, bool requireLinearFiltering = false)
        {
            DepthFormatSupportInfo info{};

            VkFormatProperties props{};
            vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &props);

            info.optimalFeatures = props.optimalTilingFeatures;
            info.depthAttachment = (info.optimalFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0u;
            info.sampled = (info.optimalFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0u;
            info.linearFilter = !requireLinearFiltering || ((info.optimalFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0u);

            VkImageFormatProperties imageProps{};
            const VkResult imageFormatResult = vkGetPhysicalDeviceImageFormatProperties(
                physicalDevice,
                format,
                VK_IMAGE_TYPE_2D,
                VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                0u,
                &imageProps);
            info.combinedUsage = (imageFormatResult == VK_SUCCESS);
            return info;
        }

        bool SupportsDepthAttachmentSampling(VkPhysicalDevice physicalDevice, VkFormat format, bool requireLinearFiltering = false)
        {
            const DepthFormatSupportInfo info = QueryDepthFormatSupport(physicalDevice, format, requireLinearFiltering);
            return info.depthAttachment && info.sampled && info.linearFilter && info.combinedUsage;
        }

        void LogDepthFormatSupport(VkPhysicalDevice physicalDevice, VkFormat format, const char* reason, bool requireLinearFiltering = false)
        {
            const DepthFormatSupportInfo info = QueryDepthFormatSupport(physicalDevice, format, requireLinearFiltering);

            if (!info.depthAttachment || !info.sampled || !info.linearFilter || !info.combinedUsage)
            {
                Debug::LogWarning(
                    "VulkanDepthFormat: format=%d is missing required shadow-map capabilities (attachment=%d sampled=%d linear=%d combinedUsage=%d)",
                    static_cast<int>(format),
                    info.depthAttachment ? 1 : 0,
                    info.sampled ? 1 : 0,
                    info.linearFilter ? 1 : 0,
                    info.combinedUsage ? 1 : 0);
            }
        }

        VkFormat FindSupportedDepthFormat(VkPhysicalDevice physicalDevice)
        {
            const std::array<VkFormat, 2> candidates{
                VK_FORMAT_D24_UNORM_S8_UINT,
                VK_FORMAT_D32_SFLOAT
            };
            for (VkFormat fmt : candidates)
            {
                if (SupportsDepthAttachmentSampling(physicalDevice, fmt))
                    return fmt;
            }
            return VK_FORMAT_D32_SFLOAT;
        }

    } // namespace

    namespace {

        VkAccessFlags AccessMaskForImageLayout(VkImageLayout layout) noexcept
        {
            switch (layout)
            {
            case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_ACCESS_TRANSFER_WRITE_BIT;
            case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_ACCESS_TRANSFER_READ_BIT;
            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return VK_ACCESS_SHADER_READ_BIT;
            case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return VK_ACCESS_SHADER_READ_BIT;
            case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return 0u;
            case VK_IMAGE_LAYOUT_UNDEFINED:
            default: return 0u;
            }
        }

        VkPipelineStageFlags StageMaskForImageLayout(VkImageLayout layout) noexcept
        {
            switch (layout)
            {
            case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
                return VK_PIPELINE_STAGE_TRANSFER_BIT;
            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                return VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
                return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
                return VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            case VK_IMAGE_LAYOUT_UNDEFINED:
            default:
                return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            }
        }

    } // namespace

    VulkanDevice::VulkanDevice() = default;
    VulkanDevice::~VulkanDevice() { Shutdown(); }

    // TODO: GPU-seitige Frame-Zeitmessung via vkCmdWriteTimestamp (Vulkan) und
    // ID3D12QueryHeap (DX12) implementieren, sobald DX12-Backend vorhanden ist.
    // Beide Backends sollen dann BackendFrameDiagnostics::gpuFrameMs befuellen,
    // sodass frame= in den Stats echte GPU-Zeit zeigt statt CPU-Wartezeit.
    void VulkanDevice::ResetBackendFrameDiagnostics() noexcept
    {
        std::lock_guard<std::mutex> lock(m_backendDiagnosticsMutex);
        m_backendDiagnostics = {};
    }

    void VulkanDevice::AddBackendAcquireTime(float ms) noexcept
    {
        std::lock_guard<std::mutex> lock(m_backendDiagnosticsMutex);
        m_backendDiagnostics.acquireMs += ms;
    }

    void VulkanDevice::AddBackendQueueSubmitTime(float ms) noexcept
    {
        std::lock_guard<std::mutex> lock(m_backendDiagnosticsMutex);
        m_backendDiagnostics.queueSubmitMs += ms;
    }

    void VulkanDevice::SetBackendPresentTime(float ms) noexcept
    {
        std::lock_guard<std::mutex> lock(m_backendDiagnosticsMutex);
        m_backendDiagnostics.presentMs = ms;
    }

    void VulkanDevice::AddBackendDescriptorRematerialization() noexcept
    {
        std::lock_guard<std::mutex> lock(m_backendDiagnosticsMutex);
        ++m_backendDiagnostics.descriptorRematerializations;
    }

    void VulkanDevice::AddBackendDescriptorSetAllocation() noexcept
    {
        std::lock_guard<std::mutex> lock(m_backendDiagnosticsMutex);
        ++m_backendDiagnostics.descriptorSetAllocations;
    }

    void VulkanDevice::AddBackendDescriptorSetUpdate() noexcept
    {
        std::lock_guard<std::mutex> lock(m_backendDiagnosticsMutex);
        ++m_backendDiagnostics.descriptorSetUpdates;
    }

    void VulkanDevice::AddBackendDescriptorSetBind() noexcept
    {
        std::lock_guard<std::mutex> lock(m_backendDiagnosticsMutex);
        ++m_backendDiagnostics.descriptorSetBinds;
    }

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
        VkPhysicalDeviceFeatures deviceFeatures{};
        vkGetPhysicalDeviceFeatures(m_physicalDevice, &deviceFeatures);
        m_samplerAnisotropySupported = deviceFeatures.samplerAnisotropy == VK_TRUE;

        uint32_t familyCount = 0u;
        vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &familyCount, families.data());

        bool foundGraphics = false;
        m_graphicsQueueFamily = 0u;
        m_computeQueueFamily = UINT32_MAX;
        m_transferQueueFamily = UINT32_MAX;

        for (uint32_t i = 0u; i < familyCount; ++i)
        {
            const VkQueueFlags flags = families[i].queueFlags;
            if (!foundGraphics && (flags & VK_QUEUE_GRAPHICS_BIT) != 0u)
            {
                m_graphicsQueueFamily = i;
                foundGraphics = true;
            }
            if ((flags & VK_QUEUE_COMPUTE_BIT) != 0u && (flags & VK_QUEUE_GRAPHICS_BIT) == 0u && m_computeQueueFamily == UINT32_MAX)
                m_computeQueueFamily = i;
            if ((flags & VK_QUEUE_TRANSFER_BIT) != 0u && (flags & VK_QUEUE_GRAPHICS_BIT) == 0u && (flags & VK_QUEUE_COMPUTE_BIT) == 0u && m_transferQueueFamily == UINT32_MAX)
                m_transferQueueFamily = i;
        }

        if (!foundGraphics)
        {
            Debug::LogError("VulkanDevice: no graphics queue family found");
            return false;
        }

        if (m_computeQueueFamily == UINT32_MAX)
            m_computeQueueFamily = m_graphicsQueueFamily;
        if (m_transferQueueFamily == UINT32_MAX)
        {
            for (uint32_t i = 0u; i < familyCount; ++i)
            {
                const VkQueueFlags flags = families[i].queueFlags;
                if ((flags & VK_QUEUE_TRANSFER_BIT) != 0u && i != m_graphicsQueueFamily)
                {
                    m_transferQueueFamily = i;
                    break;
                }
            }
        }
        if (m_transferQueueFamily == UINT32_MAX)
            m_transferQueueFamily = m_graphicsQueueFamily;

        return true;
    }

    bool VulkanDevice::CreateLogicalDevice(const DeviceDesc&)
    {
        const float priority = 1.0f;
        std::vector<uint32_t> uniqueFamilies;
        uniqueFamilies.push_back(m_graphicsQueueFamily);
        if (m_computeQueueFamily != m_graphicsQueueFamily)
            uniqueFamilies.push_back(m_computeQueueFamily);
        if (m_transferQueueFamily != m_graphicsQueueFamily && m_transferQueueFamily != m_computeQueueFamily)
            uniqueFamilies.push_back(m_transferQueueFamily);

        std::vector<VkDeviceQueueCreateInfo> queueInfos;
        queueInfos.reserve(uniqueFamilies.size());
        for (uint32_t familyIndex : uniqueFamilies)
        {
            VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
            qci.queueFamilyIndex = familyIndex;
            qci.queueCount = 1u;
            qci.pQueuePriorities = &priority;
            queueInfos.push_back(qci);
        }

        std::vector<const char*> extensions{
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
            // Ermöglicht VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT: UAV-Slots im
            // globalen Descriptor-Set-Layout müssen dann nicht beschrieben sein,
            // solange kein aktiver Shader auf sie zugreift.
            VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME
        };

        VkPhysicalDeviceDescriptorIndexingFeaturesEXT descriptorIndexing{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT
        };
        descriptorIndexing.descriptorBindingPartiallyBound = VK_TRUE;

        VkPhysicalDeviceDynamicRenderingFeatures dynamicRendering{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES };
        dynamicRendering.dynamicRendering = VK_TRUE;
        dynamicRendering.pNext = &descriptorIndexing;

        VkPhysicalDeviceFeatures enabledFeatures{};
        enabledFeatures.samplerAnisotropy = m_samplerAnisotropySupported ? VK_TRUE : VK_FALSE;

        VkDeviceCreateInfo ci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        ci.pNext = &dynamicRendering;
        ci.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
        ci.pQueueCreateInfos = queueInfos.data();
        ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        ci.ppEnabledExtensionNames = extensions.data();
        ci.pEnabledFeatures = &enabledFeatures;

        if (vkCreateDevice(m_physicalDevice, &ci, nullptr, &m_device) != VK_SUCCESS)
        {
            Debug::LogError("VulkanDevice: vkCreateDevice failed");
            return false;
        }

        vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0u, &m_graphicsQueue);
        vkGetDeviceQueue(m_device, m_computeQueueFamily, 0u, &m_computeQueue);
        vkGetDeviceQueue(m_device, m_transferQueueFamily, 0u, &m_transferQueue);
        return true;
    }

    bool VulkanDevice::CreateGlobalDescriptors()
    {
        std::string descriptorLayoutError;
        if (!ValidateDescriptorRuntimeLayout(m_descriptorRuntimeLayout, &descriptorLayoutError))
        {
            Debug::LogError("VulkanDevice: invalid engine descriptor runtime layout: %s", descriptorLayoutError.c_str());
            return false;
        }

        std::vector<VkDescriptorSetLayoutBinding> bindings;
        bindings.reserve(m_bindingLayout.CountDescriptors(BindingHeapKind::Resource, DescriptorType::ConstantBuffer) +
            m_bindingLayout.CountDescriptors(BindingHeapKind::Resource, DescriptorType::ShaderResource) +
            m_bindingLayout.CountDescriptors(BindingHeapKind::Sampler, DescriptorType::Sampler) +
            m_bindingLayout.CountDescriptors(BindingHeapKind::Resource, DescriptorType::UnorderedAccess));

        for (uint32_t rangeIndex = 0u; rangeIndex < m_bindingLayout.rangeCount; ++rangeIndex)
        {
            const BindingRangeDesc& range = m_bindingLayout.ranges[rangeIndex];
            // Hinweis: UnorderedAccess (STORAGE_IMAGE) wird bewusst eingeschlossen –
            // FlushDescriptors beschreibt alle UAV-Slots mit einem Fallback-Image,
            // damit der VVL keine uninitialisierten Bindungen im Pool sieht.

            const VkDescriptorType descriptorType = ToVkDescriptorType(range.type, range.usesDynamicOffset);
            if (descriptorType == VK_DESCRIPTOR_TYPE_MAX_ENUM)
                continue;

            const VkShaderStageFlags stageFlags = ToVkStageFlags(range.visibility);
            for (uint32_t i = 0u; i < range.descriptorCount; ++i)
            {
                VkDescriptorSetLayoutBinding binding{};
                binding.binding = range.registerBase + i;
                binding.descriptorType = descriptorType;
                binding.descriptorCount = 1u;
                binding.stageFlags = stageFlags;
                bindings.push_back(binding);
            }
        }

        // VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT für alle STORAGE_IMAGE-Slots
        // (UAV-Bindungen): UAV-Slots müssen nur dann gültig sein, wenn ein Shader
        // sie tatsächlich zugreift. Da aktuell kein Graphics-Shader UAVs nutzt,
        // entfällt jede Pflicht, die Slots vor jedem Draw zu beschreiben.
        std::vector<VkDescriptorBindingFlags> bindingFlags(bindings.size(), 0u);
        for (uint32_t j = 0u; j < static_cast<uint32_t>(bindings.size()); ++j)
        {
            if (bindings[j].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
                bindingFlags[j] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT_EXT;
        }
        VkDescriptorSetLayoutBindingFlagsCreateInfoEXT flagsInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT
        };
        flagsInfo.bindingCount = static_cast<uint32_t>(bindingFlags.size());
        flagsInfo.pBindingFlags = bindingFlags.data();

        VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layoutInfo.pNext = &flagsInfo;
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_globalSetLayout) != VK_SUCCESS)
        {
            Debug::LogError("VulkanDevice: vkCreateDescriptorSetLayout failed");
            return false;
        }

        VkPipelineLayoutCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pipelineInfo.setLayoutCount = 1u;
        pipelineInfo.pSetLayouts = &m_globalSetLayout;
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.offset = 0u;
        pushRange.size = static_cast<uint32_t>(sizeof(VulkanPerObjectPushConstants));
        pipelineInfo.pushConstantRangeCount = 1u;
        pipelineInfo.pPushConstantRanges = &pushRange;
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

        if (!InitializeFrameContexts(m_framesInFlight))
        {
            DestroyGlobalDescriptors();
            vkDestroyDevice(m_device, nullptr);
            DestroyDebugMessenger(m_instance, m_debugMessenger);
            vkDestroyInstance(m_instance, nullptr);
            m_device = VK_NULL_HANDLE;
            m_instance = VK_NULL_HANDLE;
            m_physicalDevice = VK_NULL_HANDLE;
            m_graphicsQueue = VK_NULL_HANDLE;
            m_computeQueue = VK_NULL_HANDLE;
            m_transferQueue = VK_NULL_HANDLE;
            m_debugMessenger = VK_NULL_HANDLE;
            return false;
        }

        m_initialized = true;
        Debug::Log("VulkanDevice: initialized");
        return true;
    }


    bool VulkanDevice::InitializeFrameContexts(uint32_t framesInFlight)
    {
        DestroyFrameContexts();

        m_framesInFlight = std::max(1u, framesInFlight);
        m_frameContexts.assign(m_framesInFlight, {});

        VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (FrameContext& frame : m_frameContexts)
        {
            for (QueueType queue : { QueueType::Graphics, QueueType::Compute, QueueType::Transfer })
            {
                QueueFrameContext& queueFrame = frame.queues[QueueIndex(queue)];
                if (vkCreateFence(m_device, &fenceInfo, nullptr, &queueFrame.submitFence) == VK_SUCCESS)
                    continue;

                Debug::LogError("VulkanDevice: vkCreateFence failed for %s queue", QueueName(queue));
                DestroyFrameContexts();
                return false;
            }
        }

        m_currentFrameSlot = 0u;
        m_completedFenceValue = 0u;
        m_lastSubmittedFenceValue = 0u;
        m_nextExternalFenceValue = 0u;
        m_lastSubmittedQueueFenceValues = {};
        m_completedQueueFenceValues = {};
        m_externalFenceTimeline.clear();
        return true;
    }

    void VulkanDevice::DestroyFrameContexts() noexcept
    {
        if (!m_device)
        {
            m_frameContexts.clear();
            return;
        }

        for (FrameContext& frame : m_frameContexts)
        {
            for (QueueFrameContext& queueFrame : frame.queues)
            {
                if (queueFrame.submitFence != VK_NULL_HANDLE)
                    vkDestroyFence(m_device, queueFrame.submitFence, nullptr);
                queueFrame = {};
            }
        }
        m_frameContexts.clear();
    }

    void VulkanDevice::EnsureFrameContextsForSwapchainImages(uint32_t swapchainImageCount)
    {
        const uint32_t desiredFrames = std::max(2u, swapchainImageCount + 1u);
        if (desiredFrames == m_framesInFlight && !m_frameContexts.empty())
            return;

        WaitIdle();
        if (!InitializeFrameContexts(desiredFrames))
            Debug::LogError("VulkanDevice: failed to rebuild frame contexts for %u swapchain images", swapchainImageCount);
    }


    void VulkanDevice::ProcessPendingBufferDestroys() noexcept
    {
        if (!m_device || m_pendingBufferDestroys.empty())
            return;

        const uint64_t completedFenceValue = m_completedFenceValue;
        auto it = std::remove_if(m_pendingBufferDestroys.begin(), m_pendingBufferDestroys.end(),
            [&](PendingBufferDestroy& pending)
            {
                if (pending.retireAfterFence > completedFenceValue)
                    return false;

                if (pending.entry.mapped)
                    vkUnmapMemory(m_device, pending.entry.memory);
                if (pending.entry.buffer)
                    vkDestroyBuffer(m_device, pending.entry.buffer, nullptr);
                if (pending.entry.memory)
                    vkFreeMemory(m_device, pending.entry.memory, nullptr);
                return true;
            });
        m_pendingBufferDestroys.erase(it, m_pendingBufferDestroys.end());
    }

    void VulkanDevice::ProcessPendingTextureDestroys() noexcept
    {
        if (!m_device || m_pendingTextureDestroys.empty())
            return;

        const uint64_t completedFenceValue = m_completedFenceValue;
        auto it = std::remove_if(m_pendingTextureDestroys.begin(), m_pendingTextureDestroys.end(),
            [&](PendingTextureDestroy& pending)
            {
                if (pending.retireAfterFence > completedFenceValue)
                    return false;

                if (pending.entry.sampleView && pending.entry.sampleView != pending.entry.view)
                    vkDestroyImageView(m_device, pending.entry.sampleView, nullptr);
                if (pending.entry.view)
                    vkDestroyImageView(m_device, pending.entry.view, nullptr);
                if (pending.entry.ownsImage && pending.entry.image)
                    vkDestroyImage(m_device, pending.entry.image, nullptr);
                if (pending.entry.memory)
                    vkFreeMemory(m_device, pending.entry.memory, nullptr);
                return true;
            });
        m_pendingTextureDestroys.erase(it, m_pendingTextureDestroys.end());
    }

    void VulkanDevice::ProcessPendingObjectDestroys() noexcept
    {
        if (!m_device || m_pendingObjectDestroys.empty())
            return;

        const uint64_t completedFenceValue = m_completedFenceValue;
        auto it = std::remove_if(m_pendingObjectDestroys.begin(), m_pendingObjectDestroys.end(),
            [&](PendingObjectDestroy& pending)
            {
                if (pending.retireAfterFence > completedFenceValue)
                    return false;

                switch (pending.kind)
                {
                case PendingObjectKind::ShaderModule:
                    vkDestroyShaderModule(m_device, reinterpret_cast<VkShaderModule>(pending.handle), nullptr);
                    break;
                case PendingObjectKind::Pipeline:
                    vkDestroyPipeline(m_device, reinterpret_cast<VkPipeline>(pending.handle), nullptr);
                    break;
                case PendingObjectKind::Sampler:
                    vkDestroySampler(m_device, reinterpret_cast<VkSampler>(pending.handle), nullptr);
                    break;
                case PendingObjectKind::ImageView:
                    vkDestroyImageView(m_device, reinterpret_cast<VkImageView>(pending.handle), nullptr);
                    break;
                case PendingObjectKind::Semaphore:
                    vkDestroySemaphore(m_device, reinterpret_cast<VkSemaphore>(pending.handle), nullptr);
                    break;
                case PendingObjectKind::Swapchain:
                    vkDestroySwapchainKHR(m_device, reinterpret_cast<VkSwapchainKHR>(pending.handle), nullptr);
                    break;
                case PendingObjectKind::DescriptorPool:
                    vkDestroyDescriptorPool(m_device, reinterpret_cast<VkDescriptorPool>(pending.handle), nullptr);
                    break;
                case PendingObjectKind::CommandPool:
                    vkDestroyCommandPool(m_device, reinterpret_cast<VkCommandPool>(pending.handle), nullptr);
                    break;
                }
                return true;
            });
        m_pendingObjectDestroys.erase(it, m_pendingObjectDestroys.end());
    }

    uint64_t VulkanDevice::GetSafeRetireFenceValue() const noexcept
    {
        return std::max(m_completedFenceValue, m_lastSubmittedFenceValue);
    }

    void VulkanDevice::DeferDestroyShaderModule(VkShaderModule module, uint64_t retireAfterFence)
    {
        if (!module)
            return;
        if (retireAfterFence == 0u || retireAfterFence <= m_completedFenceValue)
        {
            vkDestroyShaderModule(m_device, module, nullptr);
            return;
        }
        m_pendingObjectDestroys.push_back(PendingObjectDestroy{ PendingObjectKind::ShaderModule, retireAfterFence, reinterpret_cast<uint64_t>(module) });
    }

    void VulkanDevice::DeferDestroyPipeline(VkPipeline pipeline, uint64_t retireAfterFence)
    {
        if (!pipeline)
            return;
        if (retireAfterFence == 0u || retireAfterFence <= m_completedFenceValue)
        {
            vkDestroyPipeline(m_device, pipeline, nullptr);
            return;
        }
        m_pendingObjectDestroys.push_back(PendingObjectDestroy{ PendingObjectKind::Pipeline, retireAfterFence, reinterpret_cast<uint64_t>(pipeline) });
    }

    void VulkanDevice::DeferDestroySampler(VkSampler sampler, uint64_t retireAfterFence)
    {
        if (!sampler)
            return;
        if (retireAfterFence == 0u || retireAfterFence <= m_completedFenceValue)
        {
            vkDestroySampler(m_device, sampler, nullptr);
            return;
        }
        m_pendingObjectDestroys.push_back(PendingObjectDestroy{ PendingObjectKind::Sampler, retireAfterFence, reinterpret_cast<uint64_t>(sampler) });
    }

    void VulkanDevice::DeferDestroyImageView(VkImageView view, uint64_t retireAfterFence)
    {
        if (!view)
            return;
        if (retireAfterFence == 0u || retireAfterFence <= m_completedFenceValue)
        {
            vkDestroyImageView(m_device, view, nullptr);
            return;
        }
        m_pendingObjectDestroys.push_back(PendingObjectDestroy{ PendingObjectKind::ImageView, retireAfterFence, reinterpret_cast<uint64_t>(view) });
    }

    void VulkanDevice::DeferDestroySemaphore(VkSemaphore semaphore, uint64_t retireAfterFence)
    {
        if (!semaphore)
            return;
        if (retireAfterFence == 0u || retireAfterFence <= m_completedFenceValue)
        {
            vkDestroySemaphore(m_device, semaphore, nullptr);
            return;
        }
        m_pendingObjectDestroys.push_back(PendingObjectDestroy{ PendingObjectKind::Semaphore, retireAfterFence, reinterpret_cast<uint64_t>(semaphore) });
    }

    void VulkanDevice::DeferDestroySwapchain(VkSwapchainKHR swapchain, uint64_t retireAfterFence)
    {
        if (!swapchain)
            return;
        if (retireAfterFence == 0u || retireAfterFence <= m_completedFenceValue)
        {
            vkDestroySwapchainKHR(m_device, swapchain, nullptr);
            return;
        }
        m_pendingObjectDestroys.push_back(PendingObjectDestroy{ PendingObjectKind::Swapchain, retireAfterFence, reinterpret_cast<uint64_t>(swapchain) });
    }

    void VulkanDevice::DeferDestroyDescriptorPool(VkDescriptorPool pool, uint64_t retireAfterFence)
    {
        if (!pool)
            return;
        if (retireAfterFence == 0u || retireAfterFence <= m_completedFenceValue)
        {
            vkDestroyDescriptorPool(m_device, pool, nullptr);
            return;
        }
        m_pendingObjectDestroys.push_back(PendingObjectDestroy{ PendingObjectKind::DescriptorPool, retireAfterFence, reinterpret_cast<uint64_t>(pool) });
    }

    void VulkanDevice::DeferDestroyCommandPool(VkCommandPool pool, uint64_t retireAfterFence)
    {
        if (!pool)
            return;
        if (retireAfterFence == 0u || retireAfterFence <= m_completedFenceValue)
        {
            vkDestroyCommandPool(m_device, pool, nullptr);
            return;
        }
        m_pendingObjectDestroys.push_back(PendingObjectDestroy{ PendingObjectKind::CommandPool, retireAfterFence, reinterpret_cast<uint64_t>(pool) });
    }

    VkSemaphore VulkanDevice::FindSubmissionSignalSemaphore(uint32_t submissionId) const noexcept
    {
        auto it = m_submissionSignalSemaphores.find(submissionId);
        return it != m_submissionSignalSemaphores.end() ? it->second : VK_NULL_HANDLE;
    }

    VkSemaphore VulkanDevice::CreateSubmissionSignalSemaphore(uint32_t submissionId, QueueType /*queue*/) noexcept
    {
        auto it = m_submissionSignalSemaphores.find(submissionId);
        if (it != m_submissionSignalSemaphores.end())
            return it->second;

        VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkSemaphore semaphore = VK_NULL_HANDLE;
        if (vkCreateSemaphore(m_device, &sci, nullptr, &semaphore) != VK_SUCCESS)
        {
            Debug::LogError("VulkanDevice: vkCreateSemaphore failed for submissionId=%u", submissionId);
            return VK_NULL_HANDLE;
        }
        m_submissionSignalSemaphores.emplace(submissionId, semaphore);
        return semaphore;
    }

    void VulkanDevice::Shutdown()
    {
        WaitIdle();
        RefreshCompletedFrameFences();
        m_completedFenceValue = std::max(m_completedFenceValue, m_lastSubmittedFenceValue);
        ProcessPendingBufferDestroys();
        ProcessPendingTextureDestroys();
        ProcessPendingObjectDestroys();
        DestroyImmediateUploadBuffer();

        if (!m_initialized)
            return;

        if (m_activeSwapchain)
        {
            m_activeSwapchain->Destroy();
            m_activeSwapchain = nullptr;
        }

        WaitIdle();

        m_resources.pipelines.ForEach([&](VulkanPipelineEntry& p) {
            if (p.pipeline) vkDestroyPipeline(m_device, p.pipeline, nullptr);
            });
        m_resources.shaders.ForEach([&](VulkanShaderEntry& s) {
            if (s.module) vkDestroyShaderModule(m_device, s.module, nullptr);
            });
        m_resources.renderTargets.Clear();
        m_resources.textures.ForEach([&](VulkanTextureEntry& t) {
            if (t.sampleView && t.sampleView != t.view) vkDestroyImageView(m_device, t.sampleView, nullptr);
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

        for (auto& [id, semaphore] : m_submissionSignalSemaphores)
            if (semaphore != VK_NULL_HANDLE)
                vkDestroySemaphore(m_device, semaphore, nullptr);
        m_submissionSignalSemaphores.clear();

        DestroyGlobalDescriptors();

        DestroyFrameContexts();

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
        if (!m_device)
            return;

        vkDeviceWaitIdle(m_device);
        m_completedFenceValue = std::max(m_completedFenceValue, m_lastSubmittedFenceValue);
        for (FrameContext& frame : m_frameContexts)
        {
            for (size_t queueIndex = 0u; queueIndex < frame.queues.size(); ++queueIndex)
            {
                QueueFrameContext& queueFrame = frame.queues[queueIndex];
                if (!queueFrame.inFlight)
                    continue;
                queueFrame.inFlight = false;
                queueFrame.completedQueueFenceValue = queueFrame.submittedQueueFenceValue;
                queueFrame.lifecycleState = QueueFrameLifecycleState::Idle;
                queueFrame.owner = nullptr;
                m_completedQueueFenceValues[queueIndex] = std::max(m_completedQueueFenceValues[queueIndex], queueFrame.completedQueueFenceValue);
            }
        }
        ProcessPendingBufferDestroys();
        ProcessPendingTextureDestroys();
        ProcessPendingObjectDestroys();
    }

    std::unique_ptr<ISwapchain> VulkanDevice::CreateSwapchain(const SwapchainDesc& desc)
    {
        auto sc = std::make_unique<VulkanSwapchain>(*this, desc);
        if (!sc->Initialize())
            return nullptr;
        m_activeSwapchain = sc.get();
        if (m_activeSwapchain)
            EnsureFrameContextsForSwapchainImages(m_activeSwapchain->GetBufferCount());
        return sc;
    }

    bool VulkanDevice::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties, uint32_t& outMemoryTypeIndex) const
    {
        for (uint32_t i = 0u; i < m_memoryProperties.memoryTypeCount; ++i)
        {
            const bool supported = (typeBits & (1u << i)) != 0u;
            const bool match = (m_memoryProperties.memoryTypes[i].propertyFlags & properties) == properties;
            if (supported && match)
            {
                outMemoryTypeIndex = i;
                return true;
            }
        }
        outMemoryTypeIndex = UINT32_MAX;
        return false;
    }


    bool VulkanDevice::EnsureImmediateUploadBuffer(VkDeviceSize requiredSize)
    {
        if (!m_device)
            return false;

        if (m_immediateUploadBuffer.buffer != VK_NULL_HANDLE &&
            m_immediateUploadBuffer.memory != VK_NULL_HANDLE &&
            m_immediateUploadBuffer.mapped != nullptr &&
            m_immediateUploadCapacity >= requiredSize)
        {
            return true;
        }

        DestroyImmediateUploadBuffer();

        VkDeviceSize newCapacity = std::max<VkDeviceSize>(requiredSize, 65536u);
        if (newCapacity < requiredSize)
            newCapacity = requiredSize;

        VulkanBufferEntry entry{};
        entry.byteSize = newCapacity;
        entry.stride = 0u;
        entry.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        entry.access = MemoryAccess::CpuWrite;
        entry.memoryFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        entry.stateRecord.currentState = ResourceState::Common;
        entry.stateRecord.authoritativeOwner = ResourceStateAuthority::BackendResource;
        entry.stateRecord.lastSubmissionFenceValue = 0u;

        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = newCapacity;
        bci.usage = entry.usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(m_device, &bci, nullptr, &entry.buffer) != VK_SUCCESS)
        {
            Debug::LogError("VulkanDevice: vkCreateBuffer failed for immediate upload buffer");
            return false;
        }

        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(m_device, entry.buffer, &req);

        uint32_t memoryTypeIndex = UINT32_MAX;
        if (!FindMemoryType(req.memoryTypeBits, entry.memoryFlags, memoryTypeIndex))
        {
            Debug::LogError("VulkanDevice: no compatible memory type for immediate upload buffer (typeBits=0x%X size=%llu)",
                req.memoryTypeBits,
                static_cast<unsigned long long>(req.size));
            vkDestroyBuffer(m_device, entry.buffer, nullptr);
            entry.buffer = VK_NULL_HANDLE;
            return false;
        }

        VkMemoryAllocateInfo alloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = memoryTypeIndex;

        if (vkAllocateMemory(m_device, &alloc, nullptr, &entry.memory) != VK_SUCCESS)
        {
            Debug::LogError("VulkanDevice: vkAllocateMemory failed for immediate upload buffer");
            vkDestroyBuffer(m_device, entry.buffer, nullptr);
            entry.buffer = VK_NULL_HANDLE;
            return false;
        }

        const VkResult bindResult = vkBindBufferMemory(m_device, entry.buffer, entry.memory, 0u);
        if (bindResult != VK_SUCCESS)
        {
            Debug::LogError("VulkanDevice: vkBindBufferMemory failed for immediate upload buffer (%d)", static_cast<int>(bindResult));
            vkFreeMemory(m_device, entry.memory, nullptr);
            entry.memory = VK_NULL_HANDLE;
            vkDestroyBuffer(m_device, entry.buffer, nullptr);
            entry.buffer = VK_NULL_HANDLE;
            return false;
        }

        void* mapped = nullptr;
        const VkResult mapResult = vkMapMemory(m_device, entry.memory, 0u, newCapacity, 0u, &mapped);
        if (mapResult != VK_SUCCESS || mapped == nullptr)
        {
            Debug::LogError("VulkanDevice: vkMapMemory failed for immediate upload buffer (result=%d size=%llu)",
                static_cast<int>(mapResult),
                static_cast<unsigned long long>(newCapacity));
            vkFreeMemory(m_device, entry.memory, nullptr);
            entry.memory = VK_NULL_HANDLE;
            vkDestroyBuffer(m_device, entry.buffer, nullptr);
            entry.buffer = VK_NULL_HANDLE;
            return false;
        }

        entry.mapped = mapped;
        m_immediateUploadBuffer = entry;
        m_immediateUploadCapacity = newCapacity;
        return true;
    }

    void VulkanDevice::DestroyImmediateUploadBuffer() noexcept
    {
        if (!m_device)
        {
            m_immediateUploadBuffer = {};
            m_immediateUploadCapacity = 0u;
            return;
        }

        if (m_immediateUploadBuffer.mapped && m_immediateUploadBuffer.memory)
            vkUnmapMemory(m_device, m_immediateUploadBuffer.memory);
        if (m_immediateUploadBuffer.buffer)
            vkDestroyBuffer(m_device, m_immediateUploadBuffer.buffer, nullptr);
        if (m_immediateUploadBuffer.memory)
            vkFreeMemory(m_device, m_immediateUploadBuffer.memory, nullptr);

        m_immediateUploadBuffer = {};
        m_immediateUploadCapacity = 0u;
    }

    BufferHandle VulkanDevice::CreateBuffer(const BufferDesc& desc)
    {
        VulkanBufferEntry entry{};
        entry.byteSize = desc.byteSize;
        entry.stride = desc.stride;
        entry.usage = ToVkBufferUsage(desc) | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        entry.access = desc.access;
        switch (desc.access)
        {
        case MemoryAccess::GpuOnly:
            entry.memoryFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            break;
        case MemoryAccess::CpuRead:
            entry.memoryFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            break;
        case MemoryAccess::CpuWrite:
        default:
            entry.memoryFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            break;
        }

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

        uint32_t memoryTypeIndex = UINT32_MAX;
        if (!FindMemoryType(req.memoryTypeBits, entry.memoryFlags, memoryTypeIndex))
        {
            vkDestroyBuffer(m_device, entry.buffer, nullptr);
            Debug::LogError("VulkanDevice: no compatible buffer memory type found (typeBits=0x%X requiredFlags=0x%X size=%llu)",
                req.memoryTypeBits,
                static_cast<unsigned>(entry.memoryFlags),
                static_cast<unsigned long long>(req.size));
            return BufferHandle::Invalid();
        }

        VkMemoryAllocateInfo alloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = memoryTypeIndex;

        if (vkAllocateMemory(m_device, &alloc, nullptr, &entry.memory) != VK_SUCCESS)
        {
            vkDestroyBuffer(m_device, entry.buffer, nullptr);
            Debug::LogError("VulkanDevice: vkAllocateMemory(buffer) failed");
            return BufferHandle::Invalid();
        }

        const VkResult bindResult = vkBindBufferMemory(m_device, entry.buffer, entry.memory, 0u);
        if (bindResult != VK_SUCCESS)
        {
            vkFreeMemory(m_device, entry.memory, nullptr);
            vkDestroyBuffer(m_device, entry.buffer, nullptr);
            Debug::LogError("VulkanDevice: vkBindBufferMemory failed (%d)", static_cast<int>(bindResult));
            return BufferHandle::Invalid();
        }

        entry.stateRecord.currentState = (desc.type == BufferType::Vertex) ? ResourceState::VertexBuffer
            : (desc.type == BufferType::Index) ? ResourceState::IndexBuffer
            : (desc.type == BufferType::Constant) ? ResourceState::ConstantBuffer
            : ResourceState::Common;
        entry.stateRecord.authoritativeOwner = ResourceStateAuthority::BackendResource;
        entry.stateRecord.lastSubmissionFenceValue = 0u;

        return m_resources.buffers.Add(entry);
    }

    void VulkanDevice::DestroyBuffer(BufferHandle handle)
    {
        auto* buffer = m_resources.buffers.Get(handle);
        if (!buffer)
            return;

        VulkanBufferEntry entry = *buffer;
        m_resources.buffers.Remove(handle);

        RefreshCompletedFrameFences();

        const uint64_t retireAfterFence = std::max({ m_completedFenceValue, m_lastSubmittedFenceValue, entry.stateRecord.lastSubmissionFenceValue });
        if (retireAfterFence == 0u || retireAfterFence <= m_completedFenceValue)
        {
            if (entry.mapped)
                vkUnmapMemory(m_device, entry.memory);
            if (entry.buffer)
                vkDestroyBuffer(m_device, entry.buffer, nullptr);
            if (entry.memory)
                vkFreeMemory(m_device, entry.memory, nullptr);
            return;
        }

        PendingBufferDestroy pending{};
        pending.entry = entry;
        pending.retireAfterFence = retireAfterFence;
        m_pendingBufferDestroys.push_back(std::move(pending));
    }

    void* VulkanDevice::MapBuffer(BufferHandle handle)
    {
        auto* buffer = m_resources.buffers.Get(handle);
        if (!buffer)
            return nullptr;
        if (!buffer->mapped)
        {
            void* mapped = nullptr;
            const VkResult mapResult = vkMapMemory(m_device, buffer->memory, 0u, buffer->byteSize, 0u, &mapped);
            if (mapResult != VK_SUCCESS)
            {
                Debug::LogError("VulkanDevice: vkMapMemory failed (result=%d size=%llu access=%u memoryFlags=0x%X)",
                    static_cast<int>(mapResult),
                    static_cast<unsigned long long>(buffer->byteSize),
                    static_cast<unsigned>(buffer->access),
                    static_cast<unsigned>(buffer->memoryFlags));
                return nullptr;
            }
            buffer->mapped = mapped;
        }
        return buffer->mapped;
    }

    void VulkanDevice::UnmapBuffer(BufferHandle handle)
    {
        auto* buffer = m_resources.buffers.Get(handle);
        if (!buffer || !buffer->mapped) return;
        vkUnmapMemory(m_device, buffer->memory);
        buffer->mapped = nullptr;
    }

    bool VulkanDevice::CreateImage(const TextureDesc& desc, VkFormat format,
        VkImageUsageFlags usage, VkImageAspectFlags aspect,
        VulkanTextureEntry& outEntry)
    {
        const uint32_t arrayLayers = std::max(1u, desc.dimension == TextureDimension::Cubemap ? 6u : desc.arraySize);
        const bool isCube = desc.dimension == TextureDimension::Cubemap;

        outEntry.width = desc.width;
        outEntry.height = desc.height;
        outEntry.depth = std::max(1u, desc.depth);
        outEntry.arraySize = arrayLayers;
        outEntry.mipLevels = std::max(1u, desc.mipLevels);
        outEntry.format = format;
        outEntry.aspect = aspect;
        outEntry.usage = usage;

        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.flags = isCube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.extent.width = desc.width;
        ici.extent.height = desc.height;
        ici.extent.depth = 1u;
        ici.mipLevels = outEntry.mipLevels;
        ici.arrayLayers = arrayLayers;
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

        uint32_t memoryTypeIndex = UINT32_MAX;
        if (!FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memoryTypeIndex))
        {
            Debug::LogError("VulkanDevice: no compatible image memory type found (typeBits=0x%X requiredFlags=0x%X size=%llu)",
                req.memoryTypeBits,
                static_cast<unsigned>(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
                static_cast<unsigned long long>(req.size));
            vkDestroyImage(m_device, outEntry.image, nullptr);
            outEntry.image = VK_NULL_HANDLE;
            return false;
        }

        VkMemoryAllocateInfo alloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = memoryTypeIndex;

        if (vkAllocateMemory(m_device, &alloc, nullptr, &outEntry.memory) != VK_SUCCESS)
        {
            vkDestroyImage(m_device, outEntry.image, nullptr);
            outEntry.image = VK_NULL_HANDLE;
            return false;
        }

        const VkResult bindResult = vkBindImageMemory(m_device, outEntry.image, outEntry.memory, 0u);
        if (bindResult != VK_SUCCESS)
        {
            vkFreeMemory(m_device, outEntry.memory, nullptr);
            outEntry.memory = VK_NULL_HANDLE;
            vkDestroyImage(m_device, outEntry.image, nullptr);
            outEntry.image = VK_NULL_HANDLE;
            Debug::LogError("VulkanDevice: vkBindImageMemory failed (%d)", static_cast<int>(bindResult));
            return false;
        }

        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = outEntry.image;
        if (isCube)
            vci.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        else if (arrayLayers > 1u)
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        else
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format;
        vci.subresourceRange.aspectMask = aspect;
        vci.subresourceRange.baseMipLevel = 0u;
        vci.subresourceRange.levelCount = outEntry.mipLevels;
        vci.subresourceRange.baseArrayLayer = 0u;
        vci.subresourceRange.layerCount = arrayLayers;

        if (vkCreateImageView(m_device, &vci, nullptr, &outEntry.view) != VK_SUCCESS)
        {
            vkFreeMemory(m_device, outEntry.memory, nullptr);
            outEntry.memory = VK_NULL_HANDLE;
            vkDestroyImage(m_device, outEntry.image, nullptr);
            outEntry.image = VK_NULL_HANDLE;
            return false;
        }

        outEntry.sampleView = outEntry.view;
        if ((aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0u)
        {
            if (vkCreateImageView(m_device, &vci, nullptr, &outEntry.sampleView) != VK_SUCCESS)
            {
                vkDestroyImageView(m_device, outEntry.view, nullptr);
                outEntry.view = VK_NULL_HANDLE;
                vkFreeMemory(m_device, outEntry.memory, nullptr);
                outEntry.memory = VK_NULL_HANDLE;
                vkDestroyImage(m_device, outEntry.image, nullptr);
                outEntry.image = VK_NULL_HANDLE;
                return false;
            }
        }

        outEntry.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        outEntry.aspect = aspect;
        outEntry.contentsUndefined = true;
        outEntry.stateRecord.currentState = ResourceState::Unknown;
        outEntry.stateRecord.authoritativeOwner = ResourceStateAuthority::BackendResource;
        outEntry.stateRecord.lastSubmissionFenceValue = 0u;
        return true;
    }

    TextureHandle VulkanDevice::CreateTexture(const TextureDesc& desc)
    {
        VulkanTextureEntry entry{};
        entry.descFormat = desc.format;
        const VkFormat format = (desc.format == Format::D24_UNORM_S8_UINT && m_physicalDevice)
            ? FindSupportedDepthFormat(m_physicalDevice)
            : ToVkFormat(desc.format);
        const VkImageAspectFlags aspect = AspectMaskForVkFormat(format);

        if (m_physicalDevice != VK_NULL_HANDLE && (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0u)
            LogDepthFormatSupport(m_physicalDevice, format, desc.debugName.c_str());

        if (format == VK_FORMAT_UNDEFINED)
        {
            Debug::LogError("VulkanDevice: unsupported texture format");
            return TextureHandle::Invalid();
        }
        if (m_physicalDevice != VK_NULL_HANDLE)
        {
            VkFormatProperties props{};
            vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &props);
            const VkFormatFeatureFlags optimal = props.optimalTilingFeatures;
            const bool sampledOk = !HasFlag(desc.usage, ResourceUsage::ShaderResource) || ((optimal & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0u);
            const bool colorOk = !HasFlag(desc.usage, ResourceUsage::RenderTarget) || ((optimal & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0u);
            const bool depthOk = !HasFlag(desc.usage, ResourceUsage::DepthStencil) || ((optimal & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0u);
            const bool storageOk = !HasFlag(desc.usage, ResourceUsage::UnorderedAccess) || ((optimal & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0u);
            if (!sampledOk || !colorOk || !depthOk || !storageOk)
            {
                Debug::LogError("VulkanDevice: texture format %u is not supported for requested usage on this device",
                    static_cast<unsigned>(desc.format));
                return TextureHandle::Invalid();
            }
        }

        if (!CreateImage(desc, format, ToVkImageUsage(desc), aspect, entry))
        {
            Debug::LogError("VulkanDevice: CreateImage failed");
            return TextureHandle::Invalid();
        }


        // Frisch erzeugte Images befinden sich physisch weiterhin in VK_IMAGE_LAYOUT_UNDEFINED.
        // Der erste echte Nutzungsübergang muss diesen Zustand herstellen, statt ein gewünschtes
        // Initial-Layout nur logisch zu spiegeln.
        entry.stateRecord.currentState = ResourceState::Unknown;
        entry.stateRecord.authoritativeOwner = ResourceStateAuthority::BackendResource;
        entry.stateRecord.lastSubmissionFenceValue = 0u;
        entry.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        return m_resources.textures.Add(entry);
    }

    void VulkanDevice::DestroyTexture(TextureHandle handle)
    {
        auto* tex = m_resources.textures.Get(handle);
        if (!tex) return;

        VulkanTextureEntry entry = *tex;
        m_resources.textures.Remove(handle);

        RefreshCompletedFrameFences();
        const uint64_t retireAfterFence = std::max({ m_completedFenceValue, m_lastSubmittedFenceValue, entry.stateRecord.lastSubmissionFenceValue });
        if (retireAfterFence == 0u || retireAfterFence <= m_completedFenceValue)
        {
            if (entry.sampleView && entry.sampleView != entry.view) vkDestroyImageView(m_device, entry.sampleView, nullptr);
            if (entry.view) vkDestroyImageView(m_device, entry.view, nullptr);
            if (entry.ownsImage && entry.image) vkDestroyImage(m_device, entry.image, nullptr);
            if (entry.memory) vkFreeMemory(m_device, entry.memory, nullptr);
            return;
        }

        PendingTextureDestroy pending{};
        pending.entry = entry;
        pending.retireAfterFence = retireAfterFence;
        m_pendingTextureDestroys.push_back(std::move(pending));
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
        const VkShaderModule module = shader->module;
        m_resources.shaders.Remove(handle);
        RefreshCompletedFrameFences();
        DeferDestroyShaderModule(module, GetSafeRetireFenceValue());
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

        VulkanPipelineEntry entry{};
        entry.pipelineClass = desc.pipelineClass;

        if (desc.pipelineClass == PipelineClass::Compute)
        {
            VkPipelineShaderStageCreateInfo computeStage{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
            bool foundComputeStage = false;
            for (const VkPipelineShaderStageCreateInfo& stage : stages)
            {
                if (stage.stage != VK_SHADER_STAGE_COMPUTE_BIT)
                    continue;
                computeStage = stage;
                foundComputeStage = true;
                break;
            }

            if (!foundComputeStage)
            {
                Debug::LogError("VulkanDevice: compute pipeline creation failed - no compute shader stage");
                return PipelineHandle::Invalid();
            }

            VkComputePipelineCreateInfo ci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            ci.stage = computeStage;
            ci.layout = m_pipelineLayout;

            if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1u, &ci, nullptr, &entry.pipeline) != VK_SUCCESS)
            {
                Debug::LogError("VulkanDevice: vkCreateComputePipelines failed");
                return PipelineHandle::Invalid();
            }

            return m_resources.pipelines.Add(entry);
        }

        if (desc.pipelineClass != PipelineClass::Graphics)
        {
            Debug::LogError("VulkanDevice: unsupported pipeline class=%u",
                static_cast<uint32_t>(desc.pipelineClass));
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
            vka.location = static_cast<uint32_t>(a.semantic);
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
        rs.depthBiasEnable = (desc.rasterizer.depthBias != 0.f || desc.rasterizer.slopeScaledBias != 0.f) ? VK_TRUE : VK_FALSE;
        rs.depthBiasConstantFactor = desc.rasterizer.depthBias;
        rs.depthBiasClamp = desc.rasterizer.depthBiasClamp;
        rs.depthBiasSlopeFactor = desc.rasterizer.slopeScaledBias;

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
        if (m_physicalDevice != VK_NULL_HANDLE && depthFormat != VK_FORMAT_UNDEFINED)
            LogDepthFormatSupport(m_physicalDevice, depthFormat, "CreateGraphicsPipeline depthFormat");

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

        entry.topology = desc.topology;
        entry.hasColor = desc.colorFormat != Format::Unknown;
        entry.hasDepth = desc.depthFormat != Format::Unknown;

        Debug::LogVerbose("VulkanDevice: CreatePipeline layoutKey=%llu psoKey=%llu name=%s",
            static_cast<unsigned long long>(desc.pipelineLayoutHash),
            static_cast<unsigned long long>(desc.shaderContractHash),
            desc.debugName.c_str());

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
        const VkPipeline vkPipeline = pipeline->pipeline;
        m_resources.pipelines.Remove(handle);
        RefreshCompletedFrameFences();
        DeferDestroyPipeline(vkPipeline, GetSafeRetireFenceValue());
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
        const bool useAnisotropy = m_samplerAnisotropySupported && desc.maxAniso > 1u;
        ci.maxAnisotropy = useAnisotropy ? static_cast<float>(desc.maxAniso) : 1.0f;
        ci.anisotropyEnable = useAnisotropy ? VK_TRUE : VK_FALSE;
        ci.compareEnable = desc.compareFunc != CompareFunc::Never ? VK_TRUE : VK_FALSE;
        ci.compareOp = ToVkCompareOp(desc.compareFunc);
        const bool borderWhite = desc.borderColor[0] >= 0.5f && desc.borderColor[1] >= 0.5f
                             && desc.borderColor[2] >= 0.5f && desc.borderColor[3] >= 0.5f;
        ci.borderColor = borderWhite ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                     : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;

        if (vkCreateSampler(m_device, &ci, nullptr, &entry.sampler) != VK_SUCCESS)
            return 0u;

        m_resources.samplers.push_back(entry);
        const uint32_t handle = static_cast<uint32_t>(m_resources.samplers.size() - 1u);
        if (ci.compareEnable == VK_TRUE)
        {
            Debug::Log("ShadowSamplerDesc(vulkan): handle=%u min=%d mag=%d mip=%d addr=(%d,%d,%d) compareEnable=%d compareOp=%d border=%d",
                handle,
                static_cast<int>(ci.minFilter),
                static_cast<int>(ci.magFilter),
                static_cast<int>(ci.mipmapMode),
                static_cast<int>(ci.addressModeU),
                static_cast<int>(ci.addressModeV),
                static_cast<int>(ci.addressModeW),
                static_cast<int>(ci.compareEnable),
                static_cast<int>(ci.compareOp),
                static_cast<int>(ci.borderColor));
        }
        return handle;
    }

    std::unique_ptr<ICommandList> VulkanDevice::CreateCommandList(QueueType queue)
    {
        return std::make_unique<VulkanCommandList>(*this, m_resources, queue);
    }

    std::unique_ptr<IFence> VulkanDevice::CreateFence(uint64_t initialValue)
    {
        return std::make_unique<VulkanFence>(*this, initialValue);
    }

    CommandListRuntimeDesc VulkanDevice::GetCommandListRuntime() const
    {
        CommandListRuntimeDesc desc = BuildDefaultCommandListRuntimeDesc();
        desc.queues[0] = CommandQueueRuntimeDesc{ QueueType::Graphics, NativeCommandListClass::Graphics, m_graphicsQueue != VK_NULL_HANDLE, false, true, true, false, true };
        desc.queues[1] = CommandQueueRuntimeDesc{ QueueType::Compute, NativeCommandListClass::Compute, m_computeQueue != VK_NULL_HANDLE, m_computeQueueFamily != m_graphicsQueueFamily, false, false, true, true };
        desc.queues[2] = CommandQueueRuntimeDesc{ QueueType::Transfer, NativeCommandListClass::Copy, m_transferQueue != VK_NULL_HANDLE, m_transferQueueFamily != m_graphicsQueueFamily, false, false, false, true };
        desc.allocator.separateUploadCopyRecording = m_transferQueue != VK_NULL_HANDLE && m_transferQueueFamily != m_graphicsQueueFamily;
        desc.lifecycle = CommandListLifecycleRuntimeDesc{};
        desc.compute.runtime = GetComputeRuntime();
        desc.compute.computeCommandListsMaterialized = m_computeQueue != VK_NULL_HANDLE;
        desc.compute.computePsoMaterialized = true;
        desc.compute.uavBarriersMaterialized = true;
        desc.compute.queueRoutingMaterialized = m_computeQueue != VK_NULL_HANDLE;
        desc.compute.crossQueueSyncMaterialized = m_computeQueue != VK_NULL_HANDLE;
        desc.queueSync.queueLocalFenceSignalSupported = true;
        desc.queueSync.queueLocalFenceWaitSupported = true;
        desc.queueSync.interQueueDependenciesPrepared = true;
        desc.queueSync.ownershipTransfersPrepared = m_computeQueue != VK_NULL_HANDLE || m_transferQueue != VK_NULL_HANDLE;
        desc.queueSync.ownershipTransfersMaterialized = m_computeQueue != VK_NULL_HANDLE || m_transferQueue != VK_NULL_HANDLE;
        desc.queueSync.graphToSubmissionMappingPrepared = true;
        desc.queueSync.graphToSubmissionMappingMaterialized = m_computeQueue != VK_NULL_HANDLE || m_transferQueue != VK_NULL_HANDLE;
        desc.multiQueue.preparedForCopyToGraphics = m_transferQueue != VK_NULL_HANDLE || m_graphicsQueue != VK_NULL_HANDLE;
        desc.multiQueue.preparedForGraphicsToPresent = m_graphicsQueue != VK_NULL_HANDLE;
        desc.multiQueue.preparedForAsyncCompute = m_computeQueue != VK_NULL_HANDLE;
        desc.multiQueue.queueOwnershipTransfersPrepared = m_computeQueue != VK_NULL_HANDLE || m_transferQueue != VK_NULL_HANDLE;
        desc.multiQueue.queueOwnershipTransfersMaterialized = m_computeQueue != VK_NULL_HANDLE || m_transferQueue != VK_NULL_HANDLE;
        desc.multiQueue.interQueueDependenciesMaterialized = m_computeQueue != VK_NULL_HANDLE || m_transferQueue != VK_NULL_HANDLE;
        desc.bindings.usesDescriptorTableModel = true;
        desc.bindings.resourceTableBindPerCommandList = true;
        desc.bindings.samplerTableBindPerCommandList = true;
        desc.bindings.constantBufferRangesUseDynamicOffsets = true;
        return desc;
    }

    bool VulkanDevice::BeginQueueFrameRecording(QueueType queue, const void* owner, uint64_t* blockingFenceValue)
    {
        if (blockingFenceValue)
            *blockingFenceValue = 0u;
        if (m_frameContexts.empty() || owner == nullptr)
            return false;

        RefreshCompletedFrameFences();
        FrameContext& frame = m_frameContexts[m_currentFrameSlot % static_cast<uint32_t>(m_frameContexts.size())];
        QueueFrameContext& queueFrame = frame.queues[QueueIndex(queue)];

        if (queueFrame.inFlight && queueFrame.submittedExternalFenceValue > m_completedFenceValue)
        {
            if (blockingFenceValue)
                *blockingFenceValue = queueFrame.submittedExternalFenceValue;
            return false;
        }

        if (queueFrame.inFlight && queueFrame.submittedExternalFenceValue <= m_completedFenceValue)
        {
            queueFrame.inFlight = false;
            queueFrame.completedQueueFenceValue = std::max(queueFrame.completedQueueFenceValue, queueFrame.submittedQueueFenceValue);
        }

        if (queueFrame.frameIndexStamp == m_frameIndex &&
            queueFrame.lifecycleState != QueueFrameLifecycleState::Idle &&
            queueFrame.owner != owner)
        {
            return false;
        }

        if (queueFrame.frameIndexStamp != m_frameIndex)
        {
            queueFrame.owner = nullptr;
            queueFrame.lifecycleState = QueueFrameLifecycleState::Idle;
            queueFrame.frameIndexStamp = m_frameIndex;
            queueFrame.submittedExternalFenceValue = 0u;
            queueFrame.submittedQueueFenceValue = 0u;
        }

        if (queueFrame.lifecycleState == QueueFrameLifecycleState::Submitted)
            return false;

        queueFrame.owner = owner;
        queueFrame.lifecycleState = QueueFrameLifecycleState::Recording;
        queueFrame.frameIndexStamp = m_frameIndex;
        return true;
    }

    bool VulkanDevice::EndQueueFrameRecording(QueueType queue, const void* owner)
    {
        if (m_frameContexts.empty() || owner == nullptr)
            return false;

        FrameContext& frame = m_frameContexts[m_currentFrameSlot % static_cast<uint32_t>(m_frameContexts.size())];
        QueueFrameContext& queueFrame = frame.queues[QueueIndex(queue)];
        if (queueFrame.owner != owner || queueFrame.lifecycleState != QueueFrameLifecycleState::Recording)
            return false;

        queueFrame.lifecycleState = QueueFrameLifecycleState::Executable;
        return true;
    }

    bool VulkanDevice::CanSubmitQueueFrame(QueueType queue, const void* owner) const
    {
        if (m_frameContexts.empty() || owner == nullptr)
            return false;

        const FrameContext& frame = m_frameContexts[m_currentFrameSlot % static_cast<uint32_t>(m_frameContexts.size())];
        const QueueFrameContext& queueFrame = frame.queues[QueueIndex(queue)];
        return queueFrame.owner == owner && queueFrame.lifecycleState == QueueFrameLifecycleState::Executable;
    }

    void VulkanDevice::ImmediateSubmit(const std::function<void(VkCommandBuffer)>& fn)
    {
        ImmediateSubmit(QueueType::Graphics, fn);
    }

    void VulkanDevice::ImmediateSubmit(QueueType queueType, const std::function<void(VkCommandBuffer)>& fn)
    {
        const uint32_t queueFamilyIndex = GetQueueFamilyIndex(queueType);
        const VkQueue queueHandle = GetQueueHandle(queueType);

        VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.queueFamilyIndex = queueFamilyIndex;
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
        vkQueueSubmit(queueHandle, 1u, &submit, VK_NULL_HANDLE);
        vkQueueWaitIdle(queueHandle);

        vkFreeCommandBuffers(m_device, pool, 1u, &cmd);
        vkDestroyCommandPool(m_device, pool, nullptr);
    }

    void VulkanDevice::UploadBufferData(BufferHandle handle, const void* data, size_t byteSize, size_t dstOffset)
    {
        auto* dstBuffer = m_resources.buffers.Get(handle);
        if (!dstBuffer || !data || byteSize == 0u)
            return;
        if ((dstOffset + byteSize) > dstBuffer->byteSize)
        {
            Debug::LogError("VulkanDevice: UploadBufferData out of bounds (size=%zu offset=%zu capacity=%llu)",
                byteSize,
                dstOffset,
                static_cast<unsigned long long>(dstBuffer->byteSize));
            return;
        }

        const bool hostVisible = (dstBuffer->memoryFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0u;
        if (hostVisible)
        {
            void* mapped = MapBuffer(handle);
            if (!mapped)
                return;
            std::memcpy(static_cast<uint8_t*>(mapped) + dstOffset, data, byteSize);
            return;
        }

        if (!EnsureImmediateUploadBuffer(static_cast<VkDeviceSize>(byteSize)))
        {
            Debug::LogError("VulkanDevice: failed to allocate persistent immediate upload buffer");
            return;
        }

        std::memcpy(m_immediateUploadBuffer.mapped, data, byteSize);

        const auto* dstBufferFresh = m_resources.buffers.Get(handle);
        if (!dstBufferFresh || m_immediateUploadBuffer.buffer == VK_NULL_HANDLE)
        {
            Debug::LogError("VulkanDevice: buffer upload lost resource entry before submit");
            return;
        }

        const VkBuffer dstVkBuffer = dstBufferFresh->buffer;
        const VkDeviceSize dstVkSize = dstBufferFresh->byteSize;
        const ResourceState dstState = dstBufferFresh->stateRecord.currentState;
        const VkBuffer stagingVkBuffer = m_immediateUploadBuffer.buffer;

        ImmediateSubmit(QueueType::Graphics, [=](VkCommandBuffer cmd)
            {
                VkBufferMemoryBarrier acquireDst{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
                acquireDst.srcAccessMask = ToVkAccessFlags(dstState);
                acquireDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                acquireDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                acquireDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                acquireDst.buffer = dstVkBuffer;
                acquireDst.offset = 0u;
                acquireDst.size = dstVkSize;
                vkCmdPipelineBarrier(cmd,
                    ToVkPipelineStage(dstState),
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0u,
                    0u, nullptr,
                    1u, &acquireDst,
                    0u, nullptr);

                VkBufferCopy region{};
                region.srcOffset = 0u;
                region.dstOffset = dstOffset;
                region.size = byteSize;
                vkCmdCopyBuffer(cmd, stagingVkBuffer, dstVkBuffer, 1u, &region);

                VkBufferMemoryBarrier releaseDst{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
                releaseDst.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                releaseDst.dstAccessMask = ToVkAccessFlags(dstState);
                releaseDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                releaseDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                releaseDst.buffer = dstVkBuffer;
                releaseDst.offset = 0u;
                releaseDst.size = dstVkSize;
                vkCmdPipelineBarrier(cmd,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    ToVkPipelineStage(dstState),
                    0u,
                    0u, nullptr,
                    1u, &releaseDst,
                    0u, nullptr);
            });
    }

    void VulkanDevice::UploadTextureData(TextureHandle handle,
        const void* data,
        size_t byteSize,
        uint32_t mipLevel,
        uint32_t arrayLayer)
    {
        auto* tex = m_resources.textures.Get(handle);
        if (!tex || !data || byteSize == 0u)
            return;
        if (mipLevel >= tex->mipLevels || arrayLayer >= tex->arraySize)
            return;

        if ((tex->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0u)
        {
            Debug::LogError("VulkanDevice: UploadTextureData requires VK_IMAGE_USAGE_TRANSFER_DST_BIT");
            return;
        }

        if (!EnsureImmediateUploadBuffer(static_cast<VkDeviceSize>(byteSize)))
        {
            Debug::LogError("VulkanDevice: failed to allocate persistent immediate upload buffer for texture upload");
            return;
        }

        const uint32_t mipW = std::max(1u, tex->width >> mipLevel);
        const uint32_t mipH = std::max(1u, tex->height >> mipLevel);
        const TextureUploadLayout layout = ComputeTextureUploadLayout(tex->descFormat, mipW, mipH, 1u);
        if (layout.byteSize == 0u || byteSize < layout.byteSize)
        {
            Debug::LogError("VulkanDevice: UploadTextureData received invalid upload layout for mip=%u layer=%u", mipLevel, arrayLayer);
            return;
        }

        std::memcpy(m_immediateUploadBuffer.mapped, data, byteSize);

        const auto* texFresh = m_resources.textures.Get(handle);
        if (!texFresh || m_immediateUploadBuffer.buffer == VK_NULL_HANDLE)
        {
            Debug::LogError("VulkanDevice: texture upload lost resource entry before submit");
            return;
        }

        const VkImage image = texFresh->image;
        const VkFormat format = texFresh->format;
        const uint32_t width = texFresh->width;
        const uint32_t height = texFresh->height;
        const VkImageUsageFlags usage = texFresh->usage;
        const VkBuffer stagingVkBuffer = m_immediateUploadBuffer.buffer;
        const bool textureFullyInitialized = !texFresh->contentsUndefined;
        const VkImageLayout sourceLayout = textureFullyInitialized
            ? GetAuthoritativeTextureLayout(*texFresh)
            : VK_IMAGE_LAYOUT_UNDEFINED;

        ImmediateSubmit(QueueType::Graphics, [=](VkCommandBuffer cmd) {
            const uint32_t mipW = std::max(1u, width >> mipLevel);
            const uint32_t mipH = std::max(1u, height >> mipLevel);

            VkImageMemoryBarrier toCopy{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            toCopy.oldLayout = sourceLayout;
            toCopy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toCopy.srcAccessMask = 0u;
            toCopy.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toCopy.image = image;
            toCopy.subresourceRange.aspectMask = AspectMaskForVkFormat(format);
            toCopy.subresourceRange.baseMipLevel = mipLevel;
            toCopy.subresourceRange.levelCount = 1u;
            toCopy.subresourceRange.baseArrayLayer = arrayLayer;
            toCopy.subresourceRange.layerCount = 1u;
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                0u,
                0u, nullptr,
                0u, nullptr,
                1u, &toCopy);

            VkBufferImageCopy copy{};
            copy.bufferOffset = 0u;
            copy.bufferRowLength = 0u;
            copy.bufferImageHeight = 0u;
            copy.imageSubresource.aspectMask = AspectMaskForVkFormat(format);
            copy.imageSubresource.mipLevel = mipLevel;
            copy.imageSubresource.baseArrayLayer = arrayLayer;
            copy.imageSubresource.layerCount = 1u;
            copy.imageExtent.width = mipW;
            copy.imageExtent.height = mipH;
            copy.imageExtent.depth = 1u;
            vkCmdCopyBufferToImage(cmd, stagingVkBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &copy);

            VkImageLayout finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkAccessFlags finalAccess = VK_ACCESS_SHADER_READ_BIT;
            VkPipelineStageFlags finalStage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0u)
            {
                finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                finalAccess = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                finalStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            }
            else if ((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0u)
            {
                finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                finalAccess = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                finalStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            }

            VkImageMemoryBarrier toFinal{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            toFinal.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toFinal.newLayout = finalLayout;
            toFinal.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toFinal.dstAccessMask = finalAccess;
            toFinal.image = image;
            toFinal.subresourceRange.aspectMask = AspectMaskForVkFormat(format);
            toFinal.subresourceRange.baseMipLevel = mipLevel;
            toFinal.subresourceRange.levelCount = 1u;
            toFinal.subresourceRange.baseArrayLayer = arrayLayer;
            toFinal.subresourceRange.layerCount = 1u;
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                finalStage,
                0u,
                0u, nullptr,
                0u, nullptr,
                1u, &toFinal);
            });

        auto* texAfterUpload = m_resources.textures.Get(handle);
        if (texAfterUpload)
        {
            const ResourceState finalState = ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0u)
                ? ResourceState::RenderTarget
                : ((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0u)
                ? ResourceState::DepthWrite
                : ResourceState::ShaderRead;
            texAfterUpload->uploadedMipMask |= (1u << mipLevel);
            ++texAfterUpload->uploadedSubresourceCount;
            const uint32_t totalSubresources = texAfterUpload->mipLevels * std::max(1u, texAfterUpload->arraySize);
            if (texAfterUpload->uploadedSubresourceCount >= totalSubresources)
            {
                texAfterUpload->contentsUndefined = false;
                SetAuthoritativeTextureState(*texAfterUpload,
                    finalState,
                    ResourceStateAuthority::BackendResource,
                    0u);
            }
        }
    }

    VkImageLayout VulkanDevice::GetAuthoritativeImageLayout(ResourceState state) const noexcept
    {
        return ToVkImageLayout(state);
    }

    VkImageLayout VulkanDevice::GetAuthoritativeTextureLayout(const VulkanTextureEntry& texture) const noexcept
    {
        const VkImageLayout authoritativeLayout = GetAuthoritativeImageLayout(texture.stateRecord.currentState);
        if (authoritativeLayout != VK_IMAGE_LAYOUT_UNDEFINED)
            return authoritativeLayout;
        return texture.layout;
    }

    void VulkanDevice::SetAuthoritativeTextureState(VulkanTextureEntry& texture,
        ResourceState state,
        ResourceStateAuthority owner,
        uint64_t lastSubmissionFenceValue) noexcept
    {
        texture.stateRecord.currentState = state;
        texture.stateRecord.authoritativeOwner = owner;
        texture.stateRecord.lastSubmissionFenceValue = std::max(texture.stateRecord.lastSubmissionFenceValue,
            lastSubmissionFenceValue);

        const VkImageLayout authoritativeLayout = GetAuthoritativeImageLayout(state);
        if (authoritativeLayout != VK_IMAGE_LAYOUT_UNDEFINED)
            texture.layout = authoritativeLayout;
    }

    ResourceStateRecord VulkanDevice::QueryBufferState(BufferHandle handle) const
    {
        if (const auto* entry = m_resources.buffers.Get(handle))
            return entry->stateRecord;
        return {};
    }

    ResourceStateRecord VulkanDevice::QueryTextureState(TextureHandle handle) const
    {
        if (const auto* entry = m_resources.textures.Get(handle))
            return entry->stateRecord;
        return {};
    }

    ResourceStateRecord VulkanDevice::QueryRenderTargetState(RenderTargetHandle handle) const
    {
        const auto* entry = m_resources.renderTargets.Get(handle);
        if (!entry)
            return {};
        if (entry->colorHandle.IsValid())
            return QueryTextureState(entry->colorHandle);
        if (entry->depthHandle.IsValid())
            return QueryTextureState(entry->depthHandle);
        return {};
    }

    VkFence VulkanDevice::GetCurrentFrameFence(QueueType queue) const noexcept
    {
        if (m_frameContexts.empty())
            return VK_NULL_HANDLE;
        const FrameContext& frame = m_frameContexts[m_currentFrameSlot % static_cast<uint32_t>(m_frameContexts.size())];
        return frame.queues[QueueIndex(queue)].submitFence;
    }

    uint64_t VulkanDevice::AllocateSubmittedFenceValue(QueueType queue) noexcept
    {
        const size_t queueIndex = QueueIndex(queue);
        const uint64_t queueFenceValue = ++m_lastSubmittedQueueFenceValues[queueIndex];
        const uint64_t externalFenceValue = ++m_nextExternalFenceValue;

        if (m_externalFenceTimeline.size() <= externalFenceValue)
            m_externalFenceTimeline.resize(static_cast<size_t>(externalFenceValue + 1u));
        m_externalFenceTimeline[static_cast<size_t>(externalFenceValue)] = ExternalFencePoint{ queue, queueFenceValue };
        m_lastSubmittedFenceValue = externalFenceValue;
        return externalFenceValue;
    }

    void VulkanDevice::MarkCurrentFrameSubmitted(QueueType queue, uint64_t fenceValue) noexcept
    {
        if (m_frameContexts.empty() || fenceValue == 0u || fenceValue >= m_externalFenceTimeline.size())
            return;

        FrameContext& frame = m_frameContexts[m_currentFrameSlot % static_cast<uint32_t>(m_frameContexts.size())];
        QueueFrameContext& queueFrame = frame.queues[QueueIndex(queue)];
        const ExternalFencePoint& point = m_externalFenceTimeline[static_cast<size_t>(fenceValue)];
        queueFrame.submittedExternalFenceValue = fenceValue;
        queueFrame.submittedQueueFenceValue = point.queueFenceValue;
        queueFrame.inFlight = point.queueFenceValue != 0u;
        queueFrame.lifecycleState = QueueFrameLifecycleState::Submitted;
    }

    void VulkanDevice::RefreshCompletedFrameFences() noexcept
    {
        if (!m_device)
            return;

        for (FrameContext& frame : m_frameContexts)
        {
            for (size_t queueIndex = 0u; queueIndex < frame.queues.size(); ++queueIndex)
            {
                QueueFrameContext& queueFrame = frame.queues[queueIndex];
                if (!queueFrame.inFlight || queueFrame.submitFence == VK_NULL_HANDLE)
                    continue;
                if (vkGetFenceStatus(m_device, queueFrame.submitFence) != VK_SUCCESS)
                    continue;

                queueFrame.inFlight = false;
                queueFrame.completedQueueFenceValue = queueFrame.submittedQueueFenceValue;
                queueFrame.lifecycleState = QueueFrameLifecycleState::Idle;
                queueFrame.owner = nullptr;
                m_completedQueueFenceValues[queueIndex] = std::max(m_completedQueueFenceValues[queueIndex], queueFrame.completedQueueFenceValue);
            }
        }

        while ((m_completedFenceValue + 1u) < m_externalFenceTimeline.size())
        {
            const ExternalFencePoint& point = m_externalFenceTimeline[static_cast<size_t>(m_completedFenceValue + 1u)];
            if (m_completedQueueFenceValues[QueueIndex(point.queue)] < point.queueFenceValue)
                break;
            ++m_completedFenceValue;
        }

        ProcessPendingBufferDestroys();
        ProcessPendingTextureDestroys();
        ProcessPendingObjectDestroys();
    }

    void VulkanDevice::WaitForFenceValue(uint64_t value, uint64_t timeoutNs)
    {
        if (!m_device || value == 0u)
            return;

        RefreshCompletedFrameFences();
        if (m_completedFenceValue >= value)
            return;
        if (value >= m_externalFenceTimeline.size())
            return;

        const ExternalFencePoint point = m_externalFenceTimeline[static_cast<size_t>(value)];
        const size_t targetQueueIndex = QueueIndex(point.queue);

        for (FrameContext& frame : m_frameContexts)
        {
            QueueFrameContext& queueFrame = frame.queues[targetQueueIndex];
            if (!queueFrame.inFlight || queueFrame.submitFence == VK_NULL_HANDLE)
                continue;
            if (queueFrame.submittedQueueFenceValue < point.queueFenceValue)
                continue;

            vkWaitForFences(m_device, 1u, &queueFrame.submitFence, VK_TRUE, timeoutNs);
            RefreshCompletedFrameFences();
            return;
        }

        RefreshCompletedFrameFences();
    }

    void VulkanDevice::BeginFrame()
    {
        const auto beginFrameStart = std::chrono::steady_clock::now();
        ResetBackendFrameDiagnostics();
        RefreshCompletedFrameFences();
        ++m_frameIndex;
        m_totalDrawCalls = 0u;
        m_currentFrameSlot = (m_currentFrameSlot + 1u) % std::max(1u, m_framesInFlight);

        if (!m_frameContexts.empty())
        {
            FrameContext& frame = m_frameContexts[m_currentFrameSlot % static_cast<uint32_t>(m_frameContexts.size())];
            for (size_t queueIndex = 0u; queueIndex < frame.queues.size(); ++queueIndex)
            {
                QueueFrameContext& queueFrame = frame.queues[queueIndex];
                if (!queueFrame.inFlight || queueFrame.submitFence == VK_NULL_HANDLE)
                    continue;

                vkWaitForFences(m_device, 1u, &queueFrame.submitFence, VK_TRUE, UINT64_MAX);
                queueFrame.inFlight = false;
                queueFrame.completedQueueFenceValue = queueFrame.submittedQueueFenceValue;
                queueFrame.lifecycleState = QueueFrameLifecycleState::Idle;
                queueFrame.owner = nullptr;
                m_completedQueueFenceValues[queueIndex] = std::max(m_completedQueueFenceValues[queueIndex], queueFrame.completedQueueFenceValue);
            }

            while ((m_completedFenceValue + 1u) < m_externalFenceTimeline.size())
            {
                const ExternalFencePoint& point = m_externalFenceTimeline[static_cast<size_t>(m_completedFenceValue + 1u)];
                if (m_completedQueueFenceValues[QueueIndex(point.queue)] < point.queueFenceValue)
                    break;
                ++m_completedFenceValue;
            }

            ProcessPendingBufferDestroys();
            ProcessPendingTextureDestroys();
            ProcessPendingObjectDestroys();
        }

        if (m_activeSwapchain)
            m_activeSwapchain->AcquireForFrame();

        const auto beginFrameEnd = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(m_backendDiagnosticsMutex);
        m_backendDiagnostics.beginFrameMs =
            std::chrono::duration<float, std::milli>(beginFrameEnd - beginFrameStart).count();
    }

    void VulkanDevice::EndFrame()
    {
        RefreshCompletedFrameFences();
    }

    SwapchainRuntimeDesc VulkanDevice::GetSwapchainRuntime() const
    {
        SwapchainRuntimeDesc desc{};
        desc.presentQueue = QueueType::Graphics;
        desc.explicitAcquire = true;
        desc.explicitPresentTransition = true;
        desc.tracksPerBufferOwnership = true;
        desc.resizeRequiresRecreate = true;
        desc.destructionRequiresFenceRetirement = true;
        return desc;
    }

    QueueCapabilities VulkanDevice::GetQueueCapabilities(QueueType queue) const
    {
        switch (queue)
        {
        case QueueType::Graphics:
            return QueueCapabilities{ QueueType::Graphics, m_graphicsQueue != VK_NULL_HANDLE, false, true };
        case QueueType::Compute:
            return QueueCapabilities{ QueueType::Compute, m_computeQueue != VK_NULL_HANDLE, m_computeQueueFamily != m_graphicsQueueFamily, false };
        case QueueType::Transfer:
            return QueueCapabilities{ QueueType::Transfer, m_transferQueue != VK_NULL_HANDLE, m_transferQueueFamily != m_graphicsQueueFamily, false };
        default:
            return {};
        }
    }

    QueueType VulkanDevice::GetPreferredUploadQueue() const
    {
        const QueueCapabilities transfer = GetQueueCapabilities(QueueType::Transfer);
        if (transfer.supported)
            return QueueType::Transfer;
        return QueueType::Graphics;
    }

    VkQueue VulkanDevice::GetQueueHandle(QueueType queue) const noexcept
    {
        switch (queue)
        {
        case QueueType::Compute: return m_computeQueue != VK_NULL_HANDLE ? m_computeQueue : m_graphicsQueue;
        case QueueType::Transfer: return m_transferQueue != VK_NULL_HANDLE ? m_transferQueue : m_graphicsQueue;
        case QueueType::Graphics:
        default: return m_graphicsQueue;
        }
    }

    uint32_t VulkanDevice::GetQueueFamilyIndex(QueueType queue) const noexcept
    {
        switch (queue)
        {
        case QueueType::Compute: return m_computeQueue != VK_NULL_HANDLE ? m_computeQueueFamily : m_graphicsQueueFamily;
        case QueueType::Transfer: return m_transferQueue != VK_NULL_HANDLE ? m_transferQueueFamily : m_graphicsQueueFamily;
        case QueueType::Graphics:
        default: return m_graphicsQueueFamily;
        }
    }

    bool VulkanDevice::SupportsFeature(const char* feature) const
    {
        if (!feature) return false;
        const std::string f(feature);
        return f == "dynamic_rendering" || f == "spirv";
    }

    bool VulkanDevice::SupportsTextureFormat(Format format, ResourceUsage usage) const
    {
        if (m_physicalDevice == VK_NULL_HANDLE)
            return false;

        const VkFormat vkFormat = ToVkFormat(format);
        if (vkFormat == VK_FORMAT_UNDEFINED)
            return false;

        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice, vkFormat, &props);
        const VkFormatFeatureFlags optimal = props.optimalTilingFeatures;

        if (HasFlag(usage, ResourceUsage::ShaderResource) && (optimal & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0u)
            return false;
        if (HasFlag(usage, ResourceUsage::RenderTarget) && (optimal & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0u)
            return false;
        if (HasFlag(usage, ResourceUsage::DepthStencil) && (optimal & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0u)
            return false;
        if (HasFlag(usage, ResourceUsage::UnorderedAccess) && (optimal & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0u)
            return false;
        return true;
    }

    math::Mat4 VulkanDevice::GetShadowClipSpaceAdjustment() const
    {
        // Kein Y-Flip. Vulkan nutzt negativen Viewport (height < 0) fuer alle Passes
        // inkl. Shadow — V = (1-clip_y)/2, identisch zu DX11. HLSL-Formel
        // uv.y = 0.5 - posNDC.y*0.5 passt ohne zusaetzliche Matrix-Korrektur.
        return math::Mat4::Identity();
    }

} // namespace engine::renderer::vulkan
