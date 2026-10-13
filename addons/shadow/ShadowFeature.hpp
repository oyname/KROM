#pragma once
#include "renderer/RenderFeatureInterfaces.hpp"
#include <cstdint>
#include <memory>

namespace engine::addons::shadow {

struct ShadowFeatureConfig
{
    uint32_t maxShadowLights = 16u;
    uint32_t maxShadowViews = 64u;
    uint32_t maxShadowAtlasResolution = 4096u;
};

[[nodiscard]] std::unique_ptr<renderer::IEngineFeature> CreateShadowFeature(
    ShadowFeatureConfig config = {});

} // namespace engine::addons::shadow
