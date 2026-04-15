#pragma once
#include "renderer/MaterialTypes.hpp"

namespace engine::renderer {

struct MaterialCBLayout {
    static CbLayout Build(const std::vector<MaterialParam>& params) noexcept;
    static void BuildCBData(MaterialInstance& inst, const MaterialDesc& desc);
};

} // namespace engine::renderer
