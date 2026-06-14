#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::renderer {

static constexpr uint32_t kFrameFeaturePayloadBytes = 512u;
static constexpr uint32_t kMaxShadowLightsPerFrame = 4u;
static constexpr uint32_t kMaxShadowViewsPerFrame = 16u;

enum DebugFlags : uint32_t
{
    DBG_DISABLE_IBL_SPEC  = 1u << 0,
    DBG_DISABLE_IBL       = 1u << 1,
    DBG_DISABLE_SHADOWS   = 1u << 2,
    DBG_DISABLE_AO        = 1u << 3,
    DBG_DISABLE_NORMALMAP = 1u << 4,
    DBG_VIEW_NORMALS      = 1u << 8,
    DBG_VIEW_NOL          = 1u << 9,
    DBG_VIEW_ROUGHNESS    = 1u << 10,
    DBG_VIEW_METALLIC     = 1u << 11,
    DBG_VIEW_AO           = 1u << 12,
    DBG_VIEW_SHADOW       = 1u << 13,
    DBG_VIEW_DIRECT_DIFF  = 1u << 14,
    DBG_VIEW_DIRECT_SPEC  = 1u << 15,
    DBG_VIEW_IBL_DIFF     = 1u << 16,
    DBG_VIEW_IBL_SPEC     = 1u << 17,
    DBG_VIEW_FRESNEL_F0   = 1u << 18,
    DBG_VIEW_LINEAR_DEPTH  = 1u << 19,
    DBG_VIEW_GTAO          = 1u << 20,
    DBG_DISABLE_GTAO       = 1u << 21,
};

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
    uint32_t shadowCascadeCount;
    float    nearPlane;
    float    farPlane;
    std::byte featurePayload[kFrameFeaturePayloadBytes];
    float    iblPrefilterLevels;
    float    shadowBias;
    float    shadowNormalBias;
    float    shadowStrength;
    float    shadowTexelSize;
    uint32_t debugFlags;
    uint32_t shadowFilterMode;
    float    _shadowPad;
    float    shadowLightMeta[kMaxShadowLightsPerFrame][4];
    float    shadowLightExtra[kMaxShadowLightsPerFrame][4];
    float    shadowViewRect[kMaxShadowViewsPerFrame][4];
    float    shadowViewProj[kMaxShadowViewsPerFrame][16];
    uint32_t shadowLightCount;
    uint32_t shadowViewCount;
    float    _shadowArrayPad[2];

    // GTAO post-process parameters (set by GtaoFeature via IFrameConstantsContributor)
    float    gtaoRadius;    // offset 2320  packoffset(c145)
    float    gtaoBias;      // offset 2324  packoffset(c145.y)
    float    gtaoIntensity; // offset 2328  packoffset(c145.z)
    uint32_t gtaoEnabled;   // offset 2332  packoffset(c145.w)
};
static_assert(sizeof(FrameConstants) == 2336u, "FrameConstants size mismatch - update all shaders");
static_assert(offsetof(FrameConstants, featurePayload) == 352u, "featurePayload must start at offset 352");

} // namespace engine::renderer
