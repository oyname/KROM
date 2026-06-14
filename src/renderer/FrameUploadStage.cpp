#include "renderer/FrameUploadStage.hpp"
#include "renderer/runtime/MaterialRuntimeBridge.hpp"
#include "renderer/runtime/MaterialRuntimeDesc.hpp"
#include "core/Debug.hpp"
#include <cstring>
#include <functional>
#include <unordered_map>
#include <vector>

namespace engine::renderer {
namespace {

struct FrameMeshCacheKey
{
    uint32_t meshValue     = 0u;
    uint32_t materialValue = 0u;
    uint32_t submeshIndex  = 0u;

    [[nodiscard]] bool operator==(const FrameMeshCacheKey& other) const noexcept
    {
        return meshValue == other.meshValue
            && materialValue == other.materialValue
            && submeshIndex == other.submeshIndex;
    }
};

struct FrameMeshCacheKeyHash
{
    [[nodiscard]] size_t operator()(const FrameMeshCacheKey& key) const noexcept
    {
        uint64_t h = 1469598103934665603ull;
        const auto mix = [&h](uint32_t value)
        {
            h ^= static_cast<uint64_t>(value);
            h *= 1099511628211ull;
        };
        mix(key.meshValue);
        mix(key.materialValue);
        mix(key.submeshIndex);
        return std::hash<uint64_t>{}(h);
    }
};

} // namespace

bool FrameUploadStage::BuildRenderQueues(const FrameUploadStageContext& context) const
{
    const math::Mat4 cullingViewProj = context.view.projection * context.view.view;
    context.snapshot.BuildDrawLists(context.view.view,
                                    cullingViewProj,
                                    context.view.nearPlane,
                                    context.view.farPlane,
                                    context.materials,
                                    context.renderPassRegistry,
                                    context.view.visibilityLayerMask,
                                    context.jobSystem);
    return true;
}

bool FrameUploadStage::CollectUploadRequests(const FrameUploadStageContext& context,
                                             FrameUploadResult& result) const
{
    result.meshUploadRequests.clear();
    return context.gpuRuntime.CollectUploadRequests(context.snapshot.GetQueue(), result.meshUploadRequests);
}

bool FrameUploadStage::CommitUploads(const FrameUploadStageContext& context,
                                     FrameUploadResult& result) const
{
    result.perFrameCB = context.gpuRuntime.AllocateUploadBuffer(
        sizeof(FrameConstants), BufferType::Constant, "PerFrameCB");
    if (result.perFrameCB.IsValid())
        context.gpuRuntime.UploadBuffer(result.perFrameCB, &context.frameData.frameConstants, sizeof(FrameConstants));

    assets::AssetRegistry* assetReg = context.shaderRuntime.GetAssetRegistry();
    if (assetReg)
    {
        // Frame-lokaler Cache: vermeidet doppelte GetOrUploadMesh-Aufrufe für
        // dasselbe (Mesh, Material)-Paar über opaque- und shadow-Listen hinweg.
        // Key = (mesh.value << 32) | material.value — eindeutig pro Layout-Kombination.
        std::unordered_map<FrameMeshCacheKey,
                           const GpuResourceRuntime::GpuMeshEntry*,
                           FrameMeshCacheKeyHash> frameLocalMeshCache;
        frameLocalMeshCache.reserve(64);

        auto bindGpuBuffers = [&](DrawList& list)
        {
            for (auto& item : list.items)
            {
                if (!item.mesh.IsValid())
                    continue;

                // Key: Bits 63-32 = mesh, Bits 31-8 = material, Bits 7-0 = submesh.
                // Eindeutig pro (Mesh, Vertex-Layout, Submesh)-Kombination.
                const FrameMeshCacheKey cacheKey{
                    item.mesh.value,
                    item.material.value,
                    item.submeshIndex
                };
                auto cacheIt = frameLocalMeshCache.find(cacheKey);
                if (cacheIt != frameLocalMeshCache.end())
                {
                    const GpuResourceRuntime::GpuMeshEntry* cached = cacheIt->second;
                    if (!cached) continue;
                    item.gpuVertexBuffer = cached->vertexBuffer;
                    item.gpuIndexBuffer  = cached->indexBuffer;
                    item.gpuIndexCount   = cached->indexCount;
                    item.gpuVertexStride = cached->vertexStride;
                    continue;
                }

                const MaterialRuntimeDesc* matRuntime = MaterialRuntimeBridge::GetRuntimeDesc(context.materials, item.material);
                const GpuResourceRuntime::GpuMeshEntry* gpuMesh =
                    (matRuntime && !matRuntime->vertexLayout.attributes.empty())
                    ? context.gpuRuntime.GetOrUploadMesh(item.mesh, item.submeshIndex, matRuntime->vertexLayout, *assetReg)
                    : context.gpuRuntime.GetOrUploadMesh(item.mesh, item.submeshIndex, *assetReg);

                frameLocalMeshCache.emplace(cacheKey, gpuMesh);
                if (!gpuMesh)
                    continue;
                item.gpuVertexBuffer = gpuMesh->vertexBuffer;
                item.gpuIndexBuffer  = gpuMesh->indexBuffer;
                item.gpuIndexCount   = gpuMesh->indexCount;
                item.gpuVertexStride = gpuMesh->vertexStride;
            }
        };

        auto& q = context.snapshot.GetQueue();
        for (DrawList& list : q.GetLists())
            bindGpuBuffers(list);
    }

    // Auf Vulkan werden per-Object-Daten via Push Constants übertragen —
    // der Arena-Buffer wird vom Shader nie gelesen, Upload wäre reiner Waste.
    const IDevice* device = context.shaderRuntime.GetDevice();
    const bool needsPerObjectArena = !device ||
        device->GetShaderTargetProfile() != assets::ShaderTargetProfile::Vulkan_SPIRV;

    const auto& objectConstants = context.snapshot.GetQueue().objectConstants;
    if (needsPerObjectArena && !objectConstants.empty())
    {
        const auto arena = context.gpuRuntime.AllocateConstantArena(
            static_cast<uint32_t>(sizeof(PerObjectConstants)),
            static_cast<uint32_t>(objectConstants.size()),
            "PerObjectCBArena");

        if (arena.buffer.IsValid())
        {
            std::vector<uint8_t> staging(
                static_cast<size_t>(arena.alignedStride) * objectConstants.size(), 0u);
            for (size_t i = 0u; i < objectConstants.size(); ++i)
            {
                std::memcpy(staging.data() + i * arena.alignedStride,
                            &objectConstants[i],
                            sizeof(PerObjectConstants));
            }
            context.gpuRuntime.UploadBuffer(arena.buffer, staging.data(), staging.size(), 0u);
            result.perObjectArena = arena.buffer;
            result.perObjectStride = arena.alignedStride;
        }
    }

    return true;
}

} // namespace engine::renderer
