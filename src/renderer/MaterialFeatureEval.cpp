#include "renderer/MaterialFeatureEval.hpp"
#include "renderer/RenderPassRegistry.hpp"
#include <cstring>

namespace engine::renderer {

void MaterialFeatureEval::NormalizeDesc(MaterialDesc& desc) noexcept
{
    if (desc.renderPolicy.alphaTest)
        desc.features = desc.features | MaterialFeatureFlags::AlphaTest;
    if (desc.renderPolicy.cull.doubleSided)
        desc.features = desc.features | MaterialFeatureFlags::DoubleSided;
    if (HasFlag(desc.features, MaterialFeatureFlags::Unlit))
        desc.surface = MaterialSurfaceType::Unlit;
    if (desc.surface == MaterialSurfaceType::Cutout)
        desc.renderPolicy.alphaTest = true;
    if (desc.surface == MaterialSurfaceType::Transparent)
        desc.renderPolicy.blend.mode = MaterialBlendMode::AlphaBlend;
}

CompiledMaterialDesc MaterialFeatureEval::BuildCompiledDesc(const MaterialDefinition& definition) noexcept
{
    return CompileMaterialDefinition(definition);
}

} // namespace engine::renderer
