#pragma once

#include "renderer/MaterialTypes.hpp"
#include "renderer/ParameterBlob.hpp"
#include <string>

namespace engine::renderer {

struct MaterialInstance
{
    MaterialHandle        desc;
    RenderPassID          renderPass = StandardRenderPasses::Opaque();
    ShaderHandle          shader;
    std::string           shaderSourceCode;
    ShaderParameterLayout layout{};
    ParameterBlob         parameters{};
    PipelineKey           pipelineKey{};
    uint32_t              pipelineKeyHash = 0u;
    std::vector<MaterialParam> instanceParams;
    CbLayout              cbLayout{};
    std::vector<uint8_t>  cbData;
    bool                  cbDirty = true;
    bool                  layoutDirty = true;
    uint64_t              revision = 1ull;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return shader.IsValid() && !shaderSourceCode.empty() && layout.IsValid();
    }

    RenderPassID RenderPass() const noexcept { return renderPass; }
    [[nodiscard]] float* GetFloatPtr(const std::string& name) noexcept;
};

} // namespace engine::renderer
