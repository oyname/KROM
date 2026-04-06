#pragma once

#include "renderer/FeatureRegistry.hpp"
#include <memory>

namespace engine::renderer::addons::forward {

std::unique_ptr<IEngineFeature> CreateForwardFeature();

} // namespace engine::renderer::addons::forward
