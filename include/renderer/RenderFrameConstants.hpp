#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace engine::renderer {

static constexpr uint32_t kFrameFeaturePayloadBytes = 512u;
static constexpr uint32_t kMaxShadowLightsPerFrame = 4u;
static constexpr uint32_t kMaxShadowViewsPerFrame = 16u;

// Generic per-frame parameter area for add-on blocks (see FrameParameterBlockDesc).
// The core only zeroes it; its meaning is owned by the add-on that claimed the slots.
static constexpr uint32_t kFrameParameterSlotBytes = 16u;
static constexpr uint32_t kFrameParameterSlotCount = 4u;

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
    uint32_t shadowBufferLightCount;
    uint32_t shadowBufferViewCount;


    // Add-on parameter blocks: kFrameParameterSlotCount slots of 16 bytes.
    // offset 2320  packoffset(c145) .. c148; HLSL/GLSL: float4/vec4 featureParams[4]
    float    featureParams[kFrameParameterSlotCount][4];
};
static_assert(sizeof(FrameConstants) == 2384u, "FrameConstants size mismatch - update all shaders");
static_assert(offsetof(FrameConstants, featurePayload) == 352u, "featurePayload must start at offset 352");
static_assert(offsetof(FrameConstants, featureParams) == 2320u, "featureParams must start at offset 2320 (c145)");

// An add-on claims a fixed slot range of FrameConstants::featureParams at registration
// (FeatureRegistrationContext::RegisterFrameParameterBlock). The registry rejects overlaps.
// Shaders address the block by the same slot index (featureParams[firstSlot]).
struct FrameParameterBlockDesc
{
    std::string_view name;
    uint32_t firstSlot = 0u;
    uint32_t slotCount = 0u;
};

[[nodiscard]] constexpr bool IsFrameParameterBlockInRange(const FrameParameterBlockDesc& block) noexcept
{
    return block.slotCount > 0u
        && block.firstSlot < kFrameParameterSlotCount
        && block.slotCount <= kFrameParameterSlotCount - block.firstSlot;
}

// Copies an add-on's block struct into its claimed slots. Returns false if it does not fit.
template<typename T>
bool WriteFrameParameterBlock(FrameConstants& frameConstants,
                              const FrameParameterBlockDesc& block,
                              const T& value) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>, "frame parameter blocks must be trivially copyable");
    if (!IsFrameParameterBlockInRange(block) || sizeof(T) > block.slotCount * kFrameParameterSlotBytes)
        return false;
    std::memcpy(frameConstants.featureParams[block.firstSlot], &value, sizeof(T));
    return true;
}

} // namespace engine::renderer
