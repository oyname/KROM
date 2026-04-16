#pragma once
// =============================================================================
// KROM Engine - renderer/RenderWorld.hpp
// Render-Laufzeitrepräsentation: Proxies, Lichter, DrawQueues.
// Wird von ISceneExtractionStep direkt befüllt – kein SceneSnapshot-Umweg.
// =============================================================================
#include "renderer/MaterialSystem.hpp"
#include "ecs/Components.hpp"
#include <vector>
#include <array>

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
    float      spotInner   = 0.f;  // Grad
    float      spotOuter   = 0.f;  // Grad
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
    EntityID       entity;

    BufferHandle gpuVertexBuffer;
    BufferHandle gpuIndexBuffer;
    uint32_t     gpuIndexCount   = 0u;
    uint32_t     gpuVertexStride = 0u;

    uint32_t   cbOffset     = 0u;
    uint32_t   cbSize       = 0u;

    uint32_t   instanceCount = 1u;
    uint32_t   firstInstance = 0u;

    [[nodiscard]] bool hasGpuData() const noexcept
    { return gpuVertexBuffer.IsValid() && gpuIndexBuffer.IsValid() && gpuIndexCount > 0u; }

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
    RenderPassTag          passTag = RenderPassTag::Opaque;
    std::vector<DrawItem>  items;
    bool                   sorted = false;

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
    DrawList opaque;
    DrawList alphaCutout;
    DrawList transparent;
    DrawList shadow;
    DrawList ui;
    DrawList particles;

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
// GpuLightData - GPU-seitige Lichtdarstellung, inline im PerFrame-CB.
//
// Layout (std140-kompatibel, 4 × float4 = 64 Byte pro Licht):
//   positionWS:     xyz = Position (Point/Spot) bzw. Direction (Directional),
//                   w   = 1.0 (Point/Spot), 0.0 (Directional)
//   directionWS:    xyz = normierte Lichtrichtung, w = 0
//   colorIntensity: xyz = Lichtfarbe (linear), w = Intensität
//   params:         x   = cos(spotInnerAngle)
//                   y   = cos(spotOuterAngle)
//                   z   = range (maximale Reichweite)
//                   w   = lightType: 0=Directional, 1=Point, 2=Spot
//
// Alle Felder müssen mit den Shader-Deklarationen übereinstimmen (kein Padding
// durch std140 erforderlich, da alle Member vec4 sind).
// =============================================================================
static constexpr uint32_t kMaxLightsPerFrame = 8u;

struct alignas(16) GpuLightData
{
    float positionWS[4];      // xyz + w=0.0/1.0 je nach Typ
    float directionWS[4];     // xyz + w=0.0
    float colorIntensity[4];  // xyz=Farbe, w=Intensität
    float params[4];          // x=cosInner, y=cosOuter, z=range, w=lightType
};
static_assert(sizeof(GpuLightData) == 64u, "GpuLightData must be exactly 64 bytes");

// =============================================================================
// FrameConstants - globale Shader-Konstanten pro Frame (CB0 / PerFrame)
//
// ACHTUNG: Dieses Layout ist der verbindliche Vertrag zwischen CPU und allen
// Shader-Backends (HLSL, GLSL, SPIR-V). Änderungen erfordern synchrone
// Anpassungen in ALLEN Shader-Dateien, die CB0 deklarieren.
//
// Byte-Offset-Übersicht:
//   0   – 63  : viewMatrix (mat4)
//   64  – 127 : projMatrix (mat4)
//   128 – 191 : viewProjMatrix (mat4)
//   192 – 255 : invViewProjMatrix (mat4)
//   256 – 271 : cameraPosition (vec4)
//   272 – 287 : cameraForward (vec4)
//   288 – 303 : screenSize (vec4)
//   304 – 319 : timeData (vec4)
//   320 – 335 : ambientColor (vec4)
//   336 – 339 : lightCount (uint)
//   340 – 343 : shadowCascadeCount (uint)
//   344 – 347 : nearPlane (float)
//   348 – 351 : farPlane (float)
//   352 – 863 : lights[8] (8 × GpuLightData, je 64 Byte)
//   864 – 867 : iblPrefilterLevels (float, = kIBLPrefilterMipCount - 1)
//   868 – 879 : _padFC[3] (reserved, zero)
//   Gesamt    : 880 Byte
// =============================================================================
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
    uint32_t lightCount;
    uint32_t shadowCascadeCount;
    float    nearPlane;
    float    farPlane;
    GpuLightData lights[kMaxLightsPerFrame];
    // IBL prefilter LOD range: maxLod = kIBLPrefilterMipCount - 1.
    // Packed here so all backends use the same value — avoids hardcoded constants
    // in individual shaders drifting from the CPU-side EnvironmentSystem constants.
    float    iblPrefilterLevels;
    float    _padFC[3];
};
static_assert(sizeof(FrameConstants) == 880u, "FrameConstants size mismatch – update all shaders");
static_assert(offsetof(FrameConstants, lights) == 352u, "lights must start at offset 352");

// =============================================================================
// RenderWorld
// =============================================================================
class RenderWorld
{
public:
    // Direktes Befüllen durch ECSExtractor / ISceneExtractionStep.
    // Cosinus-Konvertierung für Spot-Winkel findet in AddLight statt.
    void AddRenderable(EntityID entity, MeshHandle mesh, MaterialHandle material,
                       const math::Mat4& worldMatrix, const math::Mat4& worldMatrixInvT,
                       const math::Vec3& boundsCenter, const math::Vec3& boundsExtents,
                       float boundsRadius, uint32_t layerMask, bool castShadows);

    void AddLight(EntityID entity, LightType lightType,
                  const math::Vec3& positionWorld, const math::Vec3& directionWorld,
                  const math::Vec3& color, float intensity, float range,
                  float spotInnerDeg, float spotOuterDeg, bool castShadows);

    void BuildDrawLists(const math::Mat4& view,
                        const math::Mat4& viewProj,
                        float              nearZ,
                        float              farZ,
                        const MaterialSystem& materials,
                        uint32_t           layerMask = 0xFFFFFFFFu);

    void SetFrameConstants(const FrameConstants& fc) { m_frameConstants = fc; }

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

    // Frustum-Culling-Hilfsfunktionen
    static bool FrustumTest(const math::Vec3& center,
                             const math::Vec3& extents,
                             const math::Vec4  planes[6]) noexcept;

    static void ExtractFrustumPlanes(const math::Mat4& viewProj,
                                      math::Vec4 outPlanes[6]) noexcept;

    // Lineare Tiefe eines Proxy im View-Space [0..1].
    static float ComputeLinearDepth(const math::Vec3& worldPos,
                                     const math::Mat4& view,
                                     float nearZ, float farZ) noexcept;

    // DrawItem aus Proxy bauen (ohne GPU-Buffer-Handles, die erst in CommitUploads gesetzt werden).
    DrawItem BuildDrawItem(const RenderProxy& proxy,
                            const MaterialSystem& materials,
                            float linearDepth,
                            bool  isShadow) const noexcept;
};

} // namespace engine::renderer
