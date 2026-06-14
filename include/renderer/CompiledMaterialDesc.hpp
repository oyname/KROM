#pragma once

#include "renderer/MaterialTypes.hpp"

namespace engine::renderer {

enum class MaterialShaderStageRequirement : uint8_t
{
    Vertex = 0,
    Fragment,
    Compute,
};

enum class MaterialShaderPassRequirement : uint8_t
{
    Forward = 0,
    Shadow,
    Depth,
    PostProcess,
    Custom,
};

enum class MaterialResourceBindingKind : uint8_t
{
    Constant,
    Texture,
    Sampler,
    Buffer,
};

enum class MaterialPrimitivePolicy : uint8_t
{
    TriangleList = 0,
    TriangleStrip,
    LineList,
};

struct MaterialShaderRequirement
{
    MaterialShaderStageRequirement stage = MaterialShaderStageRequirement::Fragment;
    MaterialShaderPassRequirement  pass = MaterialShaderPassRequirement::Forward;
    std::string                    entryPoint;
    std::string                    shaderAsset;
    MaterialFeatureFlags           featureMask = MaterialFeatureFlags::None;
};

struct MaterialResourceBindingDesc
{
    std::string name;
    MaterialResourceBindingKind kind = MaterialResourceBindingKind::Constant;
    MaterialValueType valueType = MaterialValueType::Float;
    MaterialTextureSemantic textureSemantic = MaterialTextureSemantic::Custom;
    uint32_t logicalSlot = 0u;
    uint32_t byteOffset = 0u;
    uint32_t byteSize = 0u;
    bool required = false;
};

struct MaterialCompiledRenderState
{
    MaterialRenderPolicy renderPolicy{};
    MaterialPrimitivePolicy primitive = MaterialPrimitivePolicy::TriangleList;
    uint8_t sortLayer = 0u;
};

struct CompiledMaterialDesc
{
    std::string stableId;
    std::string name;
    uint32_t definitionVersion = 1u;

    MaterialSurfaceType surface = MaterialSurfaceType::Opaque;
    MaterialFeatureFlags features = MaterialFeatureFlags::None;
    MaterialCompiledRenderState renderState{};
    MaterialParameterLayout parameterLayout{};

    std::vector<MaterialShaderRequirement> shaderRequirements;
    std::vector<MaterialResourceBindingDesc> resourceBindings;

    uint64_t definitionHash = 0ull;
    uint64_t variantHash = 0ull;
    uint64_t resourceLayoutHash = 0ull;
    uint64_t compiledHash = 0ull;
};

[[nodiscard]] CompiledMaterialDesc CompileMaterialDefinition(const MaterialDefinition& definition) noexcept;
[[nodiscard]] uint64_t HashCompiledMaterialDesc(const CompiledMaterialDesc& compiled) noexcept;

} // namespace engine::renderer
