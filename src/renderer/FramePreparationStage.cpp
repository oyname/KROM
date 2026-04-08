#include "renderer/FramePreparationStage.hpp"
#include "core/Debug.hpp"
#include <algorithm>
#include <cstring>
#include <vector>

namespace engine::renderer {
namespace {

void FillMatrixRowMajor(const math::Mat4& m, float out[16]) noexcept
{
    std::memcpy(out, m.Data(), sizeof(float) * 16u);
}

math::Mat4 MakeOpenGLClipSpaceRemap() noexcept
{
    math::Mat4 r = math::Mat4::Identity();
    r.m[2][2] = 2.0f;
    r.m[3][2] = -1.0f;
    return r;
}

} // namespace

bool FramePreparationStage::PrepareFrameData(const FramePreparationStageContext& context,
                                             FramePreparationStageResult& result) const
{
    result.projectionForBackend = context.view.projection;
    if (context.isOpenGLBackend)
        result.projectionForBackend = MakeOpenGLClipSpaceRemap() * result.projectionForBackend;

    result.viewProjForBackend = result.projectionForBackend * context.view.view;

    FrameConstants fc{};
    FillMatrixRowMajor(context.view.view, fc.viewMatrix);
    FillMatrixRowMajor(result.projectionForBackend, fc.projMatrix);
    FillMatrixRowMajor(result.viewProjForBackend, fc.viewProjMatrix);
    FillMatrixRowMajor(result.viewProjForBackend.Inverse(), fc.invViewProjMatrix);
    fc.cameraPosition[0] = context.view.cameraPosition.x;
    fc.cameraPosition[1] = context.view.cameraPosition.y;
    fc.cameraPosition[2] = context.view.cameraPosition.z;
    fc.cameraPosition[3] = 1.f;
    fc.cameraForward[0] = context.view.cameraForward.x;
    fc.cameraForward[1] = context.view.cameraForward.y;
    fc.cameraForward[2] = context.view.cameraForward.z;
    fc.cameraForward[3] = 0.f;
    fc.screenSize[0] = static_cast<float>(context.viewportWidth);
    fc.screenSize[1] = static_cast<float>(context.viewportHeight);
    fc.screenSize[2] = context.viewportWidth ? 1.f / static_cast<float>(context.viewportWidth) : 0.f;
    fc.screenSize[3] = context.viewportHeight ? 1.f / static_cast<float>(context.viewportHeight) : 0.f;
    fc.timeData[0] = static_cast<float>(context.timing.GetTimeSeconds());
    fc.timeData[1] = context.timing.GetDeltaSecondsF();
    fc.timeData[2] = static_cast<float>(context.timing.GetFrameCount());
    fc.ambientColor[0] = context.view.ambientColor.x;
    fc.ambientColor[1] = context.view.ambientColor.y;
    fc.ambientColor[2] = context.view.ambientColor.z;
    fc.ambientColor[3] = context.view.ambientIntensity;
    fc.lightCount = static_cast<uint32_t>(context.renderWorld.GetLights().size());
    fc.nearPlane = context.view.nearPlane;
    fc.farPlane = context.view.farPlane;
    context.renderWorld.SetFrameConstants(fc);
    result.frameConstants = fc;
    result.perFrameCB = BufferHandle::Invalid();
    return true;
}

bool FramePreparationStage::CollectShaderRequests(const FramePreparationStageContext& context,
                                                  FramePreparationStageResult& result) const
{
    result.shaderRequests.clear();
    return context.shaderRuntime.CollectShaderRequests(context.materials, result.shaderRequests);
}

bool FramePreparationStage::CollectMaterialRequests(const FramePreparationStageContext& context,
                                                    FramePreparationStageResult& result) const
{
    result.materialRequests.clear();
    return context.shaderRuntime.CollectMaterialRequests(context.materials, result.materialRequests);
}

bool FramePreparationStage::BuildRenderQueues(const FramePreparationStageContext& context,
                                              const FramePreparationStageResult& result) const
{
    context.renderWorld.BuildDrawLists(context.view.view,
                                       result.viewProjForBackend,
                                       context.view.nearPlane,
                                       context.view.farPlane,
                                       context.materials);
    return true;
}

bool FramePreparationStage::CollectUploadRequests(const FramePreparationStageContext& context,
                                                  FramePreparationStageResult& result) const
{
    result.meshUploadRequests.clear();
    return context.gpuRuntime.CollectUploadRequests(context.renderWorld, result.meshUploadRequests);
}

bool FramePreparationStage::CommitShaderRequests(const FramePreparationStageContext& context,
                                                 const FramePreparationStageResult& result) const
{
    if (!context.shaderRuntime.CommitShaderRequests(result.shaderRequests))
    {
        Debug::LogError("FramePreparationStage: shader asset preparation failed");
        return false;
    }
    return true;
}

bool FramePreparationStage::CommitMaterialRequests(const FramePreparationStageContext& context,
                                                   const FramePreparationStageResult& result) const
{
    if (!context.shaderRuntime.CommitMaterialRequests(context.materials, result.materialRequests))
    {
        Debug::LogError("FramePreparationStage: material preparation failed");
        return false;
    }
    return true;
}

bool FramePreparationStage::CommitUploads(const FramePreparationStageContext& context,
                                          FramePreparationStageResult& result) const
{
    result.perFrameCB = context.gpuRuntime.AllocateUploadBuffer(sizeof(FrameConstants), BufferType::Constant, "PerFrameCB");
    if (result.perFrameCB.IsValid())
        context.gpuRuntime.UploadBuffer(result.perFrameCB, &result.frameConstants, sizeof(FrameConstants));

    assets::AssetRegistry* assetReg = context.shaderRuntime.GetAssetRegistry();
    if (assetReg)
    {
        if (!context.gpuRuntime.CommitUploads(result.meshUploadRequests, *assetReg))
        {
            Debug::LogError("FramePreparationStage: mesh upload commit failed");
            return false;
        }

        auto bindGpuBuffers = [&](DrawList& list)
        {
            for (auto& item : list.items)
            {
                if (!item.mesh.IsValid())
                    continue;
                const auto* gpuMesh = context.gpuRuntime.GetOrUploadMesh(item.mesh, 0u, *assetReg);
                if (!gpuMesh)
                    continue;
                item.gpuVertexBuffer = gpuMesh->vertexBuffer;
                item.gpuIndexBuffer = gpuMesh->indexBuffer;
                item.gpuIndexCount = gpuMesh->indexCount;
                item.gpuVertexStride = gpuMesh->vertexStride;
            }
        };

        auto& q = context.renderWorld.GetQueue();
        bindGpuBuffers(q.opaque);
        bindGpuBuffers(q.transparent);
        bindGpuBuffers(q.shadow);
        bindGpuBuffers(q.alphaCutout);
        bindGpuBuffers(q.ui);
        bindGpuBuffers(q.particles);
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
            // Alle Slots in einen lokal gestrideten Staging-Buffer schreiben,
            // dann in EINEM UploadBuffer-Aufruf übertragen.
            // Verhindert, dass MAP_WRITE_DISCARD bei jedem Aufruf die vorherigen Slots löscht (DX11).
            std::vector<uint8_t> staging(
                static_cast<size_t>(arena.alignedStride) * objectConstants.size(), 0u);
            for (size_t i = 0u; i < objectConstants.size(); ++i)
            {
                std::memcpy(staging.data() + i * arena.alignedStride,
                            &objectConstants[i],
                            sizeof(PerObjectConstants));
            }
            context.gpuRuntime.UploadBuffer(arena.buffer, staging.data(), staging.size(), 0u);
            result.perObjectArena  = arena.buffer;
            result.perObjectStride = arena.alignedStride;

            // DIAG: Staging-Werte alle 60 Frames — müssen sich ändern
            static uint32_t s_diagFrame = 0u;
            if ((++s_diagFrame % 60u) == 0u)
            {
                const float* wm = reinterpret_cast<const float*>(staging.data());
                Debug::Log("DIAG FramePrep  frame=%u arena=0x%x [0]=%.4f [8]=%.4f [2]=%.4f",
                    s_diagFrame, arena.buffer.value, wm[0], wm[8], wm[2]);
            }
        }
    }

    return true;
}

bool FramePreparationStage::Execute(const FramePreparationStageContext& context,
                                    FramePreparationStageResult& result) const
{
    if (!PrepareFrameData(context, result))
        return false;
    if (!CollectShaderRequests(context, result))
        return false;
    if (!CollectMaterialRequests(context, result))
        return false;
    if (!BuildRenderQueues(context, result))
        return false;
    if (!CollectUploadRequests(context, result))
        return false;
    if (!CommitShaderRequests(context, result))
        return false;
    if (!CommitMaterialRequests(context, result))
        return false;
    if (!CommitUploads(context, result))
        return false;
    return true;
}

} // namespace engine::renderer
