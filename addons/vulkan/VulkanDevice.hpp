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
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
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
            slot.generation = (slot.generation + 1u) & HandleT::GEN_MASK;
            return HandleT::Make(idx, slot.generation);
        }

        EntryT* Get(HandleT handle) noexcept
        {
            const uint32_t idx = handle.Index();
            if (idx == 0u || idx >= m_slots.size())
                return nullptr;
            auto& slot = m_slots[idx];
            return (slot.alive && (slot.generation & HandleT::GEN_MASK) == handle.Generation()) ? &slot.data : nullptr;
        }

        const EntryT* Get(HandleT handle) const noexcept { return const_cast<VulkanStore*>(this)->Get(handle); }

        bool Remove(HandleT handle) noexcept
        {
            const uint32_t idx = handle.Index();
            if (idx == 0u || idx >= m_slots.size())
                return false;
            auto& slot = m_slots[idx];
            if (!slot.alive || (slot.generation & HandleT::GEN_MASK) != handle.Generation())
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
        MemoryAccess   access = MemoryAccess::GpuOnly;
        void* mapped = nullptr;
        ResourceStateRecord stateRecord{};
        uint32_t       owningQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    };

    struct VulkanTextureEntry
    {
        VkImage        image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView    view = VK_NULL_HANDLE;
        VkImageView    sampleView = VK_NULL_HANDLE;
        VkFormat       format = VK_FORMAT_UNDEFINED;
        uint32_t       width = 0u;
        uint32_t       height = 0u;
        uint32_t       depth = 1u;
        uint32_t       mipLevels = 1u;
        uint32_t       arraySize = 1u;
        VkImageLayout  layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageAspectFlags aspect = 0u;
        VkImageUsageFlags usage = 0u;
        bool           ownsImage = true;
        bool           contentsUndefined = false;
        uint32_t       uploadedMipMask = 0u;   // bit N set = mip N has been uploaded and transitioned
        ResourceStateRecord stateRecord{};
        uint32_t       owningQueueFamily = VK_QUEUE_FAMILY_IGNORED;
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
        PipelineClass pipelineClass = PipelineClass::Graphics;
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
        friend class VulkanDevice;
    public:
        VulkanSwapchain(VulkanDevice& device, const IDevice::SwapchainDesc& desc);
        ~VulkanSwapchain() override;

        bool Initialize();
        bool AcquireForFrame() override;
        void Present(bool vsync) override;
        void Resize(uint32_t width, uint32_t height) override;
        void AcquireNextImage();

        [[nodiscard]] uint32_t GetCurrentBackbufferIndex() const override { return HasUsableBackbuffer() ? m_currentImageIndex : 0u; }
        [[nodiscard]] TextureHandle GetBackbufferTexture(uint32_t index) const override;
        [[nodiscard]] RenderTargetHandle GetBackbufferRenderTarget(uint32_t index) const override;
        [[nodiscard]] uint32_t GetWidth() const override { return m_width; }
        [[nodiscard]] uint32_t GetHeight() const override { return m_height; }
        [[nodiscard]] bool CanRenderFrame() const override { return HasUsableBackbuffer(); }
        [[nodiscard]] bool NeedsRecreate() const override { return m_recreatePending; }
        [[nodiscard]] SwapchainFrameStatus QueryFrameStatus() const override;
        [[nodiscard]] SwapchainRuntimeDesc GetRuntimeDesc() const override;
        [[nodiscard]] Format GetBackbufferFormat() const override { return m_engineColorFormat; }

        [[nodiscard]] VkSurfaceKHR GetSurface() const noexcept { return m_surface; }
        [[nodiscard]] VkSwapchainKHR GetSwapchain() const noexcept { return m_swapchain; }
        [[nodiscard]] VkSemaphore GetImageAvailableSemaphore() const noexcept;
        [[nodiscard]] VkSemaphore GetRenderFinishedSemaphore() const noexcept;
        void NotifyCurrentImageSubmitted(uint64_t fenceValue) noexcept;
        [[nodiscard]] VkFormat GetColorFormat() const noexcept { return m_colorFormat; }
        [[nodiscard]] uint32_t GetBufferCount() const noexcept { return m_bufferCount; }
        [[nodiscard]] bool HasAcquiredImage() const noexcept { return m_hasAcquiredImage; }

    private:
        static constexpr uint32_t kInvalidImageIndex = UINT32_MAX;
        [[nodiscard]] bool HasUsableBackbuffer() const noexcept
        {
            return m_swapchain != VK_NULL_HANDLE
                && m_hasAcquiredImage
                && m_currentImageIndex != kInvalidImageIndex
                && m_currentImageIndex < m_backbufferTextures.size()
                && m_currentImageIndex < m_backbufferRTs.size();
        }
        void InvalidateCurrentImage() noexcept
        {
            m_currentImageIndex = kInvalidImageIndex;
            m_hasAcquiredImage = false;
        }

        void Destroy();
        bool CreateSurface();
        bool CreateSwapchainResources(VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE);
        bool RecreateSwapchainResources();
        void DestroySwapchainResources();
        void DestroyBackbufferResources();
        void DestroySyncObjects();

        VulkanDevice* m_device = nullptr;
        IDevice::SwapchainDesc m_desc{};
        VkSurfaceKHR m_surface = VK_NULL_HANDLE;
        VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
        std::vector<VkSemaphore> m_imageAvailableSemaphores;
        std::vector<VkSemaphore> m_renderFinishedSemaphores;
        std::vector<uint64_t> m_imageFenceValues;
        uint32_t m_currentAcquireSemaphoreIndex = 0u;
        VkFormat m_colorFormat         = VK_FORMAT_B8G8R8A8_SRGB;
        Format   m_engineColorFormat   = Format::BGRA8_UNORM_SRGB;
        VkPresentModeKHR m_presentMode = VK_PRESENT_MODE_FIFO_KHR;
        std::vector<VkImage> m_images;
        std::vector<TextureHandle> m_backbufferTextures;
        std::vector<RenderTargetHandle> m_backbufferRTs;
        uint32_t m_width = 0u;
        uint32_t m_height = 0u;
        uint32_t m_bufferCount = 2u;
        uint32_t m_currentImageIndex = kInvalidImageIndex;
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
        uint64_t m_lastSubmittedValue = 0u;
        uint64_t m_completedValue = 0u;
    };

    class VulkanCommandList final : public ICommandList
    {
    public:
        VulkanCommandList(VulkanDevice& device, VulkanDeviceResources& resources, QueueType queueType);
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
        void Submit(const CommandSubmissionDesc& submission) override;
        void Submit(QueueType queue = QueueType::Graphics) override;
        void ReleaseQueueOwnership(BufferHandle buffer, QueueType dstQueue, ResourceState state) override;
        void AcquireQueueOwnership(BufferHandle buffer, QueueType srcQueue, ResourceState state) override;
        void ReleaseQueueOwnership(TextureHandle texture, QueueType dstQueue, ResourceState state) override;
        void AcquireQueueOwnership(TextureHandle texture, QueueType srcQueue, ResourceState state) override;
        void ReleaseQueueOwnership(RenderTargetHandle rt, QueueType dstQueue, ResourceState state) override;
        void AcquireQueueOwnership(RenderTargetHandle rt, QueueType srcQueue, ResourceState state) override;
        [[nodiscard]] QueueType GetQueueType() const override { return m_queueType; }
        [[nodiscard]] uint64_t GetLastSubmittedFenceValue() const override { return m_lastSubmittedFenceValue; }

    private:
        struct DescriptorArenaPool
        {
            VkDescriptorPool pool = VK_NULL_HANDLE;
            uint32_t allocatedSetCount = 0u;
            uint32_t maxSetCount = 0u;
        };

        struct DescriptorArena
        {
            std::vector<DescriptorArenaPool> pools;
            uint32_t activePoolCount = 0u;
            uint32_t maxSetsPerPool = 0u;
            uint64_t allocationEpoch = 0u;
            uint64_t lastCompletedFenceValue = 0u;
            uint64_t lastSubmittedFenceValue = 0u;
        };

        struct FrameContext
        {
            VkCommandPool commandPool = VK_NULL_HANDLE;
            VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
            DescriptorArena descriptorArena;
            VkDescriptorSet materializedDescriptorSet = VK_NULL_HANDLE;
            uint64_t materializedAllocationEpoch = 0u;
            DescriptorBindingState bindingState = BuildDefaultDescriptorBindingState();
            DescriptorMaterializationState materializationState = BuildDefaultDescriptorMaterializationState();
            DescriptorBindingInvalidationReason lastInvalidationReason = DescriptorBindingInvalidationReason::ExplicitDirty;
            bool descriptorsDirty = true;
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
        void BuildDescriptorBindingState(DescriptorBindingState& state) const;
        void BuildDescriptorMaterializationState(DescriptorMaterializationState& state);
        [[nodiscard]] VkImageLayout ResolveDescriptorImageLayout(TextureHandle texture) const;
        bool MaterializationStateMatches(const FrameContext& frame, const DescriptorMaterializationState& candidate) const;
        bool BindingStateMatches(const FrameContext& frame, const DescriptorBindingState& candidate) const;
        void ResetDescriptorState(FrameContext& frame);
        void TransitionTexture(TextureHandle texture, VkImageLayout newLayout, VkAccessFlags dstAccess, VkPipelineStageFlags dstStage, ResourceState targetState);
        void MarkTextureUsage(TextureHandle texture);
        void MarkRenderTargetUsage(RenderTargetHandle rt);
        void MarkBufferUsage(BufferHandle buffer);
        void StampSubmittedUsage(uint64_t submittedFenceValue);

        FrameContext& GetCurrentFrameContext();
        const FrameContext& GetCurrentFrameContext() const;

        VulkanDevice* m_device = nullptr;
        VulkanDeviceResources* m_resources = nullptr;
        QueueType m_queueType = QueueType::Graphics;
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
        uint64_t m_lastSubmittedFenceValue = 0u;
        std::vector<TextureHandle> m_touchedTextures;
        std::vector<RenderTargetHandle> m_touchedRenderTargets;
        std::vector<BufferHandle> m_touchedBuffers;
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
        [[nodiscard]] CommandListRuntimeDesc GetCommandListRuntime() const override;

        void UploadBufferData(BufferHandle handle, const void* data, size_t byteSize, size_t dstOffset = 0u) override;
        void UploadTextureData(TextureHandle handle, const void* data, size_t byteSize, uint32_t mipLevel = 0u, uint32_t arraySlice = 0u) override;

        void BeginFrame() override;
        void EndFrame() override;

        [[nodiscard]] uint32_t GetDrawCallCount() const override { return m_totalDrawCalls; }
        [[nodiscard]] const char* GetBackendName() const override { return "Vulkan"; }
        [[nodiscard]] assets::ShaderTargetProfile GetShaderTargetProfile() const override
        {
            return assets::ShaderTargetProfile::Vulkan_SPIRV;
        }
        [[nodiscard]] math::Mat4 GetShadowClipSpaceAdjustment() const override;
        [[nodiscard]] bool SupportsFeature(const char* feature) const override;
        [[nodiscard]] QueueCapabilities GetQueueCapabilities(QueueType queue) const override;
        [[nodiscard]] SwapchainRuntimeDesc GetSwapchainRuntime() const override;
        [[nodiscard]] QueueType GetPreferredUploadQueue() const override;
        [[nodiscard]] ResourceStateRecord QueryBufferState(BufferHandle handle) const override;
        [[nodiscard]] ResourceStateRecord QueryTextureState(TextureHandle handle) const override;
        [[nodiscard]] ResourceStateRecord QueryRenderTargetState(RenderTargetHandle handle) const override;
        [[nodiscard]] VkImageLayout GetAuthoritativeImageLayout(ResourceState state) const noexcept;
        [[nodiscard]] VkImageLayout GetAuthoritativeTextureLayout(const VulkanTextureEntry& texture) const noexcept;
        void SetAuthoritativeTextureState(VulkanTextureEntry& texture,
            ResourceState state,
            ResourceStateAuthority owner,
            uint64_t lastSubmissionFenceValue = 0u) noexcept;

        // Direkter mutabler Zugriff auf den internen Texture-Eintrag.
        // Nur für Initialisierungspfade (Fallback-Ressourcen, ImmediateSubmit-Lambdas).
        [[nodiscard]] VulkanTextureEntry* GetTextureEntryMutable(TextureHandle handle) noexcept
        {
            return m_resources.textures.Get(handle);
        }

        static std::vector<AdapterInfo> EnumerateAdaptersImpl();

        [[nodiscard]] VkInstance GetInstance() const noexcept { return m_instance; }
        [[nodiscard]] VkPhysicalDevice GetPhysicalDevice() const noexcept { return m_physicalDevice; }
        [[nodiscard]] VkDevice GetVkDevice() const noexcept { return m_device; }
        [[nodiscard]] VkQueue GetGraphicsQueue() const noexcept { return m_graphicsQueue; }
        [[nodiscard]] uint32_t GetGraphicsQueueFamily() const noexcept { return m_graphicsQueueFamily; }
        [[nodiscard]] VkQueue GetComputeQueue() const noexcept { return m_computeQueue; }
        [[nodiscard]] uint32_t GetComputeQueueFamily() const noexcept { return m_computeQueueFamily; }
        [[nodiscard]] VkQueue GetTransferQueue() const noexcept { return m_transferQueue; }
        [[nodiscard]] uint32_t GetTransferQueueFamily() const noexcept { return m_transferQueueFamily; }
        [[nodiscard]] VkDescriptorSetLayout GetGlobalSetLayout() const noexcept { return m_globalSetLayout; }
        [[nodiscard]] VkPipelineLayout GetPipelineLayout() const noexcept { return m_pipelineLayout; }
        [[nodiscard]] const PipelineBindingLayoutDesc& GetBindingLayout() const noexcept { return m_bindingLayout; }
        [[nodiscard]] const FrameDescriptorArenaDesc& GetFrameDescriptorArenaDesc() const noexcept { return m_frameDescriptorArenaDesc; }
        [[nodiscard]] DescriptorRuntimeLayoutDesc GetDescriptorRuntimeLayout() const override { return m_descriptorRuntimeLayout; }
        [[nodiscard]] uint32_t GetCurrentFrameSlot() const noexcept { return m_currentFrameSlot; }
        [[nodiscard]] VulkanDeviceResources& GetResources() noexcept { return m_resources; }
        [[nodiscard]] const VulkanDeviceResources& GetResources() const noexcept { return m_resources; }
        [[nodiscard]] VulkanSwapchain* GetActiveSwapchain() const noexcept { return m_activeSwapchain; }

        bool FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties, uint32_t& outMemoryTypeIndex) const;
        bool CreateImage(const TextureDesc& desc, VkFormat format,
            VkImageUsageFlags usage, VkImageAspectFlags aspect,
            VulkanTextureEntry& outEntry);
        bool EnsureImmediateUploadBuffer(VkDeviceSize requiredSize);
        void DestroyImmediateUploadBuffer() noexcept;
        void ImmediateSubmit(const std::function<void(VkCommandBuffer)>& fn);
        void ImmediateSubmit(QueueType queueType, const std::function<void(VkCommandBuffer)>& fn);
        void SetActiveSwapchain(VulkanSwapchain* swapchain) noexcept { m_activeSwapchain = swapchain; }
        [[nodiscard]] uint32_t GetFramesInFlight() const noexcept { return m_framesInFlight; }
        void EnsureFrameContextsForSwapchainImages(uint32_t swapchainImageCount);
        [[nodiscard]] VkFence GetCurrentFrameFence(QueueType queue) const noexcept;
        uint64_t AllocateSubmittedFenceValue(QueueType queue) noexcept;
        void MarkCurrentFrameSubmitted(QueueType queue, uint64_t fenceValue) noexcept;
        void RefreshCompletedFrameFences() noexcept;
        void WaitForFenceValue(uint64_t value, uint64_t timeoutNs = UINT64_MAX);
        [[nodiscard]] uint64_t GetCompletedFenceValue() const noexcept { return m_completedFenceValue; }
        bool BeginQueueFrameRecording(QueueType queue, const void* owner, uint64_t* blockingFenceValue = nullptr);
        bool EndQueueFrameRecording(QueueType queue, const void* owner);
        bool CanSubmitQueueFrame(QueueType queue, const void* owner) const;
        void ProcessPendingBufferDestroys() noexcept;
        void ProcessPendingTextureDestroys() noexcept;
        void ProcessPendingObjectDestroys() noexcept;
        void DeferDestroyShaderModule(VkShaderModule module, uint64_t retireAfterFence);
        void DeferDestroyPipeline(VkPipeline pipeline, uint64_t retireAfterFence);
        void DeferDestroySampler(VkSampler sampler, uint64_t retireAfterFence);
        void DeferDestroyImageView(VkImageView view, uint64_t retireAfterFence);
        void DeferDestroySemaphore(VkSemaphore semaphore, uint64_t retireAfterFence);
        void DeferDestroySwapchain(VkSwapchainKHR swapchain, uint64_t retireAfterFence);
        void DeferDestroyDescriptorPool(VkDescriptorPool pool, uint64_t retireAfterFence);
        void DeferDestroyCommandPool(VkCommandPool pool, uint64_t retireAfterFence);

        // Submission-signal semaphore registry: cross-submission queue synchronization.
        // FindSubmissionSignalSemaphore returns VK_NULL_HANDLE when the id is unknown.
        // CreateSubmissionSignalSemaphore allocates a new semaphore on first call for a given id;
        // subsequent calls for the same id return the existing semaphore.
        [[nodiscard]] VkSemaphore FindSubmissionSignalSemaphore(uint32_t submissionId) const noexcept;
        [[nodiscard]] VkSemaphore CreateSubmissionSignalSemaphore(uint32_t submissionId, QueueType queue) noexcept;
        [[nodiscard]] uint64_t GetSafeRetireFenceValue() const noexcept;
        [[nodiscard]] VkQueue GetQueueHandle(QueueType queue) const noexcept;
        [[nodiscard]] uint32_t GetQueueFamilyIndex(QueueType queue) const noexcept;

    private:
        bool CreateInstance(const DeviceDesc& desc);
        bool PickPhysicalDevice(uint32_t adapterIndex);
        bool CreateLogicalDevice(const DeviceDesc& desc);
        bool CreateGlobalDescriptors();
        void DestroyGlobalDescriptors();
        bool InitializeFrameContexts(uint32_t framesInFlight);
        void DestroyFrameContexts() noexcept;

        enum class QueueFrameLifecycleState : uint8_t
        {
            Idle = 0,
            Recording,
            Executable,
            Submitted,
        };

        struct QueueFrameContext
        {
            VkFence submitFence = VK_NULL_HANDLE;
            uint64_t submittedQueueFenceValue = 0u;
            uint64_t completedQueueFenceValue = 0u;
            uint64_t submittedExternalFenceValue = 0u;
            uint64_t frameIndexStamp = 0u;
            const void* owner = nullptr;
            QueueFrameLifecycleState lifecycleState = QueueFrameLifecycleState::Idle;
            bool inFlight = false;
        };

        struct FrameContext
        {
            std::array<QueueFrameContext, 3u> queues{};
        };

        struct ExternalFencePoint
        {
            QueueType queue = QueueType::Graphics;
            uint64_t queueFenceValue = 0u;
        };

        struct PendingBufferDestroy
        {
            VulkanBufferEntry entry{};
            uint64_t retireAfterFence = 0u;
        };

        struct PendingTextureDestroy
        {
            VulkanTextureEntry entry{};
            uint64_t retireAfterFence = 0u;
        };

        enum class PendingObjectKind : uint8_t
        {
            ShaderModule,
            Pipeline,
            Sampler,
            ImageView,
            Semaphore,
            Swapchain,
            DescriptorPool,
            CommandPool,
        };

        struct PendingObjectDestroy
        {
            PendingObjectKind kind = PendingObjectKind::ShaderModule;
            uint64_t retireAfterFence = 0u;
            uint64_t handle = 0u;
        };

        VkInstance m_instance = VK_NULL_HANDLE;
        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE;
        VkQueue m_graphicsQueue = VK_NULL_HANDLE;
        VkQueue m_computeQueue = VK_NULL_HANDLE;
        VkQueue m_transferQueue = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_globalSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
        PipelineBindingLayoutDesc m_bindingLayout = BuildEnginePipelineBindingLayout();
        FrameDescriptorArenaDesc m_frameDescriptorArenaDesc = BuildEngineFrameDescriptorArenaDesc();
        DescriptorRuntimeLayoutDesc m_descriptorRuntimeLayout = BuildEngineDescriptorRuntimeLayout();
        uint32_t m_graphicsQueueFamily = 0u;
        uint32_t m_computeQueueFamily = 0u;
        uint32_t m_transferQueueFamily = 0u;
        bool m_initialized = false;
        bool m_debugEnabled = false;
        uint64_t m_frameIndex = 0u;
        uint32_t m_currentFrameSlot = 0u;
        uint32_t m_framesInFlight = 3u;
        uint32_t m_totalDrawCalls = 0u;
        uint64_t m_completedFenceValue = 0u;
        uint64_t m_lastSubmittedFenceValue = 0u;
        uint64_t m_nextExternalFenceValue = 0u;
        std::array<uint64_t, 3u> m_lastSubmittedQueueFenceValues{};
        std::array<uint64_t, 3u> m_completedQueueFenceValues{};
        std::vector<ExternalFencePoint> m_externalFenceTimeline;
        VulkanSwapchain* m_activeSwapchain = nullptr;
        VulkanDeviceResources m_resources;
        std::vector<FrameContext> m_frameContexts;
        std::vector<PendingBufferDestroy> m_pendingBufferDestroys;
        std::vector<PendingTextureDestroy> m_pendingTextureDestroys;
        std::vector<PendingObjectDestroy> m_pendingObjectDestroys;
        std::unordered_map<uint32_t, VkSemaphore> m_submissionSignalSemaphores;
        VkPhysicalDeviceMemoryProperties m_memoryProperties{};
        VulkanBufferEntry m_immediateUploadBuffer{};
        VkDeviceSize m_immediateUploadCapacity = 0u;
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
