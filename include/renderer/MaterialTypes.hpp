#pragma once

#include "core/Types.hpp"
#include "renderer/MaterialParameterLayout.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::renderer {

enum class MaterialValueType : uint8_t
{
    Float,
    Vec2,
    Vec3,
    Vec4,
    Int,
    Bool,
    Texture,
    Sampler,
    Buffer,
};

struct MaterialParameter
{
    using Type = MaterialValueType;

    std::string name;
    MaterialValueType type = MaterialValueType::Float;

    union Value
    {
        float    f[4];
        int32_t  i;
        bool     b;
        Value() { f[0]=f[1]=f[2]=f[3]=0.f; }
    } value{};

    TextureHandle  texture;
    BufferHandle   buffer;
    uint32_t       samplerIdx = 0u;
};

enum class MaterialTextureSemantic : uint8_t
{
    Custom = 0,
    BaseColor,
    Normal,
    MetallicRoughnessOcclusion,
    Emissive,
    Opacity,
    Shadow,
    IblIrradiance,
    IblPrefiltered,
    BrdfLut,
};

struct MaterialTextureSlot
{
    std::string name;
    MaterialTextureSemantic semantic = MaterialTextureSemantic::Custom;
    std::string  defaultTextureAsset;
    TextureHandle defaultTexture;
    uint32_t      defaultSampler = 0u;
    uint8_t       uvSet = 0u;
    bool          required = false;
};

enum class MaterialFeatureFlags : uint64_t
{
    None                  = 0ull,
    Skinned               = 1ull << 0,
    VertexColor           = 1ull << 1,
    AlphaTest             = 1ull << 2,
    NormalMap             = 1ull << 3,
    Unlit                 = 1ull << 4,
    ShadowCaster          = 1ull << 5,
    Instanced             = 1ull << 6,
    BaseColorMap          = 1ull << 7,
    MetallicMap           = 1ull << 8,
    RoughnessMap          = 1ull << 9,
    OcclusionMap          = 1ull << 10,
    EmissiveMap           = 1ull << 11,
    OpacityMap            = 1ull << 12,
    PbrMetalRough         = 1ull << 13,
    DoubleSided           = 1ull << 14,
    OrmMap                = 1ull << 15,
    IblMap                = 1ull << 16,
    NormalMapBC5          = 1ull << 17,
    ChannelMap            = 1ull << 18,
};

inline MaterialFeatureFlags operator|(MaterialFeatureFlags a, MaterialFeatureFlags b) noexcept
{
    return static_cast<MaterialFeatureFlags>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
}

inline MaterialFeatureFlags operator&(MaterialFeatureFlags a, MaterialFeatureFlags b) noexcept
{
    return static_cast<MaterialFeatureFlags>(static_cast<uint64_t>(a) & static_cast<uint64_t>(b));
}

inline bool HasFlag(MaterialFeatureFlags flags, MaterialFeatureFlags bit) noexcept
{
    return (static_cast<uint64_t>(flags) & static_cast<uint64_t>(bit)) != 0ull;
}

enum class MaterialSurfaceType : uint8_t
{
    Opaque = 0,
    Transparent,
    Cutout,
    Unlit,
    ShadowOnly,
};

// Semantische Kategorie eines Materials — bestimmt, welche Pass-States
// der Pass erzwingt und was das Material frei wählen darf.
enum class MaterialDomain : uint8_t
{
    Mesh        = 0,  // Geometrie-Pass (Opaque, AlphaCutout, Transparent)
    Postprocess = 1,  // Fullscreen-Quad (Tonemap, Blur, Composite)
    UI          = 2,  // 2-D-UI-Overlay
    Shadow      = 3,  // Shadow-Map-Caster (depth-only)
    DepthOnly   = 4,  // Tiefenvorpass ohne Farbausgabe
    Debug       = 5,  // Wireframe, Overlay, Debug-Draw
};

enum class MaterialBlendMode : uint8_t
{
    Opaque = 0,
    AlphaBlend,
    Additive,
};

enum class MaterialCullMode : uint8_t
{
    Back = 0,
    Front,
    None,
};

enum class MaterialDepthCompare : uint8_t
{
    Less = 0,
    LessEqual,
    Equal,
    Always,
};

struct MaterialBlendPolicy
{
    MaterialBlendMode mode = MaterialBlendMode::Opaque;
};

struct MaterialCullPolicy
{
    MaterialCullMode mode = MaterialCullMode::Back;
    bool             doubleSided = false;
};

struct MaterialDepthPolicy
{
    bool                 test = true;
    bool                 write = true;
    MaterialDepthCompare compare = MaterialDepthCompare::Less;
};

struct MaterialRenderPolicy
{
    MaterialBlendPolicy blend{};
    MaterialCullPolicy  cull{};
    MaterialDepthPolicy depth{};
    bool                alphaTest = false;
    float               alphaCutoff = 0.5f;
    bool                castShadows = true;
    bool                receiveShadows = true;
};

struct MaterialDesc
{
    std::string name;
    MaterialSurfaceType  surface = MaterialSurfaceType::Opaque;
    MaterialFeatureFlags features = MaterialFeatureFlags::None;
    MaterialDomain       domain   = MaterialDomain::Mesh;

    std::vector<MaterialParameter>  parameters;
    std::vector<MaterialTextureSlot> textureSlots;
    MaterialParameterLayout         parameterLayout{};
    std::unordered_map<std::string, uint32_t> textureBindingOverrides;
    MaterialRenderPolicy renderPolicy{};

    uint8_t sortLayer = 0u;
    std::string materialGraph;
};

using MaterialParam = MaterialParameter;

struct MaterialDefinitionIdentity
{
    std::string stableId;
    std::string displayName;
    std::string sourceAsset;
    uint32_t    version = 1u;
};

struct MaterialGraphEndpoint
{
    std::string nodeId;
    std::string outputName;
};

struct MaterialGraphDefinition
{
    std::string graphId;
    std::string serializedGraph;
    std::vector<MaterialGraphEndpoint> outputs;
};

struct MaterialDefinition
{
    MaterialDefinitionIdentity identity{};
    MaterialDesc               desc{};
    MaterialGraphDefinition    graph{};
    uint64_t                   contentHash = 0ull;

    [[nodiscard]] const std::vector<MaterialParameter>& ParameterDefinitions() const noexcept
    {
        return desc.parameters;
    }

    [[nodiscard]] const std::vector<MaterialTextureSlot>& TextureSlots() const noexcept
    {
        return desc.textureSlots;
    }

    [[nodiscard]] MaterialFeatureFlags Features() const noexcept
    {
        return desc.features;
    }

    [[nodiscard]] MaterialSurfaceType Surface() const noexcept
    {
        return desc.surface;
    }

    [[nodiscard]] const MaterialRenderPolicy& RenderPolicy() const noexcept
    {
        return desc.renderPolicy;
    }
};

[[nodiscard]] uint64_t HashMaterialDefinition(const MaterialDefinition& definition) noexcept;

struct CbFieldDesc
{
    std::string  name;
    uint32_t     offset = 0u;
    uint32_t     size = 0u;
    uint32_t     arrayCount = 1u;
    MaterialValueType type = MaterialValueType::Float;
};

struct CbLayout
{
    std::vector<CbFieldDesc> fields;
    uint32_t                 totalSize = 0u;

    [[nodiscard]] uint32_t GetOffset(const std::string& name) const noexcept
    {
        for (const auto& f : fields)
            if (f.name == name) return f.offset;
        return UINT32_MAX;
    }
};

} // namespace engine::renderer
