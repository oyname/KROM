#pragma once

#include "renderer/RenderFrameTypes.hpp"
#include "renderer/ShaderRuntime.hpp"

namespace engine::renderer {

class MaterialSystem;

struct FrameShaderStageContext
{
    const MaterialSystem& materials;
    ShaderRuntime& shaderRuntime;
};

class FrameShaderStage
{
public:
    [[nodiscard]] bool CollectShaderRequests(const FrameShaderStageContext& context,
                                             FrameShaderResult& result) const;
    [[nodiscard]] bool CollectMaterialRequests(const FrameShaderStageContext& context,
                                               FrameShaderResult& result) const;
    [[nodiscard]] bool CommitShaderRequests(const FrameShaderStageContext& context,
                                            const FrameShaderResult& result) const;
    [[nodiscard]] bool CommitMaterialRequests(const FrameShaderStageContext& context,
                                              const FrameShaderResult& result) const;
};

} // namespace engine::renderer
