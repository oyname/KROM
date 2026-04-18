#include "renderer/FrameUploadStage.hpp"
#include "core/Debug.hpp"
#include <cstring>
#include <vector>

namespace engine::renderer {

bool FrameUploadStage::BuildRenderQueues(const FrameUploadStageContext& context) const
{
    context.renderWorld.BuildDrawLists(context.view.view,
                                       context.frameData.viewProjForBackend,
                                       context.view.nearPlane,
                                       context.view.farPlane,
                                       context.materials,
                                       context.renderPassRegistry);
    return true;
}

bool FrameUploadStage::CollectUploadRequests(const FrameUploadStageContext& context,
                                             FrameUploadResult& result) const
{
    result.meshUploadRequests.clear();
    return context.gpuRuntime.CollectUploadRequests(context.renderWorld, result.meshUploadRequests);
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

        auto bindGpuBuffers = [&](DrawList& list)
        {
            for (auto& item : list.items)
            {
                if (!item.mesh.IsValid())
                    continue;

                const MaterialDesc* matDesc = context.materials.GetDesc(item.material);
                const auto* gpuMesh =
                    (matDesc && !matDesc->vertexLayout.attributes.empty())
                    ? context.gpuRuntime.GetOrUploadMesh(item.mesh, 0u, matDesc->vertexLayout, *assetReg)
                    : context.gpuRuntime.GetOrUploadMesh(item.mesh, 0u, *assetReg);

                if (!gpuMesh)
                    continue;
                item.gpuVertexBuffer = gpuMesh->vertexBuffer;
                item.gpuIndexBuffer = gpuMesh->indexBuffer;
                item.gpuIndexCount = gpuMesh->indexCount;
                item.gpuVertexStride = gpuMesh->vertexStride;
            }
        };

        auto& q = context.renderWorld.GetQueue();
        for (DrawList& list : q.GetLists())
            bindGpuBuffers(list);
    }

    const auto& objectConstants = context.renderWorld.GetQueue().objectConstants;
    if (!objectConstants.empty())
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
