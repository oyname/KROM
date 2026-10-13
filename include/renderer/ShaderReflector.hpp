#pragma once

#include "assets/AssetRegistry.hpp"
#include "renderer/ShaderParameterLayout.hpp"
#include <string>

namespace engine::renderer {

class IShaderReflector
{
public:
    virtual ~IShaderReflector() = default;

    [[nodiscard]] virtual bool Reflect(const assets::ShaderAsset& shader,
                                       ShaderParameterLayout& outLayout,
                                       std::string* outError = nullptr) const = 0;
};

} // namespace engine::renderer
