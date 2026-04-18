#pragma once
// =============================================================================
// KROM Engine - renderer/RenderWorld.hpp
// Render-Laufzeitrepraesentation: Proxies, generische Feature-Daten, DrawQueues.
// Wird von ISceneExtractionStep direkt befuellt - kein SceneSnapshot-Umweg.
// =============================================================================
#include "ecs/Components.hpp"
#include "renderer/MaterialSystem.hpp"
#include <any>
#include <cstddef>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace engine::renderer {

// =============================================================================
// RenderProxy - extrahierte Render-Einheit aus ECS
// =============================================================================
struct RenderProxy
{
    EntityID       entity;
    MeshHandle     mesh;
    MaterialHandle material;
    math::Mat4     worldMatrix;
    math::Mat4     worldMatrixInvT;
    math::Vec3     boundsCenter;
    math::Vec3     boundsExtents;
    float          boundsRadius = 1.f;
    uint32_t       layerMask    = 0xFFFFFFFFu;
    bool           castShadows  = true;
    bool           visible      = true;
};

// =============================================================================
// DrawItem - konkreter GPU-Zeichenbefehl
// =============================================================================
struct DrawItem
{
    SortKey    sortKey;

    MeshHandle     mesh;
    MaterialHandle material;
    EntityID       entity;

    BufferHandle gpuVertexBuffer;
    BufferHandle gpuIndexBuffer;
    uint32_t     gpuIndexCount   = 0u;
    uint32_t     gpuVertexStride = 0u;

    uint32_t   cbOffset      = 0u;
    uint32_t   cbSize        = 0u;
    uint32_t   instanceCount = 1u;
    uint32_t   firstInstance = 0u;

    [[nodiscard]] bool hasGpuData() const noexcept
    {
        return gpuVertexBuffer.IsValid() && gpuIndexBuffer.IsValid() && gpuIndexCount > 0u;
    }

    bool operator<(const DrawItem& o) const noexcept { return sortKey < o.sortKey; }
};

// =============================================================================
// PerObjectConstants - wird pro DrawItem in den CB-Pool geschrieben
// =============================================================================
struct alignas(16) PerObjectConstants
{
    float worldMatrix[16];
    float worldMatrixInvT[16];
    float entityId[4];
};

// =============================================================================
// DrawList
// =============================================================================
struct DrawList
{
    RenderPassID          passId = StandardRenderPasses::Opaque();
    std::vector<DrawItem> items;
    bool                  sorted = false;

    void Clear() { items.clear(); sorted = false; }
    void Add(DrawItem&& item) { items.push_back(std::move(item)); sorted = false; }
    void Sort();
    size_t Size() const noexcept { return items.size(); }
};

// =============================================================================
// RenderQueue
// =============================================================================
struct RenderQueue
{
    std::vector<DrawList> lists;
    std::unordered_map<RenderPassID, size_t> indices;
    std::vector<PerObjectConstants> objectConstants;

    void Clear()
    {
        lists.clear();
        indices.clear();
        objectConstants.clear();
    }

    void SortAll()
    {
        for (DrawList& list : lists)
            list.Sort();
    }

    [[nodiscard]] DrawList& GetOrCreateList(RenderPassID passId) noexcept
    {
        auto it = indices.find(passId);
        if (it != indices.end())
            return lists[it->second];

        const size_t index = lists.size();
        DrawList list{};
        list.passId = passId;
        lists.push_back(std::move(list));
        indices.emplace(passId, index);
        return lists.back();
    }

    [[nodiscard]] DrawList* FindList(RenderPassID passId) noexcept
    {
        const auto it = indices.find(passId);
        return it != indices.end() ? &lists[it->second] : nullptr;
    }

    [[nodiscard]] const DrawList* FindList(RenderPassID passId) const noexcept
    {
        const auto it = indices.find(passId);
        return it != indices.end() ? &lists[it->second] : nullptr;
    }

    [[nodiscard]] std::vector<DrawList>& GetLists() noexcept { return lists; }
    [[nodiscard]] const std::vector<DrawList>& GetLists() const noexcept { return lists; }
};

// =============================================================================
// FrameConstants - globale Shader-Konstanten pro Frame (CB0 / PerFrame)
//
// ACHTUNG: Dieses Layout ist der verbindliche Vertrag zwischen CPU und allen
// Shader-Backends (HLSL, GLSL, SPIR-V). Aenderungen erfordern synchrone
// Anpassungen in ALLEN Shader-Dateien, die CB0 deklarieren.
//
// Byte-Offset-Uebersicht:
//   0   - 63  : viewMatrix (mat4)
//   64  - 127 : projMatrix (mat4)
//   128 - 191 : viewProjMatrix (mat4)
//   192 - 255 : invViewProjMatrix (mat4)
//   256 - 271 : cameraPosition (vec4)
//   272 - 287 : cameraForward (vec4)
//   288 - 303 : screenSize (vec4)
//   304 - 319 : timeData (vec4)
//   320 - 335 : ambientColor (vec4)
//   336 - 339 : featureCount0 (feature-spezifisch, aktuell Lighting)
//   340 - 343 : featureCount1 (feature-spezifisch reserviert)
//   344 - 347 : nearPlane (float)
//   348 - 351 : farPlane (float)
//   352 - 863 : featurePayload (512 Byte feature-spezifische Frame-Daten)
//   864 - 867 : iblPrefilterLevels (float, = kIBLPrefilterMipCount - 1)
//   868 - 879 : _padFC[3] (reserved, zero)
//   Gesamt    : 880 Byte
// =============================================================================
static constexpr uint32_t kFrameFeaturePayloadBytes = 512u;

struct alignas(16) FrameConstants
{
    float    viewMatrix[16];
    float    projMatrix[16];
    float    viewProjMatrix[16];
    float    invViewProjMatrix[16];
    float    cameraPosition[4];
    float    cameraForward[4];
    float    screenSize[4];
    float    timeData[4];
    float    ambientColor[4];
    uint32_t featureCount0;
    uint32_t featureCount1;
    float    nearPlane;
    float    farPlane;
    std::byte featurePayload[kFrameFeaturePayloadBytes];
    float    iblPrefilterLevels;
    float    _padFC[3];
};
static_assert(sizeof(FrameConstants) == 880u, "FrameConstants size mismatch - update all shaders");
static_assert(offsetof(FrameConstants, featurePayload) == 352u, "featurePayload must start at offset 352");

// =============================================================================
// RenderWorld
// =============================================================================
class RenderWorld
{
public:
    void AddRenderable(EntityID entity, MeshHandle mesh, MaterialHandle material,
                       const math::Mat4& worldMatrix, const math::Mat4& worldMatrixInvT,
                       const math::Vec3& boundsCenter, const math::Vec3& boundsExtents,
                       float boundsRadius, uint32_t layerMask, bool castShadows);

    void BuildDrawLists(const math::Mat4& view,
                        const math::Mat4& viewProj,
                        float nearZ,
                        float farZ,
                        const MaterialSystem& materials,
                        uint32_t layerMask = 0xFFFFFFFFu);

    [[nodiscard]] const std::vector<RenderProxy>& GetProxies() const { return m_proxies; }
    [[nodiscard]] RenderQueue& GetQueue() { return m_queue; }
    [[nodiscard]] const RenderQueue& GetQueue() const { return m_queue; }
    [[nodiscard]] uint32_t VisibleCount() const { return m_visibleCount; }
    [[nodiscard]] uint32_t TotalProxyCount() const { return static_cast<uint32_t>(m_proxies.size()); }

    template<typename T>
    [[nodiscard]] T& GetOrCreateFeatureData()
    {
        const std::type_index key{typeid(T)};
        auto [it, inserted] = m_featureData.try_emplace(key, T{});
        (void)inserted;
        return *std::any_cast<T>(&it->second);
    }

    template<typename T>
    [[nodiscard]] T* GetFeatureData() noexcept
    {
        const auto it = m_featureData.find(std::type_index{typeid(T)});
        if (it == m_featureData.end())
            return nullptr;
        return std::any_cast<T>(&it->second);
    }

    template<typename T>
    [[nodiscard]] const T* GetFeatureData() const noexcept
    {
        const auto it = m_featureData.find(std::type_index{typeid(T)});
        if (it == m_featureData.end())
            return nullptr;
        return std::any_cast<T>(&it->second);
    }

    void Clear()
    {
        m_proxies.clear();
        m_featureData.clear();
        m_queue.Clear();
        m_visibleCount = 0u;
    }

private:
    std::vector<RenderProxy> m_proxies;
    std::unordered_map<std::type_index, std::any> m_featureData;
    RenderQueue m_queue;
    uint32_t m_visibleCount = 0u;

    static bool FrustumTest(const math::Vec3& center,
                            const math::Vec3& extents,
                            const math::Vec4 planes[6]) noexcept;

    static void ExtractFrustumPlanes(const math::Mat4& viewProj,
                                     math::Vec4 outPlanes[6]) noexcept;

    static float ComputeLinearDepth(const math::Vec3& worldPos,
                                    const math::Mat4& view,
                                    float nearZ, float farZ) noexcept;

    DrawItem BuildDrawItem(const RenderProxy& proxy,
                           const MaterialSystem& materials,
                           float linearDepth,
                           bool isShadow,
                           uint32_t submissionOrder) const noexcept;
};

} // namespace engine::renderer
