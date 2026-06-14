#pragma once

#include "renderer/ShaderReflector.hpp"
#include "assets/AssetRegistry.hpp"
#include <vector>

namespace engine::renderer::dx11 {

/// Ein einzelnes Feld innerhalb eines Cbuffers (aus DX11-Reflection).
struct CBufferField
{
    std::string name;
    uint32_t    byteOffset = 0u;
    uint32_t    byteSize   = 0u;   // Gesamtgröße in Bytes
    uint32_t    rows       = 1u;   // 1=Scalar/Vec, 4=Mat4
    uint32_t    columns    = 1u;   // 1=float, 2=float2, 3=float3, 4=float4

    /// Mappt das Feld auf den passenden MaterialParam::Type
    [[nodiscard]] assets::MaterialParam::Type ToParamType() const noexcept;
};

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

    /// Liest alle Felder aus dem Cbuffer mit dem angegebenen Namen.
    /// Gibt true zurück wenn der Cbuffer gefunden und Felder ausgelesen wurden.
    [[nodiscard]] bool ReflectCBufferFields(const assets::ShaderAsset& shader,
                                            const char* cbufferName,
                                            std::vector<CBufferField>& outFields,
                                            std::string* outError = nullptr) const;
};

} // namespace engine::renderer::dx11
