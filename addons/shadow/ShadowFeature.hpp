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
    uint32_t maxShadowAtlasResolution = 4096u;
};

// Filter parameters of the first shadow-casting light, in the add-on's frame
// parameter block. The PBR shaders read featureParams[6]
// (assets/pbr_shadow_poisson.*); keep the slot in sync with them.
struct ShadowFilterFrameParams
{
    float lightSize = 0.f;   // ShadowSettings::lightSize (ShadowFilter::PCSS)
    float _pad[3] = { 0.f, 0.f, 0.f };
};
static_assert(sizeof(ShadowFilterFrameParams) == 16u, "ShadowFilterFrameParams must fill one slot");
inline constexpr renderer::FrameParameterBlockDesc kShadowFilterFrameParameterBlock{ "krom-shadow-filter", 6u, 1u };

[[nodiscard]] std::unique_ptr<renderer::IEngineFeature> CreateShadowFeature(
    ShadowFeatureConfig config = {});

} // namespace engine::addons::shadow
