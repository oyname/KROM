#pragma once

#include "renderer/MaterialSystem.hpp"
#include "renderer/ShaderBindingModel.hpp"

namespace engine::renderer::lit {

enum class LitShaderBackend : uint8_t
{
    DX11 = 0,
    OpenGL,
    Vulkan,
};

struct LitShaderAssetSet
{
    const char* vertexShader = nullptr;
    const char* fragmentShader = nullptr;
    const char* shadowShader = nullptr;
    RenderPassID renderPass = StandardRenderPasses::Opaque();
};

struct LitMaterialCreateInfo
{
    std::string  name = "Lit";
    ShaderHandle vertexShader;
    ShaderHandle fragmentShader;
    ShaderHandle shadowShader;
    VertexLayout vertexLayout;
    Format       colorFormat = Format::RGBA16_FLOAT;
    Format       depthFormat = Format::D24_UNORM_S8_UINT;
    RenderPassID renderPass = StandardRenderPasses::Opaque();

    math::Vec4 baseColorFactor{1.f, 1.f, 1.f, 1.f};
    math::Vec4 emissiveFactor{0.f, 0.f, 0.f, 1.f};
    float specularStrength = 0.25f;
    float roughnessFactor = 0.65f;
    float opacityFactor = 1.0f;
    float alphaCutoff = 0.5f;

    bool enableBaseColorMap = true;
    bool enableEmissiveMap = false;
    bool alphaTest = false;
    bool doubleSided = false;
    bool castShadows = true;

    MaterialCullMode cullMode = MaterialCullMode::Back;
    WindingOrder frontFace = WindingOrder::CCW;
};

class LitMaterial
{
public:
    LitMaterial() = default;
    LitMaterial(MaterialSystem& materials, MaterialHandle handle) noexcept;

    [[nodiscard]] static LitShaderAssetSet DefaultShaderAssetSet(LitShaderBackend backend) noexcept;
    static void ApplyDefaultShaderAssetSet(LitMaterialCreateInfo& info, LitShaderBackend backend) noexcept;

    [[nodiscard]] static MaterialDesc BuildDesc(const LitMaterialCreateInfo& info);
    [[nodiscard]] static MaterialHandle Register(MaterialSystem& materials, const LitMaterialCreateInfo& info);
    [[nodiscard]] static LitMaterial Create(MaterialSystem& materials, const LitMaterialCreateInfo& info) noexcept;

    [[nodiscard]] bool IsValid() const noexcept;

    [[nodiscard]] bool SetBaseColorFactor(const math::Vec4& value) noexcept;
    [[nodiscard]] bool SetEmissiveFactor(const math::Vec4& value) noexcept;
    [[nodiscard]] bool SetSpecularStrength(float value) noexcept;
    [[nodiscard]] bool SetRoughnessFactor(float value) noexcept;
    [[nodiscard]] bool SetOpacityFactor(float value) noexcept;
    [[nodiscard]] bool SetAlphaCutoff(float value) noexcept;
    [[nodiscard]] bool SetAlbedo(TextureHandle texture) noexcept;
    [[nodiscard]] bool SetEmissive(TextureHandle texture) noexcept;

    [[nodiscard]] MaterialHandle Handle() const noexcept { return m_handle; }
    [[nodiscard]] MaterialInstance& Raw();
    [[nodiscard]] const MaterialInstance& Raw() const;

private:
    [[nodiscard]] bool SetTextureAtSlot(int32_t slotIndex, const char* slotName, TextureHandle texture) noexcept;
    void CacheSlots() noexcept;

    MaterialSystem*   m_materials = nullptr;
    MaterialHandle    m_handle = MaterialHandle::Invalid();
    MaterialInstance* m_instance = nullptr;
    int32_t           m_albedoSlot = -1;
    int32_t           m_emissiveSlot = -1;
};

} // namespace engine::renderer::lit
