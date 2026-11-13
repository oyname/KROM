#pragma once
// =============================================================================
// KROM Engine - addons/forward/ForwardShading.hpp
// BRDF options of the PBR shaders (pbr_forward_plus.ps.hlsl, pbr_lit.ps.hlsl,
// pbr_lit.opengl.fs.glsl). Owned by the forward add-on; the shaders read them
// from the add-on's frame parameter block (assets/pbr_shading_options.*).
// All options off = the shading before these options existed.
// =============================================================================
#include "renderer/RenderFrameConstants.hpp"
#include <cstdint>

namespace engine::renderer::addons::forward {

struct ShadingSettings
{
    // Direct specular: height-correlated Smith GGX visibility instead of the
    // separable Schlick-GGX form.
    bool heightCorrelatedVisibility = false;
    // Direct diffuse: Fresnel weight per light from its half vector instead of
    // one view-angle value for all lights.
    bool perLightDiffuseFresnel = false;
    // Specular (direct and IBL): multi-scatter energy compensation from the
    // BRDF LUT. Only in shaders built with IBL (the LUT is bound there).
    bool multiScatterCompensation = false;

    [[nodiscard]] friend constexpr bool operator==(const ShadingSettings&, const ShadingSettings&) noexcept = default;
};

inline constexpr uint32_t kShadingFlagHeightCorrelatedVisibility = 1u << 0;
inline constexpr uint32_t kShadingFlagPerLightDiffuseFresnel     = 1u << 1;
inline constexpr uint32_t kShadingFlagMultiScatterCompensation   = 1u << 2;

[[nodiscard]] inline constexpr uint32_t ShadingFlags(const ShadingSettings& settings) noexcept
{
    return (settings.heightCorrelatedVisibility ? kShadingFlagHeightCorrelatedVisibility : 0u)
         | (settings.perLightDiffuseFresnel     ? kShadingFlagPerLightDiffuseFresnel     : 0u)
         | (settings.multiScatterCompensation   ? kShadingFlagMultiScatterCompensation   : 0u);
}

// featureParams[2]; must match the #defines in assets/pbr_shading_options.*.
struct ShadingFrameParams
{
    uint32_t flags = 0u;
    float    _pad[3] = { 0.f, 0.f, 0.f };
};
static_assert(sizeof(ShadingFrameParams) == 16u, "ShadingFrameParams must fill one slot");
inline constexpr FrameParameterBlockDesc kShadingFrameParameterBlock{ "krom-forward-shading", 2u, 1u };

} // namespace engine::renderer::addons::forward
