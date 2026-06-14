#include "renderer/GpuResourceRuntime.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <thread>
#include <cstring>
#include <cmath>

namespace engine::renderer {

// =============================================================================
// Vertex-Layout-Hilfsfunktionen
// =============================================================================

// Byte-Größe eines Format-Werts (nur Vertex-relevante Formate).
static uint32_t FormatByteSize(Format fmt) noexcept
{
    switch (fmt)
    {
    case Format::R8_UNORM:
    case Format::R8_SNORM:
    case Format::R8_UINT:
    case Format::R8_SINT:
        return 1u;

    case Format::RG8_UNORM:
    case Format::R16_FLOAT:
    case Format::R16_UINT:
    case Format::R16_SNORM:
        return 2u;

    case Format::RGBA8_UNORM:
    case Format::RGBA8_SNORM:
    case Format::BGRA8_UNORM:
    case Format::R32_FLOAT:
    case Format::R32_UINT:
    case Format::R32_SINT:
    case Format::RG16_FLOAT:
    case Format::RG16_UINT:
    case Format::RG16_SNORM:
    case Format::R11G11B10_FLOAT:
    case Format::RGB10A2_UNORM:
        return 4u;

    case Format::RG32_FLOAT:
    case Format::RG32_UINT:
    case Format::RGBA16_FLOAT:
    case Format::RGBA16_UINT:
    case Format::RGBA16_SNORM:
        return 8u;

    case Format::RGB32_FLOAT:
    case Format::RGB32_UINT:
        return 12u;

    case Format::RGBA32_FLOAT:
    case Format::RGBA32_UINT:
        return 16u;

    default:
        return 0u;
    }
}

uint32_t GpuResourceRuntime::ComputeVertexLayoutHash(const VertexLayout& layout) noexcept
{
    // FNV-1a-artiger Mix über alle Attribute und Bindings.
    // Reihenfolge der Attribute ist semantisch bedeutsam (Offsets können variieren).
    uint32_t h = 2166136261u;
    for (const auto& a : layout.attributes)
    {
        h ^= static_cast<uint32_t>(a.semantic); h *= 16777619u;
        h ^= static_cast<uint32_t>(a.format);   h *= 16777619u;
        h ^= a.binding;                          h *= 16777619u;
        h ^= a.offset;                           h *= 16777619u;
    }
    for (const auto& b : layout.bindings)
    {
        h ^= b.binding;                          h *= 16777619u;
        h ^= b.stride;                           h *= 16777619u;
        h ^= static_cast<uint32_t>(b.inputRate); h *= 16777619u;
    }
    return h;
}

// =============================================================================
// Kern-Impl: VB nach VertexLayout bauen
// =============================================================================

// Schreibt ein float-Array an dst, clampiert auf maxBytes.
static void WriteFloats(uint8_t* dst, const float* src, uint32_t count, uint32_t maxBytes) noexcept
{
    const uint32_t bytes = std::min(count * 4u, maxBytes);
    std::memcpy(dst, src, bytes);
}

static void WriteUInts(uint8_t* dst, const uint32_t* src, uint32_t count, uint32_t maxBytes) noexcept
{
    const uint32_t bytes = std::min(count * 4u, maxBytes);
    std::memcpy(dst, src, bytes);
}

// Baut den interleaved Vertex-Buffer gemäß layout aus den Submesh-Quelldaten.
// Gibt den Byte-Stride zurück (0 bei Fehler).
// missingChannels wird gesetzt wenn ein Attribut im Mesh fehlt (für Logging).
static bool HasVertexChannel(const assets::SubMeshData& sub,
                             VertexSemantic           semantic,
                             uint32_t                 vertexCount) noexcept
{
    const size_t vc = static_cast<size_t>(vertexCount);
    switch (semantic)
    {
    case VertexSemantic::Position:   return sub.positions.size()   >= vc * 3u;
    case VertexSemantic::Normal:     return sub.normals.size()     >= vc * 3u;
    case VertexSemantic::Tangent:    return sub.tangents.size()    >= vc * 4u;
    case VertexSemantic::TexCoord0:  return sub.uvs.size()         >= vc * 2u;
    case VertexSemantic::Color0:     return sub.colors.size()      >= vc * 4u;
    case VertexSemantic::BoneWeight:
    case VertexSemantic::BoneIndex:
        return sub.boneWeights.size() >= vc * 4u
            && sub.boneIndices.size() >= vc * 4u;
    default:                         return false;
    }
}

static VertexLayout BuildCanonicalMeshLayout(const assets::SubMeshData& sub,
                                             uint32_t                   vertexCount) noexcept
{
    VertexLayout layout;
    uint32_t offset = 0u;

    auto add = [&](VertexSemantic semantic, Format format, uint32_t sizeBytes)
    {
        layout.attributes.push_back({ semantic, format, 0u, offset });
        offset += sizeBytes;
    };

    if (!HasVertexChannel(sub, VertexSemantic::Position, vertexCount))
        return {};

    add(VertexSemantic::Position, Format::RGB32_FLOAT, 12u);
    if (HasVertexChannel(sub, VertexSemantic::Normal, vertexCount))
        add(VertexSemantic::Normal, Format::RGB32_FLOAT, 12u);
    if (HasVertexChannel(sub, VertexSemantic::Tangent, vertexCount))
        add(VertexSemantic::Tangent, Format::RGBA32_FLOAT, 16u);
    if (HasVertexChannel(sub, VertexSemantic::TexCoord0, vertexCount))
        add(VertexSemantic::TexCoord0, Format::RG32_FLOAT, 8u);
    if (HasVertexChannel(sub, VertexSemantic::Color0, vertexCount))
        add(VertexSemantic::Color0, Format::RGBA32_FLOAT, 16u);
    if (HasVertexChannel(sub, VertexSemantic::BoneWeight, vertexCount))
    {
        add(VertexSemantic::BoneWeight, Format::RGBA32_FLOAT, 16u);
        add(VertexSemantic::BoneIndex, Format::RGBA32_UINT, 16u);
    }

    layout.bindings = { { 0u, offset, VertexInputRate::PerVertex } };
    return layout;
}

static uint32_t BuildVertexBuffer(
    const assets::SubMeshData& sub,
    const VertexLayout&        layout,
    uint32_t                   vertexCount,
    std::vector<uint8_t>&      outVbData,
    uint32_t&                  outMissingChannelMask)
{
    outMissingChannelMask = 0u;

    // Stride aus VertexBinding für Binding 0 bestimmen.
    uint32_t stride = 0u;
    for (const auto& b : layout.bindings)
    {
        if (b.binding == 0u)
        {
            stride = b.stride;
            break;
        }
    }

    // Fallback: Stride aus Attributen ableiten (max(offset + size))
    if (stride == 0u)
    {
        for (const auto& a : layout.attributes)
        {
            if (a.binding != 0u) continue;
            const uint32_t end = a.offset + FormatByteSize(a.format);
            stride = std::max(stride, end);
        }
    }

    if (stride == 0u)
        return 0u;

    // Buffer initialisieren (nullbytes → korrekte Default-Werte für fehlende Kanäle)
    outVbData.assign(static_cast<size_t>(stride) * vertexCount, 0u);

    for (const auto& attr : layout.attributes)
    {
        if (attr.binding != 0u)
            continue; // nur Binding 0 unterstützt (Erweiterung für Multi-Stream ist möglich)

        const uint32_t attrBytes = FormatByteSize(attr.format);
        if (attrBytes == 0u)
            continue;

        // Überprüfung, ob der Kanal im Mesh vorhanden ist (einmal pro Attribut, vor dem Loop)
        bool hasChannel = false;
        switch (attr.semantic)
        {
        case VertexSemantic::Position:
            hasChannel = sub.positions.size() >= static_cast<size_t>(vertexCount) * 3u;
            break;
        case VertexSemantic::Normal:
            hasChannel = sub.normals.size() >= static_cast<size_t>(vertexCount) * 3u;
            break;
        case VertexSemantic::Tangent:
            hasChannel = sub.tangents.size() >= static_cast<size_t>(vertexCount) * 4u;
            break;
        case VertexSemantic::Bitangent:
            hasChannel = sub.tangents.size() >= static_cast<size_t>(vertexCount) * 4u
                      && sub.normals.size() >= static_cast<size_t>(vertexCount) * 3u;
            break;
        case VertexSemantic::TexCoord0:
        case VertexSemantic::TexCoord1:
            hasChannel = sub.uvs.size() >= static_cast<size_t>(vertexCount) * 2u;
            break;
        case VertexSemantic::Color0:
            hasChannel = sub.colors.size() >= static_cast<size_t>(vertexCount) * 4u;
            break;
        case VertexSemantic::BoneWeight:
            hasChannel = sub.boneWeights.size() >= static_cast<size_t>(vertexCount) * 4u;
            break;
        case VertexSemantic::BoneIndex:
            hasChannel = sub.boneIndices.size() >= static_cast<size_t>(vertexCount) * 4u;
            break;
        default:
            hasChannel = false;
            break;
        }

        if (!hasChannel)
            outMissingChannelMask |= (1u << static_cast<uint32_t>(attr.semantic));

        for (uint32_t v = 0u; v < vertexCount; ++v)
        {
            uint8_t* dst = outVbData.data() + static_cast<size_t>(v) * stride + attr.offset;

            switch (attr.semantic)
            {
            case VertexSemantic::Position:
                if (hasChannel)
                {
                    const float p[3] = {
                        sub.positions[v * 3u + 0u],
                        sub.positions[v * 3u + 1u],
                        sub.positions[v * 3u + 2u]
                    };
                    WriteFloats(dst, p, 3u, attrBytes);
                }
                // Kein Fallback: fehlende Position = Nullvektor (bereits null-initialisiert)
                break;

            case VertexSemantic::Normal:
                if (hasChannel)
                {
                    const float n[3] = {
                        sub.normals[v * 3u + 0u],
                        sub.normals[v * 3u + 1u],
                        sub.normals[v * 3u + 2u]
                    };
                    WriteFloats(dst, n, 3u, attrBytes);
                }
                else
                {
                    // Default: Weltup (0,1,0) — sinnvoller als Nullnormal
                    const float def[3] = { 0.f, 1.f, 0.f };
                    WriteFloats(dst, def, 3u, attrBytes);
                }
                break;

            case VertexSemantic::Tangent:
                if (hasChannel)
                {
                    const float t[4] = {
                        sub.tangents[v * 4u + 0u],
                        sub.tangents[v * 4u + 1u],
                        sub.tangents[v * 4u + 2u],
                        sub.tangents[v * 4u + 3u]
                    };
                    WriteFloats(dst, t, 4u, attrBytes);
                }
                else
                {
                    const float def[4] = { 1.f, 0.f, 0.f, 1.f };
                    WriteFloats(dst, def, 4u, attrBytes);
                }
                break;

            case VertexSemantic::Bitangent:
                if (hasChannel)
                {
                    const float n[3] = {
                        sub.normals[v * 3u + 0u],
                        sub.normals[v * 3u + 1u],
                        sub.normals[v * 3u + 2u]
                    };
                    const float t[4] = {
                        sub.tangents[v * 4u + 0u],
                        sub.tangents[v * 4u + 1u],
                        sub.tangents[v * 4u + 2u],
                        sub.tangents[v * 4u + 3u]
                    };

                    float nx = n[0], ny = n[1], nz = n[2];
                    float tx = t[0], ty = t[1], tz = t[2];
                    const float sign = t[3];

                    const float tDotN = tx * nx + ty * ny + tz * nz;
                    tx -= nx * tDotN;
                    ty -= ny * tDotN;
                    tz -= nz * tDotN;

                    const float tLenSq = tx * tx + ty * ty + tz * tz;
                    if (tLenSq > 1e-20f)
                    {
                        const float invTLen = 1.0f / std::sqrt(tLenSq);
                        tx *= invTLen; ty *= invTLen; tz *= invTLen;
                    }
                    else
                    {
                        tx = 1.f; ty = 0.f; tz = 0.f;
                    }

                    float bx = (ny * tz - nz * ty) * sign;
                    float by = (nz * tx - nx * tz) * sign;
                    float bz = (nx * ty - ny * tx) * sign;
                    const float bLenSq = bx * bx + by * by + bz * bz;
                    if (bLenSq > 1e-20f)
                    {
                        const float invBLen = 1.0f / std::sqrt(bLenSq);
                        bx *= invBLen; by *= invBLen; bz *= invBLen;
                    }
                    const float b[3] = { bx, by, bz };
                    WriteFloats(dst, b, 3u, attrBytes);
                }
                else
                {
                    const float def[3] = { 0.f, 0.f, 1.f };
                    WriteFloats(dst, def, 3u, attrBytes);
                }
                break;

            case VertexSemantic::TexCoord0:
            case VertexSemantic::TexCoord1:
                if (hasChannel)
                {
                    const float uv[2] = {
                        sub.uvs[v * 2u + 0u],
                        sub.uvs[v * 2u + 1u]
                    };
                    WriteFloats(dst, uv, 2u, attrBytes);
                }
                // Fehlend: (0,0) — bereits null-initialisiert
                break;

            case VertexSemantic::Color0:
                if (hasChannel)
                {
                    const float c[4] = {
                        sub.colors[v * 4u + 0u],
                        sub.colors[v * 4u + 1u],
                        sub.colors[v * 4u + 2u],
                        sub.colors[v * 4u + 3u]
                    };
                    WriteFloats(dst, c, 4u, attrBytes);
                }
                else
                {
                    // Default: Weiß, volle Deckkraft — sichtbares Signal dass Kanal fehlt
                    const float def[4] = { 1.f, 1.f, 1.f, 1.f };
                    WriteFloats(dst, def, 4u, attrBytes);
                }
                break;

            case VertexSemantic::BoneWeight:
                if (hasChannel)
                {
                    const float w[4] = {
                        sub.boneWeights[v * 4u + 0u],
                        sub.boneWeights[v * 4u + 1u],
                        sub.boneWeights[v * 4u + 2u],
                        sub.boneWeights[v * 4u + 3u]
                    };
                    WriteFloats(dst, w, 4u, attrBytes);
                }
                else
                {
                    const float def[4] = { 1.f, 0.f, 0.f, 0.f };
                    WriteFloats(dst, def, 4u, attrBytes);
                }
                break;

            case VertexSemantic::BoneIndex:
                if (hasChannel)
                {
                    const uint32_t j[4] = {
                        sub.boneIndices[v * 4u + 0u],
                        sub.boneIndices[v * 4u + 1u],
                        sub.boneIndices[v * 4u + 2u],
                        sub.boneIndices[v * 4u + 3u]
                    };
                    WriteUInts(dst, j, 4u, attrBytes);
                }
                break;

            default:
                // Unbekannte Semantik: null-initialisiert — keine Aktion nötig
                break;
            }
        }
    }

    return stride;
}

// =============================================================================
// GpuResourceRuntime Methoden
// =============================================================================

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
    case MemoryHeapKind::Upload:   m_stats.uploadHeapBytes  += byteSize; break;
    case MemoryHeapKind::Readback: m_stats.readbackHeapBytes += byteSize; break;
    default: break;
    }
}

void GpuResourceRuntime::TrackRelease(uint64_t byteSize, const ResourceAllocationInfo& allocationInfo) noexcept
{
    uint64_t* counter = nullptr;
    switch (allocationInfo.heapKind)
    {
    case MemoryHeapKind::Default:  counter = &m_stats.deviceLocalBytes;  break;
    case MemoryHeapKind::Upload:   counter = &m_stats.uploadHeapBytes;   break;
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

    m_frameSlots.clear();
    m_transientRTPool.clear();
    m_pendingDestroy.clear();

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
        if (m_frameFence)
        {
            m_frameFence->Wait(slot.fenceValue);
            RefreshCompletedFenceValue();
        }
        else
        {
            Debug::LogWarning("GpuResourceRuntime: frame slot %u reused before completion without fence (completed=%llu slot=%llu)",
                              m_currentFrameSlot,
                              static_cast<unsigned long long>(completedFenceValue),
                              static_cast<unsigned long long>(slot.fenceValue));
        }
    }

    const uint64_t retiredSlotFenceValue = slot.fenceValue;
    for (const FrameUploadBuffer& upload : slot.uploadBuffers)
    {
        if (!upload.handle.IsValid())
            continue;
        TrackRelease(upload.byteSize, m_device->QueryBufferAllocation(upload.handle));
        if (retiredSlotFenceValue != 0u && m_completedFenceValue >= retiredSlotFenceValue)
            m_device->DestroyBuffer(upload.handle);
        else
            ScheduleDestroy(upload.handle, retiredSlotFenceValue != 0u ? retiredSlotFenceValue : m_completedFenceValue);
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

    auto buildDesc = [](const rendergraph::RGResourceDesc& res)
    {
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
            desc.hasDepth = true;
            desc.depthFormat = Format::D24_UNORM_S8_UINT;
        }

        return desc;
    };

    std::vector<std::pair<RenderTargetDesc, uint32_t>> requiredPerFrame;
    for (const auto& res : rg.GetResources())
    {
        if (res.lifetime != rendergraph::RGResourceLifetime::Transient)
            continue;
        const RenderTargetDesc desc = buildDesc(res);
        auto it = std::find_if(requiredPerFrame.begin(), requiredPerFrame.end(),
            [&](const auto& entry) { return Matches(entry.first, desc); });
        if (it != requiredPerFrame.end())
            ++it->second;
        else
            requiredPerFrame.push_back({ desc, 1u });
    }

    auto maxResidentForDesc = [&](const RenderTargetDesc& desc) noexcept
    {
        uint32_t requiredCount = 1u;
        for (const auto& entry : requiredPerFrame)
        {
            if (Matches(entry.first, desc))
            {
                requiredCount = std::max(requiredCount, entry.second);
                break;
            }
        }
        return std::max(1u, m_framesInFlight) * requiredCount;
    };

    for (size_t i = 0; i < rg.GetResources().size(); ++i)
    {
        const auto& res = rg.GetResources()[i];
        if (res.lifetime != rendergraph::RGResourceLifetime::Transient)
            continue;
        if (res.renderTarget.IsValid())
            continue;

        RenderTargetDesc desc = buildDesc(res);

        PooledTransientRT* pooledMatch = nullptr;
        PooledTransientRT* blockedMatch = nullptr;
        uint64_t blockedFenceValue = UINT64_MAX;
        uint32_t matchingDescCount = 0u;

        for (PooledTransientRT& pooled : m_transientRTPool)
        {
            if (!pooled.renderTarget.IsValid())
                continue;
            if (!Matches(pooled.desc, desc))
                continue;

            ++matchingDescCount;
            if (pooled.inUse)
                continue;

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

        if (!pooledMatch && blockedMatch && matchingDescCount >= maxResidentForDesc(desc) && m_frameFence)
        {
            const char* debugName = desc.debugName.empty() ? "TransientRT" : desc.debugName.c_str();
            Debug::LogVerbose("GpuResourceRuntime: Wait - transient target '%s' fence=%llu",
                debugName, static_cast<unsigned long long>(blockedFenceValue));

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
    m_stats.uploadedBytesTotal     += static_cast<uint64_t>(byteSize);
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

    // Upload-/Staging-Buffer werden nur als CPU-schreibbare Zwischenressourcen
    // verwendet und nie als echte Structured-Buffer gelesen.
    BufferHandle staging = AllocateUploadBuffer(byteSize, BufferType::Vertex,
                                                 debugName ? debugName : "BufferUpload");
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



void GpuResourceRuntime::ScheduleDestroy(ShaderHandle handle, uint64_t retirementFenceValue)
{
    if (!handle.IsValid()) return;
    PendingDestroy p;
    p.type = PendingDestroy::Type::Shader;
    p.shader = handle;
    p.retireAfterFence = retirementFenceValue;
    m_pendingDestroy.push_back(p);
}

void GpuResourceRuntime::ScheduleDestroy(PipelineHandle handle, uint64_t retirementFenceValue)
{
    if (!handle.IsValid()) return;
    PendingDestroy p;
    p.type = PendingDestroy::Type::Pipeline;
    p.pipeline = handle;
    p.retireAfterFence = retirementFenceValue;
    m_pendingDestroy.push_back(p);
}

void GpuResourceRuntime::WaitForCompletedValue(uint64_t fenceValue)
{
    if (fenceValue == 0u) return;
    if (m_completedFenceValue >= fenceValue) return;
    if (!m_frameFence) return;

    m_frameFence->Wait(fenceValue);
    RefreshCompletedFenceValue();
}

void GpuResourceRuntime::RefreshCompletedFenceValue() noexcept
{
    if (!m_frameFence)
        return;
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
    if (!m_device) return;
    switch (pending.type)
    {
    case PendingDestroy::Type::Buffer:       m_device->DestroyBuffer(pending.buffer); break;
    case PendingDestroy::Type::Texture:      m_device->DestroyTexture(pending.texture); break;
    case PendingDestroy::Type::RenderTarget: m_device->DestroyRenderTarget(pending.renderTarget); break;
    case PendingDestroy::Type::Shader:       m_device->DestroyShader(pending.shader); break;
    case PendingDestroy::Type::Pipeline:     m_device->DestroyPipeline(pending.pipeline); break;
    }
}

// =============================================================================
// Mesh-Upload — layout-getrieben
// =============================================================================

const GpuResourceRuntime::GpuMeshEntry* GpuResourceRuntime::GetOrUploadMeshImpl(
    MeshHandle mesh, uint32_t submeshIndex,
    const VertexLayout& layout, uint32_t layoutHash,
    assets::AssetRegistry& registry)
{
    if (!RequireRenderThread("GetOrUploadMesh"))
        return nullptr;
    if (!m_device || !mesh.IsValid())
        return nullptr;

    const auto* meshAsset = registry.meshes.Get(mesh);
    if (!meshAsset || submeshIndex >= meshAsset->submeshes.size())
        return nullptr;

    const auto& sub = meshAsset->submeshes[submeshIndex];
    if (sub.positions.empty() || sub.indices.empty())
        return nullptr;

    const uint32_t vertexCount = static_cast<uint32_t>(sub.positions.size() / 3u);

    const VertexLayout canonicalLayout = BuildCanonicalMeshLayout(sub, vertexCount);
    const bool hasRequestedLayout = !layout.attributes.empty() && !layout.bindings.empty();
    const VertexLayout& uploadLayout = hasRequestedLayout ? layout : canonicalLayout;
    const uint32_t uploadLayoutHash = layoutHash != 0u
        ? layoutHash
        : ComputeVertexLayoutHash(uploadLayout);

    const MeshCacheKey key{ mesh.value, submeshIndex, uploadLayoutHash };
    auto it = m_meshCache.find(key);
    if (it != m_meshCache.end() && it->second.uploaded)
        return &it->second;

    // VB im kanonischen Mesh-Layout bauen; Pipeline-Layouts lesen daraus nur ihre Semantics.
    std::vector<uint8_t> vbData;
    uint32_t missingMask = 0u;
    uint32_t stride;

    uint32_t uploadStride = 0u;
    if (!uploadLayout.bindings.empty())
        uploadStride = uploadLayout.bindings[0].stride;

    const size_t expectedRawBytes = static_cast<size_t>(vertexCount) * sub.rawVertexStride;
    const bool canUseRawBytes =
        !sub.rawInterleavedBytes.empty()
        && sub.rawVertexStride > 0u
        && sub.rawVertexStride == uploadStride
        && sub.rawInterleavedBytes.size() == expectedRawBytes;

    if (canUseRawBytes)
    {
        // Fast path: .kmesh bytes already interleaved — direct copy, no per-vertex work.
        stride = sub.rawVertexStride;
        vbData = sub.rawInterleavedBytes;
    }
    else
    {
        stride = BuildVertexBuffer(sub, uploadLayout, vertexCount, vbData, missingMask);
    }

    if (stride == 0u || vbData.empty())
    {
        Debug::LogError("GpuResourceRuntime: VertexLayout ergibt stride=0 für Mesh '%s' sub[%u]",
                        meshAsset->debugName.c_str(), submeshIndex);
        return nullptr;
    }

    // Fehlende Kanäle einmalig pro Upload melden
    if (missingMask != 0u)
    {
        const char* names[] = { "Position", "Normal", "Tangent", "Bitangent",
                                 "TexCoord0", "TexCoord1", "Color0", "BoneWeight", "BoneIndex" };
        for (uint32_t bit = 0u; bit < 9u; ++bit)
        {
            if (missingMask & (1u << bit))
            {
                Debug::LogWarning(
                    "GpuResourceRuntime: Mesh '%s' sub[%u] hat kein '%s'-Kanal für das angeforderte "
                    "VertexLayout — Default-Wert wird verwendet.",
                    meshAsset->debugName.c_str(), submeshIndex,
                    (bit < 9u) ? names[bit] : "Unknown");
            }
        }
    }

    // Vertex-Buffer anlegen und hochladen
    BufferDesc vbDesc{};
    vbDesc.byteSize     = static_cast<uint64_t>(vbData.size());
    vbDesc.stride       = stride;
    vbDesc.type         = BufferType::Vertex;
    vbDesc.usage        = ResourceUsage::VertexBuffer;
    vbDesc.access       = MemoryAccess::GpuOnly;
    vbDesc.initialState = ResourceState::CopyDest;
    vbDesc.debugName    = meshAsset->debugName + "_VB";

    BufferHandle vb = m_device->CreateBuffer(vbDesc);
    if (!vb.IsValid())
    {
        Debug::LogError("GpuResourceRuntime: CreateBuffer VB fehlgeschlagen für '%s'",
                        meshAsset->debugName.c_str());
        return nullptr;
    }
    TrackAllocation(vbDesc.byteSize, m_device->QueryBufferAllocation(vb));
    EnqueueBufferUpload(vb, vbData.data(), vbData.size(), 0u,
                        ResourceState::VertexBuffer, vbDesc.debugName.c_str());

    // Index-Buffer anlegen und hochladen
    BufferDesc ibDesc{};
    ibDesc.byteSize     = static_cast<uint64_t>(sub.indices.size() * sizeof(uint32_t));
    ibDesc.stride       = sizeof(uint32_t);
    ibDesc.type         = BufferType::Index;
    ibDesc.usage        = ResourceUsage::IndexBuffer;
    ibDesc.access       = MemoryAccess::GpuOnly;
    ibDesc.initialState = ResourceState::CopyDest;
    ibDesc.debugName    = meshAsset->debugName + "_IB";

    BufferHandle ib = m_device->CreateBuffer(ibDesc);
    if (!ib.IsValid())
    {
        Debug::LogError("GpuResourceRuntime: CreateBuffer IB fehlgeschlagen für '%s'",
                        meshAsset->debugName.c_str());
        m_device->DestroyBuffer(vb);
        return nullptr;
    }
    TrackAllocation(ibDesc.byteSize, m_device->QueryBufferAllocation(ib));
    EnqueueBufferUpload(ib, sub.indices.data(),
                        static_cast<size_t>(ibDesc.byteSize), 0u,
                        ResourceState::IndexBuffer, ibDesc.debugName.c_str());

    GpuMeshEntry entry;
    entry.vertexBuffer = vb;
    entry.indexBuffer  = ib;
    entry.indexCount   = static_cast<uint32_t>(sub.indices.size());
    entry.vertexStride = stride;
    entry.layoutHash   = uploadLayoutHash;
    entry.uploaded     = true;

    Debug::LogVerbose("GpuResourceRuntime: Mesh '%s' sub[%u] hochgeladen — "
               "%u Vertices, %u Indices, stride=%u, layoutHash=0x%08x",
               meshAsset->debugName.c_str(), submeshIndex,
               vertexCount, entry.indexCount, stride, uploadLayoutHash);

    m_stats.liveMeshBuffers += 2u;
    auto& cached = m_meshCache[key];
    cached = std::move(entry);
    return &cached;
}

const GpuResourceRuntime::GpuMeshEntry* GpuResourceRuntime::GetOrUploadMesh(
    MeshHandle mesh, uint32_t submeshIndex,
    const VertexLayout& layout,
    assets::AssetRegistry& registry)
{
    const uint32_t layoutHash = (!layout.attributes.empty() && !layout.bindings.empty())
        ? ComputeVertexLayoutHash(layout)
        : 0u;
    return GetOrUploadMeshImpl(mesh, submeshIndex, layout, layoutHash, registry);
}

const GpuResourceRuntime::GpuMeshEntry* GpuResourceRuntime::GetOrUploadMesh(
    MeshHandle mesh, uint32_t submeshIndex,
    assets::AssetRegistry& registry)
{
    // Rückwärtskompatible Überladung: kanonisches Layout, hash=0
    static const VertexLayout kEmpty;
    return GetOrUploadMeshImpl(mesh, submeshIndex, kEmpty, 0u, registry);
}

bool GpuResourceRuntime::CollectUploadRequests(const RenderQueue& renderQueue,
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

    for (const DrawList& list : renderQueue.GetLists())
        collectList(list);

    std::sort(outRequests.begin(), outRequests.end());
    outRequests.erase(std::unique(outRequests.begin(), outRequests.end()), outRequests.end());
    return true;
}

bool GpuResourceRuntime::CommitUploads(const std::vector<MeshUploadRequest>& requests,
                                       assets::AssetRegistry& registry)
{
    if (!RequireRenderThread("CommitUploads"))
        return false;

    // Kanonisches Layout für Vorwärmung ohne explizites Layout-Wissen.
    // FramePreparationStage::bindGpuBuffers lädt layout-spezifisch nach.
    bool ok = true;
    for (const MeshUploadRequest& request : requests)
        ok = (GetOrUploadMesh(request.mesh, request.submeshIndex, registry) != nullptr) && ok;
    return ok;
}

bool GpuResourceRuntime::IsMeshUploaded(MeshHandle mesh, uint32_t submeshIndex,
                                         uint32_t layoutHash) const noexcept
{
    if (layoutHash == 0u)
    {
        for (const auto& [key, entry] : m_meshCache)
        {
            if (key.meshHandleValue == mesh.value && key.submeshIndex == submeshIndex && entry.uploaded)
                return true;
        }
        return false;
    }

    const MeshCacheKey key{ mesh.value, submeshIndex, layoutHash };
    auto it = m_meshCache.find(key);
    return it != m_meshCache.end() && it->second.uploaded;
}

} // namespace engine::renderer
