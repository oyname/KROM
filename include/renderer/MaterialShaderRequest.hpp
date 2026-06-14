#pragma once

#include "renderer/CompiledMaterialDesc.hpp"
#include "renderer/RendererTypes.hpp"

namespace engine::renderer {

enum class MaterialLightingMode : uint8_t
{
    Unlit = 0,
    Lit,
    Pbr,
};

enum class MaterialRenderPassRequirement : uint8_t
{
    Forward = 0,
    Shadow,
    Depth,
    PostProcess,
    Custom,
};

struct MaterialVertexLayoutRequirement
{
    uint64_t semanticMask = 0ull;
    uint32_t layoutHash = 0u;
    bool requiresNormals = false;
    bool requiresTangents = false;
    bool requiresTexCoords = false;
    bool requiresVertexColor = false;
    bool supportsInstancing = false;
    bool supportsSkinning = false;
};

struct MaterialShaderVariantKey
{
    uint64_t shaderAssetHash = 0ull;
    MaterialShaderStageRequirement stage = MaterialShaderStageRequirement::Fragment;
    MaterialRenderPassRequirement renderPass = MaterialRenderPassRequirement::Forward;
    MaterialLightingMode lighting = MaterialLightingMode::Lit;
    ShaderVariantFlag flags = ShaderVariantFlag::None;
    uint32_t vertexLayoutHash = 0u;
    uint64_t resourceLayoutHash = 0ull;

    [[nodiscard]] MaterialShaderVariantKey Normalized() const noexcept;
    [[nodiscard]] uint64_t Hash() const noexcept;

    bool operator==(const MaterialShaderVariantKey& other) const noexcept
    {
        return shaderAssetHash == other.shaderAssetHash &&
               stage == other.stage &&
               renderPass == other.renderPass &&
               lighting == other.lighting &&
               flags == other.flags &&
               vertexLayoutHash == other.vertexLayoutHash &&
               resourceLayoutHash == other.resourceLayoutHash;
    }
};

struct MaterialShaderRequest
{
    std::string materialStableId;
    std::string materialName;
    std::string shaderAsset;
    std::string entryPoint;

    MaterialShaderStageRequirement stage = MaterialShaderStageRequirement::Fragment;
    MaterialRenderPassRequirement renderPass = MaterialRenderPassRequirement::Forward;
    MaterialLightingMode lighting = MaterialLightingMode::Lit;
    MaterialVertexLayoutRequirement vertexLayout{};
    ShaderVariantFlag flags = ShaderVariantFlag::None;
    MaterialShaderVariantKey variantKey{};
    uint64_t requestHash = 0ull;
};

[[nodiscard]] std::vector<MaterialShaderRequest> BuildMaterialShaderRequests(
    const CompiledMaterialDesc& compiled,
    MaterialFeatureFlags instanceOverrides = MaterialFeatureFlags::None) noexcept;

[[nodiscard]] uint64_t HashMaterialShaderRequest(const MaterialShaderRequest& request) noexcept;

} // namespace engine::renderer

namespace std {
    template<> struct hash<engine::renderer::MaterialShaderVariantKey> {
        size_t operator()(const engine::renderer::MaterialShaderVariantKey& key) const noexcept {
            return static_cast<size_t>(key.Hash());
        }
    };
}
