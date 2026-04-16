#pragma once

#include "renderer/MaterialTypes.hpp"
#include "renderer/ParameterBlob.hpp"
#include <string>

namespace engine::renderer {

struct MaterialInstance
{
    MaterialHandle        desc;
    RenderPassTag         passTag = RenderPassTag::Opaque;
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

    RenderPassTag PassTag() const noexcept { return passTag; }
    [[nodiscard]] float* GetFloatPtr(const std::string& name) noexcept;
};

} // namespace engine::renderer
