#pragma once
#include "renderer/MaterialTypes.hpp"

namespace engine::renderer {

struct MaterialFeatureEval {
    static uint32_t DeriveFeatureMask(const MaterialDesc&, const MaterialInstance&) noexcept;
    static ShaderVariantFlag BuildShaderVariantFlags(const MaterialDesc&, const MaterialInstance&) noexcept;
    static PipelineKey BuildPipelineKey(const MaterialDesc&, const MaterialInstance&) noexcept;
    static void NormalizeDesc(MaterialDesc&) noexcept;
    static MaterialSemanticValue DefaultSemanticValue(MaterialSemantic semantic,
                                                      float alphaCutoff) noexcept;
    static MaterialSemanticValue ResolveSemanticValue(const MaterialDesc&, const MaterialInstance&, MaterialSemantic) noexcept;
};

} // namespace engine::renderer
