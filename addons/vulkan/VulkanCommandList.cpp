#include "VulkanDevice.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <array>
#include <cstring>

namespace engine::renderer::vulkan {

VulkanFence::VulkanFence(VulkanDevice& device, uint64_t initialValue)
    : m_device(&device), m_value(initialValue)
{
}

void VulkanFence::Signal(uint64_t value)
{
    m_value = std::max(m_value, value);
    if (m_device)
        m_device->MarkCurrentFrameSubmitted(value);
}

void VulkanFence::Wait(uint64_t value, uint64_t timeoutNs)
{
    if (!m_device)
        return;

    m_device->WaitForFenceValue(value, timeoutNs);
    m_value = std::max(m_value, m_device->GetCompletedFenceValue());
}

uint64_t VulkanFence::GetValue() const
{
    if (!m_device)
        return m_value;

    m_device->RefreshCompletedFrameFences();
    return std::max(m_value, m_device->GetCompletedFenceValue());
}

VulkanCommandList::VulkanCommandList(VulkanDevice& device, VulkanDeviceResources& resources)
    : m_device(&device), m_resources(&resources)
{
    m_frameCount = std::max(1u, device.GetFramesInFlight());
    m_frames.resize(m_frameCount);

    for (FrameContext& frame : m_frames)
    {
        VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.queueFamilyIndex = device.GetGraphicsQueueFamily();
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
        for (FrameContext& frame : m_frames)
        {
            for (VkDescriptorPool pool : frame.descriptorPools)
                if (pool)
                    vkDestroyDescriptorPool(m_device->GetVkDevice(), pool, nullptr);
            frame.descriptorPools.clear();
            frame.activeDescriptorPoolCount = 0u;
            if (frame.commandPool)
                vkDestroyCommandPool(m_device->GetVkDevice(), frame.commandPool, nullptr);
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
    std::array<VkDescriptorPoolSize, 3> sizes{};
    sizes[0] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, CBSlots::COUNT * 256u };
    sizes[1] = { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, TexSlots::COUNT * 256u };
    sizes[2] = { VK_DESCRIPTOR_TYPE_SAMPLER, SamplerSlots::COUNT * 256u };

    VkDescriptorPoolCreateInfo pool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pool.maxSets = 256u;
    pool.poolSizeCount = static_cast<uint32_t>(sizes.size());
    pool.pPoolSizes = sizes.data();
    pool.flags = 0u;

    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(m_device->GetVkDevice(), &pool, nullptr, &descriptorPool) != VK_SUCCESS)
    {
        Debug::LogError("VulkanCommandList: vkCreateDescriptorPool failed");
        return false;
    }

    frame.descriptorPools.push_back(descriptorPool);
    frame.activeDescriptorPoolCount = static_cast<uint32_t>(frame.descriptorPools.size());
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
        if (frame.activeDescriptorPoolCount == 0u && !CreateDescriptorPool(frame))
            return VK_NULL_HANDLE;

        VkDescriptorPool currentPool = frame.descriptorPools[frame.activeDescriptorPoolCount - 1u];
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo alloc{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        alloc.descriptorPool = currentPool;
        alloc.descriptorSetCount = 1u;
        alloc.pSetLayouts = &layout;

        const VkResult result = vkAllocateDescriptorSets(m_device->GetVkDevice(), &alloc, &set);
        if (result == VK_SUCCESS)
            return set;

        if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL)
        {
            Debug::LogError("VulkanCommandList: vkAllocateDescriptorSets failed (%d)", static_cast<int>(result));
            return VK_NULL_HANDLE;
        }

        if (!CreateDescriptorPool(frame))
            return VK_NULL_HANDLE;
    }
}

void VulkanCommandList::Begin()
{
    auto& frame = GetCurrentFrameContext();
    vkResetCommandPool(m_device->GetVkDevice(), frame.commandPool, 0u);
    const uint32_t poolCount = static_cast<uint32_t>(frame.descriptorPools.size());
    for (uint32_t i = 0u; i < poolCount; ++i)
        vkResetDescriptorPool(m_device->GetVkDevice(), frame.descriptorPools[i], 0u);
    if (poolCount == 0u)
        CreateDescriptorPool(frame);
    frame.activeDescriptorPoolCount = std::min<uint32_t>(1u, static_cast<uint32_t>(frame.descriptorPools.size()));

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
                              VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        }
    }

    vkEndCommandBuffer(GetCurrentFrameContext().commandBuffer);
    m_recording = false;
}

void VulkanCommandList::TransitionTexture(TextureHandle texture, VkImageLayout newLayout, VkAccessFlags dstAccess, VkPipelineStageFlags dstStage)
{
    auto* tex = m_resources->textures.Get(texture);
    if (!tex || tex->layout == newLayout)
        return;

    VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = tex->layout;
    barrier.newLayout = newLayout;
    barrier.srcAccessMask = 0u;
    barrier.dstAccessMask = dstAccess;
    barrier.image = tex->image;
    barrier.subresourceRange.aspectMask = tex->aspect;
    barrier.subresourceRange.levelCount = tex->mipLevels;
    barrier.subresourceRange.layerCount = 1u;

    vkCmdPipelineBarrier(GetCurrentFrameContext().commandBuffer,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         dstStage,
                         0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
    tex->layout = newLayout;
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

    VkRenderingAttachmentInfo colorAttachment{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingAttachmentInfo depthAttachment{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    std::vector<VkClearValue> clearValues;

    if (rt->hasColor && rt->colorHandle.IsValid())
    {
        auto* color = m_resources->textures.Get(rt->colorHandle);
        TransitionTexture(rt->colorHandle, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

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
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);

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
    vkCmdBindPipeline(GetCurrentFrameContext().commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, entry->pipeline);
}

void VulkanCommandList::SetVertexBuffer(uint32_t slot, BufferHandle buffer, uint32_t offset)
{
    if (slot >= 8u) return;
    m_vertexBuffers[slot] = buffer;
    m_vertexOffsets[slot] = offset;
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
    m_constantBuffers[slot].buffer = buffer;
    m_constantBuffers[slot].offset = 0u;
    m_constantBuffers[slot].size = static_cast<uint32_t>(entry->byteSize);
}

void VulkanCommandList::SetConstantBufferRange(uint32_t slot, BufferBinding binding, ShaderStageMask)
{
    if (slot >= CBSlots::COUNT || !binding.IsValid()) return;
    m_constantBuffers[slot].buffer = binding.buffer;
    m_constantBuffers[slot].offset = binding.offset;
    m_constantBuffers[slot].size = binding.size;
}

void VulkanCommandList::SetShaderResource(uint32_t slot, TextureHandle texture, ShaderStageMask)
{
    if (slot >= TexSlots::COUNT) return;
    m_textures[slot] = texture;
}

void VulkanCommandList::SetSampler(uint32_t slot, uint32_t samplerIndex, ShaderStageMask)
{
    if (slot >= SamplerSlots::COUNT) return;
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
    VkDescriptorSet descriptorSet = AllocateDescriptorSet();
    if (descriptorSet == VK_NULL_HANDLE)
    {
        Debug::LogError("VulkanCommandList: failed to allocate descriptor set");
        return;
    }

    std::vector<VkWriteDescriptorSet> writes;
    std::array<VkDescriptorBufferInfo, CBSlots::COUNT> cbInfos{};
    std::array<VkDescriptorImageInfo, TexSlots::COUNT + SamplerSlots::COUNT> imageInfos{};
    std::array<uint32_t, CBSlots::COUNT> dynamicOffsets{};

    for (uint32_t i = 0u; i < CBSlots::COUNT; ++i)
    {
        auto* buffer = m_resources->buffers.Get(m_constantBuffers[i].buffer);
        if (!buffer)
            buffer = m_resources->buffers.Get(m_fallbackCB);
        if (!buffer) continue;
        cbInfos[i].buffer = buffer->buffer;
        cbInfos[i].offset = 0u;
        cbInfos[i].range = m_constantBuffers[i].size == 0u ? buffer->byteSize : m_constantBuffers[i].size;
        dynamicOffsets[i] = m_constantBuffers[i].offset;

        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = descriptorSet;
        write.dstBinding = i;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        write.descriptorCount = 1u;
        write.pBufferInfo = &cbInfos[i];
        writes.push_back(write);
    }

    for (uint32_t i = 0u; i < TexSlots::COUNT; ++i)
    {
        auto* tex = m_resources->textures.Get(m_textures[i]);
        if (!tex)
            tex = m_resources->textures.Get(m_fallbackTexture);
        if (!tex) continue;
        imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfos[i].imageView = tex->view;

        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = descriptorSet;
        write.dstBinding = 16u + i;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        write.descriptorCount = 1u;
        write.pImageInfo = &imageInfos[i];
        writes.push_back(write);
    }

    for (uint32_t i = 0u; i < SamplerSlots::COUNT; ++i)
    {
        uint32_t samplerIndex = m_samplers[i];
        if (samplerIndex >= m_resources->samplers.size())
            samplerIndex = m_fallbackSampler;
        if (samplerIndex >= m_resources->samplers.size()) continue;
        imageInfos[TexSlots::COUNT + i].sampler = m_resources->samplers[samplerIndex].sampler;

        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = descriptorSet;
        write.dstBinding = 32u + i;
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        write.descriptorCount = 1u;
        write.pImageInfo = &imageInfos[TexSlots::COUNT + i];
        writes.push_back(write);
    }

    if (!writes.empty())
        vkUpdateDescriptorSets(m_device->GetVkDevice(), static_cast<uint32_t>(writes.size()), writes.data(), 0u, nullptr);

    vkCmdBindDescriptorSets(GetCurrentFrameContext().commandBuffer,
                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_device->GetPipelineLayout(),
                            0u,
                            1u,
                            &descriptorSet,
                            CBSlots::COUNT,
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

void VulkanCommandList::Dispatch(uint32_t, uint32_t, uint32_t)
{
}

void VulkanCommandList::TransitionResource(BufferHandle, ResourceState, ResourceState)
{
}

void VulkanCommandList::TransitionResource(TextureHandle texture, ResourceState, ResourceState after)
{
    TransitionTexture(texture, ToVkImageLayout(after), ToVkAccessFlags(after), ToVkPipelineStage(after));
}

void VulkanCommandList::TransitionRenderTarget(RenderTargetHandle rt, ResourceState, ResourceState after)
{
    auto* entry = m_resources->renderTargets.Get(rt);
    if (!entry) return;
    if (entry->colorHandle.IsValid())
        TransitionResource(entry->colorHandle, ResourceState::Unknown, after);
    if (entry->depthHandle.IsValid())
        TransitionResource(entry->depthHandle, ResourceState::Unknown, after);
}

void VulkanCommandList::CopyBuffer(BufferHandle dst, uint64_t dstOffset, BufferHandle src, uint64_t srcOffset, uint64_t size)
{
    auto* dstBuffer = m_resources->buffers.Get(dst);
    auto* srcBuffer = m_resources->buffers.Get(src);
    if (!dstBuffer || !srcBuffer) return;
    VkBufferCopy region{ srcOffset, dstOffset, size };
    vkCmdCopyBuffer(GetCurrentFrameContext().commandBuffer, srcBuffer->buffer, dstBuffer->buffer, 1u, &region);
}

void VulkanCommandList::CopyTexture(TextureHandle dst, uint32_t, TextureHandle src, uint32_t)
{
    auto* dstTex = m_resources->textures.Get(dst);
    auto* srcTex = m_resources->textures.Get(src);
    if (!dstTex || !srcTex) return;

    TransitionTexture(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    TransitionTexture(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImageCopy copy{};
    copy.srcSubresource.aspectMask = srcTex->aspect;
    copy.srcSubresource.layerCount = 1u;
    copy.dstSubresource.aspectMask = dstTex->aspect;
    copy.dstSubresource.layerCount = 1u;
    copy.extent.width = std::min(srcTex->width, dstTex->width);
    copy.extent.height = std::min(srcTex->height, dstTex->height);
    copy.extent.depth = 1u;
    vkCmdCopyImage(GetCurrentFrameContext().commandBuffer,
                   srcTex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dstTex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1u, &copy);
}

void VulkanCommandList::Submit(QueueType)
{
    auto& frame = GetCurrentFrameContext();
    auto* swapchain = m_device->GetActiveSwapchain();
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1u;
    VkCommandBuffer commandBuffer = frame.commandBuffer;
    submit.pCommandBuffers = &commandBuffer;

    VkSemaphore waitSem = VK_NULL_HANDLE;
    VkSemaphore signalSem = VK_NULL_HANDLE;
    if (swapchain)
    {
        waitSem = swapchain->GetImageAvailableSemaphore();
        signalSem = swapchain->GetRenderFinishedSemaphore();
        if (waitSem != VK_NULL_HANDLE)
        {
            submit.waitSemaphoreCount = 1u;
            submit.pWaitSemaphores = &waitSem;
            submit.pWaitDstStageMask = &waitStage;
        }
        if (signalSem != VK_NULL_HANDLE)
        {
            submit.signalSemaphoreCount = 1u;
            submit.pSignalSemaphores = &signalSem;
        }
    }

    vkQueueSubmit(m_device->GetGraphicsQueue(), 1u, &submit, m_device->GetCurrentFrameFence());
}

} // namespace engine::renderer::vulkan
