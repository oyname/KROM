#pragma once

#include "renderer/MaterialInstance.hpp"

namespace engine::renderer {

struct MaterialFeatureEval {
    static ShaderVariantFlag BuildShaderVariantFlags(const MaterialDesc&, const MaterialInstance&) noexcept;
    static PipelineKey BuildPipelineKey(const MaterialDesc&, const MaterialInstance&) noexcept;
    static void NormalizeDesc(MaterialDesc&) noexcept;
};

} // namespace engine::renderer
