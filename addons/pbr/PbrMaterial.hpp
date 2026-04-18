#pragma once

#include "renderer/MaterialSystem.hpp"
#include "renderer/ShaderBindingModel.hpp"

namespace engine::renderer::pbr {

enum class PbrShaderBackend : uint8_t
{
    DX11 = 0,
    OpenGL,
    Vulkan,
};

struct PbrShaderAssetSet
{
    const char* vertexShader = nullptr;
    const char* fragmentShader = nullptr;
    const char* shadowShader = nullptr;
    RenderPassID renderPass = StandardRenderPasses::Opaque();
};

struct PbrMaterialCreateInfo
{
    std::string  name = "PBR";
    ShaderHandle vertexShader;
    ShaderHandle fragmentShader;
    ShaderHandle shadowShader;
    VertexLayout vertexLayout;
    Format       colorFormat = Format::RGBA16_FLOAT;
    Format       depthFormat = Format::D24_UNORM_S8_UINT;
    RenderPassID renderPass = StandardRenderPasses::Opaque();

    math::Vec4 baseColorFactor{1.f, 1.f, 1.f, 1.f};
    math::Vec4 emissiveFactor{0.f, 0.f, 0.f, 1.f};
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float occlusionStrength = 1.0f;
    float opacityFactor = 1.0f;
    float alphaCutoff = 0.5f;
    int32_t materialFeatureMask = 0;
    float materialModel = 0.0f;

    bool enableBaseColorMap = true;
    bool enableNormalMap = true;
    bool enableORMMap = true;
    bool enableEmissiveMap = true;
    bool enableIBL = true;

    MaterialCullMode cullMode = MaterialCullMode::None;
    bool castShadows = true;
    bool receiveShadows = true;
    bool doubleSided = false;
};

class PbrMaterial
{
public:
    PbrMaterial() = default;
    PbrMaterial(MaterialSystem& materials, MaterialHandle handle) noexcept;

    [[nodiscard]] static PbrShaderAssetSet DefaultShaderAssetSet(PbrShaderBackend backend) noexcept;
    static void ApplyDefaultShaderAssetSet(PbrMaterialCreateInfo& info, PbrShaderBackend backend) noexcept;

    [[nodiscard]] static MaterialDesc BuildDesc(const PbrMaterialCreateInfo& info);
    [[nodiscard]] static MaterialHandle Register(MaterialSystem& materials, const PbrMaterialCreateInfo& info);
    [[nodiscard]] static PbrMaterial Create(MaterialSystem& materials, const PbrMaterialCreateInfo& info) noexcept;

    [[nodiscard]] bool IsValid() const noexcept;

    [[nodiscard]] bool SetBaseColorFactor(const math::Vec4& value) noexcept;
    [[nodiscard]] bool SetEmissiveFactor(const math::Vec4& value) noexcept;
    [[nodiscard]] bool SetMetallicFactor(float value) noexcept;
    [[nodiscard]] bool SetRoughnessFactor(float value) noexcept;
    [[nodiscard]] bool SetOcclusionStrength(float value) noexcept;
    [[nodiscard]] bool SetOpacityFactor(float value) noexcept;
    [[nodiscard]] bool SetAlphaCutoff(float value) noexcept;

    [[nodiscard]] bool SetAlbedo(TextureHandle texture) noexcept;
    [[nodiscard]] bool SetNormal(TextureHandle texture) noexcept;
    [[nodiscard]] bool SetORM(TextureHandle texture) noexcept;
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
    int32_t           m_normalSlot = -1;
    int32_t           m_ormSlot = -1;
    int32_t           m_emissiveSlot = -1;
};

} // namespace engine::renderer::pbr
