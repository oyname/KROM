#pragma once
// =============================================================================
// KROM Engine - assets/AssetRegistry.hpp
// Asset-Handle-System mit Generationen - CPU-seitige Asset-Verwaltung.
// Trennung: CPU-Asset (Rohdaten) vs GPU-Ressource (backend-spezifisch).
// Architektur geeignet für Hot-Reload und Streaming.
//
// header-only (bewusst): AssetStore<T, Tag> ist ein vollständig parametrisiertes
// Klassen-Template. Alle Member-Definitionen müssen in jedem includierenden
// Translation-Unit sichtbar sein - eine Auslagerung in eine .cpp-Datei würde
// explizite Template-Instantiierungen für alle Asset-Typen erfordern und die
// Typsicherheit durch den Tag-Parameter unterlaufen.
// Nicht-Template-Hilfsfunktionen (z.B. ComputeBounds) sind inline, da sie eng
// an die Asset-Daten gebunden sind und keine externe Abhängigkeit erzeugen.
// =============================================================================
#include "core/Types.hpp"
#include "core/Math.hpp"
#include "core/Debug.hpp"
#include "renderer/ShaderContract.hpp"
#include <cfloat>
#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <unordered_map>

namespace engine::assets {

// Zustand eines Assets im Ladeprozess
enum class AssetState : uint8_t
{
    Unloaded  = 0,
    Loading   = 1,
    Loaded    = 2,
    Failed    = 3,
    Evicted   = 4, // War geladen, wurde entfernt (Hot-Reload pending)
};

// Gemeinsame Basisklasse aller CPU-seitigen Assets
struct AssetBase
{
    std::string  path;
    std::string  debugName;
    AssetState   state = AssetState::Unloaded;
    uint64_t     lastModifiedTimestamp = 0ull; // für Hot-Reload
};

// GPU-Ressourcen-Status - backend-seitig befüllt
struct GpuUploadStatus
{
    bool uploaded = false;
    bool dirty    = false; // Asset wurde geändert, GPU-Ressource veraltet
};

// =============================================================================
// Mesh-Asset (CPU)
// =============================================================================
struct SubMeshData
{
    std::vector<float>    positions;  // 3 floats per vertex
    std::vector<float>    normals;    // 3 floats
    std::vector<float>    tangents;   // 3 floats
    std::vector<float>    uvs;        // 2 floats
    std::vector<float>    colors;     // 4 floats per vertex (RGBA) 
    std::vector<uint32_t> indices;
    uint32_t              materialIndex = 0u;
};

struct MeshAsset : AssetBase
{
    std::vector<SubMeshData> submeshes;
    GpuUploadStatus gpuStatus{};

    // Berechnet AABB aller Vertices
    void ComputeBounds(math::Vec3& outMin, math::Vec3& outMax) const
    {
        outMin = math::Vec3( FLT_MAX,  FLT_MAX,  FLT_MAX);
        outMax = math::Vec3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (const auto& sub : submeshes)
        {
            for (size_t i = 0; i + 2 < sub.positions.size(); i += 3)
            {
                math::Vec3 p{ sub.positions[i], sub.positions[i+1], sub.positions[i+2] };
                outMin.x = std::min(outMin.x, p.x); outMax.x = std::max(outMax.x, p.x);
                outMin.y = std::min(outMin.y, p.y); outMax.y = std::max(outMax.y, p.y);
                outMin.z = std::min(outMin.z, p.z); outMax.z = std::max(outMax.z, p.z);
            }
        }
    }
};

// =============================================================================
// Texture-Asset (CPU)
// =============================================================================
enum class TextureFormat : uint8_t
{
    Unknown = 0,
    RGBA8_UNORM, RGBA8_SRGB,
    R8_UNORM, RG8_UNORM,
    BC1, BC3, BC4, BC5, BC7,
    RGBA16F, R11G11B10F,
    DEPTH24_STENCIL8, DEPTH32F,
};

struct TextureAsset : AssetBase
{
    uint32_t            width       = 0u;
    uint32_t            height      = 0u;
    uint32_t            depth       = 1u;
    uint32_t            mipLevels   = 1u;
    uint32_t            arraySize   = 1u;
    TextureFormat       format      = TextureFormat::RGBA8_UNORM;
    std::vector<uint8_t> pixelData;
    GpuUploadStatus     gpuStatus{};
    bool                isCubemap   = false;
    bool                sRGB        = true;
};

// =============================================================================
// Shader-Asset (CPU)
// =============================================================================
enum class ShaderStage : uint8_t
{
    Vertex   = 0,
    Fragment = 1,
    Compute  = 2,
    Geometry = 3,
    Hull     = 4,
    Domain   = 5,
};

enum class ShaderSourceLanguage : uint8_t
{
    Unknown = 0,
    HLSL,
    GLSL,
    WGSL,
};

enum class ShaderTargetProfile : uint8_t
{
    Generic = 0,
    Null,
    DirectX11_SM5,
    DirectX12_SM6,
    Vulkan_SPIRV,
    OpenGL_GLSL450,
};

struct ShaderDependencyRecord
{
    std::string path;
    uint64_t    contentHash = 0ull;
};

struct CompiledShaderArtifact
{
    ShaderTargetProfile  target = ShaderTargetProfile::Generic;
    ShaderStage          stage = ShaderStage::Vertex;
    std::string          entryPoint = "main";
    std::string          debugName;
    std::vector<uint8_t> bytecode;
    std::string          sourceText;
    uint64_t             sourceHash = 0ull;
    std::vector<std::string> defines;
    std::string          cacheKey;
    uint32_t             cacheSchemaVersion = 0u;
    std::vector<ShaderDependencyRecord> dependencies;
    engine::renderer::ShaderPipelineContract contract;

    [[nodiscard]] bool IsValid() const noexcept { return !bytecode.empty() || !sourceText.empty(); }
};

struct ShaderAsset : AssetBase
{
    ShaderStage                       stage     = ShaderStage::Vertex;
    ShaderSourceLanguage              sourceLanguage = ShaderSourceLanguage::Unknown;
    std::string                       entryPoint = "main";
    std::string                       sourceCode;    // Logische Shader-Quelle
    std::string                       resolvedPath;  // absolute/aufgeloeste Quelldatei fuer Includes/Cache
    std::vector<uint8_t>              bytecode;      // Legacy-Fallback / vorkompiliert
    std::vector<CompiledShaderArtifact> compiledArtifacts; // backend-/profilgerichtete Artefakte
    GpuUploadStatus                   gpuStatus{};
};

// =============================================================================
// Material-Asset (CPU)
// =============================================================================
struct MaterialParam
{
    enum class Type : uint8_t { Float, Vec2, Vec3, Vec4, Int, Bool, Texture };
    std::string name;
    Type        type;
    union {
        float     f[4];
        int32_t   i;
        bool      b;
    } value{};
    TextureHandle texture; // wenn type == Texture
};

struct MaterialAsset : AssetBase
{
    ShaderHandle              vertexShader;
    ShaderHandle              fragmentShader;
    std::vector<MaterialParam> params;
    GpuUploadStatus           gpuStatus{};
    bool                      transparent     = false;
    bool                      doubleSided     = false;
    bool                      castShadows     = true;
};

// =============================================================================
// AssetStore<T, Tag>
// Typsicherer Handle-basierter Store für einen Asset-Typ.
// Generationssichere Slots (analog zu KROM's ResourceStore).
// =============================================================================
template<typename T, typename Tag>
class AssetStore
{
public:
    using HandleType = Handle<Tag>;

    HandleType Add(std::unique_ptr<T> asset)
    {
        if (!m_freeSlots.empty())
        {
            uint32_t idx = m_freeSlots.back();
            m_freeSlots.pop_back();
            uint32_t gen = m_generations[idx] + 1u;
            if (gen == 0u) gen = 1u;
            m_generations[idx] = gen;
            m_slots[idx] = std::move(asset);
            return HandleType::Make(idx, gen);
        }
        uint32_t idx = static_cast<uint32_t>(m_slots.size());
        m_slots.push_back(std::move(asset));
        m_generations.push_back(1u);
        return HandleType::Make(idx, 1u);
    }

    [[nodiscard]] T* Get(HandleType handle) noexcept
    {
        if (!IsValid(handle)) return nullptr;
        return m_slots[handle.Index()].get();
    }

    [[nodiscard]] const T* Get(HandleType handle) const noexcept
    {
        if (!IsValid(handle)) return nullptr;
        return m_slots[handle.Index()].get();
    }

    void Remove(HandleType handle)
    {
        if (!IsValid(handle)) return;
        m_slots[handle.Index()].reset();
        m_freeSlots.push_back(handle.Index());
    }

    [[nodiscard]] bool IsValid(HandleType handle) const noexcept
    {
        const uint32_t idx = handle.Index();
        if (!handle.IsValid()) return false;
        if (idx >= m_slots.size()) return false;
        return m_generations[idx] == handle.Generation() && m_slots[idx] != nullptr;
    }

    [[nodiscard]] size_t Count() const noexcept { return m_slots.size() - m_freeSlots.size(); }

    // Iteration über alle gültigen Assets
    template<typename Func>
    void ForEach(Func&& fn)
    {
        for (uint32_t i = 0; i < m_slots.size(); ++i)
        {
            if (m_slots[i])
                fn(HandleType::Make(i, m_generations[i]), *m_slots[i]);
        }
    }

private:
    std::vector<std::unique_ptr<T>> m_slots;
    std::vector<uint32_t>           m_generations;
    std::vector<uint32_t>           m_freeSlots;
};

// =============================================================================
// AssetRegistry - zentrale Asset-Verwaltung
// Registriert Assets, vergibt Handles, erlaubt Pfad-basiertes Lookup.
// =============================================================================
class AssetRegistry
{
public:
    // --- Stores ---
    AssetStore<MeshAsset,     MeshTag>     meshes;
    AssetStore<TextureAsset,  TextureTag>  textures;
    AssetStore<ShaderAsset,   ShaderTag>   shaders;
    AssetStore<MaterialAsset, MaterialTag> materials;

    // --- Pfad-Cache ---
    [[nodiscard]] MeshHandle GetOrAddMesh(const std::string& path, std::unique_ptr<MeshAsset> asset)
    {
        auto it = m_meshByPath.find(path);
        if (it != m_meshByPath.end()) return it->second;
        asset->path = path;
        MeshHandle h = meshes.Add(std::move(asset));
        m_meshByPath[path] = h;
        return h;
    }

    [[nodiscard]] TextureHandle GetOrAddTexture(const std::string& path, std::unique_ptr<TextureAsset> asset)
    {
        auto it = m_texByPath.find(path);
        if (it != m_texByPath.end()) return it->second;
        asset->path = path;
        TextureHandle h = textures.Add(std::move(asset));
        m_texByPath[path] = h;
        return h;
    }

    [[nodiscard]] ShaderHandle GetOrAddShader(const std::string& path, std::unique_ptr<ShaderAsset> asset)
    {
        auto it = m_shaderByPath.find(path);
        if (it != m_shaderByPath.end()) return it->second;
        asset->path = path;
        ShaderHandle h = shaders.Add(std::move(asset));
        m_shaderByPath[path] = h;
        return h;
    }

    [[nodiscard]] MaterialHandle GetOrAddMaterial(const std::string& path, std::unique_ptr<MaterialAsset> asset)
    {
        auto it = m_materialByPath.find(path);
        if (it != m_materialByPath.end()) return it->second;
        asset->path = path;
        MaterialHandle h = materials.Add(std::move(asset));
        m_materialByPath[path] = h;
        return h;
    }

    // Callback für Hot-Reload: wird aufgerufen wenn Asset neu geladen wurde
    using OnAssetReloaded = std::function<void(MeshHandle)>;
    void SetMeshReloadCallback(OnAssetReloaded cb) { m_meshReloadCb = std::move(cb); }

    void NotifyMeshReloaded(MeshHandle h) { if (m_meshReloadCb) m_meshReloadCb(h); }

private:
    std::unordered_map<std::string, MeshHandle>     m_meshByPath;
    std::unordered_map<std::string, TextureHandle>  m_texByPath;
    std::unordered_map<std::string, ShaderHandle>   m_shaderByPath;
    std::unordered_map<std::string, MaterialHandle> m_materialByPath;
    OnAssetReloaded m_meshReloadCb;
};

} // namespace engine::assets
