#pragma once

#include "renderer/MaterialInstance.hpp"
#include "renderer/MaterialTypes.hpp"
#include "renderer/RenderPassRegistry.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::renderer { class MaterialSystem; }

namespace engine::renderer::unlit {

struct UnlitShaderAssetSet
{
    const char* vertexShader   = nullptr;
    const char* fragmentShader = nullptr;
    RenderPassID renderPass    = StandardRenderPasses::Opaque();
};

struct UnlitMaterialCreateInfo
{
    std::string  name = "Unlit";
    ShaderHandle vertexShader;
    ShaderHandle fragmentShader;
    VertexLayout vertexLayout;
    Format       colorFormat = Format::RGBA16_FLOAT;
    Format       depthFormat = Format::D24_UNORM_S8_UINT;
    RenderPassID renderPass  = StandardRenderPasses::Opaque();

    math::Vec4 baseColorFactor{1.f, 1.f, 1.f, 1.f};
    math::Vec4 emissiveFactor{0.f, 0.f, 0.f, 1.f};
    float opacityFactor = 1.0f;
    float alphaCutoff   = 0.5f;

    bool enableBaseColorMap = true;
    bool enableEmissiveMap  = false;
    bool alphaTest          = false;
    bool doubleSided        = false;
    bool castShadows        = true;

    MaterialCullMode cullMode  = MaterialCullMode::Back;
    WindingOrder     frontFace = WindingOrder::CCW;
    std::vector<MaterialParam> extraParameters;
    std::unordered_map<std::string, uint32_t> textureBindingOverrides;
};

class UnlitMaterial
{
public:
    UnlitMaterial() = default;
    UnlitMaterial(MaterialSystem& materials, MaterialHandle handle) noexcept;

    [[nodiscard]] static UnlitShaderAssetSet DefaultShaderAssetSet() noexcept;

    [[nodiscard]] static MaterialDesc   BuildDesc(const UnlitMaterialCreateInfo& info);
    [[nodiscard]] static MaterialHandle Register(MaterialSystem& materials,
                                                  const UnlitMaterialCreateInfo& info);
    [[nodiscard]] static UnlitMaterial  Create(MaterialSystem& materials,
                                               const UnlitMaterialCreateInfo& info) noexcept;

    [[nodiscard]] bool IsValid() const noexcept;

    [[nodiscard]] bool SetBaseColorFactor(const math::Vec4& value) noexcept;
    [[nodiscard]] bool SetEmissiveFactor(const math::Vec4& value) noexcept;
    [[nodiscard]] bool SetOpacityFactor(float value) noexcept;
    [[nodiscard]] bool SetAlphaCutoff(float value) noexcept;
    [[nodiscard]] bool SetAlbedo(TextureHandle texture) noexcept;
    [[nodiscard]] bool SetEmissive(TextureHandle texture) noexcept;

    [[nodiscard]] MaterialHandle    Handle() const noexcept { return m_handle; }
    [[nodiscard]] MaterialInstance& Raw();
    [[nodiscard]] const MaterialInstance& Raw() const;

private:
    [[nodiscard]] bool SetTextureAtSlot(int32_t slotIndex, const char* slotName,
                                        TextureHandle texture) noexcept;
    void CacheSlots() noexcept;

    MaterialSystem*   m_materials  = nullptr;
    MaterialHandle    m_handle     = MaterialHandle::Invalid();
    MaterialInstance* m_instance   = nullptr;
    int32_t           m_albedoSlot   = -1;
    int32_t           m_emissiveSlot = -1;
};

} // namespace engine::renderer::unlit
