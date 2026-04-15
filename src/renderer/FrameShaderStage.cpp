#include "renderer/FrameShaderStage.hpp"
#include "core/Debug.hpp"

namespace engine::renderer {

bool FrameShaderStage::CollectShaderRequests(const FrameShaderStageContext& context,
                                             FrameShaderResult& result) const
{
    result.shaderRequests.clear();
    return context.shaderRuntime.CollectShaderRequests(context.materials, result.shaderRequests);
}

bool FrameShaderStage::CollectMaterialRequests(const FrameShaderStageContext& context,
                                               FrameShaderResult& result) const
{
    result.materialRequests.clear();
    return context.shaderRuntime.CollectMaterialRequests(context.materials, result.materialRequests);
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
    if (!context.shaderRuntime.CommitMaterialRequests(context.materials, result.materialRequests))
    {
        Debug::LogError("FrameShaderStage: material preparation failed");
        return false;
    }
    return true;
}

} // namespace engine::renderer
