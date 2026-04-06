#pragma once
// =============================================================================
// KROM Engine - renderer/MaterialSystem.hpp
// Materialsystem: PipelineKey, MaterialDesc, ShaderPermutation, SortKey.
// Deklaration. Implementierung: src/renderer/MaterialSystem.cpp
//
// Designprinzip:
//   - Material = Beschreibung (welche Shaders, welche Parameter, welcher Pass)
//   - PipelineKey = Hash der gesamten Pipeline-Konfiguration → Cache-Key
//   - SortKey = 64-Bit-Schlüssel für Draw-Call-Sortierung
//   - BindingSlots = API-neutrale Binding-Positionen für CB/SRV/Sampler
// =============================================================================
#include "renderer/RendererTypes.hpp"
#include "core/Types.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

// RenderPassTag und PipelineKey sind nach RendererTypes.hpp verschoben
// (Backends bekommen sie über IDevice.hpp → RendererTypes.hpp)

namespace engine::renderer {

// =============================================================================
// ShaderPermutation - Variante eines Shaders via Defines
// =============================================================================
struct ShaderPermutationKey
{
    uint32_t baseShader = 0u;  // Basis-ShaderHandle.value
    uint64_t defineFlags = 0ull; // Bitfeld: welche Defines aktiv

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

// =============================================================================
// MaterialParameter - typisierter Shader-Parameter
// =============================================================================
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
    TextureHandle  texture;   // wenn type == Texture
    uint32_t       samplerIdx = 0u; // wenn type == Sampler
};

// Binding-Slot für GPU-Ressource-Upload
struct MaterialBinding
{
    uint32_t        slot   = 0u;
    uint32_t        space  = 0u;
    ShaderStageMask stages = ShaderStageMask::None;
    enum class Kind : uint8_t { ConstantBuffer, Texture, Sampler } kind;
};

// =============================================================================
// MaterialDesc - vollständige Beschreibung eines Materials
// Wird von MaterialSystem verarbeitet, um PipelineKey + Bindings zu erzeugen.
// =============================================================================
struct MaterialDesc
{
    std::string name;
    RenderPassTag passTag = RenderPassTag::Opaque;

    // Shaders - Handles aus ShaderStore
    ShaderHandle vertexShader;
    ShaderHandle fragmentShader;
    ShaderHandle shadowShader;    // separater Shadow-VS (optional)

    // Permutations-Flags
    uint64_t permutationFlags = 0ull;

    // Pipeline-Zustände
    RasterizerState    rasterizer;
    DepthStencilState  depthStencil;
    BlendState         blend;
    PrimitiveTopology  topology    = PrimitiveTopology::TriangleList;
    VertexLayout       vertexLayout;

    // Zielformate (müssen mit RenderTarget übereinstimmen)
    Format colorFormat = Format::RGBA16_FLOAT; // HDR default
    Format depthFormat = Format::D24_UNORM_S8_UINT;

    // Parameter
    std::vector<MaterialParam>   params;
    std::vector<MaterialBinding> bindings;

    // Verhalten
    bool doubleSided    = false;
    bool castShadows    = true;
    float alphaCutoff   = 0.5f; // für AlphaCutout

    // Sortier-Priorität innerhalb des Passes (0 = default)
    uint8_t sortLayer = 0u;
};

// =============================================================================
// SortKey - 64-Bit Draw-Call-Sortierungsschlüssel
//
// Bit-Layout für Opaque (front-to-back, Materialwechsel minimieren):
//   [63..60] Pass (4 bit)
//   [59..56] Layer (4 bit)
//   [55..32] PipelineKey-Hash truncated (24 bit)
//   [31.. 0] Depth (32 bit, front-to-back: kleiner = näher)
//
// Für Transparent (back-to-front):
//   [63..60] Pass
//   [59..56] Layer
//   [55..32] Padding
//   [31.. 0] Depth invertiert (größer = weiter weg = zuerst)
// =============================================================================
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


// =============================================================================
// CbLayout - Name→Offset-Mapping für einen Constant Buffer
// Beschreibt wo im CB-Byte-Puffer jeder Parameter liegt.
// Wird einmal pro MaterialDesc berechnet und gecacht.
//
// Ohne echte Shader-Reflection bauen wir das Layout aus MaterialDesc.params.
// Mit Reflection (DX12 ID3D12ShaderReflection / SPIRV-Cross) würde man
// stattdessen die Shader-Metadaten auslesen.
// =============================================================================
struct CbFieldDesc
{
    std::string  name;
    uint32_t     offset;     // Byte-Offset im CB-Puffer
    uint32_t     size;       // Byte-Größe (4=float, 8=float2, 12=float3, 16=float4)
    uint32_t     arrayCount; // 1 = kein Array
    MaterialParam::Type type;
};

struct CbLayout
{
    std::vector<CbFieldDesc> fields;
    uint32_t                 totalSize = 0u; // Gesamtgröße in Bytes (16-Byte-aligned)

    // Lookup: gibt Offset zurück oder UINT32_MAX wenn nicht gefunden
    [[nodiscard]] uint32_t GetOffset(const std::string& name) const noexcept
    {
        for (const auto& f : fields)
            if (f.name == name) return f.offset;
        return UINT32_MAX;
    }

    // Baut Layout aus einer Param-Liste (HLSL cbuffer Packing-Regeln, vereinfacht)
    static CbLayout Build(const std::vector<MaterialParam>& params) noexcept;
};
// =============================================================================
// MaterialInstance - Runtime-Instanz eines Materials
// Enthält aktuelle Parameter-Werte (können per-Instance überschrieben werden).
// =============================================================================
struct MaterialInstance
{
    MaterialHandle  desc;
    PipelineKey     pipelineKey;
    uint32_t        pipelineKeyHash = 0u;

    std::vector<MaterialParam> instanceParams; // overrides über desc.params

    // CB-Layout und Daten (beides aus CbLayout::Build + BuildCBData)
    CbLayout             cbLayout;
    std::vector<uint8_t> cbData;
    bool                 cbDirty  = true;
    bool                 layoutDirty = true; // true wenn params sich geändert haben

    RenderPassTag PassTag() const noexcept;
    // Gibt Byte-Zeiger auf Feld 'name' im cbData-Puffer (nullptr wenn nicht gefunden)
    [[nodiscard]] float* GetFloatPtr(const std::string& name) noexcept;
};

// =============================================================================
// MaterialSystem - verwaltet MaterialDescs und Instances
// =============================================================================
class MaterialSystem
{
public:
    MaterialSystem()  = default;
    ~MaterialSystem() = default;

    // Registriert eine MaterialDesc, gibt Handle zurück
    [[nodiscard]] MaterialHandle RegisterMaterial(MaterialDesc desc);

    // Erstellt eine Instance einer MaterialDesc
    // Instances teilen Pipeline, können eigene Parameter haben
    [[nodiscard]] MaterialHandle CreateInstance(MaterialHandle base,
                                                std::string instanceName = "");

    // Zugriff
    [[nodiscard]] const MaterialDesc*     GetDesc(MaterialHandle h) const noexcept;
    [[nodiscard]] MaterialHandle          FindMaterial(const std::string& name) const noexcept;
    [[nodiscard]] MaterialInstance*       GetInstance(MaterialHandle h) noexcept;
    [[nodiscard]] const MaterialInstance* GetInstance(MaterialHandle h) const noexcept;

    // PipelineKey für ein Material
    [[nodiscard]] PipelineKey BuildPipelineKey(MaterialHandle h) const noexcept;

    // Parameter setzen
    void SetFloat  (MaterialHandle h, const std::string& name, float v);
    void SetVec4   (MaterialHandle h, const std::string& name, const math::Vec4& v);
    void SetTexture(MaterialHandle h, const std::string& name, TextureHandle tex);
    void MarkDirty (MaterialHandle h);

    // Constant-Buffer-Daten und Layout berechnen
    const std::vector<uint8_t>& GetCBData  (MaterialHandle h);
    const CbLayout&             GetCBLayout(MaterialHandle h);

    size_t DescCount()     const noexcept { return m_descs.size(); }
    size_t InstanceCount() const noexcept { return m_instances.size(); }

private:
    struct DescSlot
    {
        MaterialDesc     desc;
        std::string      name;
        bool             isInstance = false;
        MaterialHandle   baseHandle;
    };

    std::vector<DescSlot>        m_descs;
    std::vector<MaterialInstance> m_instances;
    std::unordered_map<std::string, MaterialHandle> m_nameLookup;
    std::vector<uint32_t>        m_generations;
    std::vector<uint32_t>        m_freeSlots;

    uint32_t AllocSlot();
    [[nodiscard]] bool ValidHandle(MaterialHandle h) const noexcept;

    void BuildCBData(MaterialInstance& inst, const MaterialDesc& desc);
};

} // namespace engine::renderer
