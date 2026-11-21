#pragma once
#include "renderer/RenderFeatureInterfaces.hpp"
#include "renderer/RenderFrameConstants.hpp"
#include <cstdint>
#include <memory>

namespace engine::addons::shadow {

struct ShadowFeatureConfig
{
    uint32_t maxShadowLights = 16u;
    uint32_t maxShadowViews = 64u;
    uint32_t maxShadowAtlasResolution = 6144u;   // see ShadowExtractionConfig
};

// Shadow filter parameters in the add-on's frame parameter block, slots 5 and 6.
// The PBR shaders read them (shaders/pbr_shadow_poisson.*); keep both in sync.
//   featureParams[5]   = lightSizes: ShadowSettings::lightSize per shadow light 0..3
//                        (constant-buffer order, shadowLightMeta)
//   featureParams[6].x = lightSize of the first shadow-casting light (OpenGL, legacy lit)
//   featureParams[6].y = filterModes: ShadowFilter per shadow light 0..3, 4 bits each
// Forward+ reads filter and light size per light from GpuShadowLightData::params.
struct ShadowFilterFrameParams
{
    float    lightSizes[4] = { 0.f, 0.f, 0.f, 0.f };
    float    lightSize = 0.f;
    uint32_t filterModes = 0u;
    float    _pad[2] = { 0.f, 0.f };
};
static_assert(sizeof(ShadowFilterFrameParams) == 32u, "ShadowFilterFrameParams must fill two slots");
static_assert(renderer::kMaxShadowLightsPerFrame <= 4u, "filterModes and lightSizes hold 4 lights");
inline constexpr renderer::FrameParameterBlockDesc kShadowFilterFrameParameterBlock{ "krom-shadow-filter", 5u, 2u };

[[nodiscard]] std::unique_ptr<renderer::IEngineFeature> CreateShadowFeature(
    ShadowFeatureConfig config = {});

} // namespace engine::addons::shadow
