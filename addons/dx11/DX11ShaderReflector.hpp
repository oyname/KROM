#pragma once

#include "renderer/ShaderReflector.hpp"

namespace engine::renderer::dx11 {

class DX11ShaderReflector final : public IShaderReflector
{
public:
    [[nodiscard]] bool Reflect(const assets::ShaderAsset& shader,
                               ShaderParameterLayout& outLayout,
                               std::string* outError = nullptr) const override;

    [[nodiscard]] bool ReflectProgram(const assets::ShaderAsset& vertexShader,
                                      const assets::ShaderAsset& fragmentShader,
                                      ShaderParameterLayout& outLayout,
                                      std::string* outError = nullptr) const;
};

} // namespace engine::renderer::dx11
