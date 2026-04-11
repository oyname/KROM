#include "renderer/GpuResourceRuntime.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <thread>
#include <cstring>

namespace engine::renderer {


bool GpuResourceRuntime::IsRenderThread() const noexcept
{
    return m_renderThreadId == std::this_thread::get_id();
}

bool GpuResourceRuntime::RequireRenderThread(const char* opName) const noexcept
{
    if (IsRenderThread())
        return true;

    Debug::LogError("GpuResourceRuntime.cpp: %s must run on render thread", opName ? opName : "operation");
    return false;
}

void GpuResourceRuntime::TrackAllocation(uint64_t byteSize, const ResourceAllocationInfo& allocationInfo) noexcept
{
    switch (allocationInfo.heapKind)
    {
    case MemoryHeapKind::Default:  m_stats.deviceLocalBytes += byteSize; break;
    case MemoryHeapKind::Upload:   m_stats.uploadHeapBytes += byteSize; break;
    case MemoryHeapKind::Readback: m_stats.readbackHeapBytes += byteSize; break;
    default: break;
    }
}

void GpuResourceRuntime::TrackRelease(uint64_t byteSize, const ResourceAllocationInfo& allocationInfo) noexcept
{
    uint64_t* counter = nullptr;
    switch (allocationInfo.heapKind)
    {
    case MemoryHeapKind::Default:  counter = &m_stats.deviceLocalBytes; break;
    case MemoryHeapKind::Upload:   counter = &m_stats.uploadHeapBytes; break;
    case MemoryHeapKind::Readback: counter = &m_stats.readbackHeapBytes; break;
    default: break;
    }
    if (counter)
        *counter = (*counter > byteSize) ? (*counter - byteSize) : 0u;
}


bool GpuResourceRuntime::Initialize(IDevice& device, uint32_t framesInFlight)
{
    m_renderThreadId = std::this_thread::get_id();
    Shutdown();
    m_renderThreadId = std::this_thread::get_id();
    m_device = &device;
    m_framesInFlight = std::max(1u, framesInFlight);
    m_frameSlots.resize(m_framesInFlight);
    m_currentFrameSlot = 0u;
    m_completedFenceValue = 0u;
    m_submittedFenceValue = 0u;
    m_frameFence = nullptr;
    m_stats = {};
    return true;
}

void GpuResourceRuntime::Shutdown()
{
    if (!m_device)
    {
        m_renderThreadId = std::thread::id{};
        return;
    }
    if (!IsRenderThread())
    {
        Debug::LogWarning("GpuResourceRuntime.cpp: Shutdown not on render thread after WaitIdle - continuing cleanup");
    }

    WaitForCompletedValue(GetMaxOutstandingFenceValue());
    RetireCompleted(m_completedFenceValue);

    for (FrameSlot& slot : m_frameSlots)
    {
        for (const FrameUploadBuffer& upload : slot.uploadBuffers)
        {
            if (!upload.handle.IsValid())
                continue;
            TrackRelease(upload.byteSize, m_device->QueryBufferAllocation(upload.handle));
            m_device->DestroyBuffer(upload.handle);
        }
        slot.uploadBuffers.clear();
        slot.fenceValue = 0u;
    }

    for (PooledTransientRT& pooled : m_transientRTPool)
    {
        pooled.inUse = false;
        pooled.availableAfterFence = 0u;
        if (pooled.renderTarget.IsValid())
            m_device->DestroyRenderTarget(pooled.renderTarget);
    }

    for (const PendingDestroy& pending : m_pendingDestroy)
        DestroyNow(pending);

    m_frameSlots.clear();
    m_transientRTPool.clear();
    m_pendingDestroy.clear();

    // Mesh-GPU-Buffer freigeben
    for (auto& [key, entry] : m_meshCache)
    {
        if (entry.vertexBuffer.IsValid())
        {
            TrackRelease(0u, m_device->QueryBufferAllocation(entry.vertexBuffer));
            m_device->DestroyBuffer(entry.vertexBuffer);
        }
        if (entry.indexBuffer.IsValid())
        {
            TrackRelease(0u, m_device->QueryBufferAllocation(entry.indexBuffer));
            m_device->DestroyBuffer(entry.indexBuffer);
        }
    }
    m_meshCache.clear();
    m_stats.liveMeshBuffers = 0u;

    m_device = nullptr;
    m_renderThreadId = std::thread::id{};
    m_framesInFlight = 0u;
    m_currentFrameSlot = 0u;
    m_completedFenceValue = 0u;
    m_submittedFenceValue = 0u;
    m_frameFence = nullptr;
    m_stats = {};
}

void GpuResourceRuntime::BeginFrame(uint64_t completedFenceValue, IFence* frameFence)
{
    if (!RequireRenderThread("BeginFrame"))
        return;
    if (!m_device || m_frameSlots.empty())
        return;

    m_frameFence = frameFence;
    m_completedFenceValue = completedFenceValue;
    RetireCompleted(completedFenceValue);

    m_currentFrameSlot = static_cast<uint32_t>((m_currentFrameSlot + 1u) % m_frameSlots.size());
    FrameSlot& slot = m_frameSlots[m_currentFrameSlot];
    if (slot.fenceValue != 0u && completedFenceValue < slot.fenceValue)
    {
        Debug::LogWarning("GpuResourceRuntime: frame slot %u reused before completion (completed=%llu slot=%llu)",
                          m_currentFrameSlot,
                          static_cast<unsigned long long>(completedFenceValue),
                          static_cast<unsigned long long>(slot.fenceValue));
    }

    const uint64_t retiredSlotFenceValue = slot.fenceValue;
    for (const FrameUploadBuffer& upload : slot.uploadBuffers)
    {
        if (!upload.handle.IsValid())
            continue;

        TrackRelease(upload.byteSize, m_device->QueryBufferAllocation(upload.handle));
        if (retiredSlotFenceValue != 0u && completedFenceValue >= retiredSlotFenceValue)
            m_device->DestroyBuffer(upload.handle);
        else
            ScheduleDestroy(upload.handle, retiredSlotFenceValue != 0u ? retiredSlotFenceValue : completedFenceValue);
    }
    slot.uploadBuffers.clear();
    slot.pendingBufferUploads.clear();
    slot.fenceValue = 0u;

    m_stats.uploadedBytesThisFrame = 0u;
    m_stats.liveFrameUploadBuffers = 0u;
}

void GpuResourceRuntime::EndFrame(uint64_t submittedFenceValue)
{
    if (!RequireRenderThread("EndFrame"))
        return;
    if (!m_device || m_frameSlots.empty())
        return;

    m_submittedFenceValue = submittedFenceValue;
    m_frameSlots[m_currentFrameSlot].fenceValue = submittedFenceValue;
}

bool GpuResourceRuntime::Matches(const RenderTargetDesc& a, const RenderTargetDesc& b) const noexcept
{
    return a.width == b.width
        && a.height == b.height
        && a.colorFormat == b.colorFormat
        && a.depthFormat == b.depthFormat
        && a.sampleCount == b.sampleCount
        && a.hasDepth == b.hasDepth
        && a.hasColor == b.hasColor;
}

void GpuResourceRuntime::AllocateTransientTargets(rendergraph::RenderGraph& rg)
{
    if (!RequireRenderThread("AllocateTransientTargets"))
        return;
    if (!m_device)
        return;

    for (size_t i = 0; i < rg.GetResources().size(); ++i)
    {
        const auto& res = rg.GetResources()[i];
        if (res.lifetime != rendergraph::RGResourceLifetime::Transient)
            continue;
        if (res.renderTarget.IsValid())
            continue;

        RenderTargetDesc desc;
        desc.width = res.width;
        desc.height = res.height;
        desc.debugName = res.debugName;
        desc.sampleCount = 1u;
        desc.hasColor = true;
        desc.hasDepth = false;
        desc.colorFormat = res.format;
        desc.depthFormat = Format::Unknown;

        if (res.kind == rendergraph::RGResourceKind::DepthStencil ||
            res.kind == rendergraph::RGResourceKind::ShadowMap)
        {
            desc.hasColor = false;
            desc.hasDepth = true;
            desc.depthFormat = res.format;
            desc.colorFormat = Format::Unknown;
        }
        else if (res.kind == rendergraph::RGResourceKind::RenderTarget)
        {
            // RenderTarget (im Gegensatz zu ColorTexture) hat immer einen
            // eingebetteten Depth-Buffer - BeginRenderPass braucht keinen
            // separaten Depth-Handle.
            desc.hasDepth = true;
            desc.depthFormat = Format::D24_UNORM_S8_UINT;
        }

        PooledTransientRT* pooledMatch = nullptr;
        PooledTransientRT* blockedMatch = nullptr;
        uint64_t blockedFenceValue = UINT64_MAX;
        uint32_t matchingDescCount = 0u;

        for (PooledTransientRT& pooled : m_transientRTPool)
        {
            if (pooled.inUse || !pooled.renderTarget.IsValid())
                continue;
            if (!Matches(pooled.desc, desc))
                continue;

            ++matchingDescCount;
            if (pooled.availableAfterFence <= m_completedFenceValue)
            {
                pooledMatch = &pooled;
                break;
            }

            if (pooled.availableAfterFence < blockedFenceValue)
            {
                blockedFenceValue = pooled.availableAfterFence;
                blockedMatch = &pooled;
            }
        }

        if (!pooledMatch && blockedMatch && matchingDescCount >= m_framesInFlight && m_frameFence)
        {
            const char* debugName = desc.debugName.empty() ? "TransientRT" : desc.debugName.c_str();

            Debug::Log("GpuResourceRuntime: Wait - transient target '%s' fence=%llu",
                debugName,
                static_cast<unsigned long long>(blockedFenceValue));

            m_frameFence->Wait(blockedFenceValue);
            m_completedFenceValue = blockedFenceValue;
            RetireCompleted(m_completedFenceValue);
            pooledMatch = blockedMatch;
        }

        if (!pooledMatch)
        {
            PooledTransientRT pooled;
            pooled.desc = desc;
            pooled.renderTarget = m_device->CreateRenderTarget(desc);
            pooled.colorTexture = desc.hasColor
                ? m_device->GetRenderTargetColorTexture(pooled.renderTarget)
                : m_device->GetRenderTargetDepthTexture(pooled.renderTarget);
            pooled.availableAfterFence = 0u;
            pooled.inUse = false;
            m_transientRTPool.push_back(pooled);
            pooledMatch = &m_transientRTPool.back();
        }

        pooledMatch->inUse = true;
        rg.SetTransientRenderTarget(static_cast<rendergraph::RGResourceID>(i),
                                    pooledMatch->renderTarget,
                                    pooledMatch->colorTexture);
    }

    m_stats.pooledTransientTargets = static_cast<uint32_t>(m_transientRTPool.size());
}

void GpuResourceRuntime::ReleaseTransientTargets(const rendergraph::CompiledFrame& frame,
                                                 uint64_t retirementFenceValue)
{
    if (!RequireRenderThread("ReleaseTransientTargets"))
        return;
    for (const auto& res : frame.resources)
    {
        if (!res.renderTarget.IsValid())
            continue;

        for (PooledTransientRT& pooled : m_transientRTPool)
        {
            if (pooled.renderTarget != res.renderTarget)
                continue;
            pooled.inUse = false;
            pooled.availableAfterFence = std::max(pooled.availableAfterFence, retirementFenceValue);
            break;
        }
    }
}

BufferHandle GpuResourceRuntime::AllocateUploadBuffer(uint64_t byteSize,
                                                      BufferType type,
                                                      const char* debugName)
{
    if (!RequireRenderThread("AllocateUploadBuffer"))
        return BufferHandle::Invalid();
    if (!m_device || m_frameSlots.empty())
        return BufferHandle::Invalid();

    BufferDesc desc;
    desc.byteSize = std::max<uint64_t>(byteSize, 16u);
    desc.type = type;
    desc.access = MemoryAccess::CpuWrite;
    desc.usage = ResourceUsage::ConstantBuffer | ResourceUsage::CopySource;
    desc.initialState = ResourceState::CopySource;
    desc.lifetime = ResourceLifetimeClass::PerFrameTransient;
    desc.debugName = debugName ? debugName : "FrameUpload";

    BufferHandle handle = m_device->CreateBuffer(desc);
    if (handle.IsValid())
    {
        m_frameSlots[m_currentFrameSlot].uploadBuffers.push_back({ handle, desc.byteSize });
        ++m_stats.liveFrameUploadBuffers;
        TrackAllocation(desc.byteSize, m_device->QueryBufferAllocation(handle));
    }
    return handle;
}

void GpuResourceRuntime::UploadBuffer(BufferHandle dst, const void* data, size_t byteSize, size_t dstOffset)
{
    if (!RequireRenderThread("UploadBuffer"))
        return;
    if (!m_device || !dst.IsValid() || !data || byteSize == 0u)
        return;

    m_device->UploadBufferData(dst, data, byteSize, dstOffset);
    m_stats.uploadedBytesThisFrame += static_cast<uint64_t>(byteSize);
    m_stats.uploadedBytesTotal += static_cast<uint64_t>(byteSize);
}


void GpuResourceRuntime::EnqueueBufferUpload(BufferHandle dst,
                                             const void* data,
                                             size_t byteSize,
                                             size_t dstOffset,
                                             ResourceState dstStateAfterCopy,
                                             const char* debugName)
{
    if (!RequireRenderThread("EnqueueBufferUpload"))
        return;
    if (!m_device || m_frameSlots.empty() || !dst.IsValid() || !data || byteSize == 0u)
        return;

    BufferHandle staging = AllocateUploadBuffer(byteSize, BufferType::Structured, debugName ? debugName : "BufferUpload");
    if (!staging.IsValid())
        return;

    UploadBuffer(staging, data, byteSize, 0u);
    m_frameSlots[m_currentFrameSlot].pendingBufferUploads.push_back(PendingBufferUpload{
        staging,
        dst,
        0u,
        static_cast<uint64_t>(dstOffset),
        static_cast<uint64_t>(byteSize),
        ResourceState::CopyDest,
        dstStateAfterCopy,
        debugName ? debugName : "BufferUpload"
    });
}

bool GpuResourceRuntime::HasPendingUploads() const noexcept
{
    return m_currentFrameSlot < m_frameSlots.size()
        && !m_frameSlots[m_currentFrameSlot].pendingBufferUploads.empty();
}

const std::vector<GpuResourceRuntime::PendingBufferUpload>& GpuResourceRuntime::GetPendingBufferUploads() const noexcept
{
    static const std::vector<PendingBufferUpload> kEmpty;
    if (m_currentFrameSlot >= m_frameSlots.size())
        return kEmpty;
    return m_frameSlots[m_currentFrameSlot].pendingBufferUploads;
}

void GpuResourceRuntime::ClearPendingUploads() noexcept
{
    if (m_currentFrameSlot < m_frameSlots.size())
        m_frameSlots[m_currentFrameSlot].pendingBufferUploads.clear();
}

GpuResourceRuntime::ConstantArenaResult GpuResourceRuntime::AllocateConstantArena(
    uint32_t elementSize, uint32_t elementCount, const char* debugName)
{
    if (elementSize == 0u || elementCount == 0u)
        return {};

    // Stride auf kConstantBufferAlignment aufrunden — kompatibel mit DX12/Vulkan CBV-Alignment.
    const uint32_t alignedStride = (elementSize + kConstantBufferAlignment - 1u)
                                    & ~(kConstantBufferAlignment - 1u);
    const uint64_t totalBytes = static_cast<uint64_t>(alignedStride) * elementCount;

    BufferHandle buf = AllocateUploadBuffer(totalBytes, BufferType::Constant, debugName);
    if (!buf.IsValid())
        return {};

    return { buf, alignedStride };
}

void GpuResourceRuntime::ScheduleDestroy(BufferHandle handle, uint64_t retirementFenceValue)
{
    if (!handle.IsValid()) return;
    PendingDestroy p;
    p.type = PendingDestroy::Type::Buffer;
    p.buffer = handle;
    p.retireAfterFence = retirementFenceValue;
    m_pendingDestroy.push_back(p);
}

void GpuResourceRuntime::ScheduleDestroy(TextureHandle handle, uint64_t retirementFenceValue)
{
    if (!handle.IsValid()) return;
    PendingDestroy p;
    p.type = PendingDestroy::Type::Texture;
    p.texture = handle;
    p.retireAfterFence = retirementFenceValue;
    m_pendingDestroy.push_back(p);
}

void GpuResourceRuntime::ScheduleDestroy(RenderTargetHandle handle, uint64_t retirementFenceValue)
{
    if (!handle.IsValid()) return;
    PendingDestroy p;
    p.type = PendingDestroy::Type::RenderTarget;
    p.renderTarget = handle;
    p.retireAfterFence = retirementFenceValue;
    m_pendingDestroy.push_back(p);
}

void GpuResourceRuntime::WaitForCompletedValue(uint64_t fenceValue)
{
    if (fenceValue == 0u)
        return;

    if (m_completedFenceValue >= fenceValue)
        return;

    if (!m_frameFence)
        return;

    m_frameFence->Wait(fenceValue);
    m_completedFenceValue = std::max(m_completedFenceValue, m_frameFence->GetValue());
}

uint64_t GpuResourceRuntime::GetMaxOutstandingFenceValue() const noexcept
{
    uint64_t maxFenceValue = std::max(m_completedFenceValue, m_submittedFenceValue);

    for (const FrameSlot& slot : m_frameSlots)
        maxFenceValue = std::max(maxFenceValue, slot.fenceValue);

    for (const PooledTransientRT& pooled : m_transientRTPool)
        maxFenceValue = std::max(maxFenceValue, pooled.availableAfterFence);

    for (const PendingDestroy& pending : m_pendingDestroy)
        maxFenceValue = std::max(maxFenceValue, pending.retireAfterFence);

    return maxFenceValue;
}

void GpuResourceRuntime::RetireCompleted(uint64_t completedFenceValue)
{
    auto it = std::remove_if(m_pendingDestroy.begin(), m_pendingDestroy.end(),
        [&](const PendingDestroy& pending)
        {
            if (pending.retireAfterFence > completedFenceValue)
                return false;
            DestroyNow(pending);
            return true;
        });
    m_pendingDestroy.erase(it, m_pendingDestroy.end());
}

void GpuResourceRuntime::DestroyNow(const PendingDestroy& pending)
{
    if (!m_device)
        return;

    switch (pending.type)
    {
    case PendingDestroy::Type::Buffer:       m_device->DestroyBuffer(pending.buffer); break;
    case PendingDestroy::Type::Texture:      m_device->DestroyTexture(pending.texture); break;
    case PendingDestroy::Type::RenderTarget: m_device->DestroyRenderTarget(pending.renderTarget); break;
    }
}

// =============================================================================
// Mesh-Upload
// =============================================================================

const GpuResourceRuntime::GpuMeshEntry* GpuResourceRuntime::GetOrUploadMesh(
    MeshHandle mesh, uint32_t submeshIndex,
    assets::AssetRegistry& registry)
{
    if (!RequireRenderThread("GetOrUploadMesh"))
        return nullptr;
    if (!m_device || !mesh.IsValid())
        return nullptr;

    const MeshCacheKey key{ mesh.value, submeshIndex };
    auto it = m_meshCache.find(key);
    if (it != m_meshCache.end() && it->second.uploaded)
        return &it->second;

    const auto* meshAsset = registry.meshes.Get(mesh);
    if (!meshAsset || submeshIndex >= meshAsset->submeshes.size())
        return nullptr;

    const auto& sub = meshAsset->submeshes[submeshIndex];
    if (sub.positions.empty() || sub.indices.empty())
        return nullptr;

    // Interleaved-Vertex-Buffer bauen: Position(3) + Normal(3) + UV(2) = 8 floats
    constexpr uint32_t kFloatsPerVertex = 8u;
    constexpr uint32_t kStride          = kFloatsPerVertex * sizeof(float);
    const uint32_t vertexCount = static_cast<uint32_t>(sub.positions.size() / 3u);

    std::vector<float> vbData;
    vbData.reserve(static_cast<size_t>(vertexCount) * kFloatsPerVertex);
    for (uint32_t v = 0u; v < vertexCount; ++v)
    {
        // Position
        vbData.push_back(sub.positions[v * 3u + 0u]);
        vbData.push_back(sub.positions[v * 3u + 1u]);
        vbData.push_back(sub.positions[v * 3u + 2u]);
        // Normal
        if (sub.normals.size() >= (v + 1u) * 3u) {
            vbData.push_back(sub.normals[v * 3u + 0u]);
            vbData.push_back(sub.normals[v * 3u + 1u]);
            vbData.push_back(sub.normals[v * 3u + 2u]);
        } else {
            vbData.push_back(0.f); vbData.push_back(1.f); vbData.push_back(0.f);
        }
        // UV
        if (sub.uvs.size() >= (v + 1u) * 2u) {
            vbData.push_back(sub.uvs[v * 2u + 0u]);
            vbData.push_back(sub.uvs[v * 2u + 1u]);
        } else {
            vbData.push_back(0.f); vbData.push_back(0.f);
        }
    }

    // Vertex-Buffer
    BufferDesc vbDesc{};
    vbDesc.byteSize  = static_cast<uint64_t>(vbData.size() * sizeof(float));
    vbDesc.stride    = kStride;
    vbDesc.type      = BufferType::Vertex;
    vbDesc.usage     = ResourceUsage::VertexBuffer;
    vbDesc.access    = MemoryAccess::GpuOnly;
    vbDesc.initialState = ResourceState::CopyDest;
    vbDesc.debugName = meshAsset->debugName + "_VB";

    BufferHandle vb = m_device->CreateBuffer(vbDesc);
    if (!vb.IsValid()) {
        Debug::LogError("GpuResourceRuntime: CreateBuffer VB fehlgeschlagen für '%s'",
            meshAsset->debugName.c_str());
        return nullptr;
    }
    TrackAllocation(vbDesc.byteSize, m_device->QueryBufferAllocation(vb));
    EnqueueBufferUpload(vb, vbData.data(), static_cast<size_t>(vbDesc.byteSize), 0u, ResourceState::VertexBuffer, vbDesc.debugName.c_str());

    // Index-Buffer
    BufferDesc ibDesc{};
    ibDesc.byteSize  = static_cast<uint64_t>(sub.indices.size() * sizeof(uint32_t));
    ibDesc.stride    = sizeof(uint32_t);
    ibDesc.type      = BufferType::Index;
    ibDesc.usage     = ResourceUsage::IndexBuffer;
    ibDesc.access    = MemoryAccess::GpuOnly;
    ibDesc.initialState = ResourceState::CopyDest;
    ibDesc.debugName = meshAsset->debugName + "_IB";

    BufferHandle ib = m_device->CreateBuffer(ibDesc);
    if (!ib.IsValid()) {
        Debug::LogError("GpuResourceRuntime: CreateBuffer IB fehlgeschlagen für '%s'",
            meshAsset->debugName.c_str());
        m_device->DestroyBuffer(vb);
        return nullptr;
    }
    TrackAllocation(ibDesc.byteSize, m_device->QueryBufferAllocation(ib));
    EnqueueBufferUpload(ib, sub.indices.data(), static_cast<size_t>(ibDesc.byteSize), 0u, ResourceState::IndexBuffer, ibDesc.debugName.c_str());

    GpuMeshEntry entry;
    entry.vertexBuffer  = vb;
    entry.indexBuffer   = ib;
    entry.indexCount    = static_cast<uint32_t>(sub.indices.size());
    entry.vertexStride  = kStride;
    entry.uploaded      = true;

    Debug::Log("GpuResourceRuntime: Mesh '%s' sub[%u] hochgeladen - %u Vertices, %u Indices",
        meshAsset->debugName.c_str(), submeshIndex, vertexCount, entry.indexCount);

    m_stats.liveMeshBuffers += 2u;
    auto& cached = m_meshCache[key];
    cached = std::move(entry);
    return &cached;
}

bool GpuResourceRuntime::CollectUploadRequests(const RenderWorld& renderWorld,
                                            std::vector<MeshUploadRequest>& outRequests) const
{
    outRequests.clear();

    auto collectList = [&](const DrawList& list)
    {
        for (const DrawItem& item : list.items)
        {
            if (!item.mesh.IsValid())
                continue;
            outRequests.push_back({item.mesh, 0u});
        }
    };

    const RenderQueue& queue = renderWorld.GetQueue();
    collectList(queue.opaque);
    collectList(queue.alphaCutout);
    collectList(queue.transparent);
    collectList(queue.shadow);
    collectList(queue.ui);
    collectList(queue.particles);

    std::sort(outRequests.begin(), outRequests.end());
    outRequests.erase(std::unique(outRequests.begin(), outRequests.end()), outRequests.end());
    return true;
}

bool GpuResourceRuntime::CommitUploads(const std::vector<MeshUploadRequest>& requests,
                                  assets::AssetRegistry& registry)
{
    if (!RequireRenderThread("CommitUploads"))
        return false;

    bool ok = true;
    for (const MeshUploadRequest& request : requests)
        ok = (GetOrUploadMesh(request.mesh, request.submeshIndex, registry) != nullptr) && ok;
    return ok;
}

bool GpuResourceRuntime::IsMeshUploaded(MeshHandle mesh, uint32_t submeshIndex) const noexcept
{
    const MeshCacheKey key{ mesh.value, submeshIndex };
    auto it = m_meshCache.find(key);
    return it != m_meshCache.end() && it->second.uploaded;
}

} // namespace engine::renderer
