#pragma once
#include "renderer/RendererTypes.hpp"
#include "core/Types.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace engine::renderer {

struct ShaderPermutationKey
{
    uint32_t baseShader = 0u;
    uint64_t defineFlags = 0ull;

    bool operator==(const ShaderPermutationKey& o) const noexcept {
        return baseShader == o.baseShader && defineFlags == o.defineFlags;
    }
};

} // namespace engine::renderer

namespace std {
    template<> struct hash<engine::renderer::ShaderPermutationKey> {
        size_t operator()(const engine::renderer::ShaderPermutationKey& k) const noexcept {
            return std::hash<uint64_t>{}(k.defineFlags) ^ (std::hash<uint32_t>{}(k.baseShader) << 32);
        }
    };
}

namespace engine::renderer {

struct MaterialParam
{
    enum class Type : uint8_t { Float, Vec2, Vec3, Vec4, Int, Bool, Texture, Sampler };
    std::string name;
    Type        type = Type::Float;
    union Value {
        float    f[4];
        int32_t  i;
        bool     b;
        Value() { f[0]=f[1]=f[2]=f[3]=0.f; }
    } value{};
    TextureHandle  texture;
    uint32_t       samplerIdx = 0u;
};

struct MaterialBinding
{
    uint32_t        slot   = 0u;
    uint32_t        space  = 0u;
    ShaderStageMask stages = ShaderStageMask::None;
    enum class Kind : uint8_t { ConstantBuffer, Texture, Sampler } kind;
};

enum class MaterialModel : uint8_t
{
    PBRMetalRough = 0,
    Unlit         = 1,
};

enum class MaterialSemantic : uint8_t
{
    BaseColor = 0,
    Normal,
    Metallic,    // value-only or single-channel texture; texture path collapses to ORM slot t2
    Roughness,   // value-only or single-channel texture; texture path collapses to ORM slot t2
    Occlusion,   // value-only or single-channel texture; texture path collapses to ORM slot t2
    Emissive,
    Opacity,
    AlphaCutoff,
    // Explicit packed ORM texture (R=Occlusion, G=Roughness, B=Metallic).
    // When set, takes priority over individual Metallic/Roughness/Occlusion textures for slot t2.
    // Preferred authoring path for GLTF-style assets where ORM is pre-packed.
    ORM,
    Count
};

static constexpr size_t kMaterialSemanticCount = static_cast<size_t>(MaterialSemantic::Count);

enum class MaterialFeatureFlag : uint32_t
{
    None             = 0u,
    BaseColorValue   = 1u << 0,
    BaseColorTexture = 1u << 1,
    NormalTexture    = 1u << 2,
    MetallicValue    = 1u << 3,
    MetallicTexture  = 1u << 4,
    RoughnessValue   = 1u << 5,
    RoughnessTexture = 1u << 6,
    OcclusionValue   = 1u << 7,
    OcclusionTexture = 1u << 8,
    EmissiveValue    = 1u << 9,
    EmissiveTexture  = 1u << 10,
    OpacityValue     = 1u << 11,
    OpacityTexture   = 1u << 12,
    AlphaTest        = 1u << 13,
    DoubleSided      = 1u << 14,
    CastShadows      = 1u << 15,
    ReceiveShadows   = 1u << 16,
    Unlit            = 1u << 17,
    PBRMetalRough    = 1u << 18,
    // Explicit packed ORM texture set via MaterialSemantic::ORM.
    // When active, MetallicTexture/RoughnessTexture/OcclusionTexture flags are not emitted.
    ORMTexture       = 1u << 19,
};

inline MaterialFeatureFlag operator|(MaterialFeatureFlag a, MaterialFeatureFlag b) noexcept
{
    return static_cast<MaterialFeatureFlag>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline MaterialFeatureFlag operator&(MaterialFeatureFlag a, MaterialFeatureFlag b) noexcept
{
    return static_cast<MaterialFeatureFlag>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline MaterialFeatureFlag& operator|=(MaterialFeatureFlag& a, MaterialFeatureFlag b) noexcept
{
    a = a | b;
    return a;
}

[[nodiscard]] inline bool HasMaterialFeature(MaterialFeatureFlag flags, MaterialFeatureFlag bit) noexcept
{
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(bit)) != 0u;
}

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

struct MaterialRenderPolicy
{
    MaterialBlendMode blendMode = MaterialBlendMode::Opaque;
    MaterialCullMode  cullMode = MaterialCullMode::Back;
    bool              alphaTest = false;
    float             alphaCutoff = 0.5f;
    bool              doubleSided = false;
    bool              castShadows = true;
    bool              receiveShadows = true;
};

struct MaterialSemanticValue
{
    bool set = false;
    std::array<float, 4> data = {0.f, 0.f, 0.f, 0.f};
};

struct MaterialSemanticTexture
{
    bool set = false;
    TextureHandle texture = TextureHandle::Invalid();
    uint32_t samplerIdx = 0u;
};

struct MaterialDesc
{
    std::string name;
    RenderPassTag passTag = RenderPassTag::Opaque;

    ShaderHandle vertexShader;
    ShaderHandle fragmentShader;
    ShaderHandle shadowShader;

    uint64_t permutationFlags = 0ull;

    RasterizerState    rasterizer;
    DepthStencilState  depthStencil;
    BlendState         blend;
    PrimitiveTopology  topology    = PrimitiveTopology::TriangleList;
    VertexLayout       vertexLayout;

    Format colorFormat = Format::RGBA16_FLOAT;
    Format depthFormat = Format::D24_UNORM_S8_UINT;

    std::vector<MaterialParam>   params;
    std::vector<MaterialBinding> bindings;

    MaterialModel model = MaterialModel::PBRMetalRough;
    MaterialRenderPolicy renderPolicy{};
    std::array<MaterialSemanticValue, kMaterialSemanticCount> semanticValues{};
    std::array<MaterialSemanticTexture, kMaterialSemanticCount> semanticTextures{};

    bool doubleSided    = false;   // [Deprecated shortcut] Use renderPolicy.doubleSided instead.
                                   // Merged into renderPolicy on RegisterMaterial; not read afterwards.
    bool castShadows    = true;    // [Deprecated shortcut] Use renderPolicy.castShadows instead.
                                   // Merged into renderPolicy on RegisterMaterial; not read afterwards.
    float alphaCutoff   = 0.5f;    // [Read-only after NormalizeDesc] Authoritative value is renderPolicy.alphaCutoff.

    uint8_t sortLayer = 0u;
};

struct SortKey
{
    uint64_t value = 0ull;

    static SortKey ForOpaque(RenderPassTag pass,
                              uint8_t layer,
                              uint32_t pipelineHash,
                              float linearDepth) noexcept;

    static SortKey ForTransparent(RenderPassTag pass,
                                   uint8_t layer,
                                   float linearDepth) noexcept;

    static SortKey ForUI(uint8_t layer, uint32_t drawOrder) noexcept;

    bool operator<(const SortKey& o) const noexcept { return value < o.value; }
    bool operator==(const SortKey& o) const noexcept { return value == o.value; }
};

struct CbFieldDesc
{
    std::string  name;
    uint32_t     offset;
    uint32_t     size;
    uint32_t     arrayCount;
    MaterialParam::Type type;
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

struct MaterialInstance
{
    MaterialHandle  desc;
    PipelineKey     pipelineKey;
    uint32_t        pipelineKeyHash = 0u;

    std::vector<MaterialParam> instanceParams;
    std::array<MaterialSemanticValue, kMaterialSemanticCount> semanticValues{};
    std::array<MaterialSemanticTexture, kMaterialSemanticCount> semanticTextures{};
    uint32_t        featureMask = 0u;

    CbLayout             cbLayout;
    std::vector<uint8_t> cbData;
    bool                 cbDirty  = true;
    bool                 layoutDirty = true;

    RenderPassTag PassTag() const noexcept;
    [[nodiscard]] float* GetFloatPtr(const std::string& name) noexcept;
};

} // namespace engine::renderer
