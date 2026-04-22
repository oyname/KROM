#include "VulkanDevice.hpp"
#include "core/Debug.hpp"
#include <algorithm>

#ifdef _WIN32
#   define WIN32_LEAN_AND_MEAN
#   define NOMINMAX
#   include <windows.h>
#endif

namespace engine::renderer::vulkan {

namespace {

uint64_t ComputeSwapchainRetireFence(const VulkanDevice& device, const std::vector<uint64_t>& imageFenceValues)
{
    uint64_t retireFence = std::max(device.GetCompletedFenceValue(), device.GetSafeRetireFenceValue());
    for (uint64_t value : imageFenceValues)
        retireFence = std::max(retireFence, value);
    return retireFence;
}

void WaitForSwapchainRetirement(VulkanDevice& device, const std::vector<uint64_t>& imageFenceValues)
{
    uint64_t retireFence = ComputeSwapchainRetireFence(device, imageFenceValues);
    if (retireFence > device.GetCompletedFenceValue())
        device.WaitForFenceValue(retireFence);

    device.WaitIdle();
}

void StampSwapchainBackbufferState(VulkanDevice& device,
                                   TextureHandle handle,
                                   ResourceState state,
                                   uint64_t lastSubmissionFenceValue)
{
    if (!handle.IsValid())
        return;

    auto* tex = device.GetResources().textures.Get(handle);
    if (!tex)
        return;

    device.SetAuthoritativeTextureState(*tex,
                                      state,
                                      ResourceStateAuthority::ExternalSwapchain,
                                      lastSubmissionFenceValue);
}

}

VulkanSwapchain::VulkanSwapchain(VulkanDevice& device, const IDevice::SwapchainDesc& desc)
    : m_device(&device), m_desc(desc), m_width(desc.width), m_height(desc.height), m_bufferCount(desc.bufferCount)
{
}

VulkanSwapchain::~VulkanSwapchain()
{
    Destroy();
}

bool VulkanSwapchain::CreateSurface()
{
#ifdef _WIN32
    if (!m_desc.nativeWindowHandle)
        return false;

    VkWin32SurfaceCreateInfoKHR sci{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    sci.hinstance = GetModuleHandleW(nullptr);
    sci.hwnd = static_cast<HWND>(m_desc.nativeWindowHandle);
    if (vkCreateWin32SurfaceKHR(m_device->GetInstance(), &sci, nullptr, &m_surface) != VK_SUCCESS)
    {
        Debug::LogError("VulkanSwapchain: vkCreateWin32SurfaceKHR failed");
        return false;
    }
    return true;
#else
    Debug::LogError("VulkanSwapchain: MVP implementation currently expects Win32 native window handles");
    return false;
#endif
}

bool VulkanSwapchain::CreateSwapchainResources(VkSwapchainKHR oldSwapchain)
{
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_device->GetPhysicalDevice(), m_surface, &caps);

    uint32_t formatCount = 0u;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_device->GetPhysicalDevice(), m_surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_device->GetPhysicalDevice(), m_surface, &formatCount, formats.data());

    VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{ VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR } : formats[0];
    for (const auto& fmt : formats)
    {
        if (fmt.format == VK_FORMAT_R8G8B8A8_SRGB)
        {
            chosen = fmt;
            break;
        }
        if (fmt.format == VK_FORMAT_B8G8R8A8_SRGB)
            chosen = fmt;
    }
    m_colorFormat       = chosen.format;
    m_engineColorFormat = (m_colorFormat == VK_FORMAT_R8G8B8A8_SRGB)
                              ? Format::RGBA8_UNORM_SRGB
                              : Format::BGRA8_UNORM_SRGB;

    uint32_t presentModeCount = 0u;
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_device->GetPhysicalDevice(), m_surface, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_device->GetPhysicalDevice(), m_surface, &presentModeCount, presentModes.data());
    m_presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!m_desc.vsync)
    {
        for (const auto& mode : presentModes)
        {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR || mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
            {
                m_presentMode = mode;
                break;
            }
        }
    }

    if (caps.currentExtent.width != UINT32_MAX)
    {
        m_width = caps.currentExtent.width;
        m_height = caps.currentExtent.height;
    }
    else
    {
        m_width = std::clamp(m_width, caps.minImageExtent.width, caps.maxImageExtent.width);
        m_height = std::clamp(m_height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    if (m_width == 0u || m_height == 0u)
    {
        m_recreatePending = true;
        return false;
    }

    if (caps.maxImageExtent.width == 0u || caps.maxImageExtent.height == 0u)
    {
        m_recreatePending = true;
        return false;
    }
    const uint32_t imageCount = std::max(caps.minImageCount, std::min(m_bufferCount, caps.maxImageCount > 0u ? caps.maxImageCount : m_bufferCount));

    VkImageUsageFlags requestedUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_SAMPLED_BIT) != 0u)
        requestedUsage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    requestedUsage &= caps.supportedUsageFlags;
    if ((requestedUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0u)
    {
        Debug::LogError("VulkanSwapchain: surface does not support color-attachment swapchain images");
        return false;
    }

    VkSwapchainCreateInfoKHR ci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = m_surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = { m_width, m_height };
    ci.imageArrayLayers = 1u;
    ci.imageUsage = requestedUsage;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = m_presentMode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = oldSwapchain;

    VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(m_device->GetVkDevice(), &ci, nullptr, &newSwapchain) != VK_SUCCESS)
    {
        Debug::LogError("VulkanSwapchain: vkCreateSwapchainKHR failed");
        return false;
    }

    const uint64_t oldRetireFence = ComputeSwapchainRetireFence(*m_device, m_imageFenceValues);
    if (oldSwapchain != VK_NULL_HANDLE || !m_swapchain || !m_imageFenceValues.empty())
        WaitForSwapchainRetirement(*m_device, m_imageFenceValues);
    DestroySyncObjects();
    DestroyBackbufferResources();

    m_swapchain = newSwapchain;

    uint32_t actualCount = 0u;
    vkGetSwapchainImagesKHR(m_device->GetVkDevice(), m_swapchain, &actualCount, nullptr);
    m_images.resize(actualCount);
    vkGetSwapchainImagesKHR(m_device->GetVkDevice(), m_swapchain, &actualCount, m_images.data());
    m_bufferCount = actualCount;
    m_device->EnsureFrameContextsForSwapchainImages(actualCount);
    m_backbufferTextures.resize(actualCount);
    m_backbufferRTs.resize(actualCount);
    m_imageFenceValues.assign(actualCount, 0u);

    for (uint32_t i = 0u; i < actualCount; ++i)
    {
        VulkanTextureEntry tex{};
        tex.image = m_images[i];
        tex.ownsImage = false;
        tex.width = m_width;
        tex.height = m_height;
        tex.format = m_colorFormat;
        tex.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        tex.usage = ci.imageUsage;
        tex.mipLevels = 1u;
        tex.arraySize = 1u;
        tex.contentsUndefined = true;
        // Swapchain-Images befinden sich physisch in VK_IMAGE_LAYOUT_UNDEFINED,
        // nicht in GENERAL. ResourceState::Common würde ToVkImageLayout auf GENERAL
        // abbilden und dann einen falschen Layout im ersten Descriptor-Write liefern.
        m_device->SetAuthoritativeTextureState(tex,
                                               ResourceState::Unknown,
                                               ResourceStateAuthority::ExternalSwapchain,
                                               0u);

        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = tex.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = tex.format;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1u;
        vci.subresourceRange.layerCount = 1u;
        if (vkCreateImageView(m_device->GetVkDevice(), &vci, nullptr, &tex.view) != VK_SUCCESS)
        {
            DestroyBackbufferResources();
            if (m_swapchain)
            {
                vkDestroySwapchainKHR(m_device->GetVkDevice(), m_swapchain, nullptr);
                m_swapchain = oldSwapchain;
            }
            return false;
        }

        m_backbufferTextures[i] = m_device->GetResources().textures.Add(tex);

        VulkanRenderTargetEntry rt{};
        rt.colorHandle = m_backbufferTextures[i];
        rt.width = m_width;
        rt.height = m_height;
        rt.hasColor = true;
        rt.hasDepth = false;
        rt.isBackbuffer = true;
        m_backbufferRTs[i] = m_device->GetResources().renderTargets.Add(rt);
    }

    const uint32_t frameCount = std::max(2u, m_bufferCount + 1u);
    m_imageAvailableSemaphores.resize(frameCount, VK_NULL_HANDLE);
    m_renderFinishedSemaphores.resize(actualCount, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo sem{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0u; i < frameCount; ++i)
    {
        if (vkCreateSemaphore(m_device->GetVkDevice(), &sem, nullptr, &m_imageAvailableSemaphores[i]) != VK_SUCCESS)
        {
            DestroySyncObjects();
            DestroyBackbufferResources();
            if (m_swapchain)
            {
                vkDestroySwapchainKHR(m_device->GetVkDevice(), m_swapchain, nullptr);
                m_swapchain = oldSwapchain;
            }
            return false;
        }
    }
    for (uint32_t i = 0u; i < actualCount; ++i)
    {
        if (vkCreateSemaphore(m_device->GetVkDevice(), &sem, nullptr, &m_renderFinishedSemaphores[i]) != VK_SUCCESS)
        {
            DestroySyncObjects();
            DestroyBackbufferResources();
            if (m_swapchain)
            {
                vkDestroySwapchainKHR(m_device->GetVkDevice(), m_swapchain, nullptr);
                m_swapchain = oldSwapchain;
            }
            return false;
        }
    }

    if (oldSwapchain)
        m_device->DeferDestroySwapchain(oldSwapchain, oldRetireFence);

    m_currentImageIndex = kInvalidImageIndex;
    m_currentAcquireSemaphoreIndex = 0u;
    m_hasAcquiredImage = false;
    m_recreatePending = false;
    return true;
}

bool VulkanSwapchain::RecreateSwapchainResources()
{
    if (!m_device || !m_surface)
        return false;

    InvalidateCurrentImage();

    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_device->GetPhysicalDevice(), m_surface, &caps);
    if (caps.currentExtent.width == 0u || caps.currentExtent.height == 0u)
    {
        m_recreatePending = true;
        return false;
    }

    const VkSwapchainKHR oldSwapchain = m_swapchain;
    return CreateSwapchainResources(oldSwapchain);
}

void VulkanSwapchain::DestroyBackbufferResources()
{
    if (!m_device)
        return;

    const uint64_t retireFence = ComputeSwapchainRetireFence(*m_device, m_imageFenceValues);

    for (auto h : m_backbufferRTs)
        m_device->GetResources().renderTargets.Remove(h);

    for (auto h : m_backbufferTextures)
    {
        auto* tex = m_device->GetResources().textures.Get(h);
        if (tex && tex->view)
            m_device->DeferDestroyImageView(tex->view, retireFence);
        m_device->GetResources().textures.Remove(h);
    }

    m_backbufferRTs.clear();
    m_backbufferTextures.clear();
    m_images.clear();
    m_imageFenceValues.clear();
}

void VulkanSwapchain::DestroySyncObjects()
{
    if (!m_device)
        return;

    const uint64_t retireFence = ComputeSwapchainRetireFence(*m_device, m_imageFenceValues);

    for (VkSemaphore semaphore : m_renderFinishedSemaphores)
        if (semaphore)
            m_device->DeferDestroySemaphore(semaphore, retireFence);
    for (VkSemaphore semaphore : m_imageAvailableSemaphores)
        if (semaphore)
            m_device->DeferDestroySemaphore(semaphore, retireFence);

    m_renderFinishedSemaphores.clear();
    m_imageAvailableSemaphores.clear();
}

void VulkanSwapchain::DestroySwapchainResources()
{
    if (!m_device) return;

    const uint64_t retireFence = ComputeSwapchainRetireFence(*m_device, m_imageFenceValues);
    if (m_swapchain != VK_NULL_HANDLE || !m_imageFenceValues.empty())
        WaitForSwapchainRetirement(*m_device, m_imageFenceValues);
    DestroySyncObjects();
    DestroyBackbufferResources();
    if (m_swapchain)
        m_device->DeferDestroySwapchain(m_swapchain, retireFence);

    m_swapchain = VK_NULL_HANDLE;
    m_currentImageIndex = kInvalidImageIndex;
    m_currentAcquireSemaphoreIndex = 0u;
    m_hasAcquiredImage = false;
}

void VulkanSwapchain::Destroy()
{
    if (!m_device)
        return;

    DestroySwapchainResources();
    if (m_surface)
        vkDestroySurfaceKHR(m_device->GetInstance(), m_surface, nullptr);
    m_surface = VK_NULL_HANDLE;

    if (m_device->GetActiveSwapchain() == this)
        m_device->SetActiveSwapchain(nullptr);
}

bool VulkanSwapchain::Initialize()
{
    if (!CreateSurface())
        return false;
    return CreateSwapchainResources();
}

void VulkanSwapchain::AcquireNextImage()
{
    InvalidateCurrentImage();

    if (m_width == 0u || m_height == 0u)
        return;

    if (m_recreatePending)
    {
        if (!RecreateSwapchainResources())
            return;
    }

    if (!m_swapchain)
        return;

    if (m_imageAvailableSemaphores.empty())
    {
        m_recreatePending = true;
        return;
    }

    m_currentAcquireSemaphoreIndex = m_device->GetCurrentFrameSlot() % static_cast<uint32_t>(m_imageAvailableSemaphores.size());
    const VkSemaphore imageAvailable = GetImageAvailableSemaphore();
    if (imageAvailable == VK_NULL_HANDLE)
        return;

    const VkResult result = vkAcquireNextImageKHR(m_device->GetVkDevice(),
                                                  m_swapchain,
                                                  UINT64_MAX,
                                                  imageAvailable,
                                                  VK_NULL_HANDLE,
                                                  &m_currentImageIndex);
    if (result == VK_SUCCESS)
    {
        if (m_currentImageIndex < m_imageFenceValues.size())
        {
            const uint64_t fenceValue = m_imageFenceValues[m_currentImageIndex];
            if (fenceValue > 0u && fenceValue > m_device->GetCompletedFenceValue())
                m_device->WaitForFenceValue(fenceValue);
        }
        m_hasAcquiredImage = true;
        if (m_currentImageIndex < m_backbufferTextures.size())
        {
            const ResourceState acquiredState = m_imageFenceValues[m_currentImageIndex] > 0u
                ? ResourceState::Present
                : ResourceState::Unknown;  // erster Acquire: Image physisch noch UNDEFINED
            StampSwapchainBackbufferState(*m_device,
                                          m_backbufferTextures[m_currentImageIndex],
                                          acquiredState,
                                          m_imageFenceValues[m_currentImageIndex]);
        }
        return;
    }

    if (result == VK_SUBOPTIMAL_KHR)
    {
        m_recreatePending = true;
        if (m_currentImageIndex < m_imageFenceValues.size())
        {
            const uint64_t fenceValue = m_imageFenceValues[m_currentImageIndex];
            if (fenceValue > 0u && fenceValue > m_device->GetCompletedFenceValue())
                m_device->WaitForFenceValue(fenceValue);
        }
        m_hasAcquiredImage = true;
        if (m_currentImageIndex < m_backbufferTextures.size())
        {
            const ResourceState acquiredState = m_imageFenceValues[m_currentImageIndex] > 0u
                ? ResourceState::Present
                : ResourceState::Unknown;  // erster Acquire: Image physisch noch UNDEFINED
            StampSwapchainBackbufferState(*m_device,
                                          m_backbufferTextures[m_currentImageIndex],
                                          acquiredState,
                                          m_imageFenceValues[m_currentImageIndex]);
        }
        return;
    }

    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        m_recreatePending = true;
        InvalidateCurrentImage();
        return;
    }

    m_recreatePending = true;
    InvalidateCurrentImage();
    Debug::LogError("VulkanSwapchain: vkAcquireNextImageKHR failed (%d)", static_cast<int>(result));
}

bool VulkanSwapchain::AcquireForFrame()
{
    AcquireNextImage();
    return HasUsableBackbuffer();
}

SwapchainFrameStatus VulkanSwapchain::QueryFrameStatus() const
{
    SwapchainFrameStatus status{};
    status.bufferCount = static_cast<uint32_t>(m_backbufferTextures.size());
    status.currentBackbufferIndex = HasUsableBackbuffer() ? m_currentImageIndex : 0u;
    status.hasRenderableBackbuffer = HasUsableBackbuffer();
    status.resizePending = m_recreatePending;
    status.lastSubmissionFenceValue = (m_currentImageIndex < m_imageFenceValues.size()) ? m_imageFenceValues[m_currentImageIndex] : 0u;
    if (m_recreatePending)
        status.phase = SwapchainFramePhase::ResizePending;
    else if (m_hasAcquiredImage)
        status.phase = status.lastSubmissionFenceValue > 0u ? SwapchainFramePhase::Submitted : SwapchainFramePhase::Acquired;
    else if (m_swapchain != VK_NULL_HANDLE)
        status.phase = SwapchainFramePhase::Idle;
    return status;
}

SwapchainRuntimeDesc VulkanSwapchain::GetRuntimeDesc() const
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

void VulkanSwapchain::Present(bool vsync)
{
    if (m_desc.vsync != vsync)
    {
        m_desc.vsync = vsync;
        m_recreatePending = true;
    }

    if (!m_swapchain || !m_hasAcquiredImage)
        return;

    if (m_currentImageIndex >= m_backbufferTextures.size() || m_currentImageIndex >= m_renderFinishedSemaphores.size())
    {
        m_recreatePending = true;
        InvalidateCurrentImage();
        return;
    }

    VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    const VkSemaphore renderFinished = GetRenderFinishedSemaphore();
    if (renderFinished == VK_NULL_HANDLE)
    {
        m_recreatePending = true;
        InvalidateCurrentImage();
        return;
    }
    present.waitSemaphoreCount = 1u;
    present.pWaitSemaphores = &renderFinished;
    present.swapchainCount = 1u;
    present.pSwapchains = &m_swapchain;
    present.pImageIndices = &m_currentImageIndex;
    const uint64_t submittedFenceValue = m_currentImageIndex < m_imageFenceValues.size()
        ? m_imageFenceValues[m_currentImageIndex]
        : 0u;
    const TextureHandle presentedHandle = m_currentImageIndex < m_backbufferTextures.size()
        ? m_backbufferTextures[m_currentImageIndex]
        : TextureHandle::Invalid();
    const VkResult result = vkQueuePresentKHR(m_device->GetGraphicsQueue(), &present);
    StampSwapchainBackbufferState(*m_device, presentedHandle, ResourceState::Present, submittedFenceValue);
    InvalidateCurrentImage();

    if (result == VK_SUCCESS)
        return;

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
    {
        m_recreatePending = true;
        return;
    }

    m_recreatePending = true;
    Debug::LogError("VulkanSwapchain: vkQueuePresentKHR failed (%d)", static_cast<int>(result));
}

void VulkanSwapchain::Resize(uint32_t width, uint32_t height)
{
    m_width = width;
    m_height = height;

    InvalidateCurrentImage();
    m_recreatePending = true;

    if (width == 0u || height == 0u)
        return;
}

TextureHandle VulkanSwapchain::GetBackbufferTexture(uint32_t index) const
{
    if (!HasUsableBackbuffer() || index != m_currentImageIndex)
        return TextureHandle::Invalid();
    return index < m_backbufferTextures.size() ? m_backbufferTextures[index] : TextureHandle::Invalid();
}

RenderTargetHandle VulkanSwapchain::GetBackbufferRenderTarget(uint32_t index) const
{
    if (!HasUsableBackbuffer() || index != m_currentImageIndex)
        return RenderTargetHandle::Invalid();
    return index < m_backbufferRTs.size() ? m_backbufferRTs[index] : RenderTargetHandle::Invalid();
}

VkSemaphore VulkanSwapchain::GetImageAvailableSemaphore() const noexcept
{
    if (m_imageAvailableSemaphores.empty())
        return VK_NULL_HANDLE;
    return m_imageAvailableSemaphores[m_currentAcquireSemaphoreIndex % static_cast<uint32_t>(m_imageAvailableSemaphores.size())];
}

VkSemaphore VulkanSwapchain::GetRenderFinishedSemaphore() const noexcept
{
    if (m_renderFinishedSemaphores.empty())
        return VK_NULL_HANDLE;
    return m_currentImageIndex < m_renderFinishedSemaphores.size() ? m_renderFinishedSemaphores[m_currentImageIndex] : VK_NULL_HANDLE;
}

void VulkanSwapchain::NotifyCurrentImageSubmitted(uint64_t fenceValue) noexcept
{
    if (!m_hasAcquiredImage)
        return;
    if (m_currentImageIndex >= m_imageFenceValues.size())
        return;

    m_imageFenceValues[m_currentImageIndex] = fenceValue;
    if (m_currentImageIndex < m_backbufferTextures.size())
        StampSwapchainBackbufferState(*m_device,
                                      m_backbufferTextures[m_currentImageIndex],
                                      ResourceState::Present,
                                      fenceValue);
}

} // namespace engine::renderer::vulkan
