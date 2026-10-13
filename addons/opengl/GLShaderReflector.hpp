#pragma once

#include "renderer/ShaderReflector.hpp"

namespace engine::renderer::opengl {

class GLShaderReflector final : public IShaderReflector
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

} // namespace engine::renderer::opengl
