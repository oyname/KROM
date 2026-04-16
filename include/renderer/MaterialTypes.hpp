#pragma once

#include "renderer/RendererTypes.hpp"
#include "renderer/ShaderParameterLayout.hpp"
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

struct MaterialParam
{
    enum class Type : uint8_t { Float, Vec2, Vec3, Vec4, Int, Bool, Texture, Sampler, Buffer };
    std::string name;
    Type        type = Type::Float;
    union Value {
        float    f[4];
        int32_t  i;
        bool     b;
        Value() { f[0]=f[1]=f[2]=f[3]=0.f; }
    } value{};
    TextureHandle  texture;
    BufferHandle   buffer;
    uint32_t       samplerIdx = 0u;
};

struct MaterialBinding
{
    uint32_t        slot = 0u;
    uint32_t        space = 0u;
    ShaderStageMask stages = ShaderStageMask::None;
    enum class Kind : uint8_t { ConstantBuffer, Texture, Sampler, Buffer } kind = Kind::ConstantBuffer;
    std::string     name;
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
    ShaderParameterLayout        parameterLayout{};

    MaterialRenderPolicy renderPolicy{};

    bool doubleSided    = false;
    bool castShadows    = true;
    float alphaCutoff   = 0.5f;

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
    uint32_t     offset = 0u;
    uint32_t     size = 0u;
    uint32_t     arrayCount = 1u;
    MaterialParam::Type type = MaterialParam::Type::Float;
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

namespace std {
    template<> struct hash<engine::renderer::ShaderPermutationKey> {
        size_t operator()(const engine::renderer::ShaderPermutationKey& k) const noexcept {
            return std::hash<uint64_t>{}(k.defineFlags) ^ (std::hash<uint32_t>{}(k.baseShader) << 1u);
        }
    };
}
