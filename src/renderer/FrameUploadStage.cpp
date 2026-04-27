#include "renderer/FrameUploadStage.hpp"
#include "core/Debug.hpp"
#include <cstring>
#include <unordered_map>
#include <vector>

namespace engine::renderer {

bool FrameUploadStage::BuildRenderQueues(const FrameUploadStageContext& context) const
{
    context.snapshot.GetWorld().BuildDrawLists(context.view.view,
                                               context.frameData.viewProjForBackend,
                                               context.view.nearPlane,
                                               context.view.farPlane,
                                               context.materials,
                                               context.renderPassRegistry,
                                               0xFFFFFFFFu,
                                               context.jobSystem);
    return true;
}

bool FrameUploadStage::CollectUploadRequests(const FrameUploadStageContext& context,
                                             FrameUploadResult& result) const
{
    result.meshUploadRequests.clear();
    return context.gpuRuntime.CollectUploadRequests(context.snapshot.GetWorld(), result.meshUploadRequests);
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
        if (!context.gpuRuntime.CommitUploads(result.meshUploadRequests, *assetReg))
        {
            Debug::LogError("FrameUploadStage: mesh upload commit failed");
            return false;
        }

        // Frame-lokaler Cache: vermeidet doppelte GetOrUploadMesh-Aufrufe für
        // dasselbe (Mesh, Material)-Paar über opaque- und shadow-Listen hinweg.
        // Key = (mesh.value << 32) | material.value — eindeutig pro Layout-Kombination.
        std::unordered_map<uint64_t, const GpuResourceRuntime::GpuMeshEntry*> frameLocalMeshCache;
        frameLocalMeshCache.reserve(64);

        auto bindGpuBuffers = [&](DrawList& list)
        {
            for (auto& item : list.items)
            {
                if (!item.mesh.IsValid())
                    continue;

                const uint64_t cacheKey = (static_cast<uint64_t>(item.mesh.value) << 32)
                                        | static_cast<uint64_t>(item.material.value);
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

                const MaterialDesc* matDesc = context.materials.GetDesc(item.material);
                const GpuResourceRuntime::GpuMeshEntry* gpuMesh =
                    (matDesc && !matDesc->vertexLayout.attributes.empty())
                    ? context.gpuRuntime.GetOrUploadMesh(item.mesh, 0u, matDesc->vertexLayout, *assetReg)
                    : context.gpuRuntime.GetOrUploadMesh(item.mesh, 0u, *assetReg);

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
