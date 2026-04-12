#include "VulkanDevice.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <array>
#include <cstring>

namespace engine::renderer::vulkan {

namespace {

VkImageAspectFlags AspectMaskForFormat(VkFormat format)
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

VkAccessFlags AccessMaskForLayout(VkImageLayout layout)
{
    switch (layout)
    {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        return VK_ACCESS_SHADER_READ_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        return VK_ACCESS_TRANSFER_READ_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        return VK_ACCESS_TRANSFER_WRITE_BIT;
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
        return 0u;
    case VK_IMAGE_LAYOUT_UNDEFINED:
    default:
        return 0u;
    }
}

VkPipelineStageFlags StageMaskForLayout(VkImageLayout layout)
{
    switch (layout)
    {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        return VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        return VK_PIPELINE_STAGE_TRANSFER_BIT;
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
        return VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    case VK_IMAGE_LAYOUT_UNDEFINED:
    default:
        return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    }
}

VkDescriptorType ToVkDescriptorType(DescriptorType type)
{
    switch (type)
    {
    case DescriptorType::ConstantBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    case DescriptorType::ShaderResource: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case DescriptorType::UnorderedAccess: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    case DescriptorType::Sampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
    default: return VK_DESCRIPTOR_TYPE_MAX_ENUM;
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

} // namespace

VulkanFence::VulkanFence(VulkanDevice& device, uint64_t initialValue)
    : m_device(&device), m_completedValue(initialValue), m_lastSubmittedValue(initialValue)
{
}

void VulkanFence::Signal(uint64_t value)
{
    m_lastSubmittedValue = std::max(m_lastSubmittedValue, value);
}

void VulkanFence::Wait(uint64_t value, uint64_t timeoutNs)
{
    if (!m_device)
        return;

    m_device->WaitForFenceValue(value, timeoutNs);
    m_completedValue = std::max(m_completedValue, m_device->GetCompletedFenceValue());
    m_lastSubmittedValue = std::max(m_lastSubmittedValue, value);
}

uint64_t VulkanFence::GetValue() const
{
    if (!m_device)
        return m_completedValue;

    m_device->RefreshCompletedFrameFences();
    return std::max(m_completedValue, m_device->GetCompletedFenceValue());
}

VulkanCommandList::VulkanCommandList(VulkanDevice& device, VulkanDeviceResources& resources, QueueType queueType)
    : m_device(&device), m_resources(&resources), m_queueType(queueType)
{
    m_frameCount = std::max(1u, device.GetFramesInFlight());
    m_frames.resize(m_frameCount);

    for (FrameContext& frame : m_frames)
    {
        VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.queueFamilyIndex = device.GetQueueFamilyIndex(queueType);
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        vkCreateCommandPool(device.GetVkDevice(), &poolInfo, nullptr, &frame.commandPool);

        VkCommandBufferAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocInfo.commandPool = frame.commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1u;
        vkAllocateCommandBuffers(device.GetVkDevice(), &allocInfo, &frame.commandBuffer);

        CreateDescriptorPool(frame);
    }

    BufferDesc cbDesc{};
    cbDesc.byteSize = kConstantBufferAlignment;
    cbDesc.type = BufferType::Constant;
    cbDesc.usage = ResourceUsage::ConstantBuffer | ResourceUsage::CopyDest;
    cbDesc.access = MemoryAccess::CpuWrite;
    cbDesc.debugName = "VulkanFallbackCB";
    m_fallbackCB = m_device->CreateBuffer(cbDesc);

    TextureDesc td{};
    td.width = 1u;
    td.height = 1u;
    td.format = Format::RGBA8_UNORM;
    td.usage = ResourceUsage::ShaderResource | ResourceUsage::CopyDest;
    td.initialState = ResourceState::ShaderRead;
    td.debugName = "VulkanFallbackTexture";
    m_fallbackTexture = m_device->CreateTexture(td);
    const uint32_t white = 0xffffffffu;
    m_device->UploadTextureData(m_fallbackTexture, &white, sizeof(white));

    SamplerDesc sd{};
    sd.addressU = sd.addressV = sd.addressW = WrapMode::Clamp;
    sd.minFilter = sd.magFilter = sd.mipFilter = FilterMode::Linear;
    m_fallbackSampler = m_device->CreateSampler(sd);
}

VulkanCommandList::~VulkanCommandList()
{
    if (m_device)
    {
        if (m_fallbackTexture.IsValid())
            m_device->DestroyTexture(m_fallbackTexture);
        if (m_fallbackCB.IsValid())
            m_device->DestroyBuffer(m_fallbackCB);
    }
    if (m_device && m_device->GetVkDevice())
    {
        m_device->RefreshCompletedFrameFences();
        const uint64_t retireFence = std::max(m_lastSubmittedFenceValue, m_device->GetSafeRetireFenceValue());
        for (FrameContext& frame : m_frames)
        {
            for (const DescriptorArenaPool& poolEntry : frame.descriptorArena.pools)
                if (poolEntry.pool)
                    m_device->DeferDestroyDescriptorPool(poolEntry.pool, retireFence);
            frame.descriptorArena.pools.clear();
            frame.descriptorArena.activePoolCount = 0u;
            if (frame.commandPool)
                m_device->DeferDestroyCommandPool(frame.commandPool, retireFence);
        }
    }
}

VulkanCommandList::FrameContext& VulkanCommandList::GetCurrentFrameContext()
{
    const uint32_t slot = m_frameCount > 0u ? (m_device->GetCurrentFrameSlot() % m_frameCount) : 0u;
    return m_frames[slot];
}


bool VulkanCommandList::CreateDescriptorPool(FrameContext& frame)
{
    const DescriptorRuntimeLayoutDesc runtimeLayout = m_device->GetDescriptorRuntimeLayout();
    std::string descriptorLayoutError;
    if (!ValidateDescriptorRuntimeLayout(runtimeLayout, &descriptorLayoutError))
    {
        Debug::LogError("VulkanCommandList[%s]: invalid engine descriptor runtime layout: %s",
                        QueueName(m_queueType),
                        descriptorLayoutError.c_str());
        return false;
    }

    const FrameDescriptorArenaDesc& arenaDesc = runtimeLayout.frameArena;
    std::vector<VkDescriptorPoolSize> sizes;
    sizes.reserve(4u);

    const auto addPoolSize = [&](DescriptorType type, uint32_t count)
    {
        if (count == 0u)
            return;
        VkDescriptorPoolSize size{};
        size.type = ToVkDescriptorType(type);
        size.descriptorCount = count;
        sizes.push_back(size);
    };

    const BindingHeapRuntimeDesc* resourceHeap = FindBindingHeapRuntimeDesc(runtimeLayout, BindingHeapKind::Resource);
    const BindingHeapRuntimeDesc* samplerHeap = FindBindingHeapRuntimeDesc(runtimeLayout, BindingHeapKind::Sampler);
    const uint32_t resourceSetBudget = resourceHeap ? resourceHeap->frameVisibleDescriptorCapacity : arenaDesc.resourceDescriptorCount;
    const uint32_t samplerSetBudget = samplerHeap ? samplerHeap->frameVisibleDescriptorCapacity : arenaDesc.samplerDescriptorCount;
    const uint32_t resourceDescriptorsPerSet = std::max(1u, CountHeapDescriptorsPerSet(runtimeLayout, BindingHeapKind::Resource));
    const uint32_t samplerDescriptorsPerSet = std::max(1u, CountHeapDescriptorsPerSet(runtimeLayout, BindingHeapKind::Sampler));
    const uint32_t maxTransientResourceSets = std::max(1u, resourceSetBudget / resourceDescriptorsPerSet);
    const uint32_t maxTransientSamplerSets = std::max(1u, samplerSetBudget / samplerDescriptorsPerSet);
    const uint32_t maxSetsPerPool = std::min(arenaDesc.maxSetsPerFrame, std::min(maxTransientResourceSets, maxTransientSamplerSets));

    addPoolSize(DescriptorType::ConstantBuffer,
                runtimeLayout.bindingLayout.CountDescriptors(BindingHeapKind::Resource, DescriptorType::ConstantBuffer) * maxSetsPerPool);
    addPoolSize(DescriptorType::ShaderResource,
                runtimeLayout.bindingLayout.CountDescriptors(BindingHeapKind::Resource, DescriptorType::ShaderResource) * maxSetsPerPool);
    addPoolSize(DescriptorType::UnorderedAccess,
                runtimeLayout.bindingLayout.CountDescriptors(BindingHeapKind::Resource, DescriptorType::UnorderedAccess) * maxSetsPerPool);
    addPoolSize(DescriptorType::Sampler,
                runtimeLayout.bindingLayout.CountDescriptors(BindingHeapKind::Sampler, DescriptorType::Sampler) * maxSetsPerPool);

    VkDescriptorPoolCreateInfo pool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pool.maxSets = maxSetsPerPool;
    pool.poolSizeCount = static_cast<uint32_t>(sizes.size());
    pool.pPoolSizes = sizes.data();
    pool.flags = 0u;

    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(m_device->GetVkDevice(), &pool, nullptr, &descriptorPool) != VK_SUCCESS)
    {
        Debug::LogError("VulkanCommandList: vkCreateDescriptorPool failed");
        return false;
    }

    frame.descriptorArena.maxSetsPerPool = maxSetsPerPool;
    frame.descriptorArena.pools.push_back(DescriptorArenaPool{ descriptorPool, 0u, maxSetsPerPool });
    frame.descriptorArena.activePoolCount = static_cast<uint32_t>(frame.descriptorArena.pools.size());
    return true;
}

const VulkanCommandList::FrameContext& VulkanCommandList::GetCurrentFrameContext() const
{
    const uint32_t slot = m_frameCount > 0u ? (m_device->GetCurrentFrameSlot() % m_frameCount) : 0u;
    return m_frames[slot];
}

VkDescriptorSet VulkanCommandList::AllocateDescriptorSet()
{
    auto& frame = GetCurrentFrameContext();
    VkDescriptorSetLayout layout = m_device->GetGlobalSetLayout();

    for (;;)
    {
        if (frame.descriptorArena.activePoolCount == 0u && !CreateDescriptorPool(frame))
            return VK_NULL_HANDLE;

        DescriptorArenaPool& currentPoolEntry = frame.descriptorArena.pools[frame.descriptorArena.activePoolCount - 1u];
        if (currentPoolEntry.allocatedSetCount >= currentPoolEntry.maxSetCount)
        {
            if (!CreateDescriptorPool(frame))
                return VK_NULL_HANDLE;
            continue;
        }

        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo alloc{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        alloc.descriptorPool = currentPoolEntry.pool;
        alloc.descriptorSetCount = 1u;
        alloc.pSetLayouts = &layout;

        const VkResult result = vkAllocateDescriptorSets(m_device->GetVkDevice(), &alloc, &set);
        if (result == VK_SUCCESS)
        {
            ++currentPoolEntry.allocatedSetCount;
            return set;
        }

        if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL)
        {
            Debug::LogError("VulkanCommandList: vkAllocateDescriptorSets failed (%d)", static_cast<int>(result));
            return VK_NULL_HANDLE;
        }

        if (!CreateDescriptorPool(frame))
            return VK_NULL_HANDLE;
    }
}

void VulkanCommandList::ResetDescriptorState(FrameContext& frame)
{
    frame.bindingState = BuildDefaultDescriptorBindingState();
    frame.materializationState = BuildDefaultDescriptorMaterializationState();
}

void VulkanCommandList::BuildDescriptorBindingState(DescriptorBindingState& snapshot) const
{
    snapshot = BuildDefaultDescriptorBindingState();
    for (uint32_t i = 0u; i < CBSlots::COUNT; ++i)
        snapshot.constantBuffers[i] = BufferBinding{ m_constantBuffers[i].buffer, m_constantBuffers[i].offset, m_constantBuffers[i].size };
    for (uint32_t i = 0u; i < TexSlots::COUNT; ++i)
        snapshot.textures[i] = m_textures[i];
    for (uint32_t i = 0u; i < SamplerSlots::COUNT; ++i)
        snapshot.samplers[i] = m_samplers[i];
}

void VulkanCommandList::BuildDescriptorMaterializationState(DescriptorMaterializationState& bindings)
{
    bindings = BuildDefaultDescriptorMaterializationState();

    for (uint32_t i = 0u; i < CBSlots::COUNT; ++i)
    {
        BufferHandle bufferHandle = m_constantBuffers[i].buffer;
        auto* buffer = m_resources->buffers.Get(bufferHandle);
        if (!buffer)
        {
            bufferHandle = m_fallbackCB;
            buffer = m_resources->buffers.Get(bufferHandle);
        }

        const uint32_t range = (m_constantBuffers[i].size != 0u)
            ? m_constantBuffers[i].size
            : (buffer ? static_cast<uint32_t>(buffer->byteSize) : 0u);
        bindings.constantBuffers[i] = BufferBinding{ bufferHandle, m_constantBuffers[i].offset, range };
    }

    for (uint32_t i = 0u; i < TexSlots::COUNT; ++i)
    {
        const TextureHandle textureHandle = m_textures[i].IsValid() ? m_textures[i] : m_fallbackTexture;

        bindings.textures[i] = textureHandle;
        auto* tex = m_resources->textures.Get(textureHandle);
        if (!tex)
            continue;

        bindings.textureStates[i] = tex->stateRecord.currentState;
        bindings.textureRevisionKeys[i] = (static_cast<uint64_t>(bindings.textureStates[i]) << 32u)
                                        ^ static_cast<uint64_t>(ResolveDescriptorImageLayout(textureHandle));
    }

    for (uint32_t i = 0u; i < SamplerSlots::COUNT; ++i)
    {
        uint32_t samplerIndex = m_samplers[i];
        if (samplerIndex >= m_resources->samplers.size())
            samplerIndex = m_fallbackSampler;
        bindings.samplers[i] = samplerIndex;
    }
}

VkImageLayout VulkanCommandList::ResolveDescriptorImageLayout(TextureHandle texture) const
{
    const auto* tex = m_resources->textures.Get(texture);
    if (!tex)
        return VK_IMAGE_LAYOUT_UNDEFINED;

    return m_device->GetAuthoritativeTextureLayout(*tex);
}

bool VulkanCommandList::BindingStateMatches(const FrameContext& frame, const DescriptorBindingState& candidate) const
{
    return DescriptorBindingStatesEqual(frame.bindingState, candidate);
}

bool VulkanCommandList::MaterializationStateMatches(const FrameContext& frame, const DescriptorMaterializationState& candidate) const
{
    return DescriptorMaterializationStatesEqual(frame.materializationState, candidate);
}

void VulkanCommandList::Begin()
{
    if (m_recording)
        End();

    uint64_t blockingFenceValue = 0u;
    if (!m_device->BeginQueueFrameRecording(m_queueType, this, &blockingFenceValue))
    {
        if (blockingFenceValue != 0u)
        {
            Debug::LogError("VulkanCommandList: queue/frame-slot allocator reuse blocked on fence %llu for queue %u",
                            static_cast<unsigned long long>(blockingFenceValue),
                            static_cast<unsigned>(m_queueType));
        }
        else
        {
            Debug::LogError("VulkanCommandList: queue/frame-slot recording contract violated for queue %u",
                            static_cast<unsigned>(m_queueType));
        }
        return;
    }

    auto& frame = GetCurrentFrameContext();
    vkResetCommandPool(m_device->GetVkDevice(), frame.commandPool, 0u);
    const uint32_t poolCount = static_cast<uint32_t>(frame.descriptorArena.pools.size());
    for (uint32_t i = 0u; i < poolCount; ++i)
    {
        DescriptorArenaPool& pool = frame.descriptorArena.pools[i];
        if (pool.pool)
            vkResetDescriptorPool(m_device->GetVkDevice(), pool.pool, 0u);
        pool.allocatedSetCount = 0u;
    }
    if (poolCount == 0u)
        CreateDescriptorPool(frame);
    frame.descriptorArena.activePoolCount = std::min<uint32_t>(1u, static_cast<uint32_t>(frame.descriptorArena.pools.size()));
    frame.descriptorArena.lastCompletedFenceValue = m_device->GetCompletedFenceValue();
    ++frame.descriptorArena.allocationEpoch;
    frame.materializedDescriptorSet = VK_NULL_HANDLE;
    frame.materializedAllocationEpoch = 0u;
    frame.descriptorsDirty = true;
    ResetDescriptorState(frame);

    VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);
    m_recording = true;
    m_insideRendering = false;
    m_currentRenderTarget = RenderTargetHandle::Invalid();
    m_currentPipeline = PipelineHandle::Invalid();

    std::memset(m_constantBuffers, 0, sizeof(m_constantBuffers));
    std::memset(m_textures, 0, sizeof(m_textures));
    std::memset(m_samplers, 0, sizeof(m_samplers));
    std::memset(m_vertexBuffers, 0, sizeof(m_vertexBuffers));
    std::memset(m_vertexOffsets, 0, sizeof(m_vertexOffsets));
    m_indexBuffer = BufferHandle::Invalid();
    m_indexOffset = 0u;
    m_indexType = VK_INDEX_TYPE_UINT32;
    m_touchedTextures.clear();
    m_touchedRenderTargets.clear();
    m_touchedBuffers.clear();

    m_viewport = {};
    m_scissor = {};
}

void VulkanCommandList::End()
{
    if (m_insideRendering)
        EndRenderPass();

    if (auto* swapchain = m_device->GetActiveSwapchain())
    {
        const TextureHandle backbuffer = swapchain->GetBackbufferTexture(swapchain->GetCurrentBackbufferIndex());
        if (backbuffer.IsValid())
        {
            TransitionTexture(backbuffer,
                              VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                              0u,
                              VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                              ResourceState::Present);
        }
    }

    vkEndCommandBuffer(GetCurrentFrameContext().commandBuffer);
    m_recording = false;
    if (!m_device->EndQueueFrameRecording(m_queueType, this))
    {
        Debug::LogError("VulkanCommandList: failed to finalize recording lifecycle for queue %u",
                        static_cast<unsigned>(m_queueType));
    }
}

void VulkanCommandList::MarkTextureUsage(TextureHandle texture)
{
    if (!texture.IsValid())
        return;
    if (std::find(m_touchedTextures.begin(), m_touchedTextures.end(), texture) == m_touchedTextures.end())
        m_touchedTextures.push_back(texture);
}

void VulkanCommandList::MarkRenderTargetUsage(RenderTargetHandle rt)
{
    if (!rt.IsValid())
        return;
    if (std::find(m_touchedRenderTargets.begin(), m_touchedRenderTargets.end(), rt) == m_touchedRenderTargets.end())
        m_touchedRenderTargets.push_back(rt);

    auto* entry = m_resources->renderTargets.Get(rt);
    if (!entry)
        return;
    if (entry->colorHandle.IsValid())
        MarkTextureUsage(entry->colorHandle);
    if (entry->depthHandle.IsValid())
        MarkTextureUsage(entry->depthHandle);
}

void VulkanCommandList::MarkBufferUsage(BufferHandle buffer)
{
    if (!buffer.IsValid())
        return;
    if (std::find(m_touchedBuffers.begin(), m_touchedBuffers.end(), buffer) == m_touchedBuffers.end())
        m_touchedBuffers.push_back(buffer);
}

void VulkanCommandList::StampSubmittedUsage(uint64_t submittedFenceValue)
{
    for (TextureHandle texture : m_touchedTextures)
    {
        auto* tex = m_resources->textures.Get(texture);
        if (!tex)
            continue;
        m_device->SetAuthoritativeTextureState(*tex,
                                               tex->stateRecord.currentState,
                                               ResourceStateAuthority::BackendResource,
                                               submittedFenceValue);
    }

    for (BufferHandle bufferHandle : m_touchedBuffers)
    {
        auto* buffer = m_resources->buffers.Get(bufferHandle);
        if (!buffer)
            continue;
        buffer->stateRecord.lastSubmissionFenceValue = std::max(buffer->stateRecord.lastSubmissionFenceValue, submittedFenceValue);
        buffer->stateRecord.authoritativeOwner = ResourceStateAuthority::BackendResource;
    }

    for (RenderTargetHandle renderTargetHandle : m_touchedRenderTargets)
    {
        auto* renderTarget = m_resources->renderTargets.Get(renderTargetHandle);
        if (!renderTarget)
            continue;

        if (renderTarget->colorHandle.IsValid())
        {
            auto* color = m_resources->textures.Get(renderTarget->colorHandle);
            if (color)
            {
                color->stateRecord.lastSubmissionFenceValue = std::max(color->stateRecord.lastSubmissionFenceValue, submittedFenceValue);
                color->stateRecord.authoritativeOwner = ResourceStateAuthority::BackendResource;
            }
        }

        if (renderTarget->depthHandle.IsValid())
        {
            auto* depth = m_resources->textures.Get(renderTarget->depthHandle);
            if (depth)
            {
                depth->stateRecord.lastSubmissionFenceValue = std::max(depth->stateRecord.lastSubmissionFenceValue, submittedFenceValue);
                depth->stateRecord.authoritativeOwner = ResourceStateAuthority::BackendResource;
            }
        }
    }
}

void VulkanCommandList::TransitionTexture(TextureHandle texture, VkImageLayout newLayout, VkAccessFlags dstAccess, VkPipelineStageFlags dstStage, ResourceState targetState)
{
    auto* tex = m_resources->textures.Get(texture);
    if (!tex)
        return;

    MarkTextureUsage(texture);
    const VkImageLayout oldLayout = m_device->GetAuthoritativeTextureLayout(*tex);
    if (tex->layout != oldLayout && tex->layout != VK_IMAGE_LAYOUT_UNDEFINED)
    {
        Debug::LogWarning("VulkanCommandList: texture layout mirror was out of sync with authoritative resource state; backend mirror corrected");
        tex->layout = oldLayout;
    }

    if (oldLayout == newLayout && !tex->contentsUndefined)
    {
        m_device->SetAuthoritativeTextureState(*tex,
                                               targetState,
                                               ResourceStateAuthority::BackendResource,
                                               tex->stateRecord.lastSubmissionFenceValue);
        return;
    }

    const bool allowDiscardTransition = tex->contentsUndefined && newLayout != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = allowDiscardTransition ? VK_IMAGE_LAYOUT_UNDEFINED : oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcAccessMask = allowDiscardTransition ? 0u : AccessMaskForLayout(oldLayout);
    barrier.dstAccessMask = dstAccess;
    barrier.image = tex->image;
    barrier.subresourceRange.aspectMask = AspectMaskForFormat(tex->format);
    barrier.subresourceRange.baseMipLevel = 0u;
    barrier.subresourceRange.levelCount = tex->mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0u;
    barrier.subresourceRange.layerCount = tex->arraySize;

    vkCmdPipelineBarrier(GetCurrentFrameContext().commandBuffer,
                         allowDiscardTransition ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : StageMaskForLayout(oldLayout),
                         dstStage,
                         0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
    tex->contentsUndefined = false;
    m_device->SetAuthoritativeTextureState(*tex,
                                           targetState,
                                           ResourceStateAuthority::BackendResource,
                                           tex->stateRecord.lastSubmissionFenceValue);
}

void VulkanCommandList::BeginRenderPass(const RenderPassBeginInfo& info)
{
    if (m_insideRendering)
        EndRenderPass();

    RenderTargetHandle rtHandle = info.renderTarget;
    if (!rtHandle.IsValid())
    {
        if (auto* swapchain = m_device->GetActiveSwapchain())
            rtHandle = swapchain->GetBackbufferRenderTarget(swapchain->GetCurrentBackbufferIndex());
    }

    auto* rt = m_resources->renderTargets.Get(rtHandle);
    if (!rt)
    {
        Debug::LogError("VulkanCommandList: invalid render target");
        return;
    }

    // Stale SRV-Bindings aus vorigen Passes bereinigen:
    // Wenn ein Texture-Handle noch in m_textures[] steht, der jetzt als
    // Color- oder Depth-Attachment eingehängt wird, würde FlushDescriptors
    // ihn als SAMPLED_IMAGE schreiben – entweder mit GENERAL (wenn der
    // RenderGraph ihn kurz auf Common gesetzt hat) oder mit einer Spec-
    // verletzung (Barrier auf aktives Attachment inside Dynamic Rendering).
    // Lösung: Slot vor BeginRenderPass invalidieren; der nächste Pass, der
    // die Texture wirklich lesen will, setzt sie erneut via SetShaderResource.
    {
        auto& frame = GetCurrentFrameContext();
        for (uint32_t i = 0u; i < TexSlots::COUNT; ++i)
        {
            if (!m_textures[i].IsValid())
                continue;
            if (m_textures[i] == rt->colorHandle || m_textures[i] == rt->depthHandle)
            {
                m_textures[i] = TextureHandle::Invalid();
                frame.descriptorsDirty = true;
            }
        }
    }

    m_currentRenderTarget = rtHandle;
    MarkRenderTargetUsage(rtHandle);

    VkRenderingAttachmentInfo colorAttachment{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingAttachmentInfo depthAttachment{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    if (rt->hasColor && rt->colorHandle.IsValid())
    {
        auto* color = m_resources->textures.Get(rt->colorHandle);
        TransitionTexture(rt->colorHandle, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          ResourceState::RenderTarget);

        colorAttachment.imageView = color->view;
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = info.clearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color = {{ info.colorClear.color[0], info.colorClear.color[1], info.colorClear.color[2], info.colorClear.color[3] }};
    }

    if (rt->hasDepth && rt->depthHandle.IsValid())
    {
        auto* depth = m_resources->textures.Get(rt->depthHandle);
        TransitionTexture(rt->depthHandle, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                          ResourceState::DepthWrite);

        depthAttachment.imageView = depth->view;
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = info.clearDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthAttachment.clearValue.depthStencil = { info.depthClear.depth, info.depthClear.stencil };
    }

    VkRenderingInfo rendering{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    rendering.renderArea.offset = { 0, 0 };
    rendering.renderArea.extent = { rt->width, rt->height };
    rendering.layerCount = 1u;
    if (rt->hasColor)
    {
        rendering.colorAttachmentCount = 1u;
        rendering.pColorAttachments = &colorAttachment;
    }
    if (rt->hasDepth)
        rendering.pDepthAttachment = &depthAttachment;

    PFN_vkCmdBeginRenderingKHR beginRenderingKHR = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(
        vkGetDeviceProcAddr(m_device->GetVkDevice(), "vkCmdBeginRenderingKHR"));
    if (beginRenderingKHR)
    {
        beginRenderingKHR(GetCurrentFrameContext().commandBuffer, reinterpret_cast<const VkRenderingInfoKHR*>(&rendering));
    }
    else
    {
        Debug::LogError("VulkanCommandList: vkCmdBeginRenderingKHR not available");
        return;
    }
    m_insideRendering = true;

    if (m_viewport.width <= 0.0f || m_viewport.height <= 0.0f)
        SetViewport(0.0f, 0.0f, static_cast<float>(rt->width), static_cast<float>(rt->height), 0.0f, 1.0f);
    if (m_scissor.extent.width == 0u || m_scissor.extent.height == 0u)
        SetScissor(0, 0, rt->width, rt->height);
}

void VulkanCommandList::EndRenderPass()
{
    if (!m_insideRendering)
        return;
    PFN_vkCmdEndRenderingKHR endRenderingKHR = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(
        vkGetDeviceProcAddr(m_device->GetVkDevice(), "vkCmdEndRenderingKHR"));
    if (!endRenderingKHR)
    {
        Debug::LogError("VulkanCommandList: vkCmdEndRenderingKHR not available");
        return;
    }
    endRenderingKHR(GetCurrentFrameContext().commandBuffer);
    m_insideRendering = false;
}

void VulkanCommandList::SetPipeline(PipelineHandle pipeline)
{
    auto* entry = m_resources->pipelines.Get(pipeline);
    if (!entry) return;
    m_currentPipeline = pipeline;
    const VkPipelineBindPoint bindPoint = entry->pipelineClass == PipelineClass::Compute
        ? VK_PIPELINE_BIND_POINT_COMPUTE
        : VK_PIPELINE_BIND_POINT_GRAPHICS;
    vkCmdBindPipeline(GetCurrentFrameContext().commandBuffer, bindPoint, entry->pipeline);
}

void VulkanCommandList::SetVertexBuffer(uint32_t slot, BufferHandle buffer, uint32_t offset)
{
    if (slot >= 8u) return;
    m_vertexBuffers[slot] = buffer;
    m_vertexOffsets[slot] = offset;
    MarkBufferUsage(buffer);
    auto* entry = m_resources->buffers.Get(buffer);
    if (!entry) return;
    VkBuffer vkbuf = entry->buffer;
    VkDeviceSize vkoff = offset;
    vkCmdBindVertexBuffers(GetCurrentFrameContext().commandBuffer, slot, 1u, &vkbuf, &vkoff);
}

void VulkanCommandList::SetIndexBuffer(BufferHandle buffer, bool is32bit, uint32_t offset)
{
    m_indexBuffer = buffer;
    m_indexOffset = offset;
    MarkBufferUsage(buffer);
    m_indexType = is32bit ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    auto* entry = m_resources->buffers.Get(buffer);
    if (!entry) return;
    vkCmdBindIndexBuffer(GetCurrentFrameContext().commandBuffer, entry->buffer, offset, m_indexType);
}

void VulkanCommandList::SetConstantBuffer(uint32_t slot, BufferHandle buffer, ShaderStageMask)
{
    if (slot >= CBSlots::COUNT) return;
    auto* entry = m_resources->buffers.Get(buffer);
    if (!entry) return;
    auto& frame = GetCurrentFrameContext();
    const uint32_t size = static_cast<uint32_t>(entry->byteSize);
    if (m_constantBuffers[slot].buffer != buffer || m_constantBuffers[slot].size != size)
        frame.descriptorsDirty = true;
    m_constantBuffers[slot].buffer = buffer;
    m_constantBuffers[slot].offset = 0u;
    m_constantBuffers[slot].size = size;
    MarkBufferUsage(buffer);
}

void VulkanCommandList::SetConstantBufferRange(uint32_t slot, BufferBinding binding, ShaderStageMask)
{
    if (slot >= CBSlots::COUNT || !binding.IsValid()) return;
    auto& frame = GetCurrentFrameContext();
    if (m_constantBuffers[slot].buffer != binding.buffer || m_constantBuffers[slot].size != binding.size || m_constantBuffers[slot].offset != binding.offset)
        frame.descriptorsDirty = true;
    m_constantBuffers[slot].buffer = binding.buffer;
    m_constantBuffers[slot].offset = binding.offset;
    m_constantBuffers[slot].size = binding.size;
    MarkBufferUsage(binding.buffer);
}

void VulkanCommandList::SetShaderResource(uint32_t slot, TextureHandle texture, ShaderStageMask)
{
    if (slot >= TexSlots::COUNT) return;
    if (m_textures[slot] != texture)
        GetCurrentFrameContext().descriptorsDirty = true;
    m_textures[slot] = texture;
    if (texture.IsValid())
        MarkTextureUsage(texture);
}

void VulkanCommandList::SetSampler(uint32_t slot, uint32_t samplerIndex, ShaderStageMask)
{
    if (slot >= SamplerSlots::COUNT) return;
    if (m_samplers[slot] != samplerIndex)
        GetCurrentFrameContext().descriptorsDirty = true;
    m_samplers[slot] = samplerIndex;
}

void VulkanCommandList::SetViewport(float x, float y, float width, float height, float minDepth, float maxDepth)
{
    m_viewport.x = x;
    m_viewport.y = y + height;
    m_viewport.width = width;
    m_viewport.height = -height;
    m_viewport.minDepth = minDepth;
    m_viewport.maxDepth = maxDepth;
    vkCmdSetViewport(GetCurrentFrameContext().commandBuffer, 0u, 1u, &m_viewport);
}

void VulkanCommandList::SetScissor(int32_t x, int32_t y, uint32_t width, uint32_t height)
{
    m_scissor.offset = { x, y };
    m_scissor.extent = { width, height };
    vkCmdSetScissor(GetCurrentFrameContext().commandBuffer, 0u, 1u, &m_scissor);
}

void VulkanCommandList::FlushDescriptors()
{
    auto& frame = GetCurrentFrameContext();

    std::array<VkDescriptorBufferInfo, CBSlots::COUNT> cbInfos{};
    std::array<VkDescriptorImageInfo, TexSlots::COUNT + SamplerSlots::COUNT> imageInfos{};
    std::array<uint32_t, CBSlots::COUNT> dynamicOffsets{};

    DescriptorBindingState currentSnapshot = BuildDefaultDescriptorBindingState();
    BuildDescriptorBindingState(currentSnapshot);

    for (uint32_t i = 0u; i < CBSlots::COUNT; ++i)
        dynamicOffsets[i] = currentSnapshot.constantBuffers[i].offset;

    for (uint32_t i = 0u; i < TexSlots::COUNT; ++i)
    {
        const TextureHandle boundTexture = currentSnapshot.textures[i].IsValid() ? currentSnapshot.textures[i] : m_fallbackTexture;
        auto* tex = m_resources->textures.Get(boundTexture);
        if (!tex)
            continue;
        // Swapchain-Images (ExternalSwapchain) dürfen nie als Shader-Resource
        // transitioniert werden.
        if (tex->stateRecord.authoritativeOwner == ResourceStateAuthority::ExternalSwapchain)
            continue;
        if (m_device->GetAuthoritativeTextureLayout(*tex) != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            TransitionTexture(boundTexture,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_ACCESS_SHADER_READ_BIT,
                              VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                              ResourceState::ShaderRead);
        }
    }

    DescriptorMaterializationState currentMaterialized = BuildDefaultDescriptorMaterializationState();
    BuildDescriptorMaterializationState(currentMaterialized);

    const bool allocationChanged = frame.materializedDescriptorSet == VK_NULL_HANDLE ||
                                   frame.materializedAllocationEpoch != frame.descriptorArena.allocationEpoch;
    const DescriptorBindingInvalidationReason invalidationReason = ComputeDescriptorBindingInvalidation(frame.bindingState,
                                                                                                        currentSnapshot,
                                                                                                        frame.materializationState,
                                                                                                        currentMaterialized,
                                                                                                        allocationChanged,
                                                                                                        frame.descriptorsDirty);
    frame.lastInvalidationReason = invalidationReason;
    frame.descriptorsDirty = invalidationReason != DescriptorBindingInvalidationReason::None;

    VkDescriptorSet descriptorSet = frame.materializedDescriptorSet;
    if (frame.descriptorsDirty)
    {
        Debug::Log("VulkanCommandList[%s]: rematerialize descriptors (%s)",
                   QueueName(m_queueType),
                   DescribeDescriptorBindingInvalidation(invalidationReason).c_str());
        descriptorSet = AllocateDescriptorSet();
        if (descriptorSet == VK_NULL_HANDLE)
        {
            Debug::LogError("VulkanCommandList: failed to allocate descriptor set");
            return;
        }

        std::vector<VkWriteDescriptorSet> writes;
        for (uint32_t i = 0u; i < CBSlots::COUNT; ++i)
        {
            auto* buffer = m_resources->buffers.Get(currentMaterialized.constantBuffers[i].buffer);
            if (!buffer)
                continue;

            cbInfos[i].buffer = buffer->buffer;
            cbInfos[i].offset = 0u;
            cbInfos[i].range = currentMaterialized.constantBuffers[i].size == 0u ? buffer->byteSize : currentMaterialized.constantBuffers[i].size;

            VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = descriptorSet;
            write.dstBinding = BindingRegisterRanges::CB(i);
            write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            write.descriptorCount = 1u;
            write.pBufferInfo = &cbInfos[i];
            writes.push_back(write);
        }

        for (uint32_t i = 0u; i < TexSlots::COUNT; ++i)
        {
            auto* tex = m_resources->textures.Get(currentMaterialized.textures[i]);
            if (!tex)
                continue;

            imageInfos[i].imageLayout = ResolveDescriptorImageLayout(currentMaterialized.textures[i]);
            imageInfos[i].imageView = tex->view;

            VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = descriptorSet;
            write.dstBinding = BindingRegisterRanges::SRV(i);
            write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            write.descriptorCount = 1u;
            write.pImageInfo = &imageInfos[i];
            writes.push_back(write);
        }

        for (uint32_t i = 0u; i < SamplerSlots::COUNT; ++i)
        {
            const uint32_t samplerIndex = currentMaterialized.samplers[i];
            if (samplerIndex >= m_resources->samplers.size())
                continue;

            imageInfos[TexSlots::COUNT + i].sampler = m_resources->samplers[samplerIndex].sampler;

            VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = descriptorSet;
            write.dstBinding = BindingRegisterRanges::SMP(i);
            write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            write.descriptorCount = 1u;
            write.pImageInfo = &imageInfos[TexSlots::COUNT + i];
            writes.push_back(write);
        }

        if (!writes.empty())
            vkUpdateDescriptorSets(m_device->GetVkDevice(), static_cast<uint32_t>(writes.size()), writes.data(), 0u, nullptr);

        frame.materializedDescriptorSet = descriptorSet;
        frame.materializedAllocationEpoch = frame.descriptorArena.allocationEpoch;
        frame.bindingState = currentSnapshot;
        frame.materializationState = currentMaterialized;
        frame.descriptorsDirty = false;
    }

    VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (const auto* currentPipeline = m_resources->pipelines.Get(m_currentPipeline))
        bindPoint = currentPipeline->pipelineClass == PipelineClass::Compute
            ? VK_PIPELINE_BIND_POINT_COMPUTE
            : VK_PIPELINE_BIND_POINT_GRAPHICS;

    vkCmdBindDescriptorSets(GetCurrentFrameContext().commandBuffer,
                            bindPoint,
                            m_device->GetPipelineLayout(),
                            0u,
                            1u,
                            &descriptorSet,
                            m_device->GetBindingLayout().CountDynamicOffsets(),
                            dynamicOffsets.data());
}

void VulkanCommandList::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{
    FlushDescriptors();
    vkCmdDraw(GetCurrentFrameContext().commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
}

void VulkanCommandList::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{
    FlushDescriptors();
    vkCmdDrawIndexed(GetCurrentFrameContext().commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void VulkanCommandList::Dispatch(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ)
{
    const auto* entry = m_resources->pipelines.Get(m_currentPipeline);
    if (!entry || entry->pipelineClass != PipelineClass::Compute)
    {
        Debug::LogWarning("VulkanCommandList: Dispatch ignored without bound compute pipeline");
        return;
    }

    FlushDescriptors();
    VkCommandBuffer cmd = GetCurrentFrameContext().commandBuffer;
    vkCmdDispatch(cmd, groupsX, groupsY, groupsZ);

    VkMemoryBarrier uavBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    uavBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    uavBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0u,
                         1u, &uavBarrier,
                         0u, nullptr,
                         0u, nullptr);
}

void VulkanCommandList::TransitionResource(BufferHandle buffer, ResourceState before, ResourceState after)
{
    if (after == ResourceState::Unknown)
        return;

    auto* entry = m_resources->buffers.Get(buffer);
    if (!entry || entry->buffer == VK_NULL_HANDLE)
        return;

    const ResourceState beforeState = before != ResourceState::Unknown ? before : entry->stateRecord.currentState;
    if (beforeState == after)
        return;

    VkBufferMemoryBarrier barrier{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    barrier.srcAccessMask = ToVkAccessFlags(beforeState);
    barrier.dstAccessMask = ToVkAccessFlags(after);
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = entry->buffer;
    barrier.offset = 0u;
    barrier.size = entry->byteSize;

    vkCmdPipelineBarrier(GetCurrentFrameContext().commandBuffer,
                         ToVkPipelineStage(beforeState),
                         ToVkPipelineStage(after),
                         0u,
                         0u, nullptr,
                         1u, &barrier,
                         0u, nullptr);

    entry->stateRecord.currentState = after;
    entry->stateRecord.authoritativeOwner = ResourceStateAuthority::BackendResource;
}

void VulkanCommandList::TransitionResource(TextureHandle texture, ResourceState before, ResourceState after)
{
    if (after == ResourceState::Unknown)
        return;

    // ResourceState::Common ist ein DX12-Konzept ("relaxed" Layout ohne spezifischen
    // Zugriffstyp). In Vulkan gibt es kein entsprechendes Layout – der nächste
    // konkrete Use-Transition (z.B. Common→RenderTarget) weiß selbst, welchen
    // Übergang er braucht. Einen expliziten Barrier nach GENERAL zu emittieren
    // verschmutzt das Layout-Tracking und führt dazu, dass Descriptor-Writes
    // VK_IMAGE_LAYOUT_GENERAL eintragen, obwohl das Image kurz danach in
    // SHADER_READ_ONLY_OPTIMAL liegt.
    // Lösung: Common als "behalte aktuelles Layout" behandeln – kein Barrier nötig.
    if (after == ResourceState::Common)
        return;

    auto* tex = m_resources->textures.Get(texture);
    if (!tex)
        return;

    // before=Common ebenfalls ignorieren: wir vertrauen dem aktuellen tex->layout
    // statt es auf GENERAL zu setzen. Der nächste echte Barrier (z.B. nach
    // COLOR_ATTACHMENT_OPTIMAL) kennt dann das korrekte Ausgangslayout.
    if (before != ResourceState::Unknown && before != ResourceState::Common)
    {
        tex->stateRecord.currentState = before;
        const VkImageLayout explicitLayout = m_device->GetAuthoritativeImageLayout(before);
        if (explicitLayout != VK_IMAGE_LAYOUT_UNDEFINED)
            tex->layout = explicitLayout;
    }

    TransitionTexture(texture, ToVkImageLayout(after), ToVkAccessFlags(after), ToVkPipelineStage(after), after);
}

void VulkanCommandList::TransitionRenderTarget(RenderTargetHandle rt, ResourceState, ResourceState after)
{
    auto* entry = m_resources->renderTargets.Get(rt);
    if (!entry) return;
    if (entry->colorHandle.IsValid())
    {
        const ResourceState colorState = (after == ResourceState::DepthWrite || after == ResourceState::DepthRead) ? ResourceState::RenderTarget : after;
        TransitionResource(entry->colorHandle, ResourceState::Unknown, colorState);
    }
    if (entry->depthHandle.IsValid())
    {
        ResourceState depthState = after;
        if (after == ResourceState::RenderTarget)
            depthState = ResourceState::DepthWrite;
        TransitionResource(entry->depthHandle, ResourceState::Unknown, depthState);
    }
}

void VulkanCommandList::CopyBuffer(BufferHandle dst, uint64_t dstOffset, BufferHandle src, uint64_t srcOffset, uint64_t size)
{
    auto* dstBuffer = m_resources->buffers.Get(dst);
    auto* srcBuffer = m_resources->buffers.Get(src);
    MarkBufferUsage(dst);
    MarkBufferUsage(src);
    if (!dstBuffer || !srcBuffer) return;
    VkBufferCopy region{ srcOffset, dstOffset, size };
    vkCmdCopyBuffer(GetCurrentFrameContext().commandBuffer, srcBuffer->buffer, dstBuffer->buffer, 1u, &region);
}

void VulkanCommandList::CopyTexture(TextureHandle dst, uint32_t, TextureHandle src, uint32_t)
{
    auto* dstTex = m_resources->textures.Get(dst);
    auto* srcTex = m_resources->textures.Get(src);
    if (!dstTex || !srcTex) return;

    TransitionTexture(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, ResourceState::CopySource);
    TransitionTexture(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, ResourceState::CopyDest);

    VkImageCopy copy{};
    copy.srcSubresource.aspectMask = AspectMaskForFormat(srcTex->format);
    copy.srcSubresource.layerCount = 1u;
    copy.dstSubresource.aspectMask = AspectMaskForFormat(dstTex->format);
    copy.dstSubresource.layerCount = 1u;
    copy.extent.width = std::min(srcTex->width, dstTex->width);
    copy.extent.height = std::min(srcTex->height, dstTex->height);
    copy.extent.depth = 1u;
    vkCmdCopyImage(GetCurrentFrameContext().commandBuffer,
                   srcTex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dstTex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1u, &copy);
}

void VulkanCommandList::ReleaseQueueOwnership(BufferHandle buffer, QueueType dstQueue, ResourceState state)
{
    auto* entry = m_resources->buffers.Get(buffer);
    if (!entry || entry->buffer == VK_NULL_HANDLE)
        return;

    const uint32_t srcFamily = m_device->GetQueueFamilyIndex(m_queueType);
    const uint32_t dstFamily = m_device->GetQueueFamilyIndex(dstQueue);
    if (srcFamily == dstFamily || entry->owningQueueFamily == dstFamily)
        return;
    if (entry->owningQueueFamily != VK_QUEUE_FAMILY_IGNORED && entry->owningQueueFamily != srcFamily)
        return;

    VkBufferMemoryBarrier barrier{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    barrier.srcAccessMask = ToVkAccessFlags(state);
    barrier.dstAccessMask = 0u;
    barrier.srcQueueFamilyIndex = srcFamily;
    barrier.dstQueueFamilyIndex = dstFamily;
    barrier.buffer = entry->buffer;
    barrier.offset = 0u;
    barrier.size = entry->byteSize;

    vkCmdPipelineBarrier(GetCurrentFrameContext().commandBuffer,
                         ToVkPipelineStage(state),
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0u,
                         0u, nullptr,
                         1u, &barrier,
                         0u, nullptr);
}

void VulkanCommandList::AcquireQueueOwnership(BufferHandle buffer, QueueType srcQueue, ResourceState state)
{
    auto* entry = m_resources->buffers.Get(buffer);
    if (!entry || entry->buffer == VK_NULL_HANDLE)
        return;

    const uint32_t srcFamily = m_device->GetQueueFamilyIndex(srcQueue);
    const uint32_t dstFamily = m_device->GetQueueFamilyIndex(m_queueType);
    if (srcFamily == dstFamily)
    {
        entry->owningQueueFamily = dstFamily;
        return;
    }
    if (entry->owningQueueFamily == dstFamily)
        return;

    VkBufferMemoryBarrier barrier{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    barrier.srcAccessMask = 0u;
    barrier.dstAccessMask = ToVkAccessFlags(state);
    barrier.srcQueueFamilyIndex = srcFamily;
    barrier.dstQueueFamilyIndex = dstFamily;
    barrier.buffer = entry->buffer;
    barrier.offset = 0u;
    barrier.size = entry->byteSize;

    vkCmdPipelineBarrier(GetCurrentFrameContext().commandBuffer,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         ToVkPipelineStage(state),
                         0u,
                         0u, nullptr,
                         1u, &barrier,
                         0u, nullptr);
    entry->owningQueueFamily = dstFamily;
}

void VulkanCommandList::ReleaseQueueOwnership(TextureHandle texture, QueueType dstQueue, ResourceState state)
{
    auto* entry = m_resources->textures.Get(texture);
    if (!entry || entry->image == VK_NULL_HANDLE)
        return;

    const uint32_t srcFamily = m_device->GetQueueFamilyIndex(m_queueType);
    const uint32_t dstFamily = m_device->GetQueueFamilyIndex(dstQueue);
    if (srcFamily == dstFamily || entry->owningQueueFamily == dstFamily)
        return;
    if (entry->owningQueueFamily != VK_QUEUE_FAMILY_IGNORED && entry->owningQueueFamily != srcFamily)
        return;

    VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = m_device->GetAuthoritativeTextureLayout(*entry);
    barrier.newLayout = barrier.oldLayout;
    barrier.srcAccessMask = ToVkAccessFlags(state);
    barrier.dstAccessMask = 0u;
    barrier.srcQueueFamilyIndex = srcFamily;
    barrier.dstQueueFamilyIndex = dstFamily;
    barrier.image = entry->image;
    barrier.subresourceRange.aspectMask = AspectMaskForFormat(entry->format);
    barrier.subresourceRange.baseMipLevel = 0u;
    barrier.subresourceRange.levelCount = entry->mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0u;
    barrier.subresourceRange.layerCount = entry->arraySize;

    vkCmdPipelineBarrier(GetCurrentFrameContext().commandBuffer,
                         ToVkPipelineStage(state),
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0u,
                         0u, nullptr,
                         0u, nullptr,
                         1u, &barrier);
}

void VulkanCommandList::AcquireQueueOwnership(TextureHandle texture, QueueType srcQueue, ResourceState state)
{
    auto* entry = m_resources->textures.Get(texture);
    if (!entry || entry->image == VK_NULL_HANDLE)
        return;

    const uint32_t srcFamily = m_device->GetQueueFamilyIndex(srcQueue);
    const uint32_t dstFamily = m_device->GetQueueFamilyIndex(m_queueType);
    if (srcFamily == dstFamily)
    {
        entry->owningQueueFamily = dstFamily;
        return;
    }
    if (entry->owningQueueFamily == dstFamily)
        return;

    VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = m_device->GetAuthoritativeTextureLayout(*entry);
    barrier.newLayout = barrier.oldLayout;
    barrier.srcAccessMask = 0u;
    barrier.dstAccessMask = ToVkAccessFlags(state);
    barrier.srcQueueFamilyIndex = srcFamily;
    barrier.dstQueueFamilyIndex = dstFamily;
    barrier.image = entry->image;
    barrier.subresourceRange.aspectMask = AspectMaskForFormat(entry->format);
    barrier.subresourceRange.baseMipLevel = 0u;
    barrier.subresourceRange.levelCount = entry->mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0u;
    barrier.subresourceRange.layerCount = entry->arraySize;

    vkCmdPipelineBarrier(GetCurrentFrameContext().commandBuffer,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         ToVkPipelineStage(state),
                         0u,
                         0u, nullptr,
                         0u, nullptr,
                         1u, &barrier);
    entry->owningQueueFamily = dstFamily;
}

void VulkanCommandList::ReleaseQueueOwnership(RenderTargetHandle rt, QueueType dstQueue, ResourceState state)
{
    auto* entry = m_resources->renderTargets.Get(rt);
    if (!entry)
        return;
    if (entry->colorHandle.IsValid())
        ReleaseQueueOwnership(entry->colorHandle, dstQueue, state == ResourceState::DepthRead || state == ResourceState::DepthWrite ? ResourceState::RenderTarget : state);
    if (entry->depthHandle.IsValid())
        ReleaseQueueOwnership(entry->depthHandle, dstQueue, state == ResourceState::RenderTarget ? ResourceState::DepthWrite : state);
}

void VulkanCommandList::AcquireQueueOwnership(RenderTargetHandle rt, QueueType srcQueue, ResourceState state)
{
    auto* entry = m_resources->renderTargets.Get(rt);
    if (!entry)
        return;
    if (entry->colorHandle.IsValid())
        AcquireQueueOwnership(entry->colorHandle, srcQueue, state == ResourceState::DepthRead || state == ResourceState::DepthWrite ? ResourceState::RenderTarget : state);
    if (entry->depthHandle.IsValid())
        AcquireQueueOwnership(entry->depthHandle, srcQueue, state == ResourceState::RenderTarget ? ResourceState::DepthWrite : state);
}

void VulkanCommandList::Submit(QueueType queue)
{
    Submit(CommandSubmissionDesc{queue, queue == QueueType::Graphics, queue == QueueType::Graphics});
}

void VulkanCommandList::Submit(const CommandSubmissionDesc& submission)
{
    m_lastSubmittedFenceValue = 0u;
    if (m_recording)
        End();

    if (!m_device->CanSubmitQueueFrame(m_queueType, this))
    {
        Debug::LogError("VulkanCommandList: submit rejected because command-list lifecycle is not executable for queue %u",
                        static_cast<unsigned>(m_queueType));
        return;
    }

    auto& frame = GetCurrentFrameContext();
    auto* swapchain = m_device->GetActiveSwapchain();
    QueueType queueType = submission.queue;
    if (queueType != m_queueType)
    {
        Debug::LogWarning("VulkanCommandList: submission queue %u differs from command-list queue %u - using command-list queue", static_cast<unsigned>(queueType), static_cast<unsigned>(m_queueType));
        queueType = m_queueType;
    }

    const VkQueue submitQueue = m_device->GetQueueHandle(queueType);
    if (submitQueue == VK_NULL_HANDLE)
    {
        Debug::LogError("VulkanCommandList: no native queue available for queue type %u", static_cast<unsigned>(queueType));
        return;
    }
    const VkPipelineStageFlags waitStage = queueType == QueueType::Compute
        ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
        : (queueType == QueueType::Transfer ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1u;
    VkCommandBuffer commandBuffer = frame.commandBuffer;
    submit.pCommandBuffers = &commandBuffer;

    std::vector<VkSemaphore> waitSemaphores;
    std::vector<VkPipelineStageFlags> waitStages;
    std::vector<VkSemaphore> signalSemaphores;

    for (uint32_t producerSubmissionId : submission.waitSubmissionIds)
    {
        const VkSemaphore semaphore = m_device->FindSubmissionSignalSemaphore(producerSubmissionId);
        if (semaphore == VK_NULL_HANDLE)
            continue;
        waitSemaphores.push_back(semaphore);
        waitStages.push_back(waitStage);
    }

    const bool useSwapchainSync = queueType == QueueType::Graphics && submission.waitForSwapchainAcquire;
    if (useSwapchainSync && swapchain && swapchain->HasAcquiredImage())
    {
        const VkSemaphore acquireSemaphore = swapchain->GetImageAvailableSemaphore();
        if (acquireSemaphore != VK_NULL_HANDLE)
        {
            waitSemaphores.push_back(acquireSemaphore);
            waitStages.push_back(waitStage);
        }
        if (submission.signalSwapchainReady)
        {
            const VkSemaphore renderFinished = swapchain->GetRenderFinishedSemaphore();
            if (renderFinished != VK_NULL_HANDLE)
                signalSemaphores.push_back(renderFinished);
        }
    }

    if (submission.exportsQueueSignal)
    {
        const VkSemaphore queueSignalSemaphore = m_device->CreateSubmissionSignalSemaphore(submission.submissionId, queueType);
        if (queueSignalSemaphore != VK_NULL_HANDLE)
            signalSemaphores.push_back(queueSignalSemaphore);
    }

    if (!waitSemaphores.empty())
    {
        submit.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
        submit.pWaitSemaphores = waitSemaphores.data();
        submit.pWaitDstStageMask = waitStages.data();
    }
    if (!signalSemaphores.empty())
    {
        submit.signalSemaphoreCount = static_cast<uint32_t>(signalSemaphores.size());
        submit.pSignalSemaphores = signalSemaphores.data();
    }

    const VkFence submitFence = m_device->GetCurrentFrameFence(queueType);
    if (submitFence != VK_NULL_HANDLE)
        vkResetFences(m_device->GetVkDevice(), 1u, &submitFence);

    const VkResult result = vkQueueSubmit(submitQueue, 1u, &submit, submitFence);
    if (result != VK_SUCCESS)
    {
        Debug::LogError("VulkanCommandList: vkQueueSubmit failed (%d)", static_cast<int>(result));
        return;
    }

    const uint64_t submittedFenceValue = m_device->AllocateSubmittedFenceValue(queueType);
    m_lastSubmittedFenceValue = submittedFenceValue;
    StampSubmittedUsage(submittedFenceValue);
    GetCurrentFrameContext().descriptorArena.lastSubmittedFenceValue = submittedFenceValue;
    m_device->MarkCurrentFrameSubmitted(queueType, submittedFenceValue);
    if (useSwapchainSync && swapchain && swapchain->HasAcquiredImage())
        swapchain->NotifyCurrentImageSubmitted(submittedFenceValue);
}

} // namespace engine::renderer::vulkan
