#pragma once

#include "renderer/MaterialInstance.hpp"

namespace engine::renderer {

struct MaterialCBLayout {
    static CbLayout Build(const ShaderParameterLayout& layout) noexcept;
    static void BuildCBData(MaterialInstance& inst);
};

} // namespace engine::renderer
