#pragma once

#include "renderer/MaterialTypes.hpp"
#include "renderer/ParameterBlob.hpp"

namespace engine::renderer {

struct MaterialCBLayout {
    static CbLayout Build(const MaterialParameterLayout& layout) noexcept;
    static void BuildCBData(const MaterialParameterLayout& layout,
                            const ParameterBlob& parameters,
                            CbLayout& outLayout,
                            std::vector<uint8_t>& outData);
};

} // namespace engine::renderer
