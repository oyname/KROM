#pragma once

#include "renderer/CompiledMaterialDesc.hpp"

namespace engine::renderer {

struct MaterialFeatureEval {
    static void NormalizeDesc(MaterialDesc&) noexcept;
    static CompiledMaterialDesc BuildCompiledDesc(const MaterialDefinition&) noexcept;
};

} // namespace engine::renderer
