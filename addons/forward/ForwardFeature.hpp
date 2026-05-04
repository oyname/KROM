#pragma once

#include "renderer/FeatureRegistry.hpp"
#include <array>
#include <memory>

namespace engine::renderer::addons::forward {

struct ForwardFeatureConfig
{
    std::array<float, 4> clearColorValue = { 0.3f, 0.3f, 0.3f, 1.f };
    bool enableEnvironmentBackground = false;
};

std::unique_ptr<IEngineFeature> CreateForwardFeature(ForwardFeatureConfig config = {});

} // namespace engine::renderer::addons::forward
