#pragma once
// =============================================================================
// KROM Engine - renderer/ShaderBindingModel.hpp
// Explizites Binding-Modell für alle Backends und Shader.
//
// ALLE Backends müssen exakt diese Slot-Konventionen einhalten.
// Shader müssen diese Register nutzen.
//
// Constant Buffer Slots:
//   CB0 = PerFrame     - Kamera, Lichter, Zeit, Viewport (einmal pro Frame)
//   CB1 = PerObject    - WorldMatrix, InvTranspose, EntityID (pro Draw Call)
//   CB2 = PerMaterial  - Albedo, Roughness, Metallic, etc. (pro Materialwechsel)
//   CB3 = PerPass      - Pass-spezifische Konstanten (Shadow-Kamera, PostProc-Params)
//
// Texture/SRV Slots:
//   t0  = Albedo / BaseColor Map
//   t1  = Normal Map
//   t2  = ORM (Occlusion/Roughness/Metallic)
//   t3  = Emissive Map
//   t4  = Shadow Map (Depth)
//   t5  = IBL Irradiance Cube
//   t6  = IBL Prefiltered Cube
//   t7  = BRDF LUT
//   t8..t15 = Pass-spezifische SRVs (History Buffer, Bloom etc.)
//
// Sampler Slots:
//   s0  = LinearWrap   (Default für Texturen)
//   s1  = LinearClamp  (PostProcess, UI)
//   s2  = PointClamp   (Debug, Nearest)
//   s3  = ShadowPCF    (comparison sampler für Shadow Map)
//
// UAV Slots (Compute):
//   u0..u7 = Compute-Output-Texturen / Buffers
// =============================================================================
#include <cstdint>

namespace engine::renderer {

// ---------------------------------------------------------------------------
// CB-Slots
// ---------------------------------------------------------------------------
struct CBSlots
{
    static constexpr uint32_t PerFrame    = 0u;
    static constexpr uint32_t PerObject   = 1u;
    static constexpr uint32_t PerMaterial = 2u;
    static constexpr uint32_t PerPass     = 3u;

    static constexpr uint32_t COUNT       = 4u;
};

// ---------------------------------------------------------------------------
// Texture-Slots
// ---------------------------------------------------------------------------
struct TexSlots
{
    static constexpr uint32_t Albedo          = 0u;
    static constexpr uint32_t Normal          = 1u;
    static constexpr uint32_t ORM             = 2u; // R=Occlusion, G=Roughness, B=Metallic
    static constexpr uint32_t Emissive        = 3u;
    static constexpr uint32_t ShadowMap       = 4u;
    static constexpr uint32_t IBLIrradiance   = 5u;
    static constexpr uint32_t IBLPrefiltered  = 6u;
    static constexpr uint32_t BRDFLUT         = 7u;

    // Pass-dynamische Slots
    static constexpr uint32_t PassSRV0        = 8u;
    static constexpr uint32_t PassSRV1        = 9u;
    static constexpr uint32_t PassSRV2        = 10u;
    static constexpr uint32_t HistoryBuffer   = 11u;
    static constexpr uint32_t BloomTexture    = 12u;

    static constexpr uint32_t COUNT           = 16u;
};

// ---------------------------------------------------------------------------
// Sampler-Slots
// ---------------------------------------------------------------------------
struct SamplerSlots
{
    static constexpr uint32_t LinearWrap  = 0u;
    static constexpr uint32_t LinearClamp = 1u;
    static constexpr uint32_t PointClamp  = 2u;
    static constexpr uint32_t ShadowPCF   = 3u;

    static constexpr uint32_t COUNT       = 4u;
};

// ---------------------------------------------------------------------------
// UAV-Slots (Compute)
// ---------------------------------------------------------------------------
struct UAVSlots
{
    static constexpr uint32_t Output0 = 0u;
    static constexpr uint32_t Output1 = 1u;
    static constexpr uint32_t COUNT   = 8u;
};

// ---------------------------------------------------------------------------
// PerFrameConstants - GPU-Layout für CB0
// Exakt so in den Constant Buffer schreiben (HLSL-konformes Packing, 16-Byte-aligned).
// ---------------------------------------------------------------------------
struct alignas(16) PerFrameConstants
{
    float viewMatrix[16];           // 64 bytes
    float projMatrix[16];           // 64 bytes
    float viewProjMatrix[16];       // 64 bytes
    float invViewProjMatrix[16];    // 64 bytes
    float cameraPositionWS[4];      // xyz=pos, w=1
    float cameraForwardWS[4];       // xyz=dir, w=0
    float screenSize[4];            // x=width, y=height, z=1/width, w=1/height
    float timeParams[4];            // x=time, y=deltaTime, z=frameIndex, w=0
    float ambientColor[4];          // xyz=color, w=intensity
    uint32_t lightCount;
    uint32_t shadowEnabled;
    float    nearPlane;
    float    farPlane;
};
static constexpr size_t PerFrameConstantsSize = sizeof(PerFrameConstants);
static_assert(PerFrameConstantsSize % 16u == 0u, "PerFrameConstants must be 16-byte aligned");

// ---------------------------------------------------------------------------
// PerObjectConstants - GPU-Layout für CB1
// Wird in RenderWorld.hpp definiert und hier nur referenziert.
// Felder: worldMatrix[16], worldMatrixInvT[16], objectColor[4], entityParams[4]
// ---------------------------------------------------------------------------
// (See RenderWorld.hpp for PerObjectConstants definition)

// ---------------------------------------------------------------------------
// PerPassConstants - GPU-Layout für CB3
// Pass-spezifisch, wird von jedem Pass anders befüllt.
// ---------------------------------------------------------------------------
struct alignas(16) PerPassConstants
{
    float shadowViewProj[16];       // für Shadow Pass
    float params[4];                // pass-spezifische Parameter
    float params2[4];
};
static constexpr size_t PerPassConstantsSize = sizeof(PerPassConstants);
static_assert(PerPassConstantsSize % 16u == 0u, "PerPassConstants must be 16-byte aligned");

} // namespace engine::renderer
