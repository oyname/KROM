#pragma once

#include "renderer/ShaderReflector.hpp"
#include "assets/AssetRegistry.hpp"
#include <vector>

namespace engine::renderer::vulkan {

/// Ein einzelnes Feld innerhalb eines Cbuffers (aus SPIR-V-Reflection).
struct CBufferField
{
    std::string name;
    uint32_t    byteOffset = 0u;
    uint32_t    byteSize   = 0u;
    uint32_t    rows       = 1u;
    uint32_t    columns    = 1u;

    [[nodiscard]] assets::MaterialParam::Type ToParamType() const noexcept;
};

class VKShaderReflector final : public IShaderReflector
{
public:
    [[nodiscard]] bool Reflect(const assets::ShaderAsset& shader,
                               ShaderParameterLayout& outLayout,
                               std::string* outError = nullptr) const override;

    [[nodiscard]] bool ReflectProgram(const assets::ShaderAsset& vertexShader,
                                      const assets::ShaderAsset& fragmentShader,
                                      ShaderParameterLayout& outLayout,
                                      std::string* outError = nullptr) const;

    /// Liest alle Felder aus dem Cbuffer mit dem angegebenen Binding-Slot.
    [[nodiscard]] bool ReflectCBufferFields(const assets::ShaderAsset& shader,
                                            uint32_t bindingSlot,
                                            std::vector<CBufferField>& outFields,
                                            std::string* outError = nullptr) const;
};

} // namespace engine::renderer::vulkan
