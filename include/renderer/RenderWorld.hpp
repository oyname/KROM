#pragma once
// =============================================================================
// KROM Engine - renderer/RenderWorld.hpp
// Extract-Phase: ECS → Render-Repräsentation.
//
// Ablauf pro Frame:
//   1. ExtractPhase:   ECS-View → RenderProxy-Liste (Thread-safe lesbar)
//   2. CullPhase:      Frustum-Culling → visible DrawItems
//   3. SortPhase:      DrawList nach SortKey sortieren
//   4. SubmitPhase:    DrawList an Backend übergeben
//
// RenderProxy:   eine extrahierte Render-Einheit (mesh + material + world matrix)
// DrawItem:      ein konkreter Draw-Aufruf mit Sort-Key und CB-Offset
// DrawList:      sortierte Liste von DrawItems pro Pass
// RenderQueue:   fasst alle DrawLists zusammen (Opaque, Transparent, Shadow, UI, Particles)
// =============================================================================
#include "renderer/MaterialSystem.hpp"
#include "renderer/SceneSnapshot.hpp"
#include <vector>
#include <array>

namespace engine::renderer {

// =============================================================================
// RenderProxy - extrahierte Render-Einheit aus ECS
// Flache Kopie - kein Zeiger zurück ins ECS.
// =============================================================================
struct RenderProxy
{
    EntityID       entity;
    MeshHandle     mesh;
    MaterialHandle material;
    math::Mat4     worldMatrix;
    math::Mat4     worldMatrixInvT; // Transponierte Inverse für Normalen
    math::Vec3     boundsCenter;    // Weltkoordinaten
    math::Vec3     boundsExtents;
    float          boundsRadius = 1.f;
    uint32_t       layerMask    = 0xFFFFFFFFu;
    bool           castShadows  = true;
    bool           visible      = true; // nach Culling gesetzt
};

// =============================================================================
// LightProxy - extrahiertes Licht
// =============================================================================
struct LightProxy
{
    EntityID   entity;
    LightType  lightType;
    math::Vec3 positionWorld;
    math::Vec3 directionWorld;
    math::Vec3 color;
    float      intensity   = 1.f;
    float      range       = 10.f;
    float      spotInner   = 0.f;
    float      spotOuter   = 0.f;
    bool       castShadows = false;
};

// =============================================================================
// DrawItem - konkreter GPU-Zeichenbefehl
// =============================================================================
struct DrawItem
{
    SortKey    sortKey;

    MeshHandle     mesh;
    MaterialHandle material;
    EntityID       entity;      // für per-Object-CB-Lookup

    // GPU-Buffer-Handles (befüllt von RenderSystem vor Execute)
    BufferHandle gpuVertexBuffer;
    BufferHandle gpuIndexBuffer;
    uint32_t     gpuIndexCount   = 0u;
    uint32_t     gpuVertexStride = 0u;

    // Konstanten-Daten-Offset im Frame-CB-Pool (Backend befüllt)
    uint32_t   cbOffset     = 0u;
    uint32_t   cbSize       = 0u;

    // Instancing (für späteres Batching)
    uint32_t   instanceCount = 1u;
    uint32_t   firstInstance = 0u;

    [[nodiscard]] bool hasGpuData() const noexcept
    { return gpuVertexBuffer.IsValid() && gpuIndexBuffer.IsValid() && gpuIndexCount > 0u; }

    bool operator<(const DrawItem& o) const noexcept { return sortKey < o.sortKey; }
};

// =============================================================================
// PerObjectConstants - wird pro DrawItem in den CB-Pool geschrieben
// Entspricht den häufigsten Shader-Inputs
// =============================================================================
struct alignas(16) PerObjectConstants
{
    float worldMatrix[16];
    float worldMatrixInvT[16]; // transponierte Inverse
    float entityId[4];         // x = entity index, yzw = padding
};

// =============================================================================
// DrawList - sortierte Liste von DrawItems für einen Pass
// =============================================================================
struct DrawList
{
    RenderPassTag          passTag;
    std::vector<DrawItem>  items;
    bool                   sorted = false;

    void Clear() { items.clear(); sorted = false; }
    void Add(DrawItem&& item) { items.push_back(std::move(item)); sorted = false; }

    // Radix-Sort nach SortKey (64-Bit)
    void Sort();

    size_t Size() const noexcept { return items.size(); }
};

// =============================================================================
// RenderQueue - fasst alle DrawLists pro Frame zusammen
// =============================================================================
struct RenderQueue
{
    DrawList opaque;
    DrawList alphaCutout;
    DrawList transparent;
    DrawList shadow;
    DrawList ui;
    DrawList particles;

    // Per-Object Constant Buffer Pool (alle Objects dieses Frames)
    std::vector<PerObjectConstants> objectConstants;

    void Clear()
    {
        opaque.Clear();      opaque.passTag      = RenderPassTag::Opaque;
        alphaCutout.Clear(); alphaCutout.passTag  = RenderPassTag::AlphaCutout;
        transparent.Clear(); transparent.passTag  = RenderPassTag::Transparent;
        shadow.Clear();      shadow.passTag       = RenderPassTag::Shadow;
        ui.Clear();          ui.passTag           = RenderPassTag::UI;
        particles.Clear();   particles.passTag    = RenderPassTag::Particle;
        objectConstants.clear();
    }

    void SortAll()
    {
        opaque.Sort(); alphaCutout.Sort(); transparent.Sort();
        shadow.Sort(); ui.Sort();         particles.Sort();
    }

    [[nodiscard]] DrawList& GetList(RenderPassTag tag) noexcept
    {
        switch (tag) {
        case RenderPassTag::Opaque:      return opaque;
        case RenderPassTag::AlphaCutout: return alphaCutout;
        case RenderPassTag::Transparent: return transparent;
        case RenderPassTag::Shadow:      return shadow;
        case RenderPassTag::UI:          return ui;
        case RenderPassTag::Particle:    return particles;
        default:                         return opaque;
        }
    }
};

// =============================================================================
// FrameConstants - globale Shader-Konstanten pro Frame
// =============================================================================
struct alignas(16) FrameConstants
{
    float viewMatrix[16];
    float projMatrix[16];
    float viewProjMatrix[16];
    float invViewProjMatrix[16];
    float cameraPosition[4];    // xyz + w=1
    float cameraForward[4];     // xyz + w=0
    float screenSize[4];        // width, height, 1/width, 1/height
    float timeData[4];          // x=time, y=deltaTime, z=frame, w=0
    float ambientColor[4];      // xyz=color, w=intensity
    uint32_t lightCount;
    uint32_t shadowCascadeCount;
    float    nearPlane;
    float    farPlane;
};

// =============================================================================
// RenderWorld - vollständiges extrahiertes Frame-Snapshot
// =============================================================================
class RenderWorld
{
public:
    // Übernimmt extrahierte Daten aus einem SceneSnapshot.
    // Kein ECS-Zugriff - ECSExtractor::Extract() vorher aufrufen.
    void Extract(const SceneSnapshot& snapshot, const MaterialSystem& materials);

    // Visibility-Phase: füllt DrawLists aus sichtbaren Proxies
    // view     = reine View-Matrix (für View-Space-Tiefenberechnung)
    // viewProj = View * Proj (für Frustum-Culling)
    void BuildDrawLists(const math::Mat4& view,
                        const math::Mat4& viewProj,
                        float              nearZ,
                        float              farZ,
                        const MaterialSystem& materials,
                        uint32_t           layerMask = 0xFFFFFFFFu);

    // Frame-Konstanten setzen (Kamera, Zeit etc.)
    void SetFrameConstants(const FrameConstants& fc) { m_frameConstants = fc; }

    // Zugriff
    [[nodiscard]] const std::vector<RenderProxy>& GetProxies()        const { return m_proxies; }
    [[nodiscard]] const std::vector<LightProxy>&  GetLights()         const { return m_lights; }
    [[nodiscard]] RenderQueue&                    GetQueue()                { return m_queue; }
    [[nodiscard]] const RenderQueue&              GetQueue()           const { return m_queue; }
    [[nodiscard]] const FrameConstants&           GetFrameConstants()  const { return m_frameConstants; }
    [[nodiscard]] uint32_t                        VisibleCount()       const { return m_visibleCount; }
    [[nodiscard]] uint32_t                        TotalProxyCount()    const
    { return static_cast<uint32_t>(m_proxies.size()); }

    void Clear()
    {
        m_proxies.clear();
        m_lights.clear();
        m_queue.Clear();
        m_visibleCount = 0u;
    }

private:
    std::vector<RenderProxy> m_proxies;
    std::vector<LightProxy>  m_lights;
    RenderQueue              m_queue;
    FrameConstants           m_frameConstants{};
    uint32_t                 m_visibleCount = 0u;

    // Frustum-Culling helper
    static bool FrustumTest(const math::Vec3& center,
                             const math::Vec3& extents,
                             const math::Vec4  planes[6]) noexcept;

    static void ExtractFrustumPlanes(const math::Mat4& viewProj,
                                      math::Vec4 outPlanes[6]) noexcept;

    // Tiefe eines Proxy im View-Space.
    // Verwendet die echte View-Matrix (nicht ViewProj):
    //   p_view = view * p_world
    //   viewZ  = view.m[0][2]*x + view.m[1][2]*y + view.m[2][2]*z + view.m[3][2]
    // Kamera schaut in RH-Space in -Z → Tiefe = -viewZ
    static float ComputeLinearDepth(const math::Vec3& worldPos,
                                     const math::Mat4& view,   // echte View-Matrix
                                     float nearZ, float farZ) noexcept;

    // DrawItem aus Proxy bauen
    DrawItem BuildDrawItem(const RenderProxy& proxy,
                            const MaterialSystem& materials,
                            float linearDepth,
                            bool  isShadow) const noexcept;
};

} // namespace engine::renderer
