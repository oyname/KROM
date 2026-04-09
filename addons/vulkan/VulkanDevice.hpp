#ifdef _WIN32
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   ifndef VK_USE_PLATFORM_WIN32_KHR
#       define VK_USE_PLATFORM_WIN32_KHR
#   endif
#   include <windows.h>
#endif

#include <vulkan/vulkan.h>

#include "renderer/IDevice.hpp"
#include "renderer/ShaderBindingModel.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace engine::renderer::vulkan {

template<typename HandleT, typename EntryT>
class VulkanStore
{
    struct Slot { EntryT data{}; uint32_t generation = 0u; bool alive = false; };
    std::vector<Slot> m_slots;
    std::vector<uint32_t> m_free;
public:
    VulkanStore() { m_slots.emplace_back(); }

    HandleT Add(EntryT entry)
    {
        uint32_t idx = 0u;
        if (!m_free.empty())
        {
            idx = m_free.back();
            m_free.pop_back();
        }
        else
        {
            idx = static_cast<uint32_t>(m_slots.size());
            m_slots.emplace_back();
        }
        auto& slot = m_slots[idx];
        slot.data = std::move(entry);
        slot.alive = true;
        ++slot.generation;
        return HandleT::Make(idx, slot.generation & HandleT::GEN_MASK);
    }

    EntryT* Get(HandleT handle) noexcept
    {
        const uint32_t idx = handle.Index();
        if (idx == 0u || idx >= m_slots.size())
            return nullptr;
        auto& slot = m_slots[idx];
        return (slot.alive && slot.generation == handle.Generation()) ? &slot.data : nullptr;
    }

    const EntryT* Get(HandleT handle) const noexcept { return const_cast<VulkanStore*>(this)->Get(handle); }

    bool Remove(HandleT handle) noexcept
    {
        const uint32_t idx = handle.Index();
        if (idx == 0u || idx >= m_slots.size())
            return false;
        auto& slot = m_slots[idx];
        if (!slot.alive || slot.generation != handle.Generation())
            return false;
        slot.alive = false;
        slot.data = EntryT{};
        m_free.push_back(idx);
        return true;
    }

    template<typename Fn>
    void ForEach(Fn&& fn)
    {
        for (auto& slot : m_slots)
            if (slot.alive)
                fn(slot.data);
    }

    void Clear()
    {
        m_slots.clear();
        m_free.clear();
        m_slots.emplace_back();
    }
};

struct VulkanBufferEntry
{
    VkBuffer       buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize   byteSize = 0;
    uint32_t       stride = 0u;
    VkBufferUsageFlags usage = 0u;
    VkMemoryPropertyFlags memoryFlags = 0u;
    void*          mapped = nullptr;
};

struct VulkanTextureEntry
{
    VkImage        image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView    view = VK_NULL_HANDLE;
    VkFormat       format = VK_FORMAT_UNDEFINED;
    uint32_t       width = 0u;
    uint32_t       height = 0u;
    uint32_t       depth = 1u;
    uint32_t       mipLevels = 1u;
    uint32_t       arraySize = 1u;
    VkImageLayout  layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageAspectFlags aspect = 0u;
    bool           ownsImage = true;
};

struct VulkanRenderTargetEntry
{
    TextureHandle colorHandle;
    TextureHandle depthHandle;
    uint32_t      width = 0u;
    uint32_t      height = 0u;
    bool          hasColor = false;
    bool          hasDepth = false;
    bool          isBackbuffer = false;
};

struct VulkanShaderEntry
{
    VkShaderModule module = VK_NULL_HANDLE;
    ShaderStageMask stage = ShaderStageMask::None;
    std::string debugName;
};

struct VulkanPipelineEntry
{
    VkPipeline pipeline = VK_NULL_HANDLE;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    bool hasColor = true;
    bool hasDepth = true;
};

struct VulkanSamplerEntry
{
    VkSampler sampler = VK_NULL_HANDLE;
};

struct VulkanDeviceResources
{
    VulkanStore<BufferHandle, VulkanBufferEntry> buffers;
    VulkanStore<TextureHandle, VulkanTextureEntry> textures;
    VulkanStore<RenderTargetHandle, VulkanRenderTargetEntry> renderTargets;
    VulkanStore<ShaderHandle, VulkanShaderEntry> shaders;
    VulkanStore<PipelineHandle, VulkanPipelineEntry> pipelines;
    std::vector<VulkanSamplerEntry> samplers;
};

class VulkanDevice;

class VulkanSwapchain final : public ISwapchain
{
public:
    VulkanSwapchain(VulkanDevice& device, const IDevice::SwapchainDesc& desc);
    ~VulkanSwapchain() override;

    bool Initialize();
    void Present(bool vsync) override;
    void Resize(uint32_t width, uint32_t height) override;
    void AcquireNextImage();

    [[nodiscard]] uint32_t GetCurrentBackbufferIndex() const override { return m_currentImageIndex; }
    [[nodiscard]] TextureHandle GetBackbufferTexture(uint32_t index) const override;
    [[nodiscard]] RenderTargetHandle GetBackbufferRenderTarget(uint32_t index) const override;
    [[nodiscard]] uint32_t GetWidth() const override { return m_width; }
    [[nodiscard]] uint32_t GetHeight() const override { return m_height; }

    [[nodiscard]] VkSurfaceKHR GetSurface() const noexcept { return m_surface; }
    [[nodiscard]] VkSwapchainKHR GetSwapchain() const noexcept { return m_swapchain; }
    [[nodiscard]] VkSemaphore GetImageAvailableSemaphore() const noexcept;
    [[nodiscard]] VkSemaphore GetRenderFinishedSemaphore() const noexcept;
    [[nodiscard]] VkFormat GetColorFormat() const noexcept { return m_colorFormat; }
    [[nodiscard]] uint32_t GetBufferCount() const noexcept { return m_bufferCount; }

private:
    void Destroy();
    bool CreateSurface();
    bool CreateSwapchainResources();
    bool RecreateSwapchainResources();
    void DestroySwapchainResources();

    VulkanDevice* m_device = nullptr;
    IDevice::SwapchainDesc m_desc{};
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    std::vector<VkSemaphore> m_imageAvailableSemaphores;
    std::vector<VkSemaphore> m_renderFinishedSemaphores;
    VkFormat m_colorFormat = VK_FORMAT_B8G8R8A8_SRGB;
    VkPresentModeKHR m_presentMode = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkImage> m_images;
    std::vector<TextureHandle> m_backbufferTextures;
    std::vector<RenderTargetHandle> m_backbufferRTs;
    uint32_t m_width = 0u;
    uint32_t m_height = 0u;
    uint32_t m_bufferCount = 2u;
    uint32_t m_currentImageIndex = 0u;
    bool m_hasAcquiredImage = false;
    bool m_recreatePending = false;
};

class VulkanFence final : public IFence
{
public:
    VulkanFence(VulkanDevice& device, uint64_t initialValue);
    void Signal(uint64_t value) override;
    void Wait(uint64_t value, uint64_t timeoutNs) override;
    [[nodiscard]] uint64_t GetValue() const override;

private:
    VulkanDevice* m_device = nullptr;
    uint64_t m_value = 0u;
};

class VulkanCommandList final : public ICommandList
{
public:
    VulkanCommandList(VulkanDevice& device, VulkanDeviceResources& resources);
    ~VulkanCommandList() override;

    void Begin() override;
    void End() override;
    void BeginRenderPass(const RenderPassBeginInfo& info) override;
    void EndRenderPass() override;
    void SetPipeline(PipelineHandle pipeline) override;
    void SetVertexBuffer(uint32_t slot, BufferHandle buffer, uint32_t offset = 0u) override;
    void SetIndexBuffer(BufferHandle buffer, bool is32bit = true, uint32_t offset = 0u) override;
    void SetConstantBuffer(uint32_t slot, BufferHandle buffer, ShaderStageMask stages) override;
    void SetConstantBufferRange(uint32_t slot, BufferBinding binding, ShaderStageMask stages) override;
    void SetShaderResource(uint32_t slot, TextureHandle texture, ShaderStageMask stages) override;
    void SetSampler(uint32_t slot, uint32_t samplerIndex, ShaderStageMask stages) override;
    void SetViewport(float x, float y, float width, float height, float minDepth = 0.f, float maxDepth = 1.f) override;
    void SetScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) override;
    void Draw(uint32_t vertexCount, uint32_t instanceCount = 1u, uint32_t firstVertex = 0u, uint32_t firstInstance = 0u) override;
    void DrawIndexed(uint32_t indexCount, uint32_t instanceCount = 1u, uint32_t firstIndex = 0u, int32_t vertexOffset = 0, uint32_t firstInstance = 0u) override;
    void Dispatch(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) override;
    void TransitionResource(BufferHandle buffer, ResourceState before, ResourceState after) override;
    void TransitionResource(TextureHandle texture, ResourceState before, ResourceState after) override;
    void TransitionRenderTarget(RenderTargetHandle rt, ResourceState before, ResourceState after) override;
    void CopyBuffer(BufferHandle dst, uint64_t dstOffset, BufferHandle src, uint64_t srcOffset, uint64_t size) override;
    void CopyTexture(TextureHandle dst, uint32_t dstMip, TextureHandle src, uint32_t srcMip) override;
    void Submit(QueueType queue = QueueType::Graphics) override;

private:
    struct FrameContext
    {
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        std::vector<VkDescriptorPool> descriptorPools;
        uint32_t activeDescriptorPoolCount = 0u;
    };

    struct BoundCB
    {
        BufferHandle buffer;
        uint32_t offset = 0u;
        uint32_t size = 0u;
    };

    void FlushDescriptors();
    bool CreateDescriptorPool(FrameContext& frame);
    VkDescriptorSet AllocateDescriptorSet();
    void TransitionTexture(TextureHandle texture, VkImageLayout newLayout, VkAccessFlags dstAccess, VkPipelineStageFlags dstStage);

    FrameContext& GetCurrentFrameContext();
    const FrameContext& GetCurrentFrameContext() const;

    VulkanDevice* m_device = nullptr;
    VulkanDeviceResources* m_resources = nullptr;
    std::vector<FrameContext> m_frames;
    uint32_t m_frameCount = 0u;
    bool m_recording = false;
    bool m_insideRendering = false;
    RenderTargetHandle m_currentRenderTarget;
    PipelineHandle m_currentPipeline;
    BoundCB m_constantBuffers[CBSlots::COUNT]{};
    TextureHandle m_textures[TexSlots::COUNT]{};
    uint32_t m_samplers[SamplerSlots::COUNT]{};
    BufferHandle m_vertexBuffers[8]{};
    VkDeviceSize m_vertexOffsets[8]{};
    BufferHandle m_indexBuffer;
    VkDeviceSize m_indexOffset = 0u;
    BufferHandle m_fallbackCB;
    TextureHandle m_fallbackTexture;
    uint32_t m_fallbackSampler = 0u;
    VkIndexType m_indexType = VK_INDEX_TYPE_UINT32;
    VkViewport m_viewport{};
    VkRect2D m_scissor{};
};

class VulkanDevice final : public IDevice
{
public:
    VulkanDevice();
    ~VulkanDevice() override;

    bool Initialize(const DeviceDesc& desc) override;
    void Shutdown() override;
    void WaitIdle() override;

    std::unique_ptr<ISwapchain> CreateSwapchain(const SwapchainDesc& desc) override;

    BufferHandle CreateBuffer(const BufferDesc& desc) override;
    void DestroyBuffer(BufferHandle handle) override;
    void* MapBuffer(BufferHandle handle) override;
    void UnmapBuffer(BufferHandle handle) override;

    TextureHandle CreateTexture(const TextureDesc& desc) override;
    void DestroyTexture(TextureHandle handle) override;

    RenderTargetHandle CreateRenderTarget(const RenderTargetDesc& desc) override;
    void DestroyRenderTarget(RenderTargetHandle handle) override;
    TextureHandle GetRenderTargetColorTexture(RenderTargetHandle handle) const override;
    TextureHandle GetRenderTargetDepthTexture(RenderTargetHandle handle) const override;

    ShaderHandle CreateShaderFromSource(const std::string& source, ShaderStageMask stage, const std::string& entryPoint = "main", const std::string& debugName = "") override;
    ShaderHandle CreateShaderFromBytecode(const void* data, size_t byteSize, ShaderStageMask stage, const std::string& debugName = "") override;
    void DestroyShader(ShaderHandle handle) override;

    PipelineHandle CreatePipeline(const PipelineDesc& desc) override;
    void DestroyPipeline(PipelineHandle handle) override;

    uint32_t CreateSampler(const SamplerDesc& desc) override;

    std::unique_ptr<ICommandList> CreateCommandList(QueueType queue = QueueType::Graphics) override;
    std::unique_ptr<IFence> CreateFence(uint64_t initialValue = 0u) override;

    void UploadBufferData(BufferHandle handle, const void* data, size_t byteSize, size_t dstOffset = 0u) override;
    void UploadTextureData(TextureHandle handle, const void* data, size_t byteSize, uint32_t mipLevel = 0u, uint32_t arraySlice = 0u) override;

    void BeginFrame() override;
    void EndFrame() override;

    [[nodiscard]] uint32_t GetDrawCallCount() const override { return m_totalDrawCalls; }
    [[nodiscard]] const char* GetBackendName() const override { return "Vulkan"; }
    [[nodiscard]] bool SupportsFeature(const char* feature) const override;

    static std::vector<AdapterInfo> EnumerateAdaptersImpl();

    [[nodiscard]] VkInstance GetInstance() const noexcept { return m_instance; }
    [[nodiscard]] VkPhysicalDevice GetPhysicalDevice() const noexcept { return m_physicalDevice; }
    [[nodiscard]] VkDevice GetVkDevice() const noexcept { return m_device; }
    [[nodiscard]] VkQueue GetGraphicsQueue() const noexcept { return m_graphicsQueue; }
    [[nodiscard]] uint32_t GetGraphicsQueueFamily() const noexcept { return m_graphicsQueueFamily; }
    [[nodiscard]] VkDescriptorSetLayout GetGlobalSetLayout() const noexcept { return m_globalSetLayout; }
    [[nodiscard]] VkPipelineLayout GetPipelineLayout() const noexcept { return m_pipelineLayout; }
    [[nodiscard]] uint32_t GetCurrentFrameSlot() const noexcept { return m_currentFrameSlot; }
    [[nodiscard]] VulkanDeviceResources& GetResources() noexcept { return m_resources; }
    [[nodiscard]] const VulkanDeviceResources& GetResources() const noexcept { return m_resources; }
    [[nodiscard]] VulkanSwapchain* GetActiveSwapchain() const noexcept { return m_activeSwapchain; }

    uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const;
    bool CreateImage(uint32_t width, uint32_t height, uint32_t mipLevels, VkFormat format,
                     VkImageUsageFlags usage, VkImageAspectFlags aspect,
                     VulkanTextureEntry& outEntry);
    void ImmediateSubmit(const std::function<void(VkCommandBuffer)>& fn);
    void SetActiveSwapchain(VulkanSwapchain* swapchain) noexcept { m_activeSwapchain = swapchain; }
    [[nodiscard]] uint32_t GetFramesInFlight() const noexcept { return m_framesInFlight; }
    [[nodiscard]] VkFence GetCurrentFrameFence() const noexcept;
    void MarkCurrentFrameSubmitted(uint64_t fenceValue) noexcept;
    void RefreshCompletedFrameFences() noexcept;
    void WaitForFenceValue(uint64_t value, uint64_t timeoutNs = UINT64_MAX);
    [[nodiscard]] uint64_t GetCompletedFenceValue() const noexcept { return m_completedFenceValue; }

private:
    bool CreateInstance(const DeviceDesc& desc);
    bool PickPhysicalDevice(uint32_t adapterIndex);
    bool CreateLogicalDevice(const DeviceDesc& desc);
    bool CreateGlobalDescriptors();
    void DestroyGlobalDescriptors();

    struct FrameContext
    {
        VkFence submitFence = VK_NULL_HANDLE;
        uint64_t submittedFenceValue = 0u;
        uint64_t completedFenceValue = 0u;
        bool inFlight = false;
    };

    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_globalSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    uint32_t m_graphicsQueueFamily = 0u;
    bool m_initialized = false;
    bool m_debugEnabled = false;
    uint64_t m_frameIndex = 0u;
    uint32_t m_currentFrameSlot = 0u;
    uint32_t m_framesInFlight = 3u;
    uint32_t m_totalDrawCalls = 0u;
    uint64_t m_completedFenceValue = 0u;
    VulkanSwapchain* m_activeSwapchain = nullptr;
    VulkanDeviceResources m_resources;
    std::vector<FrameContext> m_frameContexts;
    VkPhysicalDeviceMemoryProperties m_memoryProperties{};
};

VkFormat ToVkFormat(Format format) noexcept;
VkImageAspectFlags ToVkAspectFlags(Format format) noexcept;
VkPrimitiveTopology ToVkTopology(PrimitiveTopology topology) noexcept;
VkShaderStageFlagBits ToVkShaderStage(ShaderStageMask stage) noexcept;
VkSamplerAddressMode ToVkAddressMode(WrapMode mode) noexcept;
VkFilter ToVkFilter(FilterMode mode) noexcept;
VkSamplerMipmapMode ToVkMipmapMode(FilterMode mode) noexcept;
VkCompareOp ToVkCompareOp(CompareFunc func) noexcept;
VkCompareOp ToVkCompareOp(DepthFunc func) noexcept;
VkBlendFactor ToVkBlendFactor(BlendFactor factor) noexcept;
VkBlendOp ToVkBlendOp(BlendOp op) noexcept;
VkCullModeFlags ToVkCullMode(CullMode mode) noexcept;
VkFrontFace ToVkFrontFace(WindingOrder order) noexcept;
VkBufferUsageFlags ToVkBufferUsage(const BufferDesc& desc) noexcept;
VkImageUsageFlags ToVkImageUsage(const TextureDesc& desc) noexcept;
VkImageLayout ToVkImageLayout(ResourceState state) noexcept;
VkAccessFlags ToVkAccessFlags(ResourceState state) noexcept;
VkPipelineStageFlags ToVkPipelineStage(ResourceState state) noexcept;

} // namespace engine::renderer::vulkan
