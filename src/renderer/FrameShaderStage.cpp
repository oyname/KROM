#include "renderer/FrameShaderStage.hpp"
#include "renderer/runtime/MaterialRuntimeBridge.hpp"
#include "core/Debug.hpp"

namespace engine::renderer {

bool FrameShaderStage::CollectShaderRequests(const FrameShaderStageContext& context,
                                             FrameShaderResult& result) const
{
    result.shaderRequests.clear();
    const MaterialSystemShaderMaterialSource materialSource(context.materials);
    return context.shaderRuntime.CollectShaderRequests(materialSource, result.shaderRequests);
}

bool FrameShaderStage::CollectMaterialRequests(const FrameShaderStageContext& context,
                                               FrameShaderResult& result) const
{
    result.materialRequests.clear();
    const MaterialSystemShaderMaterialSource materialSource(context.materials);
    return context.shaderRuntime.CollectMaterialRequests(materialSource, result.materialRequests);
}

bool FrameShaderStage::CommitShaderRequests(const FrameShaderStageContext& context,
                                            const FrameShaderResult& result) const
{
    if (!context.shaderRuntime.CommitShaderRequests(result.shaderRequests))
    {
        Debug::LogError("FrameShaderStage: shader asset preparation failed");
        return false;
    }
    return true;
}

bool FrameShaderStage::CommitMaterialRequests(const FrameShaderStageContext& context,
                                              const FrameShaderResult& result) const
{
    const MaterialSystemShaderMaterialSource materialSource(context.materials);
    if (!context.shaderRuntime.CommitMaterialRequests(materialSource, result.materialRequests))
    {
        Debug::LogError("FrameShaderStage: material preparation failed");
        return false;
    }
    return true;
}

} // namespace engine::renderer
