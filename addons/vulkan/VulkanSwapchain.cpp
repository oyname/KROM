#include "VulkanDevice.hpp"
#include "core/Debug.hpp"
#include <algorithm>

#ifdef _WIN32
#   define WIN32_LEAN_AND_MEAN
#   define NOMINMAX
#   include <windows.h>
#endif

namespace engine::renderer::vulkan {

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

bool VulkanSwapchain::CreateSwapchainResources()
{
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_device->GetPhysicalDevice(), m_surface, &caps);

    uint32_t formatCount = 0u;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_device->GetPhysicalDevice(), m_surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_device->GetPhysicalDevice(), m_surface, &formatCount, formats.data());

    VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{ VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR } : formats[0];
    for (const auto& fmt : formats)
    {
        if (fmt.format == VK_FORMAT_B8G8R8A8_SRGB || fmt.format == VK_FORMAT_R8G8B8A8_SRGB)
        {
            chosen = fmt;
            break;
        }
    }
    m_colorFormat = chosen.format;

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
    const uint32_t imageCount = std::max(caps.minImageCount, std::min(m_bufferCount, caps.maxImageCount > 0u ? caps.maxImageCount : m_bufferCount));

    VkSwapchainCreateInfoKHR ci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = m_surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = { m_width, m_height };
    ci.imageArrayLayers = 1u;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = m_presentMode;
    ci.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(m_device->GetVkDevice(), &ci, nullptr, &m_swapchain) != VK_SUCCESS)
    {
        Debug::LogError("VulkanSwapchain: vkCreateSwapchainKHR failed");
        return false;
    }

    uint32_t actualCount = 0u;
    vkGetSwapchainImagesKHR(m_device->GetVkDevice(), m_swapchain, &actualCount, nullptr);
    m_images.resize(actualCount);
    vkGetSwapchainImagesKHR(m_device->GetVkDevice(), m_swapchain, &actualCount, m_images.data());
    m_bufferCount = actualCount;
    m_backbufferTextures.resize(actualCount);
    m_backbufferRTs.resize(actualCount);

    for (uint32_t i = 0u; i < actualCount; ++i)
    {
        VulkanTextureEntry tex{};
        tex.image = m_images[i];
        tex.ownsImage = false;
        tex.width = m_width;
        tex.height = m_height;
        tex.format = m_colorFormat;
        tex.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        tex.layout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = tex.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = tex.format;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1u;
        vci.subresourceRange.layerCount = 1u;
        if (vkCreateImageView(m_device->GetVkDevice(), &vci, nullptr, &tex.view) != VK_SUCCESS)
            return false;

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
    m_renderFinishedSemaphores.resize(frameCount, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo sem{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0u; i < frameCount; ++i)
    {
        vkCreateSemaphore(m_device->GetVkDevice(), &sem, nullptr, &m_imageAvailableSemaphores[i]);
        vkCreateSemaphore(m_device->GetVkDevice(), &sem, nullptr, &m_renderFinishedSemaphores[i]);
    }

    m_currentImageIndex = 0u;
    m_hasAcquiredImage = false;
    m_recreatePending = false;
    return true;
}

bool VulkanSwapchain::RecreateSwapchainResources()
{
    if (!m_device || !m_surface)
        return false;

    m_device->WaitIdle();
    DestroySwapchainResources();
    return CreateSwapchainResources();
}

void VulkanSwapchain::DestroySwapchainResources()
{
    if (!m_device) return;

    for (auto h : m_backbufferRTs)
        m_device->GetResources().renderTargets.Remove(h);

    for (auto h : m_backbufferTextures)
    {
        auto* tex = m_device->GetResources().textures.Get(h);
        if (tex && tex->view)
            vkDestroyImageView(m_device->GetVkDevice(), tex->view, nullptr);
        m_device->GetResources().textures.Remove(h);
    }

    m_backbufferRTs.clear();
    m_backbufferTextures.clear();
    m_images.clear();

    for (VkSemaphore semaphore : m_renderFinishedSemaphores)
        if (semaphore)
            vkDestroySemaphore(m_device->GetVkDevice(), semaphore, nullptr);
    for (VkSemaphore semaphore : m_imageAvailableSemaphores)
        if (semaphore)
            vkDestroySemaphore(m_device->GetVkDevice(), semaphore, nullptr);
    if (m_swapchain)
        vkDestroySwapchainKHR(m_device->GetVkDevice(), m_swapchain, nullptr);

    m_renderFinishedSemaphores.clear();
    m_imageAvailableSemaphores.clear();
    m_swapchain = VK_NULL_HANDLE;
    m_currentImageIndex = 0u;
    m_hasAcquiredImage = false;
}

void VulkanSwapchain::Destroy()
{
    DestroySwapchainResources();
    if (m_surface)
        vkDestroySurfaceKHR(m_device->GetInstance(), m_surface, nullptr);
    m_surface = VK_NULL_HANDLE;
}

bool VulkanSwapchain::Initialize()
{
    if (!CreateSurface())
        return false;
    return CreateSwapchainResources();
}

void VulkanSwapchain::AcquireNextImage()
{
    m_hasAcquiredImage = false;

    if (m_recreatePending)
    {
        if (!RecreateSwapchainResources())
            return;
    }

    if (!m_swapchain)
        return;

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
        m_hasAcquiredImage = true;
        return;
    }

    if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        m_recreatePending = true;
        RecreateSwapchainResources();
        if (!m_swapchain)
            return;

        const VkSemaphore retryImageAvailable = GetImageAvailableSemaphore();
        if (retryImageAvailable == VK_NULL_HANDLE)
            return;

        const VkResult retry = vkAcquireNextImageKHR(m_device->GetVkDevice(),
                                                     m_swapchain,
                                                     UINT64_MAX,
                                                     retryImageAvailable,
                                                     VK_NULL_HANDLE,
                                                     &m_currentImageIndex);
        if (retry == VK_SUCCESS)
            m_hasAcquiredImage = true;
        else if (retry == VK_SUBOPTIMAL_KHR || retry == VK_ERROR_OUT_OF_DATE_KHR)
            m_recreatePending = true;
        return;
    }

    Debug::LogError("VulkanSwapchain: vkAcquireNextImageKHR failed (%d)", static_cast<int>(result));
}

void VulkanSwapchain::Present(bool)
{
    if (!m_swapchain || !m_hasAcquiredImage)
        return;

    VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    const VkSemaphore renderFinished = GetRenderFinishedSemaphore();
    if (renderFinished == VK_NULL_HANDLE)
    {
        m_hasAcquiredImage = false;
        return;
    }
    present.waitSemaphoreCount = 1u;
    present.pWaitSemaphores = &renderFinished;
    present.swapchainCount = 1u;
    present.pSwapchains = &m_swapchain;
    present.pImageIndices = &m_currentImageIndex;
    const VkResult result = vkQueuePresentKHR(m_device->GetGraphicsQueue(), &present);
    m_hasAcquiredImage = false;

    if (result == VK_SUCCESS)
        return;

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
    {
        m_recreatePending = true;
        RecreateSwapchainResources();
        return;
    }

    Debug::LogError("VulkanSwapchain: vkQueuePresentKHR failed (%d)", static_cast<int>(result));
}

void VulkanSwapchain::Resize(uint32_t width, uint32_t height)
{
    m_width = width;
    m_height = height;

    if (width == 0u || height == 0u)
    {
        m_recreatePending = true;
        return;
    }

    if (!RecreateSwapchainResources())
        m_recreatePending = true;
}

TextureHandle VulkanSwapchain::GetBackbufferTexture(uint32_t index) const
{
    return index < m_backbufferTextures.size() ? m_backbufferTextures[index] : TextureHandle::Invalid();
}

RenderTargetHandle VulkanSwapchain::GetBackbufferRenderTarget(uint32_t index) const
{
    return index < m_backbufferRTs.size() ? m_backbufferRTs[index] : RenderTargetHandle::Invalid();
}

VkSemaphore VulkanSwapchain::GetImageAvailableSemaphore() const noexcept
{
    if (m_imageAvailableSemaphores.empty())
        return VK_NULL_HANDLE;
    return m_imageAvailableSemaphores[m_device->GetCurrentFrameSlot() % static_cast<uint32_t>(m_imageAvailableSemaphores.size())];
}

VkSemaphore VulkanSwapchain::GetRenderFinishedSemaphore() const noexcept
{
    if (m_renderFinishedSemaphores.empty())
        return VK_NULL_HANDLE;
    return m_renderFinishedSemaphores[m_device->GetCurrentFrameSlot() % static_cast<uint32_t>(m_renderFinishedSemaphores.size())];
}

} // namespace engine::renderer::vulkan
