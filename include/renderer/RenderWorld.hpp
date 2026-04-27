#pragma once
// =============================================================================
// KROM Engine - renderer/RenderWorld.hpp
// Render-Laufzeitrepraesentation: Proxies, generische Feature-Daten, DrawQueues.
// Wird von ISceneExtractionStep direkt befuellt - kein SceneSnapshot-Umweg.
// =============================================================================
#include "ecs/Components.hpp"
#include "renderer/MaterialSystem.hpp"
#include <cstddef>
#include <memory>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <vector>

namespace engine::jobs { class JobSystem; }

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

struct alignas(16) VulkanPerObjectPushConstants
{
    float worldRow0[4];
    float worldRow1[4];
    float worldRow2[4];
    float worldInvTRow0[4];
    float worldInvTRow1[4];
    float worldInvTRow2[4];
    float entityId[4];
};
static_assert(sizeof(VulkanPerObjectPushConstants) == 112u,
              "Vulkan per-object push constants must stay within the 128-byte minimum guarantee");

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
//   336 - 339 : featureCount0 (Licht-Anzahl)
//   340 - 343 : shadowCascadeCount (0=kein Shadow, 1=Shadow aktiv; V1 nur 0 oder 1)
//   344 - 347 : nearPlane (float)
//   348 - 351 : farPlane (float)
//   352 - 799 : featurePayload[0..447]  = 7x GpuLightData (7 * 64 = 448 Byte)
//   800 - 863 : featurePayload[448..511] = Shadow Light-VP-Matrix (float[16] = 64 Byte)
//   864 - 867 : iblPrefilterLevels (float)
//   868 - 871 : shadowBias (float)
//   872 - 875 : shadowNormalBias (float)
//   876 - 879 : shadowStrength (float)
//   880 - 883 : shadowTexelSize (float, = 1/shadowResolution)
//   884 - 887 : debugMode (uint32, derzeit reserviert; fuer ABI-/Shader-Layout auf 0 gehalten)
//   888 - 895 : _shadowPad[0..1] (2x float, explizites Padding auf 16-Byte-Grenze)
//   Gesamt    : 896 Byte
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
    uint32_t shadowCascadeCount;   // 0 = kein Shadow, 1 = Shadow aktiv (V1)
    float    nearPlane;
    float    farPlane;
    std::byte featurePayload[kFrameFeaturePayloadBytes];
    float    iblPrefilterLevels;
    float    shadowBias;
    float    shadowNormalBias;
    float    shadowStrength;
    float    shadowTexelSize;    // = 1.0 / shadowResolution, vorberechnet auf CPU
    uint32_t debugMode;          // reserviert; wird derzeit immer auf 0 gehalten
    float    _shadowPad[2];      // explizites Padding auf 16-Byte-Grenze
};
static_assert(sizeof(FrameConstants) == 896u, "FrameConstants size mismatch - update all shaders");
static_assert(offsetof(FrameConstants, featurePayload) == 352u, "featurePayload must start at offset 352");

using RenderFeatureDataSlot = uint32_t;

class RenderFeatureDataRegistry
{
public:
    struct Entry
    {
        std::type_index type = typeid(void);
        std::string name;
    };

    [[nodiscard]] RenderFeatureDataSlot Register(std::type_index type, std::string name);
    [[nodiscard]] const Entry* Get(RenderFeatureDataSlot slot) const noexcept;

    template<typename T>
    [[nodiscard]] RenderFeatureDataSlot Register(std::string_view name)
    {
        return Register(std::type_index(typeid(T)), std::string(name));
    }

    [[nodiscard]] RenderFeatureDataSlot Find(std::type_index type) const noexcept;

    template<typename T>
    [[nodiscard]] RenderFeatureDataSlot Find() const noexcept
    {
        return Find(std::type_index(typeid(T)));
    }

private:
    std::vector<Entry> m_entries;
};

// =============================================================================
// RenderWorld
// =============================================================================
class RenderWorld
{
public:
    struct FeatureDataStorageBase
    {
        virtual ~FeatureDataStorageBase() = default;
    };

    template<typename T>
    struct FeatureDataStorage final : FeatureDataStorageBase
    {
        T value{};
    };

    void AddRenderable(EntityID entity, MeshHandle mesh, MaterialHandle material,
                       const math::Mat4& worldMatrix, const math::Mat4& worldMatrixInvT,
                       const math::Vec3& boundsCenter, const math::Vec3& boundsExtents,
                       float boundsRadius, uint32_t layerMask, bool castShadows);

    void BuildDrawLists(const math::Mat4& view,
                        const math::Mat4& viewProj,
                        float nearZ,
                        float farZ,
                        const MaterialSystem& materials,
                        const RenderPassRegistry& renderPassRegistry,
                        uint32_t layerMask = 0xFFFFFFFFu,
                        jobs::JobSystem* jobSystem = nullptr);

    [[nodiscard]] const std::vector<RenderProxy>& GetProxies() const { return m_proxies; }
    [[nodiscard]] RenderQueue& GetQueue() { return m_queue; }
    [[nodiscard]] const RenderQueue& GetQueue() const { return m_queue; }
    [[nodiscard]] uint32_t VisibleCount() const { return m_visibleCount; }
    [[nodiscard]] uint32_t TotalProxyCount() const { return static_cast<uint32_t>(m_proxies.size()); }
    [[nodiscard]] RenderFeatureDataRegistry& GetFeatureDataRegistry() noexcept { return m_featureDataRegistry; }
    [[nodiscard]] const RenderFeatureDataRegistry& GetFeatureDataRegistry() const noexcept { return m_featureDataRegistry; }

    template<typename T>
    [[nodiscard]] T& GetOrCreateFeatureData(std::string_view name)
    {
        const RenderFeatureDataSlot slot = m_featureDataRegistry.Register<T>(name);
        if (slot >= m_featureData.size())
            m_featureData.resize(slot + 1u);
        if (!m_featureData[slot])
            m_featureData[slot] = std::make_unique<FeatureDataStorage<T>>();
        return static_cast<FeatureDataStorage<T>&>(*m_featureData[slot]).value;
    }

    template<typename T>
    [[nodiscard]] T* GetFeatureData() noexcept
    {
        const RenderFeatureDataSlot slot = m_featureDataRegistry.Find<T>();
        if (slot == static_cast<RenderFeatureDataSlot>(-1))
            return nullptr;
        if (slot >= m_featureData.size() || !m_featureData[slot])
            return nullptr;
        return &static_cast<FeatureDataStorage<T>*>(m_featureData[slot].get())->value;
    }

    template<typename T>
    [[nodiscard]] const T* GetFeatureData() const noexcept
    {
        const RenderFeatureDataSlot slot = m_featureDataRegistry.Find<T>();
        if (slot == static_cast<RenderFeatureDataSlot>(-1))
            return nullptr;
        if (slot >= m_featureData.size() || !m_featureData[slot])
            return nullptr;
        return &static_cast<const FeatureDataStorage<T>*>(m_featureData[slot].get())->value;
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
    RenderFeatureDataRegistry m_featureDataRegistry;
    std::vector<std::unique_ptr<FeatureDataStorageBase>> m_featureData;
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
                           const RenderPassRegistry& renderPassRegistry,
                           float linearDepth,
                           bool isShadow,
                           uint32_t submissionOrder) const noexcept;
};

struct RenderSceneSnapshot
{
    RenderWorld world;

    [[nodiscard]] RenderWorld& GetWorld() noexcept
    {
        return world;
    }

    [[nodiscard]] const RenderWorld& GetWorld() const noexcept
    {
        return world;
    }

    [[nodiscard]] RenderQueue& GetQueue() noexcept
    {
        return world.GetQueue();
    }

    [[nodiscard]] const RenderQueue& GetQueue() const noexcept
    {
        return world.GetQueue();
    }

    [[nodiscard]] uint32_t VisibleCount() const noexcept
    {
        return world.VisibleCount();
    }

    [[nodiscard]] uint32_t TotalProxyCount() const noexcept
    {
        return world.TotalProxyCount();
    }

    void Clear()
    {
        world.Clear();
    }
};

} // namespace engine::renderer
