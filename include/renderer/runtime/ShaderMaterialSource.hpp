#pragma once

#include "renderer/MaterialInstance.hpp"
#include "renderer/runtime/MaterialRuntimeDesc.hpp"

namespace engine::renderer {

class IShaderMaterialSource
{
public:
    virtual ~IShaderMaterialSource() = default;

    [[nodiscard]] virtual size_t DescCount() const noexcept = 0;
    [[nodiscard]] virtual const MaterialDesc* GetDesc(MaterialHandle material) const noexcept = 0;
    [[nodiscard]] virtual const MaterialInstance* GetInstance(MaterialHandle material) const noexcept = 0;
    [[nodiscard]] virtual const MaterialParameterLayout* GetParameterLayout(MaterialHandle material) const noexcept = 0;
    [[nodiscard]] virtual const MaterialRuntimeDesc* GetRuntimeDesc(MaterialHandle material) const noexcept = 0;
    [[nodiscard]] virtual PipelineKey BuildPipelineKey(MaterialHandle material) const noexcept = 0;
    [[nodiscard]] virtual ShaderVariantFlag BuildShaderVariantFlags(MaterialHandle material) const noexcept = 0;
    [[nodiscard]] virtual uint64_t GetRevision(MaterialHandle material) const noexcept = 0;
    [[nodiscard]] virtual const std::vector<uint8_t>& GetCBData(MaterialHandle material) const = 0;
    [[nodiscard]] virtual const CbLayout& GetCBLayout(MaterialHandle material) const = 0;
};

} // namespace engine::renderer
